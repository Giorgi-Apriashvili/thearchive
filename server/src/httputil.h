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
    HttpError(int status, const std::string& message)
        : std::runtime_error(message), status_(status) {}
    int status() const { return status_; }

private:
    int status_;
};

// Unix epoch seconds — the unit every timestamp column in the schema uses.
inline std::int64_t nowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

inline drogon::HttpResponsePtr jsonError(int status, const std::string& message) {
    Json::Value body;
    body["error"] = message;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    return resp;
}

// Wraps a handler body so HttpError becomes its declared status and anything else
// becomes a 500 — logged in full, but never echoed to the client.
template <typename Fn>
drogon::HttpResponsePtr guarded(Fn&& fn) {
    try {
        return fn();
    } catch (const HttpError& e) {
        return jsonError(e.status(), e.what());
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
