#include "notifications.h"

#include <drogon/drogon.h>

#include <string>

#include "httputil.h"

namespace archive {
namespace {

// Each kind is capped: the client wants what is new since its last poll, seconds ago,
// and a backlog of hundreds would only be marked as known on its first poll anyway.
constexpr std::int64_t kPerKind = 20;
// Enough of a message to recognise it in a notification; the rest is in the room.
constexpr std::size_t kExcerptChars = 140;

// The first kExcerptChars characters (not bytes, so a Georgian message is not cut to a
// third of an English one), on one line.
std::string excerpt(const std::string& body) {
    std::string out;
    std::size_t chars = 0;
    for (const char ch : body) {
        const auto c = static_cast<unsigned char>(ch);
        const bool lead = (c & 0xC0) != 0x80;
        if (lead && chars == kExcerptChars) {
            return out + "…";
        }
        if (lead) {
            ++chars;
        }
        out += (ch == '\n' || ch == '\r') ? ' ' : ch;
    }
    return out;
}

Json::Value person(const std::string& username, const std::string& displayName) {
    Json::Value p;
    p["username"] = username;
    if (!displayName.empty()) {
        p["display_name"] = displayName;
    }
    return p;
}

// Shared by each count and its list, so the two cannot disagree about what is waiting.
// A mention counts as the room badge does: unread, not removed, in a room you are in and
// have not blocked — and not one you wrote yourself.
constexpr const char* kMentionsFrom =
    " FROM message_mentions mn"
    " JOIN messages msg ON msg.id = mn.message_id"
    " JOIN rooms r ON r.id = msg.room_id"
    " JOIN room_members m ON m.room_id = msg.room_id AND m.user_id = ?1"
    "                    AND m.state = 'member'"
    " LEFT JOIN users u ON u.id = msg.user_id"
    " WHERE mn.user_id = ?1 AND msg.deleted_at IS NULL AND msg.id > m.last_read_id"
    "   AND (msg.user_id IS NULL OR msg.user_id != ?1)"
    "   AND NOT EXISTS (SELECT 1 FROM room_blocks b"
    "                   WHERE b.user_id = ?1 AND b.room_id = msg.room_id)";

constexpr const char* kInvitationsFrom =
    " FROM room_members m"
    " JOIN rooms r ON r.id = m.room_id"
    " LEFT JOIN users u ON u.id = m.invited_by"
    " WHERE m.user_id = ?1 AND m.state = 'invited'"
    "   AND NOT EXISTS (SELECT 1 FROM room_blocks b"
    "                   WHERE b.user_id = ?1 AND b.room_id = m.room_id)";

// One row per send, as "Shared with you" shows them.
constexpr const char* kLinksFrom =
    " FROM share_deliveries d"
    " JOIN users u ON u.id = d.sender_id"
    " WHERE d.recipient_id = ?1 AND d.seen_at IS NULL";

std::int64_t count(Database& db, const std::string& sql, std::int64_t userId) {
    auto stmt = db.prepare(sql);
    stmt.bind(1, userId);
    stmt.step();
    return stmt.columnInt(0);
}

}  // namespace

void registerNotificationRoutes(Database& db, Auth& auth) {
    drogon::app().registerHandler(
        "/api/notifications",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                Json::Value items{Json::arrayValue};

                auto mentions = db.prepare(
                    std::string{"SELECT msg.id, msg.room_id, r.name, msg.author_name,"
                                "       COALESCE(u.display_name, ''), msg.body, msg.created_at"} +
                    kMentionsFrom + " ORDER BY msg.id DESC LIMIT ?2");
                mentions.bind(1, user.id).bind(2, kPerKind);
                while (mentions.step()) {
                    Json::Value item;
                    item["key"] = "m:" + std::to_string(mentions.columnInt(0));
                    item["kind"] = "mention";
                    item["at"] = static_cast<Json::Int64>(mentions.columnInt(6));
                    item["from"] = person(mentions.columnText(3), mentions.columnText(4));
                    item["room"]["id"] = static_cast<Json::Int64>(mentions.columnInt(1));
                    item["room"]["name"] = mentions.columnText(2);
                    item["excerpt"] = excerpt(mentions.columnText(5));
                    items.append(item);
                }

                // Titles of the links in each send, in the order they were picked.
                auto links = db.prepare(
                    std::string{"SELECT d.batch, u.username, COALESCE(u.display_name, ''),"
                                "       COALESCE(MAX(d.note), ''), MAX(d.sent_at),"
                                "       COUNT(*)"} +
                    kLinksFrom + " GROUP BY d.batch ORDER BY MAX(d.sent_at) DESC LIMIT ?2");
                links.bind(1, user.id).bind(2, kPerKind);
                while (links.step()) {
                    const std::string batch = links.columnText(0);
                    Json::Value item;
                    item["key"] = "l:" + batch;
                    item["kind"] = "link";
                    item["at"] = static_cast<Json::Int64>(links.columnInt(4));
                    item["from"] = person(links.columnText(1), links.columnText(2));
                    if (const std::string note = links.columnText(3); !note.empty()) {
                        item["note"] = excerpt(note);
                    }
                    Json::Value titles{Json::arrayValue};
                    auto shares = db.prepare(
                        "SELECT COALESCE(s.title, '') FROM share_deliveries d"
                        " JOIN shares s ON s.id = d.share_id"
                        " WHERE d.recipient_id = ? AND d.batch = ? ORDER BY d.position, d.id");
                    shares.bind(1, user.id).bind(2, batch);
                    while (shares.step()) {
                        titles.append(shares.columnText(0));
                    }
                    item["titles"] = titles;
                    items.append(item);
                }

                auto invitations = db.prepare(
                    std::string{"SELECT m.room_id, r.name, COALESCE(u.username, ''),"
                                "       COALESCE(u.display_name, ''), m.invited_at"} +
                    kInvitationsFrom + " ORDER BY m.invited_at DESC LIMIT ?2");
                invitations.bind(1, user.id).bind(2, kPerKind);
                while (invitations.step()) {
                    Json::Value item;
                    item["key"] = "i:" + std::to_string(invitations.columnInt(0));
                    item["kind"] = "invitation";
                    item["at"] = static_cast<Json::Int64>(invitations.columnInt(4));
                    // The inviter's account may be gone; the invitation still stands.
                    if (const std::string by = invitations.columnText(2); !by.empty()) {
                        item["from"] = person(by, invitations.columnText(3));
                    }
                    item["room"]["id"] = static_cast<Json::Int64>(invitations.columnInt(0));
                    item["room"]["name"] = invitations.columnText(1);
                    items.append(item);
                }

                Json::Value out;
                out["counts"]["mentions"] = static_cast<Json::Int64>(
                    count(db, std::string{"SELECT COUNT(*)"} + kMentionsFrom, user.id));
                out["counts"]["links"] = static_cast<Json::Int64>(
                    count(db, std::string{"SELECT COUNT(DISTINCT d.batch)"} + kLinksFrom, user.id));
                out["counts"]["invitations"] = static_cast<Json::Int64>(
                    count(db, std::string{"SELECT COUNT(*)"} + kInvitationsFrom, user.id));
                out["items"] = items;
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});
}

}  // namespace archive
