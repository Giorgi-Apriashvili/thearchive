#pragma once

#include <filesystem>

#include "auth.h"
#include "db.h"

namespace archive {

// The control panel's API. Every handler is behind requireAdmin — the frontend hiding a
// link is presentation, not access control.
//
//   GET    /api/admin/users              list with derived per-user totals
//   GET    /api/admin/users/{id}         profile, their shares, their invite chain
//   POST   /api/admin/users/{id}/role    {"role": "user"|"privileged"|"admin"}
//   POST   /api/admin/users/{id}/disable blocks sign-in, drops live sessions
//   POST   /api/admin/users/{id}/enable
//   POST   /api/admin/users/{id}/revoke  revoke all of their live shares
//   DELETE /api/admin/users/{id}         destructive, cascades to shares and sessions
//   GET    /api/admin/invites            every code, with who made and used it
//   DELETE /api/admin/invites/{code}     revoke an unredeemed code
//   GET    /api/admin/shares             every live share, any owner
//   GET    /api/admin/overview           storage totals, per-user usage, sweep state
void registerAdminRoutes(Database& db, Auth& auth, const std::filesystem::path& dataDir);

}  // namespace archive
