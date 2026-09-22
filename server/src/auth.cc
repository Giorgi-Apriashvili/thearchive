#include "auth.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <regex>

#include "crypto.h"

namespace archive {
namespace {

constexpr int kSessionDays = 30;
constexpr std::size_t kMinPasswordLength = 6;

}  // namespace

std::string normaliseUsername(const std::string& raw) {
    // Stored and compared lowercase because SQLite's default text comparison is
    // case-sensitive: without this, `Alice` and `alice` would be two distinct accounts
    // that the UNIQUE constraint happily allows. Files carry `uploaded_by` and the
    // download page shows who contributed each one, so that is an impersonation vector
    // among people who recognise each other by name.
    //
    // Normalising rather than rejecting also stops phone keyboards, which capitalise the
    // first letter by default, from producing an error the user did not cause.
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };

    std::string out = raw;
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), notSpace));
    out.erase(std::find_if(out.rbegin(), out.rend(), notSpace).base(), out.end());
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

namespace {

void validateUsername(const std::string& username) {
    static const std::regex pattern{"^[a-z0-9_-]{3,32}$"};
    if (!std::regex_match(username, pattern)) {
        throw HttpError{400,
                        "username must be 3-32 characters of a-z, 0-9, underscore or dash"};
    }
}

void validatePassword(const std::string& password) {
    // Length only. Composition rules push people toward predictable substitutions
    // without meaningfully raising the cost of a guess.
    //
    // Six is short, and it is the login endpoint's rate limiting rather than this
    // minimum that decides whether that matters. Registration is invite-only, so the
    // exposed surface is a login form for a handful of known accounts.
    if (password.size() < kMinPasswordLength) {
        throw HttpError{400, "password must be at least " +
                                 std::to_string(kMinPasswordLength) + " characters"};
    }
}

// jsonError, guarded, requireString and nowSeconds now live in httputil.h, shared with
// the share and upload handlers.

}  // namespace

bool isValidRole(const std::string& role) {
    return roleRank(role) >= 0;
}

int roleRank(const std::string& role) {
    if (role == "user") return 0;
    if (role == "privileged") return 1;
    if (role == "admin") return 2;
    return -1;  // fail closed
}

bool secureCookiesEnabled() {
    const char* value = std::getenv("ARCHIVE_SECURE_COOKIES");
    return value == nullptr || std::string{value} != "0";
}

std::string Auth::startSession(std::int64_t userId) {
    const std::string token = crypto::randomToken(32);
    // Only the hash is stored. A leaked database then yields no usable session tokens,
    // exactly as with passwords.
    auto stmt = db_.prepare(
        "INSERT INTO sessions (token, user_id, created_at, expires_at) "
        "VALUES (?, ?, ?, ?)");
    const std::int64_t now = nowSeconds();
    stmt.bind(1, crypto::sha256Hex(token))
        .bind(2, userId)
        .bind(3, now)
        .bind(4, now + std::int64_t{kSessionDays} * 86400)
        .run();
    return token;
}

bool Auth::hasAnyUser() const {
    auto stmt = db_.prepare("SELECT 1 FROM users LIMIT 1");
    return stmt.step();
}

std::string Auth::registerUser(const std::string& inviteCode, const std::string& rawUsername,
                               const std::string& password) {
    const std::string username = normaliseUsername(rawUsername);
    validateUsername(username);
    validatePassword(password);

    const std::int64_t now = nowSeconds();

    auto invite = db_.prepare(
        "SELECT code FROM invites WHERE code = ? AND used_by IS NULL "
        "AND (expires_at IS NULL OR expires_at > ?)");
    invite.bind(1, inviteCode).bind(2, now);
    if (!invite.step()) {
        throw HttpError{403, "invite code is invalid, already used, or expired"};
    }

    auto taken = db_.prepare("SELECT 1 FROM users WHERE username = ?");
    taken.bind(1, username);
    if (taken.step()) {
        throw HttpError{409, "username is taken"};
    }

    const std::string hash = crypto::hashPassword(password);

    auto insert = db_.prepare(
        "INSERT INTO users (username, password_hash, created_at) VALUES (?, ?, ?)");
    insert.bind(1, username).bind(2, hash).bind(3, now).run();
    const std::int64_t userId = db_.lastInsertId();

    auto consume = db_.prepare("UPDATE invites SET used_by = ? WHERE code = ?");
    consume.bind(1, userId).bind(2, inviteCode).run();

    return startSession(userId);
}

std::string Auth::bootstrapAdmin(const std::string& rawUsername, const std::string& password) {
    if (hasAnyUser()) {
        throw HttpError{403, "instance is already initialised"};
    }
    const std::string username = normaliseUsername(rawUsername);
    validateUsername(username);
    validatePassword(password);

    const std::string hash = crypto::hashPassword(password);
    auto insert = db_.prepare(
        "INSERT INTO users (username, password_hash, created_at, role) "
        "VALUES (?, ?, ?, 'admin')");
    insert.bind(1, username).bind(2, hash).bind(3, nowSeconds()).run();
    return startSession(db_.lastInsertId());
}

