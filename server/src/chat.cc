#include "chat.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <vector>

#include "activity.h"
#include "httputil.h"
#include "shares.h"
#include "text.h"

namespace archive {
namespace {

constexpr std::size_t kMaxBodyChars = 4000;
// A page of history when scrolling back, and the most edits and removals one poll returns.
constexpr std::int64_t kOlderPage = 100;
constexpr std::size_t kMaxTopicChars = 200;
constexpr std::int64_t kMaxChangedPerFetch = 200;
constexpr std::size_t kMaxNameChars = 60;
constexpr int kMaxMessagesPerFetch = 200;
constexpr Json::ArrayIndex kMaxSharesPerMessage = 10;

std::int64_t parseId(const std::string& raw, const char* what) {
    try {
        return std::stoll(raw);
    } catch (const std::exception&) {
        throw HttpError{404, std::string{"no such "} + what};
    }
}

std::string trimmed(const std::string& raw) {
    const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
    std::string out = raw;
    out.erase(out.begin(), std::find_if(out.begin(), out.end(), notSpace));
    out.erase(std::find_if(out.rbegin(), out.rend(), notSpace).base(), out.end());
    return out;
}

// A room exists and the caller is a full member of it. Every read or write beyond the
// room list goes through here, rather than each handler re-deriving the rule — the one
// that gets forgotten is the one that leaks a private conversation.
std::int64_t requireRoomMember(Database& db, std::int64_t roomId, const User& user) {
    auto stmt = db.prepare(
        "SELECT 1 FROM room_members m JOIN rooms r ON r.id = m.room_id "
        "WHERE m.room_id = ? AND m.user_id = ? AND m.state = 'member'");
    stmt.bind(1, roomId).bind(2, user.id);
    if (!stmt.step()) {
        // 404, not 403: a non-member already knows the room exists from the list, but
        // confirming membership boundaries per request adds nothing and 403 invites
        // probing for which rooms a given account belongs to.
        throw HttpError{404, "no such room"};
    }
    return roomId;
}

// Who may run a room — invite, rename, set its topic, remove someone, delete it: its
// creator while still in it, and administrators. A creator who has left gives that up
// with the rest; it would be odd to keep inviting people to a room you are not in.
void requireRoomManager(Database& db, std::int64_t roomId, const User& user) {
    auto exists = db.prepare("SELECT 1 FROM rooms WHERE id = ?");
    exists.bind(1, roomId);
    if (!exists.step()) {
        throw HttpError{404, "no such room"};
    }
    if (user.isAdmin()) {
        return;
    }
    auto stmt = db.prepare(
        "SELECT 1 FROM rooms r JOIN room_members m ON m.room_id = r.id AND m.user_id = ?2 "
        "WHERE r.id = ?1 AND r.created_by = ?2 AND m.state = 'member'");
    stmt.bind(1, roomId).bind(2, user.id);
    if (!stmt.step()) {
        throw HttpError{403, "only the room's creator or an administrator can do that"};
    }
}

// The characters a username can contain, which is what bounds an @token. Usernames are
// already normalised to lowercase, so a mention is matched lowercased and `@Bob` finds
// bob — writing someone's name with a capital letter should not fail to reach them.
bool isNameChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
}

// Every distinct @name in a message, lowercased, in the order written. Resolution
// against the room's membership happens at the call site: this only tokenises.
std::vector<std::string> mentionedNames(const std::string& body) {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < body.size(); ++i) {
        if (body[i] != '@') {
            continue;
        }
        // Must start a word. Without this, an email address in a message would mention
        // whoever happens to share a name with the mail host.
        if (i > 0 && isNameChar(static_cast<unsigned char>(body[i - 1]))) {
            continue;
        }
        std::size_t end = i + 1;
        while (end < body.size() && isNameChar(static_cast<unsigned char>(body[end]))) {
            ++end;
        }
        if (end == i + 1) {
            continue;
        }
        std::string name = body.substr(i + 1, end - i - 1);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        if (std::find(names.begin(), names.end(), name) == names.end()) {
            names.push_back(name);
        }
        i = end - 1;
    }
    return names;
}

// Records the mentions a message makes, resolved against who is actually in the room.
// An @name that belongs to nobody here records nothing: it is text, and treating it as a
// mention would let a message claim to have notified someone it never could.
void recordMentions(Database& db, std::int64_t messageId, std::int64_t roomId,
                    const std::string& body) {
    for (const std::string& name : mentionedNames(body)) {
        auto find = db.prepare(
            "SELECT u.id FROM users u "
            "JOIN room_members m ON m.user_id = u.id AND m.room_id = ? "
            "WHERE u.username = ? AND m.state = 'member'");
        find.bind(1, roomId).bind(2, name);
        if (!find.step()) {
            continue;
        }
        auto insert = db.prepare(
            "INSERT INTO message_mentions (message_id, user_id, mentioned_name) "
            "VALUES (?, ?, ?) ON CONFLICT DO NOTHING");
        insert.bind(1, messageId).bind(2, find.columnInt(0)).bind(3, name).run();
    }
}

}  // namespace

