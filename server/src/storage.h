#pragma once

#include <filesystem>
#include <string>

namespace archive {
class Auth;
class Database;
}  // namespace archive

namespace archive::storage {

// In-progress tus uploads, named by upload id.
inline std::filesystem::path incomingPath(const std::filesystem::path& dataDir,
                                          const std::string& id) {
    return dataDir / "incoming" / id;
}

// Content-addressed: blobs/ab/cd/<full hash>. Two levels of fan-out keep any single
// directory small enough that lookups stay cheap as the store grows.
inline std::filesystem::path blobPath(const std::filesystem::path& dataDir,
                                      const std::string& hash) {
    return dataDir / "blobs" / hash.substr(0, 2) / hash.substr(2, 2) / hash;
}

// Deletes a blob's row and file when nothing references it any more. Returns whether it
// went.
//
// Deliberately ignores the sweep's grace period. That grace exists to protect windows
// the user did not choose — an upload finishing while its share is still being
// assembled. When someone explicitly discards content, waiting a day to reclaim the
// space would just be surprising.
bool deleteIfOrphaned(Database& db, const std::filesystem::path& dataDir,
                      const std::string& hash);

// GET /api/storage — what the store holds and what the volume has left.
//
// Reports free space for the filesystem the data directory lives on, which is shared
// with whatever else is on that partition; it is not a quota. Requires a session, since
// disk figures are not something to hand out anonymously.
void registerStorageRoutes(Database& db, Auth& auth, const std::filesystem::path& dataDir);

}  // namespace archive::storage