std::string Auth::login(const std::string& rawUsername, const std::string& password) {
    // Lookup must normalise too: an account stored as `giorgi` has to be findable by
    // someone who typed `Giorgi` at the login form.
    auto stmt = db_.prepare(
        "SELECT id, password_hash, disabled_at FROM users WHERE username = ?");
    stmt.bind(1, normaliseUsername(rawUsername));

    if (!stmt.step()) {
        // Hash anyway so an unknown username takes the same time as a wrong password.
        // Otherwise the response time enumerates accounts.
        static const std::string decoy = crypto::hashPassword("decoy-password-value");
        (void)crypto::verifyPassword(decoy, password);
        throw HttpError{401, "invalid credentials"};
    }

    const std::int64_t userId = stmt.columnInt(0);
    const std::string hash = stmt.columnText(1);
    const bool disabled = !stmt.columnIsNull(2);
    if (!crypto::verifyPassword(hash, password)) {
        throw HttpError{401, "invalid credentials"};
    }
    if (disabled) {
        // Verified first regardless, so a disabled account is not distinguishable from
        // a wrong password by timing or by which error comes back.
        throw HttpError{403, "this account has been disabled"};
    }
    return startSession(userId);
}

std::optional<User> Auth::userForSession(const std::string& token) const {
    if (token.empty()) {
        return std::nullopt;
    }
    auto stmt = db_.prepare(
        "SELECT u.id, u.username, u.role FROM sessions s "
        "JOIN users u ON u.id = s.user_id "
        "WHERE s.token = ? AND s.expires_at > ? AND u.disabled_at IS NULL");
    stmt.bind(1, crypto::sha256Hex(token)).bind(2, nowSeconds());
    if (!stmt.step()) {
        return std::nullopt;
    }
    return User{stmt.columnInt(0), stmt.columnText(1), stmt.columnText(2)};
}

void Auth::logout(const std::string& token) {
    auto stmt = db_.prepare("DELETE FROM sessions WHERE token = ?");
    stmt.bind(1, crypto::sha256Hex(token)).run();
}

std::string Auth::createInvite(std::int64_t createdBy, int validDays) {
    const std::string code = crypto::randomToken(9);  // 12 characters, easy to paste
    auto stmt = db_.prepare(
        "INSERT INTO invites (code, created_by, created_at, expires_at) "
        "VALUES (?, ?, ?, ?)");
    const std::int64_t now = nowSeconds();
    stmt.bind(1, code)
        .bind(2, createdBy)
        .bind(3, now)
        .bind(4, now + std::int64_t{validDays} * 86400)
        .run();
    return code;
}

User requireUser(const drogon::HttpRequestPtr& req, const Auth& auth) {
    auto user = auth.userForSession(req->getCookie(kSessionCookie));
    if (!user) {
        throw HttpError{401, "authentication required"};
    }
    return *user;
}

User requireRole(const drogon::HttpRequestPtr& req, const Auth& auth,
                 const std::string& minimum) {
    const User user = requireUser(req, auth);
    if (roleRank(user.role) < roleRank(minimum)) {
        // 403 rather than 401: the session is perfectly valid, the account simply lacks
        // the permission, and re-authenticating would not change that.
        throw HttpError{403, "this needs " + minimum + " access"};
    }
    return user;
}

namespace {

drogon::HttpResponsePtr sessionResponse(const std::string& token, const std::string& username) {
    Json::Value body;
    body["username"] = username;
    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);

    drogon::Cookie cookie{kSessionCookie, token};
    cookie.setHttpOnly(true);
    cookie.setSecure(secureCookiesEnabled());
    cookie.setPath("/");
    cookie.setSameSite(drogon::Cookie::SameSite::kLax);
    cookie.setMaxAge(std::int64_t{kSessionDays} * 86400);
    resp->addCookie(cookie);
    return resp;
}

}  // namespace

void registerAuthRoutes(Auth& auth) {
    auto& app = drogon::app();

    // Lets the sign-in screen offer first-time setup instead of a login form nobody
    // could yet satisfy. Deliberately exposes one bit and nothing else.
    app.registerHandler(
        "/api/status",
        [&auth](const drogon::HttpRequestPtr&,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                Json::Value body;
                body["initialised"] = auth.hasAnyUser();
                return drogon::HttpResponse::newHttpJsonResponse(body);
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/auth/bootstrap",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                auto json = req->getJsonObject();
                if (!json) {
                    throw HttpError{400, "expected a JSON body"};
                }
                const auto username = normaliseUsername(requireString(*json, "username"));
                const auto token = auth.bootstrapAdmin(username, requireString(*json, "password"));
                return sessionResponse(token, username);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/auth/register",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                auto json = req->getJsonObject();
                if (!json) {
                    throw HttpError{400, "expected a JSON body"};
                }
                const auto username = normaliseUsername(requireString(*json, "username"));
                const auto token = auth.registerUser(requireString(*json, "invite"), username,
                                                     requireString(*json, "password"));
                return sessionResponse(token, username);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/auth/login",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                auto json = req->getJsonObject();
                if (!json) {
                    throw HttpError{400, "expected a JSON body"};
                }
                const auto username = normaliseUsername(requireString(*json, "username"));
                const auto token = auth.login(username, requireString(*json, "password"));
                return sessionResponse(token, username);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/auth/logout",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                auth.logout(req->getCookie(kSessionCookie));
                Json::Value body;
                body["ok"] = true;
                auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
                drogon::Cookie cleared{kSessionCookie, ""};
                cleared.setPath("/");
                cleared.setMaxAge(0);
                resp->addCookie(cleared);
                return resp;
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/me",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                Json::Value body;
                body["username"] = user.username;
                body["role"] = user.role;
                return drogon::HttpResponse::newHttpJsonResponse(body);
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/invites",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                // Privileged and above. Registration stays invite-gated so an open
                // signup cannot turn a public host into a phishing target, but vouching
                // for a newcomer no longer has to route through a single person —
                // that is what the tier is for.
                const User user = requireRole(req, auth, "privileged");
                Json::Value body;
                body["code"] = auth.createInvite(user.id);
                return drogon::HttpResponse::newHttpJsonResponse(body);
            }));
        },
        {drogon::Post});
}

}  // namespace archive
