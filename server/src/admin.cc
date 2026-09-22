#include "admin.h"

#include <drogon/drogon.h>

#include <string>
#include <system_error>
#include <vector>

#include "httputil.h"
#include "shares.h"
#include "storage.h"

namespace archive {
namespace {

namespace fs = std::filesystem;

std::int64_t parseId(const std::string& raw) {
    try {
        return std::stoll(raw);
    } catch (const std::exception&) {
        throw HttpError{404, "no such user"};
    }
}

// Guards against locking everyone out. Demoting or removing the last administrator
// would leave nobody able to issue invites or reach this panel at all, and there is no
// recovery path short of editing the database by hand.
void requireAnotherAdminRemains(Database& db, std::int64_t excluding) {
    auto stmt = db.prepare(
        "SELECT COUNT(*) FROM users WHERE role = 'admin' AND disabled_at IS NULL "
        "AND id != ?");
    stmt.bind(1, excluding);
    if (stmt.step() && stmt.columnInt(0) == 0) {
        throw HttpError{409, "this is the only administrator"};
    }
}

Json::Value userRow(Stmt& stmt) {
    Json::Value user;
    user["id"] = static_cast<Json::Int64>(stmt.columnInt(0));
    user["username"] = stmt.columnText(1);
    user["role"] = stmt.columnText(2);
    user["created_at"] = static_cast<Json::Int64>(stmt.columnInt(3));
    if (!stmt.columnIsNull(4)) {
        user["disabled_at"] = static_cast<Json::Int64>(stmt.columnInt(4));
    }
    user["share_count"] = static_cast<Json::Int64>(stmt.columnInt(5));
    user["bytes"] = static_cast<Json::Int64>(stmt.columnInt(6));
    if (const std::string by = stmt.columnText(7); !by.empty()) {
        user["invited_by"] = by;
    }
    return user;
}

// Live shares only, and their sizes, for one owner or all of them. Left joined so a
// user with nothing still appears with zeroes rather than vanishing.
constexpr const char* kUserSelect =
    "SELECT u.id, u.username, u.role, u.created_at, u.disabled_at, "
    "       COUNT(DISTINCT s.id), COALESCE(SUM(sf.size), 0), COALESCE(inv.username, '') "
    "FROM users u "
    "LEFT JOIN shares s ON s.owner_id = u.id AND s.deleted_at IS NULL "
    "                   AND s.released_at IS NULL AND s.expires_at > ?1 "
    "LEFT JOIN share_files sf ON sf.share_id = s.id "
    "LEFT JOIN invites i ON i.used_by = u.id "
    "LEFT JOIN users inv ON inv.id = i.created_by ";

}  // namespace

void registerAdminRoutes(Database& db, Auth& auth, const fs::path& dataDir) {
    auto& app = drogon::app();

    // ---- users -------------------------------------------------------------------
    app.registerHandler(
        "/api/admin/users",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                requireAdmin(req, auth);
                Json::Value out{Json::arrayValue};
                auto stmt = db.prepare(std::string{kUserSelect} +
                                       "GROUP BY u.id ORDER BY u.created_at");
                stmt.bind(1, nowSeconds());
                while (stmt.step()) {
                    out.append(userRow(stmt));
                }
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/admin/users/{id}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const User actor = requireAdmin(req, auth);
                const std::int64_t userId = parseId(id);

                if (req->method() == drogon::Delete) {
                    if (userId == actor.id) {
                        throw HttpError{409, "you cannot delete your own account"};
                    }
                    requireAnotherAdminRemains(db, userId);
                    // Shares and sessions cascade. Their blobs drop to refcount zero and
                    // leave on the normal sweep, so nothing is unlinked inline here.
                    auto del = db.prepare("DELETE FROM users WHERE id = ?");
                    del.bind(1, userId).run();
                    if (db.changes() == 0) {
                        throw HttpError{404, "no such user"};
                    }
                    LOG_WARN << "admin " << actor.username << " deleted user " << userId;
                    Json::Value out;
                    out["deleted"] = true;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                Json::Value out;
                {
                    auto stmt = db.prepare(std::string{kUserSelect} +
                                           "WHERE u.id = ?2 GROUP BY u.id");
                    stmt.bind(1, nowSeconds()).bind(2, userId);
                    if (!stmt.step()) {
                        throw HttpError{404, "no such user"};
                    }
                    out = userRow(stmt);
                }

                Json::Value shares{Json::arrayValue};
                auto list = db.prepare(
                    "SELECT s.token, COALESCE(s.title, ''), s.created_at, s.expires_at, "
                    "       s.download_count, s.deleted_at, s.password_hash IS NOT NULL, "
                    "       COUNT(sf.id), COALESCE(SUM(sf.size), 0), s.visibility "
                    "FROM shares s LEFT JOIN share_files sf ON sf.share_id = s.id "
                    "WHERE s.owner_id = ? GROUP BY s.id ORDER BY s.created_at DESC");
                list.bind(1, userId);
                while (list.step()) {
                    Json::Value share;
                    share["token"] = list.columnText(0);
                    share["title"] = list.columnText(1);
                    share["created_at"] = static_cast<Json::Int64>(list.columnInt(2));
                    share["expires_at"] = static_cast<Json::Int64>(list.columnInt(3));
                    share["download_count"] = static_cast<Json::Int64>(list.columnInt(4));
                    share["revoked"] = !list.columnIsNull(5);
                    share["password_protected"] = list.columnInt(6) != 0;
                    share["file_count"] = static_cast<Json::Int64>(list.columnInt(7));
                    share["total_bytes"] = static_cast<Json::Int64>(list.columnInt(8));
                    share["visibility"] = list.columnText(9);
                    // Built here rather than in the client: behind a proxy the browser
                    // cannot reliably know the public origin, which is why
                    // ARCHIVE_PUBLIC_URL exists.
                    share["url"] = publicBaseUrl() + "/d/" + list.columnText(0);
                    shares.append(share);
                }
                out["shares"] = shares;

                // Who they brought in. The chain was always recorded; nothing surfaced it.
                Json::Value invited{Json::arrayValue};
                auto chain = db.prepare(
                    "SELECT u.username FROM invites i JOIN users u ON u.id = i.used_by "
                    "WHERE i.created_by = ? ORDER BY u.created_at");
                chain.bind(1, userId);
                while (chain.step()) {
                    invited.append(chain.columnText(0));
                }
                out["invited"] = invited;

                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Get, drogon::Delete});

    app.registerHandler(
        "/api/admin/users/{id}/role",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User actor = requireAdmin(req, auth);
                const std::int64_t userId = parseId(id);

                std::shared_ptr<Json::Value> holder;
                const std::string role = requireString(requireJson(req, holder), "role");
                if (!isValidRole(role)) {
                    throw HttpError{400, "role must be user, privileged or admin"};
                }
                if (role != "admin") {
                    requireAnotherAdminRemains(db, userId);
                }

                auto stmt = db.prepare("UPDATE users SET role = ? WHERE id = ?");
                stmt.bind(1, role).bind(2, userId).run();
                if (db.changes() == 0) {
                    throw HttpError{404, "no such user"};
                }
                LOG_INFO << "admin " << actor.username << " set user " << userId
                         << " to " << role;
                Json::Value out;
                out["role"] = role;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    for (const bool disabling : {true, false}) {
        app.registerHandler(
            disabling ? "/api/admin/users/{id}/disable" : "/api/admin/users/{id}/enable",
            [&db, &auth, disabling](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& id) {
                callback(guarded([&] {
                    const User actor = requireAdmin(req, auth);
                    const std::int64_t userId = parseId(id);
                    if (disabling) {
                        if (userId == actor.id) {
                            throw HttpError{409, "you cannot disable your own account"};
                        }
                        requireAnotherAdminRemains(db, userId);
                    }

                    Transaction tx{db};
                    auto stmt = db.prepare("UPDATE users SET disabled_at = ? WHERE id = ?");
                    disabling ? stmt.bind(1, nowSeconds()) : stmt.bindNull(1);
                    stmt.bind(2, userId).run();
                    if (db.changes() == 0) {
                        throw HttpError{404, "no such user"};
                    }
                    if (disabling) {
                        // Sessions outlive the flag otherwise: userForSession already
                        // rejects a disabled account, but dropping the rows makes the
                        // effect immediate and leaves nothing to resurrect.
                        auto kill = db.prepare("DELETE FROM sessions WHERE user_id = ?");
                        kill.bind(1, userId).run();
                    }
                    tx.commit();

                    Json::Value out;
                    out["disabled"] = disabling;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }));
            },
            {drogon::Post});
    }

    app.registerHandler(
        "/api/admin/users/{id}/revoke",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                requireAdmin(req, auth);
                // Only the links die. Files stay, attribution stays, and the blobs go
                // out on the normal sweep once nothing references them.
                auto stmt = db.prepare(
                    "UPDATE shares SET deleted_at = ? WHERE owner_id = ? "
                    "AND deleted_at IS NULL");
                stmt.bind(1, nowSeconds()).bind(2, parseId(id)).run();
                Json::Value out;
                out["revoked"] = static_cast<Json::Int64>(db.changes());
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/admin/users/{id}/username",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& id) {
            callback(guarded([&] {
                const User actor = requireAdmin(req, auth);
                std::shared_ptr<Json::Value> holder;
                const std::int64_t userId = parseId(id);
                const std::string was = requireString(requireJson(req, holder), "username");
                // No guard on renaming yourself, or an admin, or the last admin: a name
                // change removes no privilege and ends no session, so none of the
                // reasons the other handlers here have for refusing apply.
                const std::string now = auth.renameUser(userId, was);
                LOG_WARN << "admin " << actor.username << " renamed user " << userId
                         << " to " << now;
                Json::Value out;
                out["username"] = now;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/admin/users/{id}/password",
        [&auth](const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& id) {
            callback(guarded([&] {
                const User actor = requireAdmin(req, auth);
                const std::int64_t userId = parseId(id);
                // An admin changing their own password goes through the door that asks
                // for the current one. This one does not ask, and should not become a
                // way around that for the one account that can reach it.
                if (userId == actor.id) {
                    throw HttpError{409, "change your own password from your account page"};
                }
                // No requireAnotherAdminRemains: a reset does not reduce the number of
                // administrators, and the new password is handed straight back.
                const std::string password = auth.resetPassword(userId);
                LOG_WARN << "admin " << actor.username << " reset the password of user "
                         << userId;
                Json::Value out;
                out["password"] = password;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    // ---- invites -----------------------------------------------------------------
    app.registerHandler(
        "/api/admin/invites",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                requireAdmin(req, auth);
                Json::Value out{Json::arrayValue};
                auto stmt = db.prepare(
                    "SELECT i.code, i.created_at, i.expires_at, "
                    "       COALESCE(c.username, ''), COALESCE(r.username, '') "
                    "FROM invites i "
                    "LEFT JOIN users c ON c.id = i.created_by "
                    "LEFT JOIN users r ON r.id = i.used_by "
                    "ORDER BY i.created_at DESC");
                const std::int64_t now = nowSeconds();
                while (stmt.step()) {
                    Json::Value invite;
                    invite["code"] = stmt.columnText(0);
                    invite["created_at"] = static_cast<Json::Int64>(stmt.columnInt(1));
                    invite["created_by"] = stmt.columnText(3);
                    const std::string redeemer = stmt.columnText(4);
                    const bool expired =
                        !stmt.columnIsNull(2) && stmt.columnInt(2) <= now;
                    if (!stmt.columnIsNull(2)) {
                        invite["expires_at"] = static_cast<Json::Int64>(stmt.columnInt(2));
                    }
                    if (!redeemer.empty()) {
                        invite["used_by"] = redeemer;
                    }
                    invite["state"] =
                        !redeemer.empty() ? "used" : (expired ? "expired" : "open");
                    out.append(invite);
                }
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/admin/invites/{code}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& code) {
            callback(guarded([&] {
                requireAdmin(req, auth);
                // Only unredeemed codes. Deleting a used one would erase the record of
                // who vouched for an existing member.
                auto stmt = db.prepare(
                    "DELETE FROM invites WHERE code = ? AND used_by IS NULL");
                stmt.bind(1, code).run();
                if (db.changes() == 0) {
                    throw HttpError{404, "no open invite with that code"};
                }
                Json::Value out;
                out["revoked"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Delete});

    // ---- shares ------------------------------------------------------------------
    app.registerHandler(
        "/api/admin/shares",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                requireAdmin(req, auth);
                Json::Value out{Json::arrayValue};
                auto stmt = db.prepare(
                    "SELECT s.token, COALESCE(s.title, ''), COALESCE(u.username, ''), "
                    "       s.created_at, s.expires_at, s.download_count, "
                    "       s.password_hash IS NOT NULL, COUNT(sf.id), "
                    "       COALESCE(SUM(sf.size), 0), s.visibility "
                    "FROM shares s LEFT JOIN users u ON u.id = s.owner_id "
                    "LEFT JOIN share_files sf ON sf.share_id = s.id "
                    "WHERE s.deleted_at IS NULL AND s.released_at IS NULL "
                    "AND s.expires_at > ? GROUP BY s.id ORDER BY s.created_at DESC");
                stmt.bind(1, nowSeconds());
                while (stmt.step()) {
                    Json::Value share;
                    share["token"] = stmt.columnText(0);
                    share["title"] = stmt.columnText(1);
                    share["owner"] = stmt.columnText(2);
                    share["created_at"] = static_cast<Json::Int64>(stmt.columnInt(3));
                    share["expires_at"] = static_cast<Json::Int64>(stmt.columnInt(4));
                    share["download_count"] = static_cast<Json::Int64>(stmt.columnInt(5));
                    share["password_protected"] = stmt.columnInt(6) != 0;
                    share["file_count"] = static_cast<Json::Int64>(stmt.columnInt(7));
                    share["total_bytes"] = static_cast<Json::Int64>(stmt.columnInt(8));
                    share["visibility"] = stmt.columnText(9);
                    share["url"] = publicBaseUrl() + "/d/" + stmt.columnText(0);
                    out.append(share);
                }
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/admin/shares/{token}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& token) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                requireAdmin(req, auth);

                if (req->method() == drogon::Patch) {
                    std::shared_ptr<Json::Value> holder;
                    const std::string wanted =
                        requireString(requireJson(req, holder), "visibility");
                    if (wanted != "private" && wanted != "public") {
                        throw HttpError{400, "visibility must be private or public"};
                    }
                    auto set = db.prepare(
                        "UPDATE shares SET visibility = ? WHERE token = ? "
                        "AND deleted_at IS NULL");
                    set.bind(1, wanted).bind(2, token).run();
                    if (db.changes() == 0) {
                        throw HttpError{404, "no such share"};
                    }
                    Json::Value out;
                    out["visibility"] = wanted;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                auto stmt = db.prepare(
                    "UPDATE shares SET deleted_at = ? WHERE token = ? AND deleted_at IS NULL");
                stmt.bind(1, nowSeconds()).bind(2, token).run();
                if (db.changes() == 0) {
                    throw HttpError{404, "no such share"};
                }
                Json::Value out;
                out["revoked"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Delete, drogon::Patch});

    // ---- overview ----------------------------------------------------------------
    app.registerHandler(
        "/api/admin/overview",
        [&db, &auth, dataDir](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                requireAdmin(req, auth);
                Json::Value out;

                const auto scalar = [&db](const char* sql) {
                    auto stmt = db.prepare(sql);
                    return stmt.step() ? stmt.columnInt(0) : 0;
                };
                out["users"] = static_cast<Json::Int64>(scalar("SELECT COUNT(*) FROM users"));
                out["blobs"] = static_cast<Json::Int64>(scalar("SELECT COUNT(*) FROM blobs"));
                out["stored_bytes"] = static_cast<Json::Int64>(
                    scalar("SELECT COALESCE(SUM(size),0) FROM blobs"));
                out["live_shares"] = static_cast<Json::Int64>(scalar(
                    "SELECT COUNT(*) FROM shares WHERE deleted_at IS NULL "
                    "AND released_at IS NULL"));
                out["open_invites"] = static_cast<Json::Int64>(
                    scalar("SELECT COUNT(*) FROM invites WHERE used_by IS NULL"));
                out["uploads_in_flight"] = static_cast<Json::Int64>(scalar(
                    "SELECT COUNT(*) FROM uploads WHERE completed_at IS NULL"));
                // Awaiting collection: already unreferenced, still on disk.
                out["orphan_bytes"] = static_cast<Json::Int64>(scalar(
                    "SELECT COALESCE(SUM(size),0) FROM blobs WHERE refcount <= 0"));

                Json::Value perUser{Json::arrayValue};
                auto stmt = db.prepare(
                    "SELECT u.username, COUNT(DISTINCT b.sha256), "
                    "       COALESCE(SUM(DISTINCT b.size), 0) "
                    "FROM users u LEFT JOIN blobs b ON b.first_uploader = u.id "
                    "GROUP BY u.id ORDER BY 3 DESC");
                while (stmt.step()) {
                    Json::Value row;
                    row["username"] = stmt.columnText(0);
                    row["blobs"] = static_cast<Json::Int64>(stmt.columnInt(1));
                    row["bytes"] = static_cast<Json::Int64>(stmt.columnInt(2));
                    perUser.append(row);
                }
                out["by_uploader"] = perUser;

                std::error_code ec;
                const fs::space_info space = fs::space(dataDir, ec);
                if (!ec) {
                    out["disk_total"] = static_cast<Json::Int64>(space.capacity);
                    out["disk_available"] = static_cast<Json::Int64>(space.available);
                }

                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});
}

}  // namespace archive
