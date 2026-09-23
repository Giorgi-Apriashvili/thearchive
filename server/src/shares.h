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

// A share as a card, for chat messages and the inbox:
//   {state, title, token?, file_count, total_bytes, expires_at, password_protected,
//    visibility, previews: [thumb URLs]}
// `state` is live | expired | revoked | used_up, resolved *now* — a link posted last
// month must read as expired rather than as a click that fails. The token, the link
// itself, is included only while the link works. Previews are thumbnails of its first
// few images, and are left out of a password-protected link: fetching them would need
// the password, which is exactly what a card must not carry.
Json::Value shareCard(Database& db, std::int64_t shareId);

// The id of `token` if it is `userId`'s own link and still live; otherwise a 404 for a
// link that does not exist or is not theirs — a stranger's link is indistinguishable
// from none, so a guessed token reveals nothing — and 409 for one of theirs that has
// expired, been revoked or used up. Only your own working links can be sent anywhere.
std::int64_t requireOwnLiveShare(Database& db, const std::string& token, std::int64_t userId);

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
