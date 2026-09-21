#pragma once

#include <cstdint>
#include <filesystem>

#include "auth.h"
#include "db.h"

namespace archive {

// Largest single upload accepted, advertised to clients as Tus-Max-Size.
std::int64_t maxUploadBytes();

// Registers the tus 1.0.0 endpoints:
//   OPTIONS /files        capability discovery
//   POST    /files        create an upload, returns Location
//   HEAD    /files/{id}   current Upload-Offset, so a client knows where to resume
//   PATCH   /files/{id}   append bytes at Upload-Offset
//
// On completion the file is hashed and promoted into the content-addressed blob store,
// which is where deduplication happens.
void registerUploadRoutes(Database& db, Auth& auth, const std::filesystem::path& dataDir);

}  // namespace archive
