#pragma once

#include <filesystem>

#include "auth.h"
#include "db.h"

namespace archive {

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
