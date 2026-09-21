#pragma once

#include <cstdint>
#include <filesystem>

#include "db.h"

namespace archive {

struct GcStats {
    int sharesReleased = 0;
    int uploadsExpired = 0;
    int blobsDeleted = 0;
    int sessionsPurged = 0;
    int strayFilesRemoved = 0;
    std::int64_t bytesReclaimed = 0;
};

// How long an unreferenced blob is kept before collection. This is not tidiness: a blob
// sits at refcount 0 between finishing its upload and being put into a share, so a sweep
// without a grace period would delete files out from under a user mid-flow.
std::int64_t blobGraceSeconds();

// One full pass. Safe to call concurrently with serving traffic; each step is
// individually transactional.
GcStats runGarbageCollection(Database& db, const std::filesystem::path& dataDir);

// Runs the sweep periodically for the lifetime of the application.
void scheduleGarbageCollection(Database& db, const std::filesystem::path& dataDir);

}  // namespace archive
