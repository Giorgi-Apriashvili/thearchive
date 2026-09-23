#pragma once

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "db.h"
#include "httputil.h"
#include "throttle.h"

namespace archive {

// Name of the session cookie. HttpOnly, so the frontend never reads it directly.
inline constexpr const char* kSessionCookie = "archive_session";

// How long a session lasts. Public because the privacy notice states it.
inline constexpr int kSessionDays = 30;

// Usernames are case-insensitive. Input is trimmed and lowercased before validation,
// storage and lookup, so `Giorgi`, `giorgi` and `GIORGI` are all the same account.
std::string normaliseUsername(const std::string& raw);

struct User {
    std::int64_t id = 0;
    std::string username;
    // "user", "privileged" or "admin". `privileged` is deliberately inert for now — it
    // grants nothing beyond `user` — so the tier can be assigned before anyone decides
    // what it should mean.
    std::string role = "user";
    // Profile fields, empty when unset. The display name is cosmetic and chosen by the
    // member, so it never replaces the username where something is being attributed:
    // anyone could call themselves anything, and the username is what is unique.
    std::string displayName;
    std::string avatar;  // version token of the current picture; see avatarUrl()

    bool isAdmin() const { return role == "admin"; }
};

// Where a member's picture is served: "/api/avatars/{id}/{version}", or empty when they
// have none. Built only by the server, so clients treat it as an opaque URL, and the
// version in it changes with the picture, so a URL is valid for exactly one image.
inline std::string avatarUrl(std::int64_t userId, const std::string& version) {
    return version.empty()
               ? std::string{}
               : "/api/avatars/" + std::to_string(userId) + "/" + version;
}

// The roles that may be assigned.
bool isValidRole(const std::string& role);

// Roles are ordered: user < privileged < admin. Returns -1 for anything unrecognised,
// so an unknown value in the database fails closed rather than passing every check.
int roleRank(const std::string& role);

class Auth {
public:
    explicit Auth(Database& db) : db_(db) {}

    // Consumes an invite and creates the account, returning a fresh session token.
    // Registration exists only in this form: there is no open signup. See docs/DESIGN.md.
    std::string registerUser(const std::string& inviteCode, const std::string& username,
                             const std::string& password);

    // `client` is from clientAddress(): it feeds the guessing limit, see throttle.h.
    std::string login(const std::string& username, const std::string& password,
                      const std::optional<std::string>& client);

    // Verifies `current`, sets `next`, and ends every other session for the account.
    // `keepToken` is the caller's own raw session token, which survives — changing your
    // password should not sign you out of the tab you changed it in. What stops a stolen
    // session locking the owner out is the current-password check, not the purge — and
    // that check is throttled exactly as login is, since it is the same credential.
    void changePassword(const User& user, const std::string& current, const std::string& next,
                        const std::string& keepToken, const std::optional<std::string>& client);

    // Renames an account, and carries the rename into the username snapshots that chat
    // keeps (messages.author_name, rooms.creator_name) so the person reads the same way
    // everywhere. Sessions key on user_id and are left alone — a rename is not a reason
    // to sign someone out. Returns the normalised name actually stored.
    std::string renameUser(std::int64_t userId, const std::string& rawUsername);

    // Generates a password, sets it, and ends every session for that account. Returns
    // the plaintext, which is shown once and never stored. This is the only recovery
    // path there is: no mail leaves this system, so a forgotten password has nowhere
    // else to go.
    std::string resetPassword(std::int64_t userId);

    std::optional<User> userForSession(const std::string& token) const;

    void logout(const std::string& token);

    std::string createInvite(std::int64_t createdBy, int validDays = 14);

    // Creates the first account when the instance has no users at all, so a fresh
    // deployment can be bootstrapped without an invite that nobody could have issued.
    // Refuses once any user exists.
    std::string bootstrapAdmin(const std::string& username, const std::string& password);

    bool hasAnyUser() const;

    // Shared by every password check — accounts here, share passwords in shares.cc — so
    // one client has one guessing budget however it spends it. Mutable because it is
    // internally synchronised state reached through the const Auth& that the share
    // handlers hold.
    PasswordThrottle& throttle() const { return throttle_; }

private:
    std::string startSession(std::int64_t userId);

    Database& db_;
    mutable PasswordThrottle throttle_;
};

// Resolves the session cookie to a user, or throws HttpError(401).
User requireUser(const drogon::HttpRequestPtr& req, const Auth& auth);

// As above, but also requires the account rank at or above `minimum`, or throws
// HttpError(403).
User requireRole(const drogon::HttpRequestPtr& req, const Auth& auth,
                 const std::string& minimum);

inline User requireAdmin(const drogon::HttpRequestPtr& req, const Auth& auth) {
    return requireRole(req, auth, "admin");
}

// Whether to mark cookies Secure. Off for plain-HTTP local development, since browsers
// discard Secure cookies on http:// origins and login would silently never persist.
bool secureCookiesEnabled();

void registerAuthRoutes(Auth& auth);

}  // namespace archive
