#include "inbox.h"

#include <drogon/drogon.h>

#include <string>

#include "httputil.h"
#include "shares.h"
#include "text.h"

namespace archive {
namespace {

constexpr std::size_t kMaxNote = 300;
// An inbox is a handful of recent things, not an archive; the rest stay until dismissed
// but need not ride along on every load.
constexpr std::int64_t kInboxPage = 100;

std::int64_t parseItemId(const std::string& raw) {
    try {
        return std::stoll(raw);
    } catch (const std::exception&) {
        throw HttpError{404, "no such item"};
    }
}

}  // namespace

void registerInboxRoutes(Database& db, Auth& auth) {
    auto& app = drogon::app();

    app.registerHandler(
        "/api/shares/{token}/send",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& token) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                std::shared_ptr<Json::Value> holder;
                const Json::Value& json = requireJson(req, holder);
                const std::string username = normaliseUsername(requireString(json, "username"));
                const auto note = cleanText(json.get("note", Json::nullValue), "note",
                                            kMaxNote, true);
                const std::int64_t shareId = requireOwnLiveShare(db, token, user.id);

                auto find = db.prepare(
                    "SELECT id FROM users WHERE username = ? AND disabled_at IS NULL");
                find.bind(1, username);
                if (!find.step()) {
                    throw HttpError{404, "no such member"};
                }
                const std::int64_t recipient = find.columnInt(0);
                if (recipient == user.id) {
                    throw HttpError{400, "that is you — your links are already on your page"};
                }

                Json::Value out;
                out["sent"] = true;

                // Blocked: report success and deliver nothing. See inbox.h.
                auto blocked = db.prepare(
                    "SELECT 1 FROM user_blocks WHERE user_id = ? AND blocked_id = ?");
                blocked.bind(1, recipient).bind(2, user.id);
                if (blocked.step()) {
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                // Sending the same link again brings it back to the top as unread, with the
                // new note, rather than stacking a second copy.
                auto upsert = db.prepare(
                    "INSERT INTO share_deliveries "
                    "(share_id, sender_id, recipient_id, note, sent_at) VALUES (?, ?, ?, ?, ?) "
                    "ON CONFLICT(share_id, recipient_id) DO UPDATE SET "
                    "  sender_id = excluded.sender_id, note = excluded.note, "
                    "  sent_at = excluded.sent_at, seen_at = NULL");
                upsert.bind(1, shareId).bind(2, user.id).bind(3, recipient);
                note ? upsert.bind(4, *note) : upsert.bindNull(4);
                upsert.bind(5, nowSeconds()).run();
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/inbox",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                auto stmt = db.prepare(
                    "SELECT d.id, d.share_id, COALESCE(d.note, ''), d.sent_at, "
                    "       d.seen_at IS NOT NULL, u.id, u.username, "
                    "       COALESCE(u.display_name, ''), COALESCE(u.avatar, ''), u.role "
                    "FROM share_deliveries d JOIN users u ON u.id = d.sender_id "
                    "WHERE d.recipient_id = ? ORDER BY d.sent_at DESC, d.id DESC LIMIT ?");
                stmt.bind(1, user.id).bind(2, kInboxPage);

                Json::Value items{Json::arrayValue};
                while (stmt.step()) {
                    Json::Value item;
                    item["id"] = static_cast<Json::Int64>(stmt.columnInt(0));
                    if (const std::string note = stmt.columnText(2); !note.empty()) {
                        item["note"] = note;
                    }
                    item["sent_at"] = static_cast<Json::Int64>(stmt.columnInt(3));
                    item["seen"] = stmt.columnInt(4) != 0;

                    Json::Value sender;
                    sender["username"] = stmt.columnText(6);
                    sender["role"] = stmt.columnText(9);
                    if (const std::string shown = stmt.columnText(7); !shown.empty()) {
                        sender["display_name"] = shown;
                    }
                    if (const std::string pic = stmt.columnText(8); !pic.empty()) {
                        sender["avatar"] = avatarUrl(stmt.columnInt(5), pic);
                    }
                    item["sender"] = sender;
                    // Resolved now, like chat's cards: an item sent before the link expired
                    // reads as expired rather than as a click that fails.
                    item["card"] = shareCard(db, stmt.columnInt(1));
                    items.append(item);
                }

                Json::Value out;
                out["items"] = items;
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/inbox/unread",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                auto stmt = db.prepare(
                    "SELECT COUNT(*) FROM share_deliveries "
                    "WHERE recipient_id = ? AND seen_at IS NULL");
                stmt.bind(1, user.id);
                stmt.step();
                Json::Value out;
                out["count"] = static_cast<Json::Int64>(stmt.columnInt(0));
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});

    app.registerHandler(
        "/api/inbox/seen",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                auto stmt = db.prepare(
                    "UPDATE share_deliveries SET seen_at = ? "
                    "WHERE recipient_id = ? AND seen_at IS NULL");
                stmt.bind(1, nowSeconds()).bind(2, user.id).run();
                Json::Value out;
                out["marked"] = db.changes();
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/inbox/items/{id}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                // Scoped to the recipient: an id from someone else's inbox is simply not
                // found, rather than confirming it exists.
                auto stmt = db.prepare(
                    "DELETE FROM share_deliveries WHERE id = ? AND recipient_id = ?");
                stmt.bind(1, parseItemId(id)).bind(2, user.id).run();
                if (db.changes() == 0) {
                    throw HttpError{404, "no such item"};
                }
                Json::Value out;
                out["dismissed"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Delete});
}

}  // namespace archive
