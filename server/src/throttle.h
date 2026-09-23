#pragma once

#include <drogon/drogon.h>

#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace archive {

// The address to hold a client to, or nothing when there is no meaningful one.
//
// Behind Caddy, the TCP peer is Caddy itself, so the client's address comes from
// X-Forwarded-For — but only when the peer is a private or loopback address, i.e. the
// proxy. Caddy discards any X-Forwarded-For a client sends and sets its own, so the
// rightmost entry is the one Caddy observed (tested against the production image, not
// assumed). Trusting the header from anywhere else would let a client name itself.
//
// A resolved address that is itself private — Docker's gateway, loopback — is not a
// client identity, and limiting on it would put every visitor in one bucket, turning a
// guessing limit into a lockout of everybody. Those come back empty and per-address
// limiting stands aside; per-target limiting still applies.
//
// IPv6 is keyed by /64: a single subscriber is routinely handed 2^64 addresses, so a
// per-address limit on anything narrower is a limit on nothing.
std::optional<std::string> clientAddress(const drogon::HttpRequestPtr& req);

// Limits password guessing: login, change-password and share passwords.
//
// Two layers, because they stop different attackers:
//
//   - per client address: 10 failures in 15 minutes, then refused until the oldest ages
//     out. Stops one machine guessing quickly.
//   - per target (an account, a share): the first 5 failures are free, then each further
//     attempt must wait 1s, 2s, 4s ... capped at 60s after the last failure. Stops many
//     machines guessing one password slowly.
//
// The per-target layer slows rather than locks. Anybody who knows a username can fail
// against it, and a hard lockout would hand them a way to keep its owner out; a delay
// capped at a minute cannot be turned into that.
//
// Checked *before* the password is verified, so a refused attempt costs no Argon2 — the
// hash is deliberately expensive, and unbounded attempts would also be a way to burn the
// server's CPU. Only failures count: a correct password is not an attempt to limit.
//
// In memory, per process: a restart forgets, which is acceptable for a limit measured in
// minutes. Both maps are bounded, so a flood of new addresses or targets cannot grow them
// without limit.
class PasswordThrottle {
public:
    // Seconds before a password for `target` may be checked from `client`, or 0.
    std::int64_t retryAfter(const std::optional<std::string>& client, const std::string& target);

    void failed(const std::optional<std::string>& client, const std::string& target);

    // The right person got in, so the target's slowdown is spent. The client's count is
    // left alone: clearing it on success would let anyone with one valid account reset
    // their budget between guesses at somebody else's.
    void succeeded(const std::string& target);

private:
    struct TargetRecord {
        int failures = 0;
        std::int64_t lastFailure = 0;
    };

    void prune(std::int64_t now);

    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<std::int64_t>> clients_;
    std::unordered_map<std::string, TargetRecord> targets_;
};

// Throws a 429 with Retry-After if the attempt must wait. The message is written for a
// person, because the sign-in and download pages show it as it is.
void requireNotThrottled(PasswordThrottle& throttle, const std::optional<std::string>& client,
                         const std::string& target);

}  // namespace archive
