#include "gc.h"

#include <drogon/drogon.h>

#include <cstdlib>
#include <string>
#include <system_error>
#include <vector>

#include "httputil.h"
#include "profiles.h"
#include "storage.h"
#include "thumbnail.h"

namespace archive {
namespace {

namespace fs = std::filesystem;

constexpr std::int64_t kDefaultGraceSeconds = 24 * 3600;
constexpr double kDefaultIntervalSeconds = 15 * 60;

std::int64_t envInt(const char* key, std::int64_t fallback) {
    const char* value = std::getenv(key);
    if (value == nullptr || *value == '\0') {
        return fallback;
    }
    try {
        const std::int64_t parsed = std::stoll(value);
        return parsed > 0 ? parsed : fallback;
    } catch (const std::exception&) {
        return fallback;
    }
}

// Releases the blob references held by shares that have lapsed — expired on their own,
// or revoked by their owner. The share row itself is kept as a record of what was
// shared and when; only its file rows go.
int releaseLapsedShares(Database& db, std::int64_t now) {
    std::vector<std::int64_t> lapsed;
    {
        auto find = db.prepare(
            "SELECT id FROM shares "
            "WHERE released_at IS NULL AND (expires_at <= ? OR deleted_at IS NOT NULL)");
        find.bind(1, now);
        while (find.step()) {
            lapsed.push_back(find.columnInt(0));
        }
    }

    for (const std::int64_t shareId : lapsed) {
        Transaction tx{db};
        // Decrement exactly once per file row, then remove the rows. Doing both inside
        // the transaction that sets released_at is what makes the sweep idempotent.
        // released_at marks these blobs as having been through a share, which is what
        // lets the orphan sweep skip the grace period for them.
        auto decrement = db.prepare(
            "UPDATE blobs SET refcount = MAX(0, refcount - 1), released_at = ? "
            "WHERE sha256 IN (SELECT blob_sha256 FROM share_files WHERE share_id = ?)");
        decrement.bind(1, now).bind(2, shareId).run();

        auto dropFiles = db.prepare("DELETE FROM share_files WHERE share_id = ?");
        dropFiles.bind(1, shareId).run();

        auto mark = db.prepare("UPDATE shares SET released_at = ? WHERE id = ?");
        mark.bind(1, now).bind(2, shareId).run();
        tx.commit();
    }
    return static_cast<int>(lapsed.size());
}

// Abandoned uploads: started and never finished, or finished but never made into a
// share. The row goes now; any blob it produced is collected below once past grace.
int expireUploads(Database& db, const fs::path& dataDir, std::int64_t now) {
    std::vector<std::string> ids;
    {
        auto find = db.prepare("SELECT id FROM uploads WHERE expires_at <= ?");
        find.bind(1, now);
        while (find.step()) {
            ids.push_back(find.columnText(0));
        }
    }

    for (const std::string& id : ids) {
        auto del = db.prepare("DELETE FROM uploads WHERE id = ?");
        del.bind(1, id).run();
        std::error_code ec;
        fs::remove(storage::incomingPath(dataDir, id), ec);
    }
    return static_cast<int>(ids.size());
}

struct Orphan {
    std::string hash;
    std::int64_t size = 0;
};

// Collects unreferenced, unpinned blobs.
//
// Two ways to qualify. A blob that has been through a share (released_at set) goes as
// soon as nothing references it: the grace period protects the gap between an upload
// finishing and a share being made, and such a blob is long past that. Everything else
// must wait out the grace period, because it may be a completed upload whose share is
// still being assembled.
//
// The NOT EXISTS clauses are belt-and-braces against refcount drift: if a counter were
// ever wrong, this still refuses to delete a blob some share genuinely points at.
int deleteOrphanBlobs(Database& db, const fs::path& dataDir, std::int64_t now,
                      std::int64_t& bytesReclaimed) {
    std::vector<Orphan> orphans;
    {
        auto find = db.prepare(
            "SELECT sha256, size FROM blobs "
            "WHERE refcount <= 0 AND pinned = 0 "
            "AND (released_at IS NOT NULL OR created_at < ?) "
            "AND NOT EXISTS (SELECT 1 FROM share_files WHERE blob_sha256 = blobs.sha256) "
            "AND NOT EXISTS (SELECT 1 FROM uploads WHERE blob_sha256 = blobs.sha256)");
        find.bind(1, now - blobGraceSeconds());
        while (find.step()) {
            orphans.push_back({find.columnText(0), find.columnInt(1)});
        }
    }

    int deleted = 0;
    for (const Orphan& orphan : orphans) {
        // Row first, then the file. The reverse order would leave a row pointing at a
        // missing file if the process died in between, which surfaces as a broken
        // download; this order can only leak a file, which is harmless and recoverable.
        auto del = db.prepare("DELETE FROM blobs WHERE sha256 = ? AND refcount <= 0");
        del.bind(1, orphan.hash).run();
        if (db.changes() == 0) {
            continue;  // something referenced it between the query and here
        }

        // Routed through the shared helper so thumbnails and any future derivative are
        // reaped here too, rather than only on the explicit-delete path.
        if (fs::exists(storage::blobPath(dataDir, orphan.hash))) {
            bytesReclaimed += orphan.size;
        }
        storage::removeBlobFiles(db, dataDir, orphan.hash);
        ++deleted;
    }
    return deleted;
}

// Files in incoming/ with no surviving upload row: the residue of a crash between
// creating the file and committing the row, or of an interrupted cleanup.
int removeStrayIncoming(Database& db, const fs::path& dataDir, std::int64_t now) {
    const fs::path incoming = dataDir / "incoming";
    std::error_code ec;
    if (!fs::is_directory(incoming, ec)) {
        return 0;
    }

    int removed = 0;
    for (const auto& entry : fs::directory_iterator{incoming, ec}) {
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        const auto written = fs::last_write_time(entry.path(), ec);
        if (ec) {
            continue;
        }
        const auto age = decltype(written)::clock::now() - written;
        if (std::chrono::duration_cast<std::chrono::seconds>(age).count() <
            blobGraceSeconds()) {
            continue;  // may belong to an upload in progress right now
        }

        auto stmt = db.prepare("SELECT 1 FROM uploads WHERE id = ?");
        stmt.bind(1, entry.path().filename().string());
        if (stmt.step()) {
            continue;
        }
        if (fs::remove(entry.path(), ec)) {
            ++removed;
        }
    }
    (void)now;
    return removed;
}

// Renders previews for blobs stored before thumbnailing existed. Bounded per pass: this
// runs on the event loop, and a few hundred milliseconds of rendering is fine while
// stalling it for a whole backlog is not. Successive sweeps finish the rest.
int renderMissingThumbnails(Database& db, const fs::path& dataDir) {
    constexpr int kPerSweep = 20;

    std::vector<std::pair<std::string, std::string>> pending;  // hash, content type
    {
        auto find = db.prepare(
            "SELECT sha256, COALESCE(content_type, '') FROM blobs "
            "WHERE thumb = 0 AND content_type LIKE 'image/%' LIMIT ?");
        find.bind(1, static_cast<std::int64_t>(kPerSweep));
        while (find.step()) {
            pending.emplace_back(find.columnText(0), find.columnText(1));
        }
    }

    int rendered = 0;
    for (const auto& [hash, contentType] : pending) {
        if (!thumbnail::isThumbnailable(contentType)) {
            // Marked failed so it is not reconsidered on every sweep from here on.
            auto skip = db.prepare("UPDATE blobs SET thumb = 2 WHERE sha256 = ?");
            skip.bind(1, hash).run();
            continue;
        }
        const bool ok = thumbnail::generate(storage::blobPath(dataDir, hash), dataDir, hash);
        auto mark = db.prepare("UPDATE blobs SET thumb = ? WHERE sha256 = ?");
        mark.bind(1, static_cast<std::int64_t>(ok ? 1 : 2)).bind(2, hash).run();
        rendered += ok ? 1 : 0;
    }
    return rendered;
}

int purgeExpiredSessions(Database& db, std::int64_t now) {
    auto stmt = db.prepare("DELETE FROM sessions WHERE expires_at <= ?");
    stmt.bind(1, now).run();
    return db.changes();
}

}  // namespace

std::int64_t blobGraceSeconds() {
    return envInt("ARCHIVE_BLOB_GRACE_SECONDS", kDefaultGraceSeconds);
}

GcStats runGarbageCollection(Database& db, const fs::path& dataDir) {
    const std::int64_t now = nowSeconds();
    GcStats stats;
    stats.sharesReleased = releaseLapsedShares(db, now);
    stats.uploadsExpired = expireUploads(db, dataDir, now);
    stats.blobsDeleted = deleteOrphanBlobs(db, dataDir, now, stats.bytesReclaimed);
    stats.strayFilesRemoved = removeStrayIncoming(db, dataDir, now);
    stats.sessionsPurged = purgeExpiredSessions(db, now);
    stats.thumbnailsRendered = renderMissingThumbnails(db, dataDir);
    stats.avatarsRemoved = removeStaleAvatars(db, dataDir);
    return stats;
}

std::int64_t gcIntervalSeconds() {
    return envInt("ARCHIVE_GC_INTERVAL_SECONDS", static_cast<std::int64_t>(kDefaultIntervalSeconds));
}

void scheduleGarbageCollection(Database& db, const fs::path& dataDir) {
    const double interval = static_cast<double>(gcIntervalSeconds());

    drogon::app().registerBeginningAdvice([&db, dataDir, interval]() {
        auto sweep = [&db, dataDir]() {
            try {
                const GcStats stats = runGarbageCollection(db, dataDir);
                if (stats.sharesReleased || stats.uploadsExpired || stats.blobsDeleted ||
                    stats.strayFilesRemoved || stats.sessionsPurged ||
                    stats.thumbnailsRendered) {
                    LOG_INFO << "gc: " << stats.sharesReleased << " share(s) released, "
                             << stats.uploadsExpired << " upload(s) expired, "
                             << stats.blobsDeleted << " blob(s) deleted ("
                             << stats.bytesReclaimed << " bytes), "
                             << stats.strayFilesRemoved << " stray file(s), "
                             << stats.sessionsPurged << " session(s) purged, "
                             << stats.thumbnailsRendered << " thumbnail(s) rendered, "
                             << stats.avatarsRemoved << " stale picture(s) removed";
                }
            } catch (const std::exception& e) {
                // A failed sweep must not take the server down; the next one retries.
                LOG_ERROR << "gc sweep failed: " << e.what();
            }
        };

        // Once at startup so a restart clears whatever accumulated while it was down,
        // then on the interval. This runs on the main event loop: the work is a handful
        // of queries and unlinks at this scale, and the simplicity is worth more than
        // the microseconds a worker thread would save.
        sweep();
        drogon::app().getLoop()->runEvery(interval, sweep);
    });
}

}  // namespace archive
