#include "quota.h"

#include <cmath>
#include <cstdio>

#include "httputil.h"

namespace archive::quota {

std::optional<std::int64_t> roleDefault(const std::string& role) {
    if (role == "admin") {
        return std::nullopt;
    }
    if (role == "privileged") {
        return 100 * kGiB;
    }
    return 20 * kGiB;
}

Limit limitFor(Database& db, std::int64_t userId) {
    auto stmt = db.prepare("SELECT role, storage_limit FROM users WHERE id = ?");
    stmt.bind(1, userId);
    if (!stmt.step()) {
        return {};
    }
    if (stmt.columnIsNull(1)) {
        return {roleDefault(stmt.columnText(0)), false};
    }
    const std::int64_t set = stmt.columnInt(1);
    return {set < 0 ? std::nullopt : std::optional{set}, true};
}

std::int64_t usage(Database& db, std::int64_t userId) {
    // Finished files by content, so the same file in two of your links — or in a link and
    // an upload — counts once; unfinished uploads at their declared size.
    auto stmt = db.prepare(
        "SELECT (SELECT COALESCE(SUM(n), 0) FROM ("
        "  SELECT MAX(n) AS n FROM ("
        "    SELECT sf.blob_sha256 AS h, sf.size AS n FROM share_files sf "
        "    JOIN shares s ON s.id = sf.share_id "
        "    WHERE s.owner_id = ?1 AND s.deleted_at IS NULL AND s.released_at IS NULL "
        "      AND s.expires_at > ?2 "
        "    UNION ALL "
        "    SELECT blob_sha256, total_size FROM uploads "
        "    WHERE owner_id = ?1 AND blob_sha256 IS NOT NULL"
        "  ) GROUP BY h"
        ")) "
        "+ (SELECT COALESCE(SUM(total_size), 0) FROM uploads "
        "   WHERE owner_id = ?1 AND blob_sha256 IS NULL)");
    stmt.bind(1, userId).bind(2, nowSeconds());
    stmt.step();
    return stmt.columnInt(0);
}

void requireRoom(Database& db, const User& user, std::int64_t adding) {
    const Limit limit = limitFor(db, user.id);
    if (!limit.bytes) {
        return;
    }
    const std::int64_t used = usage(db, user.id);
    if (used + adding > *limit.bytes) {
        throw HttpError{413,
                        "this would take you over your " + human(*limit.bytes) +
                            " storage limit — you are using " + human(used) +
                            ". Links that expire or are removed give their space back.",
                        "over_limit"};
    }
}

std::string human(std::int64_t bytes) {
    const double gib = static_cast<double>(bytes) / static_cast<double>(kGiB);
    char text[32];
    if (gib >= 1) {
        // Whole numbers without a decimal: "20 GB", not "20.0 GB".
        std::snprintf(text, sizeof text, std::fabs(gib - std::round(gib)) < 0.05 ? "%.0f GB" : "%.1f GB",
                      gib);
    } else if (bytes >= (1 << 20)) {
        std::snprintf(text, sizeof text, "%.0f MB", static_cast<double>(bytes) / (1 << 20));
    } else {
        std::snprintf(text, sizeof text, "%.0f KB", static_cast<double>(bytes) / 1024);
    }
    return text;
}

}  // namespace archive::quota
