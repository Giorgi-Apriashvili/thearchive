#pragma once

#include <filesystem>

#include "auth.h"
#include "db.h"

namespace archive {

// Member profiles: a display name, a short bio and a picture, alongside what the site
// already knows (role, when they joined, who invited them).
//
//   GET    /api/users/{username}           a member's profile              (members)
//   PATCH  /api/me/profile                 {display_name?, bio?}           (yourself)
//   PUT    /api/me/avatar                  raw image body                  (yourself)
//   DELETE /api/me/avatar                                                  (yourself)
//   GET    /api/avatars/{id}/{version}     the picture                     (members)
//
// Visible to signed-in members only, never to someone holding just a link.
//
// A profile lists the rooms *you share* with that member, not all of theirs. Room names
// are visible to everyone, but who is in a room is visible only to that room's members;
// listing every room a person belongs to would hand that to anyone who opened their
// profile.
void registerProfileRoutes(Database& db, Auth& auth, const std::filesystem::path& dataDir);

// avatars/<user id>-<version>.webp
std::filesystem::path avatarPath(const std::filesystem::path& dataDir, std::int64_t userId,
                                 const std::string& version);

// Deletes every picture no member currently points at — replaced ones a failed delete
// left behind, and those of deleted accounts. Part of the regular sweep.
int removeStaleAvatars(Database& db, const std::filesystem::path& dataDir);

}  // namespace archive
