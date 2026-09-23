#include "inbox.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "crypto.h"
#include "httputil.h"
#include "shares.h"
#include "text.h"

namespace archive {
namespace {

constexpr std::size_t kMaxNote = 300;
// The same ceiling as links attached to one chat message.
constexpr std::size_t kMaxLinksPerSend = 10;
// An inbox is a handful of recent things, not an archive; the rest stay until dismissed
// but need not ride along on every load.
constexpr std::int64_t kInboxPage = 100;

// Delivers `tokens` to `rawUsername` as one inbox item. See inbox.h for the rules.
drogon::HttpResponsePtr sendLinks(Database& db, const User& user,
                                  const std::string& rawUsername,
                                  const std::vector<std::string>& tokens,
                                  const std::optional<std::string>& note) {
    if (tokens.empty()) {
        throw HttpError{400, "choose at least one link to send"};
    }
    if (tokens.size() > kMaxLinksPerSend) {
        throw HttpError{400, "you can send at most " + std::to_string(kMaxLinksPerSend) +
                                 " links at once"};
    }
    // Every link checked before anything is written. The same link twice counts once.
    std::vector<std::int64_t> shareIds;
    for (const std::string& token : tokens) {
        const std::int64_t shareId = requireOwnLiveShare(db, token, user.id);
        if (std::find(shareIds.begin(), shareIds.end(), shareId) == shareIds.end()) {
            shareIds.push_back(shareId);
        }
    }

    auto find = db.prepare("SELECT id FROM users WHERE username = ? AND disabled_at IS NULL");
    find.bind(1, normaliseUsername(rawUsername));
    if (!find.step()) {
        throw HttpError{404, "no such member"};
    }
    const std::int64_t recipient = find.columnInt(0);
    if (recipient == user.id) {
        throw HttpError{400, "that is you — your links are already on your page"};
    }

    Json::Value out;
    out["sent"] = static_cast<Json::UInt64>(shareIds.size());

    // Blocked: report success and deliver nothing. See inbox.h.
    auto blocked = db.prepare("SELECT 1 FROM user_blocks WHERE user_id = ? AND blocked_id = ?");
    blocked.bind(1, recipient).bind(2, user.id);
    if (blocked.step()) {
        return drogon::HttpResponse::newHttpJsonResponse(out);
    }

    // Sending a link that is already there moves it into this item, unread again and
    // with the new note, rather than stacking a second copy.
    const std::string batch = crypto::randomToken(12);
    const std::int64_t now = nowSeconds();
    Transaction tx{db};
    std::int64_t position = 0;
    for (const std::int64_t shareId : shareIds) {
        auto upsert = db.prepare(
            "INSERT INTO share_deliveries "
            "(share_id, sender_id, recipient_id, note, sent_at, batch, position) "
            "VALUES (?, ?, ?, ?, ?, ?, ?) "
            "ON CONFLICT(share_id, recipient_id) DO UPDATE SET "
            "  sender_id = excluded.sender_id, note = excluded.note, "
            "  sent_at = excluded.sent_at, seen_at = NULL, "
            "  batch = excluded.batch, position = excluded.position");
        upsert.bind(1, shareId).bind(2, user.id).bind(3, recipient);
        note ? upsert.bind(4, *note) : upsert.bindNull(4);
        upsert.bind(5, now).bind(6, batch).bind(7, position++).run();
    }
    tx.commit();
    return drogon::HttpResponse::newHttpJsonResponse(out);
}

}  // namespace

void registerInboxRoutes(Database& db, Auth& auth) {
    auto& app = drogon::app();

    app.registerHandler(
        "/api/users/{username}/links",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& username) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                std::shared_ptr<Json::Value> holder;
                const Json::Value& json = requireJson(req, holder);
                const Json::Value& links = json["links"];
                if (!links.isArray()) {
                    throw HttpError{400, "links must be a list of links"};
                }
                std::vector<std::string> tokens;
                for (const Json::Value& token : links) {
                    if (!token.isString()) {
                        throw HttpError{400, "links must be a list of links"};
                    }
                    tokens.push_back(token.asString());
                }
                const auto note = cleanText(json.get("note", Json::nullValue), "note",
                                            kMaxNote, true);
                return sendLinks(db, user, username, tokens, note);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/shares/{token}/send",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& token) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                std::shared_ptr<Json::Value> holder;
                const Json::Value& json = requireJson(req, holder);
                const std::string username = requireString(json, "username");
                const auto note = cleanText(json.get("note", Json::nullValue), "note",
                                            kMaxNote, true);
                return sendLinks(db, user, username, {token}, note);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/inbox",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                // One row per send. Every row of a send shares its sender, note and time,
                // so the bare columns are well defined.
                auto stmt = db.prepare(
                    "SELECT d.batch, COALESCE(d.note, ''), MAX(d.sent_at) AS sent, "
                    "       MIN(d.seen_at IS NOT NULL), u.id, u.username, "
                    "       COALESCE(u.display_name, ''), COALESCE(u.avatar, ''), u.role "
                    "FROM share_deliveries d JOIN users u ON u.id = d.sender_id "
                    "WHERE d.recipient_id = ? GROUP BY d.batch "
                    "ORDER BY sent DESC, MAX(d.id) DESC LIMIT ?");
                stmt.bind(1, user.id).bind(2, kInboxPage);

                Json::Value items{Json::arrayValue};
                while (stmt.step()) {
                    Json::Value item;
                    const std::string batch = stmt.columnText(0);
                    item["id"] = batch;
                    if (const std::string note = stmt.columnText(1); !note.empty()) {
                        item["note"] = note;
                    }
                    item["sent_at"] = static_cast<Json::Int64>(stmt.columnInt(2));
                    item["seen"] = stmt.columnInt(3) != 0;

                    Json::Value sender;
                    sender["username"] = stmt.columnText(5);
                    sender["role"] = stmt.columnText(8);
                    if (const std::string shown = stmt.columnText(6); !shown.empty()) {
                        sender["display_name"] = shown;
                    }
                    if (const std::string pic = stmt.columnText(7); !pic.empty()) {
                        sender["avatar"] = avatarUrl(stmt.columnInt(4), pic);
                    }
                    item["sender"] = sender;

                    // Resolved now, like chat's cards: a link sent before it expired reads
                    // as expired rather than as a click that fails.
                    Json::Value cards{Json::arrayValue};
                    auto shares = db.prepare(
                        "SELECT share_id FROM share_deliveries "
                        "WHERE recipient_id = ? AND batch = ? ORDER BY position, id");
                    shares.bind(1, user.id).bind(2, batch);
                    while (shares.step()) {
                        cards.append(shareCard(db, shares.columnInt(0)));
                    }
                    item["cards"] = cards;
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
                    "SELECT COUNT(DISTINCT batch) FROM share_deliveries "
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
                    "DELETE FROM share_deliveries WHERE batch = ? AND recipient_id = ?");
                stmt.bind(1, id).bind(2, user.id).run();
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
