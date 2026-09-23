#pragma once

#include <filesystem>

#include "auth.h"
#include "db.h"

namespace archive {

// How long a link lives unless its creator picks otherwise, and the most they can pick.
// Public because the privacy notice states them, and it must state the numbers this code
// enforces rather than a copy that can drift.
inline constexpr int kDefaultExpiryDays = 30;
inline constexpr int kMaxExpiryDays = 365;

// Base URL used to build shareable links, e.g. https://archive.example.com.
// From ARCHIVE_PUBLIC_URL; the server cannot infer it reliably from behind a proxy.
std::string publicBaseUrl();

// Registers:
//   POST   /api/shares           bundle completed uploads into a link      (auth)
//   GET    /api/shares           list your own shares                      (auth)
//   DELETE /api/shares/{token}   revoke early                              (auth, owner)
//   GET    /api/shares/{token}   share metadata                            (public)
//   GET    /d/{token}/{fileId}   the bytes                                 (public)
//
// Public endpoints accept an optional share password via the X-Share-Password header
// or a `p` query parameter, the latter so a link can carry it for convenience.
void registerShareRoutes(Database& db, Auth& auth, const std::filesystem::path& dataDir);

}  // namespace archive
