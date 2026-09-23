#pragma once

#include <drogon/drogon.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace archive {

// An error that maps directly onto an HTTP status. Handlers throw it and the guard
// below turns it into a JSON body, which keeps the happy path free of error plumbing.
class HttpError : public std::runtime_error {
public:
    HttpError(int status, const std::string& message, std::string reason = {},
              std::int64_t retryAfter = 0)
        : std::runtime_error(message),
          status_(status),
          reason_(std::move(reason)),
          retryAfter_(retryAfter) {}

    int status() const { return status_; }

    // Optional machine-readable discriminator. Two different 401s — "this link needs a
    // password" and "this link is members only" — need different handling in the client,
    // and it should not be matching on prose.
    const std::string& reason() const { return reason_; }

    // Seconds, for a 429. Sent as Retry-After so a client can wait rather than guess.
    std::int64_t retryAfter() const { return retryAfter_; }

private:
    int status_;
    std::string reason_;
    std::int64_t retryAfter_;
};

// Unix epoch seconds — the unit every timestamp column in the schema uses.
inline std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// Makes client-supplied text safe to write into the log: control characters become '?',
// so a newline in a username or a CSP report cannot start a forged line of its own, and
// the length is capped so a single request cannot write an arbitrarily long one.
inline std::string forLog(std::string text, std::size_t maxLength = 200) {
    if (text.size() > maxLength) {
        text.resize(maxLength);
        text += "...";
    }
    for (char& c : text) {
        if (static_cast<unsigned char>(c) < 0x20 || c == 0x7f) {
            c = '?';
        }
    }
    return text;
}

inline drogon::HttpResponsePtr jsonError(int status, const std::string& message,
                                        const std::string& reason = {},
                                        std::int64_t retryAfter = 0) {
    Json::Value body;
    body["error"] = message;
    if (!reason.empty()) {
        body["reason"] = reason;
    }
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    if (retryAfter > 0) {
        resp->addHeader("Retry-After", std::to_string(retryAfter));
    }
    return resp;
}

// Wraps a handler body so HttpError becomes its declared status and anything else
// becomes a 500 — logged in full, but never echoed to the client.
template <typename Fn>
drogon::HttpResponsePtr guarded(Fn&& fn) {
    try {
        return fn();
    } catch (const HttpError& e) {
        return jsonError(e.status(), e.what(), e.reason(), e.retryAfter());
    } catch (const std::exception& e) {
        LOG_ERROR << "unhandled: " << e.what();
        return jsonError(500, "internal error");
    }
}

inline const Json::Value& requireJson(const drogon::HttpRequestPtr& req,
                                      std::shared_ptr<Json::Value>& holder) {
    holder = req->getJsonObject();
    if (!holder) {
        throw HttpError{400, "expected a JSON body"};
    }
    return *holder;
}

inline std::string requireString(const Json::Value& json, const char* key) {
    if (!json.isMember(key) || !json[key].isString()) {
        throw HttpError{400, std::string{"missing field: "} + key};
    }
    return json[key].asString();
}

inline std::string optionalString(const Json::Value& json, const char* key) {
    if (!json.isMember(key) || !json[key].isString()) {
        return {};
    }
    return json[key].asString();
}

}  // namespace archive
