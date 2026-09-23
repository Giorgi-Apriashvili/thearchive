#include "privacy.h"

#include <drogon/drogon.h>

#include <algorithm>
#include <cstdlib>
#include <string>

#include "auth.h"
#include "gc.h"
#include "httputil.h"
#include "shares.h"
#include "tus.h"

namespace archive {
namespace {

std::string env(const char* key) {
    const char* value = std::getenv(key);
    return value != nullptr ? std::string{value} : std::string{};
}

// deploy/backup.sh reads the same variable with the same default, so the retention the
// notice states and the retention the script applies come from one place.
constexpr int kDefaultBackupDays = 14;

int backupDays() {
    const std::string raw = env("ARCHIVE_BACKUP_DAYS");
    if (raw.empty()) {
        return kDefaultBackupDays;
    }
    try {
        const int days = std::stoi(raw);
        return days > 0 ? days : kDefaultBackupDays;
    } catch (const std::exception&) {
        return kDefaultBackupDays;
    }
}

}  // namespace

void registerPrivacyRoutes() {
    drogon::app().registerHandler(
        "/api/privacy",
        [](const drogon::HttpRequestPtr&,
           std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([] {
                Json::Value out;

                const std::string name = env("ARCHIVE_OPERATOR_NAME");
                const std::string contact = env("ARCHIVE_OPERATOR_CONTACT");
                // Null rather than an empty object, so the page can say plainly that no
                // operator has been named instead of rendering blanks.
                if (name.empty() && contact.empty()) {
                    out["operator"] = Json::nullValue;
                } else {
                    out["operator"]["name"] = name;
                    out["operator"]["contact"] = contact;
                }
                if (const std::string where = env("ARCHIVE_HOSTING_LOCATION"); !where.empty()) {
                    out["hosting_location"] = where;
                }

                Json::Value& retention = out["retention"];
                retention["session_days"] = kSessionDays;
                retention["link_default_days"] = kDefaultExpiryDays;
                retention["link_max_days"] = kMaxExpiryDays;
                // Rounded up: "within 15 minutes" must not undersell a 15.5-minute sweep.
                retention["sweep_minutes"] =
                    static_cast<Json::Int64>((gcIntervalSeconds() + 59) / 60);
                // An upload never made into a link goes once its own expiry has passed
                // *and* the blob grace period has — whichever is later — then at the next
                // sweep. Both default to a day; either can be the one that decides.
                const std::int64_t unshared =
                    std::max(kUploadTtlSeconds, blobGraceSeconds()) + gcIntervalSeconds();
                retention["unshared_upload_hours"] =
                    static_cast<Json::Int64>((unshared + 3599) / 3600);
                retention["backup_days"] = backupDays();

                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Get});
}

}  // namespace archive
