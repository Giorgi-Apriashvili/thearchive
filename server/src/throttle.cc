#include "throttle.h"

#include <arpa/inet.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#include "httputil.h"

namespace archive {
namespace {

// Milliseconds: at one-second resolution a "wait one second" rule rounds to zero for
// any attempt landing in the same wall-clock second. Monotonic, so a clock adjustment
// cannot release or extend a limit.
std::int64_t nowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

constexpr std::int64_t kWindowMs = 15 * 60 * 1000;
constexpr std::size_t kClientFailures = 10;
constexpr int kTargetFreeFailures = 5;
constexpr std::int64_t kTargetMaxDelayMs = 60 * 1000;
// Per map. Past this, expired entries are pruned, then the least recently active goes.
constexpr std::size_t kMaxEntries = 10000;
// Usernames arrive unvalidated from the login form, and a key is held in memory.
constexpr std::size_t kMaxKeyLength = 100;

struct Address {
    bool v6 = false;
    unsigned char bytes[16] = {};
};

std::optional<Address> parseAddress(std::string text) {
    const auto notSpace = [](unsigned char c) { return c != ' ' && c != '\t'; };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());

    Address address;
    in_addr v4{};
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        std::memcpy(address.bytes, &v4, 4);
        return address;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        // ::ffff:a.b.c.d is an IPv4 client arriving on a dual-stack socket, and has to be
        // judged — and keyed — as the IPv4 address it is.
        static constexpr unsigned char kMapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
        if (std::memcmp(v6.s6_addr, kMapped, sizeof kMapped) == 0) {
            std::memcpy(address.bytes, v6.s6_addr + 12, 4);
            return address;
        }
        address.v6 = true;
        std::memcpy(address.bytes, v6.s6_addr, 16);
        return address;
    }
    return std::nullopt;
}

// Addresses that identify a network position rather than a client. Written out rather
// than taken from trantor's isIntranetIp, which omits IPv6 unique-local (fc00::/7) —
// the range Docker's IPv6 network on the production host uses — and treats only
// 127.0.0.1 of 127/8 as loopback.
bool isPrivate(const Address& a) {
    const unsigned char* b = a.bytes;
    if (!a.v6) {
        return b[0] == 10 ||                            // 10/8
               b[0] == 127 ||                           // loopback
               b[0] == 0 ||                             // "this network"
               (b[0] == 172 && (b[1] & 0xf0) == 16) ||  // 172.16/12, Docker's default
               (b[0] == 192 && b[1] == 168) ||          // 192.168/16
               (b[0] == 169 && b[1] == 254) ||          // link-local
               (b[0] == 100 && (b[1] & 0xc0) == 64);    // 100.64/10, carrier-grade NAT
    }
    static constexpr unsigned char kUnspecified[16] = {};
    static constexpr unsigned char kLoopback[16] = {0, 0, 0, 0, 0, 0, 0, 0,
                                                    0, 0, 0, 0, 0, 0, 0, 1};
    return std::memcmp(b, kUnspecified, 16) == 0 || std::memcmp(b, kLoopback, 16) == 0 ||
           (b[0] & 0xfe) == 0xfc ||                     // fc00::/7, unique local
           (b[0] == 0xfe && (b[1] & 0xc0) == 0x80);     // fe80::/10, link-local
}

std::string keyFor(const Address& a) {
    char text[INET6_ADDRSTRLEN] = {};
    if (!a.v6) {
        inet_ntop(AF_INET, a.bytes, text, sizeof text);
        return text;
    }
    unsigned char prefix[16] = {};
    std::memcpy(prefix, a.bytes, 8);
    inet_ntop(AF_INET6, prefix, text, sizeof text);
    return std::string{text} + "/64";
}

std::int64_t targetDelayMs(int failures) {
    if (failures < kTargetFreeFailures) {
        return 0;
    }
    // 1s after the 5th failure, doubling; the exponent is clamped before shifting.
    const int doublings = std::min(failures - kTargetFreeFailures, 16);
    return std::min<std::int64_t>(kTargetMaxDelayMs, std::int64_t{1000} << doublings);
}

std::string clientForLog(const std::optional<std::string>& client) {
    return client ? *client : std::string{"(no public address)"};
}

// "3 minutes" rather than "180 seconds": this is read by a person on a sign-in page.
std::string waitPhrase(std::int64_t seconds) {
    if (seconds < 60) {
        return std::to_string(seconds) + (seconds == 1 ? " second" : " seconds");
    }
    const std::int64_t minutes = (seconds + 59) / 60;
    return std::to_string(minutes) + (minutes == 1 ? " minute" : " minutes");
}

}  // namespace

