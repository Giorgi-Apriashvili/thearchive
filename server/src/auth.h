#pragma once

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "db.h"
#include "httputil.h"

namespace archive {

// Name of the session cookie. HttpOnly, so the frontend never reads it directly.
inline constexpr const char* kSessionCookie = "archive_session";

// Usernames are case-insensitive. Input is trimmed and lowercased before validation,
// storage and lookup, so `Giorgi`, `giorgi` and `GIORGI` are all the same account.
std::string normaliseUsername(const std::string& raw);

struct User {
    std::int64_t id = 0;
    std::string username;
    bool isAdmin = false;
};

class Auth {
public:
    explicit Auth(Database& db) : db_(db) {}

    // Consumes an invite and creates the account, returning a fresh session token.
    // Registration exists only in this form: there is no open signup. See docs/DESIGN.md.
    std::string registerUser(const std::string& inviteCode, const std::string& username,
                             const std::string& password);

    std::string login(const std::string& username, const std::string& password);

    std::optional<User> userForSession(const std::string& token) const;

    void logout(const std::string& token);

    std::string createInvite(std::int64_t createdBy, int validDays = 14);

    // Creates the first account when the instance has no users at all, so a fresh
    // deployment can be bootstrapped without an invite that nobody could have issued.
    // Refuses once any user exists.
    std::string bootstrapAdmin(const std::string& username, const std::string& password);

    bool hasAnyUser() const;

private:
    std::string startSession(std::int64_t userId);

    Database& db_;
};

// Resolves the session cookie to a user, or throws HttpError(401).
User requireUser(const drogon::HttpRequestPtr& req, const Auth& auth);

// As above, but also requires the account be an administrator, or throws HttpError(403).
User requireAdmin(const drogon::HttpRequestPtr& req, const Auth& auth);

// Whether to mark cookies Secure. Off for plain-HTTP local development, since browsers
// discard Secure cookies on http:// origins and login would silently never persist.
bool secureCookiesEnabled();

void registerAuthRoutes(Auth& auth);

}  // namespace archive
