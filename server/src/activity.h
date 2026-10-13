#pragma once

#include <drogon/drogon.h>

#include <cstdint>
#include <optional>
#include <string>

#include "auth.h"
#include "db.h"

namespace archive::activity {

// The activity log: what happened, who did it if they were signed in, and from which
// network — for administrators to look into something after the fact (a stolen session, a
// link opened where it should not have been, an account change nobody remembers making).
//
// Meaningful events only — sign-ins, links opened and downloaded, uploads, changes to
// links, accounts and invitations, administrators' actions — never the background polling,
// and never content: no message text, no passwords, no link addresses. Addresses are kept
// shortened to their network (see clientNetwork in throttle.h), with the browser's
// identification string. Rows older than kDays are deleted by the sweep; the privacy
// notice states the same number, and Caddy's access log is kept for the same period.
inline constexpr int kDays = 90;

// Called once at startup, before any route can record.
void init(Database& db);

// Records one event. `actor` is the signed-in user, if any; `detail` is a short,
// human-readable description of the target ("“Night out” · 3 files", "user bob").
// Never throws: a failure to record is logged and does not fail the request.
void record(const drogon::HttpRequestPtr& req, const std::optional<User>& actor,
            const std::string& event, const std::string& detail = {});

// A failed or refused sign-in. The attempted name is kept only if it is a real account:
// what people type into a username box is sometimes their password.
void recordFailedLogin(const drogon::HttpRequestPtr& req, const std::string& username,
                       bool refused);

// Deletes rows older than kDays. Returns how many.
int prune(Database& db, std::int64_t now);

//   GET /api/admin/activity?before=<id>&user=<name>&event=<prefix>&network=<text>
// Newest first, a page at a time; admins only.
void registerActivityRoutes(Database& db, Auth& auth);

}  // namespace archive::activity