std::optional<std::string> clientAddress(const drogon::HttpRequestPtr& req) {
    const std::string peerText = req->peerAddr().toIp();
    std::string candidate = peerText;

    const auto peer = parseAddress(peerText);
    if (peer && isPrivate(*peer)) {
        const std::string forwarded = req->getHeader("X-Forwarded-For");
        if (!forwarded.empty()) {
            // Rightmost: the entry added by the proxy in front of us.
            const auto comma = forwarded.rfind(',');
            candidate = forwarded.substr(comma == std::string::npos ? 0 : comma + 1);
        }
    }

    const auto client = parseAddress(candidate);
    if (!client || isPrivate(*client)) {
        return std::nullopt;
    }
    return keyFor(*client);
}

std::int64_t PasswordThrottle::retryAfter(const std::optional<std::string>& client,
                                          const std::string& rawTarget) {
    const std::string target = rawTarget.substr(0, kMaxKeyLength);
    const std::int64_t now = nowMs();
    std::lock_guard lock{mutex_};

    std::int64_t waitMs = 0;
    if (client) {
        if (auto found = clients_.find(*client); found != clients_.end()) {
            auto& failures = found->second;
            while (!failures.empty() && failures.front() <= now - kWindowMs) {
                failures.pop_front();
            }
            if (failures.size() >= kClientFailures) {
                waitMs = failures.front() + kWindowMs - now;
            }
        }
    }
    if (auto found = targets_.find(target); found != targets_.end()) {
        const TargetRecord& record = found->second;
        if (now - record.lastFailure > kWindowMs) {
            targets_.erase(found);  // quiet long enough: the slate is clean
        } else {
            waitMs = std::max(waitMs, record.lastFailure + targetDelayMs(record.failures) - now);
        }
    }
    // Rounded up: a Retry-After of 0 for a wait of 400ms would invite an immediate retry
    // that is then refused.
    return waitMs > 0 ? (waitMs + 999) / 1000 : 0;
}

void PasswordThrottle::failed(const std::optional<std::string>& client,
                              const std::string& rawTarget) {
    const std::string target = rawTarget.substr(0, kMaxKeyLength);
    const std::int64_t now = nowMs();
    std::lock_guard lock{mutex_};
    prune(now);

    if (client) {
        auto& failures = clients_[*client];
        failures.push_back(now);
        // Only the most recent kClientFailures can ever decide anything.
        while (failures.size() > kClientFailures) {
            failures.pop_front();
        }
        if (failures.size() == kClientFailures && failures.front() > now - kWindowMs) {
            LOG_WARN << "throttle: " << *client << " reached " << kClientFailures
                     << " failed passwords in 15 minutes and is refused until the oldest ages out";
        }
    }

    TargetRecord& record = targets_[target];
    if (now - record.lastFailure > kWindowMs) {
        record.failures = 0;
    }
    ++record.failures;
    record.lastFailure = now;
    LOG_INFO << "failed password for " << forLog(target) << " from " << clientForLog(client);
    if (record.failures == kTargetFreeFailures) {
        LOG_WARN << "throttle: " << forLog(target) << " has had " << kTargetFreeFailures
                 << " failed passwords; further attempts are now slowed";
    }
}

void PasswordThrottle::succeeded(const std::string& rawTarget) {
    const std::string target = rawTarget.substr(0, kMaxKeyLength);
    std::lock_guard lock{mutex_};
    targets_.erase(target);
}

void PasswordThrottle::prune(std::int64_t now) {
    // Only when a map is full: the common case is a handful of entries, and sweeping
    // them on every failure would be work for nothing.
    if (clients_.size() >= kMaxEntries) {
        std::erase_if(clients_, [now](const auto& entry) {
            return entry.second.empty() || entry.second.back() <= now - kWindowMs;
        });
        if (clients_.size() >= kMaxEntries) {
            const auto oldest = std::min_element(
                clients_.begin(), clients_.end(),
                [](const auto& a, const auto& b) { return a.second.back() < b.second.back(); });
            clients_.erase(oldest);
        }
    }
    if (targets_.size() >= kMaxEntries) {
        std::erase_if(targets_, [now](const auto& entry) {
            return now - entry.second.lastFailure > kWindowMs;
        });
        if (targets_.size() >= kMaxEntries) {
            const auto oldest = std::min_element(
                targets_.begin(), targets_.end(), [](const auto& a, const auto& b) {
                    return a.second.lastFailure < b.second.lastFailure;
                });
            targets_.erase(oldest);
        }
    }
}

void requireNotThrottled(PasswordThrottle& throttle, const std::optional<std::string>& client,
                         const std::string& target) {
    const std::int64_t wait = throttle.retryAfter(client, target);
    if (wait > 0) {
        throw HttpError{429,
                        "too many failed attempts — try again in " + waitPhrase(wait),
                        "throttled", wait};
    }
}

}  // namespace archive