void registerChatRoutes(Database& db, Auth& auth) {
    auto& app = drogon::app();

    // ---- rooms -------------------------------------------------------------------
    app.registerHandler(
        "/api/chat/rooms",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const User user = requireUser(req, auth);

                if (req->method() == drogon::Post) {
                    std::shared_ptr<Json::Value> holder;
                    const std::string name =
                        trimmed(requireString(requireJson(req, holder), "name"));
                    if (name.empty() || name.size() > kMaxNameChars) {
                        throw HttpError{400, "a room name is 1-60 characters"};
                    }

                    Transaction tx{db};
                    auto insert = db.prepare(
                        "INSERT INTO rooms (name, created_by, creator_name, created_at) "
                        "VALUES (?, ?, ?, ?)");
                    insert.bind(1, name)
                        .bind(2, user.id)
                        .bind(3, user.username)
                        .bind(4, nowSeconds())
                        .run();
                    const std::int64_t roomId = db.lastInsertId();

                    // The creator joins outright; there is nobody to invite them.
                    auto join = db.prepare(
                        "INSERT INTO room_members (room_id, user_id, state, invited_by, "
                        "invited_at, responded_at) VALUES (?, ?, 'member', ?, ?, ?)");
                    const std::int64_t now = nowSeconds();
                    join.bind(1, roomId).bind(2, user.id).bind(3, user.id)
                        .bind(4, now).bind(5, now).run();
                    tx.commit();

                    Json::Value out;
                    out["id"] = static_cast<Json::Int64>(roomId);
                    out["name"] = name;
                    auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                    resp->setStatusCode(drogon::k201Created);
                    return resp;
                }

                // Every room, with my relationship to it. Rooms this user has blocked
                // are omitted entirely — that is what blocking a room means.
                Json::Value out{Json::arrayValue};
                auto stmt = db.prepare(
                    "SELECT r.id, r.name, r.creator_name, r.created_at, "
                    "       COALESCE(m.state, 'none'), "
                    "       r.created_by = ?1, COALESCE(inv.username, ''), "
                    "       (SELECT COUNT(*) FROM messages msg "
                    "        WHERE msg.room_id = r.id AND msg.deleted_at IS NULL "
                    "          AND msg.id > COALESCE(m.last_read_id, 0)), "
                    "       (SELECT COUNT(*) FROM room_members mm "
                    "        WHERE mm.room_id = r.id AND mm.state = 'member'), "
                    // Unread messages that named me. A removed message is excluded: a
                    // badge pointing at a tombstone is a summons to nothing.
                    "       (SELECT COUNT(*) FROM message_mentions mn "
                    "        JOIN messages msg ON msg.id = mn.message_id "
                    "        WHERE mn.user_id = ?1 AND msg.room_id = r.id "
                    "          AND msg.deleted_at IS NULL "
                    "          AND msg.id > COALESCE(m.last_read_id, 0)), "
                    "       COALESCE(r.topic, '') "
                    "FROM rooms r "
                    "LEFT JOIN room_members m ON m.room_id = r.id AND m.user_id = ?1 "
                    "LEFT JOIN users inv ON inv.id = m.invited_by "
                    "WHERE NOT EXISTS (SELECT 1 FROM room_blocks b "
                    "                  WHERE b.user_id = ?1 AND b.room_id = r.id) "
                    "ORDER BY r.created_at");
                stmt.bind(1, user.id);
                while (stmt.step()) {
                    const std::string state = stmt.columnText(4);
                    Json::Value room;
                    room["id"] = static_cast<Json::Int64>(stmt.columnInt(0));
                    room["name"] = stmt.columnText(1);
                    room["created_by"] = stmt.columnText(2);
                    room["created_at"] = static_cast<Json::Int64>(stmt.columnInt(3));
                    room["state"] = state;
                    room["is_creator"] = stmt.columnInt(5) != 0;
                    room["member_count"] = static_cast<Json::Int64>(stmt.columnInt(8));
                    if (const std::string topic = stmt.columnText(10); !topic.empty()) {
                        room["topic"] = topic;
                    }
                    // What room settings to offer; the handlers check again.
                    room["can_manage"] =
                        user.isAdmin() || (stmt.columnInt(5) != 0 && state == "member");
                    if (state == "invited") {
                        room["invited_by"] = stmt.columnText(6);
                    }
                    // Only meaningful for members; a non-member has no unread count
                    // because they have no reads.
                    if (state == "member") {
                        room["unread"] = static_cast<Json::Int64>(stmt.columnInt(7));
                        room["mentions_unread"] = static_cast<Json::Int64>(stmt.columnInt(9));
                    }
                    out.append(room);
                }
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get, drogon::Post});

    // ---- members ---------------------------------------------------------------------
    app.registerHandler(
        "/api/chat/rooms/{id}/members",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = requireRoomMember(db, parseId(id, "room"), user);

                // Members and outstanding invitations. Declines are deliberately absent:
                // whether someone turned an invitation down is their business, not a
                // status the room gets to display about them. The creator learns it the
                // only way that matters anyway — the pending entry stops being listed.
                Json::Value members{Json::arrayValue};
                Json::Value invited{Json::arrayValue};
                auto stmt = db.prepare(
                    "SELECT u.username, m.state, m.responded_at, m.invited_at, "
                    "       r.created_by = u.id, u.role, COALESCE(u.display_name, ''), "
                    "       COALESCE(u.avatar, ''), u.id "
                    "FROM room_members m JOIN users u ON u.id = m.user_id "
                    "JOIN rooms r ON r.id = m.room_id "
                    "WHERE m.room_id = ? AND m.state IN ('member', 'invited') "
                    "ORDER BY m.state = 'invited', COALESCE(m.responded_at, m.invited_at)");
                stmt.bind(1, roomId);
                while (stmt.step()) {
                    Json::Value row;
                    row["username"] = stmt.columnText(0);
                    const bool isMember = stmt.columnText(1) == "member";
                    row["since"] = static_cast<Json::Int64>(
                        stmt.columnIsNull(2) ? stmt.columnInt(3) : stmt.columnInt(2));
                    row["is_creator"] = stmt.columnInt(4) != 0;
                    row["role"] = stmt.columnText(5);
                    if (const std::string shown = stmt.columnText(6); !shown.empty()) {
                        row["display_name"] = shown;
                    }
                    if (const std::string pic = stmt.columnText(7); !pic.empty()) {
                        row["avatar"] = avatarUrl(stmt.columnInt(8), pic);
                    }
                    (isMember ? members : invited).append(row);
                }

                Json::Value out;
                out["members"] = members;
                out["invited"] = invited;
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});

    // ---- invite ------------------------------------------------------------------
    app.registerHandler(
        "/api/chat/rooms/{id}/invite",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = parseId(id, "room");

                // "Invited by a group creator", plus admins — who moderate, and cannot
                // moderate a conversation they are unable to read. This is the door they
                // use: nothing here lets an admin read a room without joining it, and
                // joining is visible to everyone in the member list. requireRoomManager
                // also answers 404 for a room that does not exist, before anything else.
                requireRoomManager(db, roomId, user);

                std::shared_ptr<Json::Value> holder;
                const std::string username =
                    normaliseUsername(requireString(requireJson(req, holder), "username"));

                std::int64_t targetId = 0;
                {
                    auto find = db.prepare(
                        "SELECT id FROM users WHERE username = ? AND disabled_at IS NULL");
                    find.bind(1, username);
                    if (!find.step()) {
                        throw HttpError{404, "no such member"};
                    }
                    targetId = find.columnInt(0);
                }

                // A block is the invitee's decision and outranks the inviter's. Report
                // it as success rather than an error: telling the inviter they have been
                // blocked hands them information the blocker did not choose to share.
                auto blocked = db.prepare(
                    "SELECT 1 FROM user_blocks WHERE user_id = ? AND blocked_id = ? "
                    "UNION ALL SELECT 1 FROM room_blocks WHERE user_id = ? AND room_id = ?");
                blocked.bind(1, targetId).bind(2, user.id).bind(3, targetId).bind(4, roomId);
                const bool isBlocked = blocked.step();

                if (!isBlocked) {
                    // Re-inviting someone who declined puts them back to `invited`;
                    // someone already a member is untouched.
                    auto upsert = db.prepare(
                        "INSERT INTO room_members (room_id, user_id, state, invited_by, "
                        "invited_at) VALUES (?, ?, 'invited', ?, ?) "
                        "ON CONFLICT(room_id, user_id) DO UPDATE SET "
                        "  state = CASE WHEN room_members.state = 'member' "
                        "               THEN 'member' ELSE 'invited' END, "
                        "  invited_by = excluded.invited_by, "
                        "  invited_at = excluded.invited_at, responded_at = NULL");
                    upsert.bind(1, roomId).bind(2, targetId).bind(3, user.id)
                        .bind(4, nowSeconds()).run();
                }

                Json::Value out;
                out["invited"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    // ---- respond to an invitation --------------------------------------------------
    app.registerHandler(
        "/api/chat/rooms/{id}/respond",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = parseId(id, "room");

                std::shared_ptr<Json::Value> holder;
                const std::string action =
                    requireString(requireJson(req, holder), "action");
                // Validated before the invitation is looked up: a malformed request is a
                // 400 whatever the room's state, and reporting it as "no such invitation"
                // would send a caller hunting for the wrong bug.
                if (action != "accept" && action != "decline" && action != "block_room" &&
                    action != "block_user") {
                    throw HttpError{400,
                                    "action must be accept, decline, block_room or block_user"};
                }

                std::int64_t inviter = 0;
                {
                    auto find = db.prepare(
                        "SELECT COALESCE(invited_by, 0) FROM room_members "
                        "WHERE room_id = ? AND user_id = ? AND state = 'invited'");
                    find.bind(1, roomId).bind(2, user.id);
                    if (!find.step()) {
                        throw HttpError{404, "no pending invitation to that room"};
                    }
                    inviter = find.columnInt(0);
                }

                Transaction tx{db};
                if (action == "accept") {
                    auto stmt = db.prepare(
                        "UPDATE room_members SET state = 'member', responded_at = ? "
                        "WHERE room_id = ? AND user_id = ?");
                    stmt.bind(1, nowSeconds()).bind(2, roomId).bind(3, user.id).run();
                } else {
                    auto stmt = db.prepare(
                        "UPDATE room_members SET state = 'declined', responded_at = ? "
                        "WHERE room_id = ? AND user_id = ?");
                    stmt.bind(1, nowSeconds()).bind(2, roomId).bind(3, user.id).run();

                    if (action == "block_room") {
                        auto block = db.prepare(
                            "INSERT INTO room_blocks (user_id, room_id, created_at) "
                            "VALUES (?, ?, ?) ON CONFLICT DO NOTHING");
                        block.bind(1, user.id).bind(2, roomId).bind(3, nowSeconds()).run();
                    } else if (action == "block_user") {
                        if (inviter == 0) {
                            throw HttpError{409, "that invitation has no sender to block"};
                        }
                        auto block = db.prepare(
                            "INSERT INTO user_blocks (user_id, blocked_id, created_at) "
                            "VALUES (?, ?, ?) ON CONFLICT DO NOTHING");
                        block.bind(1, user.id).bind(2, inviter).bind(3, nowSeconds()).run();
                    }
                }
                tx.commit();

                Json::Value out;
                out["state"] = action == "accept" ? "member" : "declined";
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    // ---- messages ------------------------------------------------------------------
    // ---- leaving, and running a room ----------------------------------------------
    // Leaving is yours to do, at any time; being asked back takes a new invitation, as
    // after declining. What you said stays — the room's history is the room's.
    app.registerHandler(
        "/api/chat/rooms/{id}/leave",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = requireRoomMember(db, parseId(id, "room"), user);
                auto stmt = db.prepare(
                    "UPDATE room_members SET state = 'left', responded_at = ? "
                    "WHERE room_id = ? AND user_id = ?");
                stmt.bind(1, nowSeconds()).bind(2, roomId).bind(3, user.id).run();
                Json::Value out;
                out["state"] = "left";
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    // PATCH {name?, topic?} renames a room or sets its topic (an empty topic clears it);
    // DELETE removes the room and, with it, everything said in it.
    app.registerHandler(
        "/api/chat/rooms/{id}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = parseId(id, "room");
                requireRoomManager(db, roomId, user);

                auto named = db.prepare("SELECT name FROM rooms WHERE id = ?");
                named.bind(1, roomId);
                named.step();
                const std::string was = named.columnText(0);

                if (req->method() == drogon::Delete) {
                    // Messages, memberships, mentions and attachments cascade. The links
                    // that were attached are untouched: they belong to their owners.
                    auto del = db.prepare("DELETE FROM rooms WHERE id = ?");
                    del.bind(1, roomId).run();
                    activity::record(req, user, "chat.room_deleted", "“" + was + "”");
                    Json::Value out;
                    out["deleted"] = true;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                std::shared_ptr<Json::Value> holder;
                const Json::Value& json = requireJson(req, holder);
                if (!json.isMember("name") && !json.isMember("topic")) {
                    throw HttpError{400, "nothing to change: give name or topic"};
                }
                Json::Value out;
                if (json.isMember("name")) {
                    const std::string name = trimmed(requireString(json, "name"));
                    if (name.empty() || name.size() > kMaxNameChars) {
                        throw HttpError{400, "a room name is 1-60 characters"};
                    }
                    auto stmt = db.prepare("UPDATE rooms SET name = ? WHERE id = ?");
                    stmt.bind(1, name).bind(2, roomId).run();
                    out["name"] = name;
                }
                if (json.isMember("topic")) {
                    const auto topic = cleanText(json["topic"], "topic", kMaxTopicChars, false);
                    auto stmt = db.prepare("UPDATE rooms SET topic = ? WHERE id = ?");
                    topic ? stmt.bind(1, *topic) : stmt.bindNull(1);
                    stmt.bind(2, roomId).run();
                    out["topic"] = topic ? Json::Value{*topic} : Json::Value{};
                }
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Patch, drogon::Delete});

    // Takes someone out of a room, or withdraws their invitation. Like leaving, it is
    // undone only by a new invitation. The creator cannot be removed: they are part of
    // the room's record, and an administrator can delete the room instead.
    app.registerHandler(
        "/api/chat/rooms/{id}/members/{username}/remove",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id, const std::string& username) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = parseId(id, "room");
                requireRoomManager(db, roomId, user);
                auto find = db.prepare(
                    "SELECT u.id, r.created_by = u.id FROM users u "
                    "JOIN room_members m ON m.user_id = u.id AND m.room_id = ? "
                    "JOIN rooms r ON r.id = m.room_id "
                    "WHERE u.username = ? AND m.state IN ('member', 'invited')");
                find.bind(1, roomId).bind(2, normaliseUsername(username));
                if (!find.step()) {
                    throw HttpError{404, "they are not in this room"};
                }
                if (find.columnInt(1) != 0) {
                    throw HttpError{409, "the room's creator cannot be removed from it"};
                }
                auto stmt = db.prepare(
                    "UPDATE room_members SET state = 'removed', responded_at = ? "
                    "WHERE room_id = ? AND user_id = ?");
                stmt.bind(1, nowSeconds()).bind(2, roomId).bind(3, find.columnInt(0)).run();
                Json::Value out;
                out["removed"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/chat/rooms/{id}/messages",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = requireRoomMember(db, parseId(id, "room"), user);

                if (req->method() == drogon::Post) {
                    std::shared_ptr<Json::Value> holder;
                    const Json::Value& json = requireJson(req, holder);
                    if (json.isMember("body") && !json["body"].isString()) {
                        throw HttpError{400, "body must be text"};
                    }
                    const std::string body = trimmed(json.get("body", "").asString());
                    if (body.size() > kMaxBodyChars) {
                        throw HttpError{400, "a message is at most 4000 characters"};
                    }

                    // Links to attach: the sender's own, still working, each once. All
                    // checked before anything is written, so a stale link is refused with
                    // a reason rather than leaving half a message behind.
                    std::vector<std::pair<std::int64_t, std::string>> attached;  // id, title
                    if (json.isMember("shares")) {
                        const Json::Value& tokens = json["shares"];
                        if (!tokens.isArray()) {
                            throw HttpError{400, "shares must be a list of links"};
                        }
                        if (tokens.size() > kMaxSharesPerMessage) {
                            throw HttpError{400, "a message carries at most " +
                                                     std::to_string(kMaxSharesPerMessage) +
                                                     " links"};
                        }
                        for (const Json::Value& token : tokens) {
                            if (!token.isString()) {
                                throw HttpError{400, "shares must be a list of links"};
                            }
                            const std::int64_t shareId =
                                requireOwnLiveShare(db, token.asString(), user.id);
                            if (std::any_of(attached.begin(), attached.end(),
                                            [&](const auto& a) { return a.first == shareId; })) {
                                continue;
                            }
                            auto title = db.prepare(
                                "SELECT COALESCE(title, '') FROM shares WHERE id = ?");
                            title.bind(1, shareId);
                            title.step();
                            attached.emplace_back(shareId, title.columnText(0));
                        }
                    }
                    if (body.empty() && attached.empty()) {
                        throw HttpError{400, "a message needs some text or a link"};
                    }

                    Transaction tx{db};
                    auto insert = db.prepare(
                        "INSERT INTO messages (room_id, user_id, author_name, body, "
                        "created_at) VALUES (?, ?, ?, ?, ?)");
                    insert.bind(1, roomId).bind(2, user.id).bind(3, user.username)
                        .bind(4, body).bind(5, nowSeconds()).run();
                    const std::int64_t messageId = db.lastInsertId();

                    // In the same transaction as the message: a message that is visible
                    // but not yet indexed for mentions is a notification that silently
                    // never fires.
                    recordMentions(db, messageId, roomId, body);

                    int position = 0;
                    for (const auto& [shareId, title] : attached) {
                        auto link = db.prepare(
                            "INSERT INTO message_shares (message_id, position, share_id, title) "
                            "VALUES (?, ?, ?, ?)");
                        link.bind(1, messageId).bind(2, std::int64_t{position++})
                            .bind(3, shareId).bind(4, title).run();
                    }

                    // Your own message is read by definition.
                    auto read = db.prepare(
                        "UPDATE room_members SET last_read_id = ? "
                        "WHERE room_id = ? AND user_id = ? AND last_read_id < ?");
                    read.bind(1, messageId).bind(2, roomId).bind(3, user.id)
                        .bind(4, messageId).run();
                    tx.commit();

                    Json::Value out;
                    out["id"] = static_cast<Json::Int64>(messageId);
                    auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                    resp->setStatusCode(drogon::k201Created);
                    return resp;
                }

                const auto idParam = [&](const char* name) -> std::int64_t {
                    const std::string raw = req->getParameter(name);
                    if (raw.empty()) {
                        return 0;
                    }
                    try {
                        return std::stoll(raw);
                    } catch (const std::exception&) {
                        throw HttpError{400, std::string{name} + " must be a number"};
                    }
                };
                const std::int64_t since = idParam("since");
                const std::int64_t before = idParam("before");
                const std::int64_t changed = idParam("changed");
                // Read before anything else, so it is no later than any change the response
                // can contain: a client that passes it back as `changed` misses nothing.
                const std::int64_t now = nowSeconds();

                // Three ways to read a room, sharing one row builder so they cannot drift:
                //   since=<id>    what came after id — the newest kMaxMessagesPerFetch when
                //                 that is more, so opening a room lands on the current
                //                 conversation rather than its first day;
                //   before=<id>   a page of older messages, for scrolling back through all
                //                 of it;
                //   changed=<t>   messages edited or removed since t (the `now` of an
                //                 earlier response), so an edit or a removal reaches
                //                 everyone without re-reading the room.
                const auto collect = [&](const std::string& where, std::int64_t value,
                                         std::int64_t limit) {
                    Json::Value messages{Json::arrayValue};
                    // Message id -> its index in `messages`, so the passes below can attach
                    // mentions and links without rescanning the array per row.
                    std::map<std::int64_t, Json::ArrayIndex> positions;
                    std::int64_t lowest = 0;
                    std::int64_t highest = 0;

                    // The author's role is read live rather than snapshotted alongside
                    // author_name. The name is history — it is what the message was signed
                    // with. The role is identity: promote someone and they should read as
                    // an admin everywhere, including in what they said last week.
                    auto stmt = db.prepare(
                        "SELECT * FROM ("
                        "  SELECT m.id, m.author_name, m.body, m.created_at, m.deleted_at, "
                        "         COALESCE(d.username, ''), m.user_id IS NULL, "
                        "         COALESCE(a.role, ''), "
                        // Whether this message named *me*, decided by account rather than
                        // by the client comparing names. Names can be changed and then
                        // reused, so an old "@bob" may not mean today's bob.
                        "         EXISTS(SELECT 1 FROM message_mentions mn "
                        "                WHERE mn.message_id = m.id AND mn.user_id = ?4), "
                        // Profile fields, live like the role: they belong to the person now.
                        "         COALESCE(a.display_name, ''), COALESCE(a.avatar, ''), "
                        "         COALESCE(m.user_id, 0), m.edited_at "
                        "  FROM messages m LEFT JOIN users d ON d.id = m.deleted_by "
                        "  LEFT JOIN users a ON a.id = m.user_id "
                        "  WHERE m.room_id = ?1 AND " + where + " ORDER BY m.id DESC LIMIT ?3"
                        ") ORDER BY 1");
                    stmt.bind(1, roomId).bind(2, value).bind(3, limit).bind(4, user.id);
                    while (stmt.step()) {
                        const std::int64_t id = stmt.columnInt(0);
                        Json::Value message;
                        message["id"] = static_cast<Json::Int64>(id);
                        message["author"] = stmt.columnText(1);
                        message["created_at"] = static_cast<Json::Int64>(stmt.columnInt(3));
                        // A removed message keeps its place as a tombstone. Closing the gap
                        // would quietly reflow a conversation around what was taken out.
                        if (!stmt.columnIsNull(4)) {
                            message["deleted"] = true;
                            message["deleted_by"] = stmt.columnText(5);
                        } else {
                            message["body"] = stmt.columnText(2);
                            if (!stmt.columnIsNull(12)) {
                                message["edited_at"] = static_cast<Json::Int64>(stmt.columnInt(12));
                            }
                        }
                        // The author's account is gone; the message is not. There is no
                        // role to report either — the badge falls back to plain.
                        if (stmt.columnInt(6) != 0) {
                            message["author_departed"] = true;
                        } else {
                            message["author_role"] = stmt.columnText(7);
                            // `author` stays the username: it is what attributes the
                            // message, and a display name is whatever its owner typed.
                            if (const std::string shown = stmt.columnText(9); !shown.empty()) {
                                message["author_display_name"] = shown;
                            }
                            if (const std::string pic = stmt.columnText(10); !pic.empty()) {
                                message["author_avatar"] = avatarUrl(stmt.columnInt(11), pic);
                            }
                        }
                        if (stmt.columnInt(8) != 0) {
                            message["mentions_me"] = true;
                        }
                        if (lowest == 0) {
                            lowest = id;
                        }
                        highest = id;
                        positions[id] = messages.size();
                        messages.append(message);
                    }

                    // Mentions in one query over the page's id range, rather than one query
                    // per message. The client is told who the server resolved rather than
                    // re-deriving it from the text, so what is highlighted is exactly what
                    // a notifier would act on.
                    if (!positions.empty()) {
                        auto names = db.prepare(
                            "SELECT mm.message_id, mm.mentioned_name "
                            "FROM message_mentions mm JOIN messages m ON m.id = mm.message_id "
                            "WHERE m.room_id = ? AND mm.message_id BETWEEN ? AND ? "
                            "ORDER BY mm.message_id");
                        names.bind(1, roomId).bind(2, lowest).bind(3, highest);
                        while (names.step()) {
                            const auto found = positions.find(names.columnInt(0));
                            if (found != positions.end()) {
                                messages[found->second]["mentions"].append(names.columnText(1));
                            }
                        }
                    }

                    // Links carried by these messages, as cards resolved now: whether a
                    // link still works belongs to the link, not to the moment it was posted.
                    if (!positions.empty()) {
                        auto links = db.prepare(
                            "SELECT ms.message_id, ms.share_id, ms.title "
                            "FROM message_shares ms JOIN messages m ON m.id = ms.message_id "
                            "WHERE m.room_id = ? AND ms.message_id BETWEEN ? AND ? "
                            "ORDER BY ms.message_id, ms.position");
                        links.bind(1, roomId).bind(2, lowest).bind(3, highest);
                        while (links.step()) {
                            const auto found = positions.find(links.columnInt(0));
                            if (found == positions.end()) {
                                continue;
                            }
                            Json::Value card;
                            if (links.columnIsNull(1)) {
                                // The link's owner deleted their account and the link with
                                // it; the title is what is left to say what was here.
                                card["state"] = "gone";
                            } else {
                                card = shareCard(db, links.columnInt(1));
                            }
                            if (card["state"].asString() == "gone") {
                                card["title"] = links.columnText(2);
                            }
                            messages[found->second]["shares"].append(card);
                        }
                    }
                    return messages;
                };

                Json::Value out;
                if (before > 0) {
                    out["messages"] = collect("m.id < ?2", before, kOlderPage);
                } else {
                    out["messages"] = collect("m.id > ?2", since,
                                              static_cast<std::int64_t>(kMaxMessagesPerFetch));
                }
                // Whether there is anything older than this page, for the "load earlier"
                // affordance — asked only of a page that starts somewhere.
                if (since == 0 || before > 0) {
                    const auto& page = out["messages"];
                    std::int64_t oldest = page.empty() ? before : page[0]["id"].asInt64();
                    auto more = db.prepare("SELECT EXISTS(SELECT 1 FROM messages "
                                           "WHERE room_id = ? AND id < ?)");
                    more.bind(1, roomId).bind(2, oldest > 0 ? oldest : 0);
                    more.step();
                    out["has_older"] = more.columnInt(0) != 0;
                }
                if (changed > 0) {
                    out["changed"] = collect("m.updated_at >= ?2", changed, kMaxChangedPerFetch);
                }
                // Where this reader had read up to, on opening the room: the "New messages"
                // line goes after it. Read before this request marks anything read.
                if (since == 0 && before == 0) {
                    auto read = db.prepare(
                        "SELECT last_read_id FROM room_members WHERE room_id = ? AND user_id = ?");
                    read.bind(1, roomId).bind(2, user.id);
                    if (read.step()) {
                        out["last_read_id"] = static_cast<Json::Int64>(read.columnInt(0));
                    }
                }
                out["now"] = static_cast<Json::Int64>(now);
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get, drogon::Post});

    app.registerHandler(
        "/api/chat/rooms/{id}/read",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t roomId = requireRoomMember(db, parseId(id, "room"), user);

                std::shared_ptr<Json::Value> holder;
                const Json::Value& json = requireJson(req, holder);
                if (!json.isMember("last_id") || !json["last_id"].isIntegral()) {
                    throw HttpError{400, "last_id must be a message id"};
                }
                // Never moves backwards: two tabs open at different scroll positions
                // would otherwise keep resurrecting an unread count.
                auto stmt = db.prepare(
                    "UPDATE room_members SET last_read_id = ? "
                    "WHERE room_id = ? AND user_id = ? AND last_read_id < ?");
                const std::int64_t lastId = json["last_id"].asInt64();
                stmt.bind(1, lastId).bind(2, roomId).bind(3, user.id).bind(4, lastId).run();

                Json::Value out;
                out["ok"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/api/chat/messages/{id}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& id) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                const std::int64_t messageId = parseId(id, "message");

                std::int64_t roomId = 0;
                std::int64_t authorId = 0;
                {
                    auto find = db.prepare(
                        "SELECT room_id, COALESCE(user_id, 0) FROM messages "
                        "WHERE id = ? AND deleted_at IS NULL");
                    find.bind(1, messageId);
                    if (!find.step()) {
                        throw HttpError{404, "no such message"};
                    }
                    roomId = find.columnInt(0);
                    authorId = find.columnInt(1);
                }
                const bool mine = authorId == user.id;
                // Your own messages, while you are still in the room; anyone's, for an
                // administrator removing one. Edits are the author's alone: an admin
                // rewriting someone's words would put them in that person's mouth.
                if (req->method() == drogon::Patch || !user.isAdmin()) {
                    if (!mine) {
                        throw HttpError{403, "you can only change your own messages"};
                    }
                    requireRoomMember(db, roomId, user);
                }

                if (req->method() == drogon::Patch) {
                    std::shared_ptr<Json::Value> holder;
                    const Json::Value& json = requireJson(req, holder);
                    const std::string body = trimmed(requireString(json, "body"));
                    if (body.size() > kMaxBodyChars) {
                        throw HttpError{400, "a message is at most 4000 characters"};
                    }
                    if (body.empty()) {
                        // Text can only be emptied from a message that still carries a
                        // link; otherwise there would be nothing left, and that is what
                        // removing it is for.
                        auto links = db.prepare(
                            "SELECT COUNT(*) FROM message_shares WHERE message_id = ?");
                        links.bind(1, messageId);
                        links.step();
                        if (links.columnInt(0) == 0) {
                            throw HttpError{400, "a message needs some text or a link — "
                                                 "remove it instead"};
                        }
                    }
                    const std::int64_t now = nowSeconds();
                    Transaction tx{db};
                    auto stmt = db.prepare(
                        "UPDATE messages SET body = ?, edited_at = ?, updated_at = ? "
                        "WHERE id = ? AND deleted_at IS NULL");
                    stmt.bind(1, body).bind(2, now).bind(3, now).bind(4, messageId).run();
                    // Mentions follow the new text: one taken out is no longer a
                    // mention, one added becomes one.
                    auto clear = db.prepare("DELETE FROM message_mentions WHERE message_id = ?");
                    clear.bind(1, messageId).run();
                    recordMentions(db, messageId, roomId, body);
                    tx.commit();
                    Json::Value out;
                    out["edited_at"] = static_cast<Json::Int64>(now);
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                // Removal erases. The row stays, as the tombstone that keeps the
                // conversation's shape, but its text is overwritten and its mentions go:
                // the confirmation promises the text is gone for good, and the privacy
                // notice repeats that, so leaving it in the database — hidden only by the
                // API declining to serve it — would make both untrue.
                const std::int64_t now = nowSeconds();
                Transaction tx{db};
                auto stmt = db.prepare(
                    "UPDATE messages SET deleted_at = ?, deleted_by = ?, body = '', "
                    "updated_at = ? WHERE id = ? AND deleted_at IS NULL");
                stmt.bind(1, now).bind(2, user.id).bind(3, now).bind(4, messageId).run();
                if (db.changes() == 0) {
                    throw HttpError{404, "no such message"};
                }
                auto mentions = db.prepare("DELETE FROM message_mentions WHERE message_id = ?");
                mentions.bind(1, messageId).run();
                // Links it carried are part of what was said, so they go too.
                auto links = db.prepare("DELETE FROM message_shares WHERE message_id = ?");
                links.bind(1, messageId).run();
                tx.commit();
                if (!mine) {
                    // Which room and whose message — never what it said, which is now gone.
                    auto where = db.prepare(
                        "SELECT r.name, m.author_name FROM messages m "
                        "JOIN rooms r ON r.id = m.room_id WHERE m.id = ?");
                    where.bind(1, messageId);
                    if (where.step()) {
                        activity::record(req, user, "admin.message_removed",
                                         "in “" + where.columnText(0) + "”, by " +
                                             where.columnText(1));
                    }
                }
                Json::Value out;
                out["deleted"] = true;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Delete, drogon::Patch});

    // ---- blocks ---------------------------------------------------------------------
    app.registerHandler(
        "/api/chat/blocks",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                const User user = requireUser(req, auth);
                Json::Value rooms{Json::arrayValue};
                Json::Value people{Json::arrayValue};

                auto roomStmt = db.prepare(
                    "SELECT r.id, r.name FROM room_blocks b JOIN rooms r ON r.id = b.room_id "
                    "WHERE b.user_id = ? ORDER BY b.created_at DESC");
                roomStmt.bind(1, user.id);
                while (roomStmt.step()) {
                    Json::Value row;
                    row["id"] = static_cast<Json::Int64>(roomStmt.columnInt(0));
                    row["name"] = roomStmt.columnText(1);
                    rooms.append(row);
                }

                auto userStmt = db.prepare(
                    "SELECT u.id, u.username FROM user_blocks b "
                    "JOIN users u ON u.id = b.blocked_id "
                    "WHERE b.user_id = ? ORDER BY b.created_at DESC");
                userStmt.bind(1, user.id);
                while (userStmt.step()) {
                    Json::Value row;
                    row["id"] = static_cast<Json::Int64>(userStmt.columnInt(0));
                    row["username"] = userStmt.columnText(1);
                    people.append(row);
                }

                Json::Value out;
                out["rooms"] = rooms;
                out["users"] = people;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Get});

    for (const bool byRoom : {true, false}) {
        app.registerHandler(
            byRoom ? "/api/chat/blocks/room/{id}" : "/api/chat/blocks/user/{id}",
            [&db, &auth, byRoom](
                const drogon::HttpRequestPtr& req,
                std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                const std::string& id) {
                callback(guarded([&] {
                    const User user = requireUser(req, auth);
                    auto stmt = db.prepare(
                        byRoom ? "DELETE FROM room_blocks WHERE user_id = ? AND room_id = ?"
                               : "DELETE FROM user_blocks WHERE user_id = ? AND blocked_id = ?");
                    stmt.bind(1, user.id).bind(2, parseId(id, "block")).run();
                    if (db.changes() == 0) {
                        throw HttpError{404, "no such block"};
                    }
                    Json::Value out;
                    out["unblocked"] = true;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }));
            },
            {drogon::Delete});
    }
}

}  // namespace archive
