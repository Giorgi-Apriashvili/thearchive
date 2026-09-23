#pragma once

#include <cstdint>
#include <filesystem>

#include "auth.h"
#include "db.h"

namespace archive {

// Largest single upload accepted, advertised to clients as Tus-Max-Size.
std::int64_t maxUploadBytes();

// How long an upload may sit, finished or not, before it is discarded unless it has been
// made into a link. Public because the privacy notice states it.
inline constexpr std::int64_t kUploadTtlSeconds = 24 * 3600;

// Registers the tus 1.0.0 endpoints:
//   OPTIONS /files        capability discovery
//   POST    /files        create an upload, returns Location
//   HEAD    /files/{id}   current Upload-Offset, so a client knows where to resume
//   PATCH   /files/{id}   append bytes at Upload-Offset
//   GET     /api/uploads  your finished uploads not yet made into a link
//
// The last exists so that finished files outlive the tab they were uploaded from: close it
// before creating the link, and the files are still offered on the next visit — from any
// device — until they expire, rather than sitting on disk where nothing can reach them.
//
// On completion the file is hashed and promoted into the content-addressed blob store,
// which is where deduplication happens.
void registerUploadRoutes(Database& db, Auth& auth, const std::filesystem::path& dataDir);

}  // namespace archive
