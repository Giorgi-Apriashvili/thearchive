#include "profiles.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <system_error>

#include "crypto.h"
#include "httputil.h"
#include "mimetype.h"
#include "text.h"
#include "thumbnail.h"

namespace archive {
namespace {

namespace fs = std::filesystem;

constexpr std::size_t kMaxDisplayName = 40;
constexpr std::size_t kMaxBio = 500;
// A phone photo is a few megabytes; this is generous without inviting abuse.
constexpr std::size_t kMaxAvatarBytes = 10 * 1024 * 1024;
// The sweep leaves pictures younger than this alone, so it can never delete one between
// its file being written and the database pointing at it.
constexpr auto kAvatarSweepGrace = std::chrono::minutes{10};

std::int64_t parseUserId(const std::string& raw) {
    try {
        return std::stoll(raw);
    } catch (const std::exception&) {
        throw HttpError{404, "no such picture"};
    }
}

void bindOptional(Stmt& stmt, int index, const std::optional<std::string>& value) {
    value ? stmt.bind(index, *value) : stmt.bindNull(index);
}

}  // namespace

fs::path avatarPath(const fs::path& dataDir, std::int64_t userId, const std::string& version) {
    return dataDir / "avatars" / (std::to_string(userId) + "-" + version + ".webp");
}

int removeStaleAvatars(Database& db, const fs::path& dataDir) {
    std::error_code ec;
    const fs::path dir = dataDir / "avatars";
    if (!fs::is_directory(dir, ec)) {
        return 0;
    }

    int removed = 0;
    for (const auto& entry : fs::directory_iterator{dir, ec}) {
        const std::string name = entry.path().filename().string();
        const auto dash = name.find('-');
        const auto dot = name.rfind(".webp");
        if (dash == std::string::npos || dot == std::string::npos || dot < dash) {
            continue;  // not ours; leave it
        }
        const auto written = fs::last_write_time(entry.path(), ec);
        if (ec || decltype(written)::clock::now() - written < kAvatarSweepGrace) {
            continue;
        }

        std::int64_t userId = 0;
        try {
            userId = std::stoll(name.substr(0, dash));
        } catch (const std::exception&) {
            continue;
        }
        const std::string version = name.substr(dash + 1, dot - dash - 1);
        auto current = db.prepare("SELECT 1 FROM users WHERE id = ? AND avatar = ?");
        current.bind(1, userId).bind(2, version);
        if (!current.step() && fs::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    return removed;
}

void registerProfileRoutes(Database& db, Auth& auth, const fs::path& dataDir) {
    auto& app = drogon::app();

    app.registerHandler(
        "/api/users/{username}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& rawName) {
            callback(guarded([&] {
                const User viewer = requireUser(req, auth);

                auto stmt = db.prepare(
                    "SELECT u.id, u.username, u.role, u.created_at, "
                    "       COALESCE(u.display_name, ''), COALESCE(u.bio, ''), "
                    "       COALESCE(u.avatar, ''), COALESCE(inv.username, '') "
                    "FROM users u "
                    "LEFT JOIN invites i ON i.used_by = u.id "
                    "LEFT JOIN users inv ON inv.id = i.created_by "
                    "WHERE u.username = ?");
                stmt.bind(1, normaliseUsername(rawName));
                if (!stmt.step()) {
                    throw HttpError{404, "no such member"};
                }
                const std::int64_t id = stmt.columnInt(0);

                Json::Value out;
                out["username"] = stmt.columnText(1);
                out["role"] = stmt.columnText(2);
                out["joined"] = static_cast<Json::Int64>(stmt.columnInt(3));
                out["is_me"] = id == viewer.id;
                if (const std::string v = stmt.columnText(4); !v.empty()) {
                    out["display_name"] = v;
                }
                if (const std::string v = stmt.columnText(5); !v.empty()) {
                    out["bio"] = v;
                }
                if (const std::string v = stmt.columnText(6); !v.empty()) {
                    out["avatar"] = avatarUrl(id, v);
                }
                if (const std::string v = stmt.columnText(7); !v.empty()) {
                    out["invited_by"] = v;
                }

                // Rooms both are members of — never the rest of theirs. See profiles.h.
                Json::Value rooms{Json::arrayValue};
                auto shared = db.prepare(
                    "SELECT r.id, r.name FROM rooms r "
                    "JOIN room_members a ON a.room_id = r.id AND a.user_id = ? "
                    "                   AND a.state = 'member' "
                    "JOIN room_members b ON b.room_id = r.id AND b.user_id = ? "
                    "                   AND b.state = 'member' "
                    "ORDER BY r.name COLLATE NOCASE");
                shared.bind(1, viewer.id).bind(2, id);
                while (shared.step()) {
                    Json::Value room;
                    room["id"] = static_cast<Json::Int64>(shared.columnInt(0));
                    room["name"] = shared.columnText(1);
                    rooms.append(room);
                }
                out["shared_rooms"] = rooms;

                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/me/profile",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                std::shared_ptr<Json::Value> holder;
                const Json::Value& json = requireJson(req, holder);

                // Only the fields present change, so the page can save one without
                // having to resend the other.
                if (json.isMember("display_name")) {
                    auto stmt = db.prepare("UPDATE users SET display_name = ? WHERE id = ?");
                    bindOptional(stmt, 1,
                                 cleanText(json["display_name"], "display name",
                                           kMaxDisplayName, false));
                    stmt.bind(2, user.id).run();
                }
                if (json.isMember("bio")) {
                    auto stmt = db.prepare("UPDATE users SET bio = ? WHERE id = ?");
                    bindOptional(stmt, 1, cleanText(json["bio"], "bio", kMaxBio, true));
                    stmt.bind(2, user.id).run();
                }

                Json::Value out;
                out["ok"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Patch});

    app.registerHandler(
        "/api/me/avatar",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::string previous = user.avatar;

                if (req->method() == drogon::Delete) {
                    auto stmt = db.prepare("UPDATE users SET avatar = NULL WHERE id = ?");
                    stmt.bind(1, user.id).run();
                    if (!previous.empty()) {
                        std::error_code ec;
                        fs::remove(avatarPath(dataDir, user.id, previous), ec);
                    }
                    Json::Value out;
                    out["avatar"] = Json::nullValue;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                const std::string_view body = req->body();
                if (body.empty()) {
                    throw HttpError{400, "send the picture as the request body"};
                }
                if (body.size() > kMaxAvatarBytes) {
                    throw HttpError{413, "a picture is at most 10 MB"};
                }
                // The same allowlist as file previews, decided from the bytes rather than
                // the declared type — so libvips is never handed a format it was not
                // meant to decode here, SVG and PDF included.
                if (!thumbnail::isThumbnailable(mime::sniffBytes(body.substr(0, 64)))) {
                    throw HttpError{415, "that is not a picture this site can use"};
                }

                const std::string version = crypto::randomToken(6);
                const fs::path target = avatarPath(dataDir, user.id, version);
                std::error_code ec;
                fs::create_directories(target.parent_path(), ec);
                if (!thumbnail::renderAvatar(body, target)) {
                    throw HttpError{422, "that picture could not be read"};
                }

                auto stmt = db.prepare("UPDATE users SET avatar = ? WHERE id = ?");
                stmt.bind(1, version).bind(2, user.id).run();
                // The old picture goes now rather than at the next sweep: a replaced
                // picture is one the member asked to be rid of.
                if (!previous.empty()) {
                    fs::remove(avatarPath(dataDir, user.id, previous), ec);
                }

                Json::Value out;
                out["avatar"] = avatarUrl(user.id, version);
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Put, drogon::Delete});

    app.registerHandler(
        "/api/avatars/{id}/{version}",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              const std::string& rawId, const std::string& version) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                requireUser(req, auth);
                const std::int64_t userId = parseUserId(rawId);

                // Served only while it is that member's current picture. The version is
                // matched against the database before it goes anywhere near a path, so a
                // crafted value cannot name a file outside avatars/.
                auto stmt = db.prepare("SELECT 1 FROM users WHERE id = ? AND avatar = ?");
                stmt.bind(1, userId).bind(2, version);
                if (!stmt.step()) {
                    throw HttpError{404, "no such picture"};
                }

                auto resp = drogon::HttpResponse::newFileResponse(
                    avatarPath(dataDir, userId, version).string(), "",
                    drogon::CT_CUSTOM, "image/webp");
                // A URL names exactly one picture forever, so it can be cached for good;
                // private, because pictures are for members only.
                resp->addHeader("Cache-Control", "private, max-age=31536000, immutable");
                return resp;
            }));
        },
        {drogon::Get});
}

}  // namespace archive
