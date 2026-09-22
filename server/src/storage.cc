#include "storage.h"

#include <drogon/drogon.h>

#include <system_error>

#include "auth.h"
#include "db.h"
#include "httputil.h"
#include "thumbnail.h"

namespace archive::storage {
namespace {

namespace fs = std::filesystem;

std::int64_t scalar(Database& db, const std::string& sql) {
    auto stmt = db.prepare(sql);
    return stmt.step() ? stmt.columnInt(0) : 0;
}

}  // namespace

void removeBlobFiles(Database& db, const fs::path& dataDir, const std::string& hash) {
    (void)db;
    std::error_code ec;
    const fs::path path = blobPath(dataDir, hash);
    fs::remove(path, ec);
    fs::remove(path.parent_path(), ec);
    fs::remove(path.parent_path().parent_path(), ec);
    thumbnail::remove(dataDir, hash);
}

bool deleteIfOrphaned(Database& db, const fs::path& dataDir, const std::string& hash) {
    if (hash.empty()) {
        return false;
    }
    // One statement so the condition and the delete cannot race: if anything takes a
    // reference between a check and a delete, this simply removes no rows.
    auto stmt = db.prepare(
        "DELETE FROM blobs WHERE sha256 = ? AND refcount <= 0 AND pinned = 0 "
        "AND NOT EXISTS (SELECT 1 FROM share_files WHERE blob_sha256 = ?) "
        "AND NOT EXISTS (SELECT 1 FROM uploads WHERE blob_sha256 = ?)");
    stmt.bind(1, hash).bind(2, hash).bind(3, hash).run();
    if (db.changes() == 0) {
        return false;
    }

    // Row first, then the files — the same ordering the sweep uses, so a crash in
    // between leaks a file rather than leaving a row pointing at nothing.
    removeBlobFiles(db, dataDir, hash);
    return true;
}

void registerStorageRoutes(Database& db, Auth& auth, const fs::path& dataDir) {
    drogon::app().registerHandler(
        "/api/storage",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const User user = requireUser(req, auth);

                Json::Value out;

                // What the blob store physically holds, after deduplication.
                out["stored_bytes"] =
                    static_cast<Json::Int64>(scalar(db, "SELECT COALESCE(SUM(size),0) FROM blobs"));
                out["blob_count"] =
                    static_cast<Json::Int64>(scalar(db, "SELECT COUNT(*) FROM blobs"));

                // Deduplication savings, measured over one consistent set: the files in
                // live shares. `shared_logical_bytes` counts every file separately;
                // `shared_stored_bytes` counts each distinct blob backing them once.
                //
                // These deliberately do not compare against stored_bytes above, which
                // covers the whole blob store — including uploads not yet in a share and
                // orphans awaiting collection. Comparing those two sets would make the
                // saving look negative whenever unshared content is on disk.
                out["shared_logical_bytes"] = static_cast<Json::Int64>(
                    scalar(db,
                           "SELECT COALESCE(SUM(sf.size),0) FROM share_files sf "
                           "JOIN shares s ON s.id = sf.share_id "
                           "WHERE s.deleted_at IS NULL AND s.released_at IS NULL"));

                out["shared_stored_bytes"] = static_cast<Json::Int64>(
                    scalar(db,
                           "SELECT COALESCE(SUM(b.size),0) FROM blobs b WHERE EXISTS ("
                           "  SELECT 1 FROM share_files sf JOIN shares s ON s.id = sf.share_id"
                           "  WHERE sf.blob_sha256 = b.sha256"
                           "    AND s.deleted_at IS NULL AND s.released_at IS NULL)"));

                // Uploads still in flight are on disk but not yet in the blob store.
                out["incoming_bytes"] = static_cast<Json::Int64>(
                    scalar(db, "SELECT COALESCE(SUM(offset_bytes),0) FROM uploads"));

                auto mine = db.prepare(
                    "SELECT COUNT(DISTINCT s.id), COALESCE(SUM(sf.size),0) "
                    "FROM shares s LEFT JOIN share_files sf ON sf.share_id = s.id "
                    "WHERE s.owner_id = ? AND s.deleted_at IS NULL AND s.expires_at > ?");
                mine.bind(1, user.id).bind(2, nowSeconds());
                if (mine.step()) {
                    out["my_shares"] = static_cast<Json::Int64>(mine.columnInt(0));
                    out["my_bytes"] = static_cast<Json::Int64>(mine.columnInt(1));
                }

                // Free space is for the whole filesystem the data directory sits on, and
                // is shared with everything else on that partition. `available` rather
                // than `free`, since the reserved blocks are not ours to use.
                std::error_code ec;
                const fs::space_info space = fs::space(dataDir, ec);
                if (!ec) {
                    out["disk_total"] = static_cast<Json::Int64>(space.capacity);
                    out["disk_available"] = static_cast<Json::Int64>(space.available);
                }

                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get});
}

}  // namespace archive::storage
