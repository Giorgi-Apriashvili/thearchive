#include "activity.h"

#include <string>

#include "httputil.h"
#include "throttle.h"

namespace archive::activity {
namespace {

Database* g_db = nullptr;

constexpr std::size_t kMaxUserAgent = 200;
constexpr std::int64_t kPage = 100;

void insert(const drogon::HttpRequestPtr& req, std::optional<std::int64_t> userId,
            const std::string& actor, const std::string& event, const std::string& detail) {
    if (g_db == nullptr) {
        return;
    }
    try {
        const auto network = clientNetwork(req);
        std::string agent = req->getHeader("User-Agent").substr(0, kMaxUserAgent);
        auto stmt = g_db->prepare(
            "INSERT INTO activity (at, user_id, actor, network, user_agent, event, detail) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)");
        stmt.bind(1, nowSeconds());
        userId ? stmt.bind(2, *userId) : stmt.bindNull(2);
        stmt.bind(3, actor);
        network ? stmt.bind(4, *network) : stmt.bindNull(4);
        agent.empty() ? stmt.bindNull(5) : stmt.bind(5, agent);
        stmt.bind(6, event).bind(7, detail).run();
    } catch (const std::exception& e) {
        // The request it describes has already succeeded or failed on its own merits;
        // losing the record of it must not change that.
        LOG_ERROR << "activity: could not record " << event << ": " << e.what();
    }
}

}  // namespace

void init(Database& db) {
    g_db = &db;
}

void record(const drogon::HttpRequestPtr& req, const std::optional<User>& actor,
            const std::string& event, const std::string& detail) {
    insert(req, actor ? std::optional{actor->id} : std::nullopt,
           actor ? actor->username : std::string{}, event, detail);
}

void recordFailedLogin(const drogon::HttpRequestPtr& req, const std::string& username,
                       bool refused) {
    const std::string event = refused ? "auth.login_refused" : "auth.login_failed";
    if (g_db == nullptr) {
        return;
    }
    std::optional<std::int64_t> userId;
    try {
        auto find = g_db->prepare("SELECT id FROM users WHERE username = ?");
        find.bind(1, username);
        if (find.step()) {
            userId = find.columnInt(0);
        }
    } catch (const std::exception&) {
    }
    // An unknown name is not kept: it may well be a password typed in the wrong box.
    insert(req, userId, userId ? username : std::string{}, event,
           userId ? std::string{} : std::string{"no such account"});
}

int prune(Database& db, std::int64_t now) {
    auto stmt = db.prepare("DELETE FROM activity WHERE at < ?");
    stmt.bind(1, now - std::int64_t{kDays} * 86400).run();
    return db.changes();
}

void registerActivityRoutes(Database& db, Auth& auth) {
    drogon::app().registerHandler(
        "/api/admin/activity",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&] {
                requireAdmin(req, auth);

                std::string sql =
                    "SELECT a.id, a.at, a.event, a.detail, COALESCE(a.network, ''), "
                    "       COALESCE(a.user_agent, ''), a.user_id, "
                    "       COALESCE(u.username, a.actor), COALESCE(u.display_name, ''), "
                    "       COALESCE(u.role, '') "
                    "FROM activity a LEFT JOIN users u ON u.id = a.user_id WHERE 1 = 1";
                const std::string before = req->getParameter("before");
                const std::string user = req->getParameter("user");
                const std::string event = req->getParameter("event");
                const std::string network = req->getParameter("network");
                if (!before.empty()) sql += " AND a.id < ?1";
                if (!user.empty()) sql += " AND (u.username = ?2 OR (u.id IS NULL AND a.actor = ?2))";
                if (!event.empty()) sql += " AND (a.event = ?3 OR a.event LIKE ?3 || '.%')";
                if (!network.empty()) sql += " AND a.network LIKE ?4 || '%'";
                sql += " ORDER BY a.id DESC LIMIT ?5";

                auto stmt = db.prepare(sql);
                if (!before.empty()) {
                    try {
                        stmt.bind(1, std::stoll(before));
                    } catch (const std::exception&) {
                        throw HttpError{400, "before must be a number"};
                    }
                }
                if (!user.empty()) stmt.bind(2, normaliseUsername(user));
                if (!event.empty()) stmt.bind(3, event);
                if (!network.empty()) stmt.bind(4, network);
                stmt.bind(5, kPage);

                Json::Value items{Json::arrayValue};
                std::int64_t last = 0;
                while (stmt.step()) {
                    Json::Value item;
                    last = stmt.columnInt(0);
                    item["id"] = static_cast<Json::Int64>(last);
                    item["at"] = static_cast<Json::Int64>(stmt.columnInt(1));
                    item["event"] = stmt.columnText(2);
                    if (const std::string d = stmt.columnText(3); !d.empty()) item["detail"] = d;
                    if (const std::string n = stmt.columnText(4); !n.empty()) item["network"] = n;
                    if (const std::string ua = stmt.columnText(5); !ua.empty()) item["user_agent"] = ua;
                    if (const std::string name = stmt.columnText(7); !name.empty()) {
                        Json::Value actor;
                        actor["username"] = name;
                        if (const std::string shown = stmt.columnText(8); !shown.empty()) {
                            actor["display_name"] = shown;
                        }
                        // No role once the account is gone: it no longer has one.
                        if (const std::string role = stmt.columnText(9); !role.empty()) {
                            actor["role"] = role;
                        }
                        actor["exists"] = !stmt.columnIsNull(6);
                        item["actor"] = actor;
                    }
                    items.append(item);
                }
                Json::Value out;
                out["items"] = items;
                out["retention_days"] = kDays;
                if (items.size() == static_cast<Json::ArrayIndex>(kPage)) {
                    out["next_before"] = static_cast<Json::Int64>(last);
                }
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});
}

}  // namespace archive::activity
