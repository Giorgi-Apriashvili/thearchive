#include <drogon/drogon.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>

#include "admin.h"
#include "auth.h"
#include "chat.h"
#include "csp.h"
#include "db.h"
#include "gc.h"
#include "privacy.h"
#include "shares.h"
#include "storage.h"
#include "thumbnail.h"
#include "tus.h"

namespace {

// Ceiling for a single request body, which in practice means one tus chunk. 64 MiB is
// comfortably above a sensible client chunk size and low enough that concurrent uploads
// cannot exhaust memory.
constexpr std::size_t kMaxRequestBody = 64UL * 1024 * 1024;

std::string envOr(const char* key, const std::string& fallback) {
    const char* value = std::getenv(key);
    return (value != nullptr && *value != '\0') ? std::string{value} : fallback;
}

}  // namespace

int main(int, char** argv) {
    // stdout is block-buffered whenever it is not a terminal, which is exactly the case
    // under systemd, under Docker, and behind any log redirect — so a long-running
    // server appears to produce no logs at all until it exits. Line buffering costs
    // nothing at this volume and makes `docker logs` and `tail -f` behave.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    archive::thumbnail::startup(argv[0]);

    try {
        // Config comes from the environment rather than Drogon's config.json so the
        // container needs no config file baked in or mounted.
        const std::filesystem::path dataDir = envOr("ARCHIVE_DATA_DIR", "./data");
        const std::string bind = envOr("ARCHIVE_BIND", "127.0.0.1");
        const int port = std::stoi(envOr("ARCHIVE_PORT", "8080"));

        std::filesystem::create_directories(dataDir / "db");
        std::filesystem::create_directories(dataDir / "blobs");
        std::filesystem::create_directories(dataDir / "incoming");

        archive::Database db{dataDir / "db" / "archive.db"};
        db.migrate();

        archive::Auth auth{db};

        LOG_INFO << "data dir " << dataDir.string()
                 << ", schema v" << db.schemaVersion();
        if (!auth.hasAnyUser()) {
            LOG_WARN << "no users yet - create the first one via POST /api/auth/bootstrap";
        }

        archive::registerAuthRoutes(auth);
        archive::registerUploadRoutes(db, auth, dataDir);
        archive::registerShareRoutes(db, auth, dataDir);
        archive::storage::registerStorageRoutes(db, auth, dataDir);
        archive::registerAdminRoutes(db, auth, dataDir);
        archive::registerChatRoutes(db, auth);
        archive::registerContentSecurityPolicy();
        archive::registerPrivacyRoutes();
        archive::scheduleGarbageCollection(db, dataDir);

        LOG_INFO << "public base url " << archive::publicBaseUrl();

        // Serving the built frontend is optional: in development Vite serves it and
        // proxies the API here, so this stays unset.
        const std::string webRoot = envOr("ARCHIVE_WEB_ROOT", "");
        if (!webRoot.empty() && std::filesystem::is_directory(webRoot)) {
            drogon::app().setDocumentRoot(webRoot);
            const std::string index =
                (std::filesystem::path{webRoot} / "index.html").string();

            // /d/<token> is a route that exists only in the browser, so an unmatched
            // path has to return the app shell rather than a 404. API prefixes are
            // excluded: a failed fetch must see its real status, not a page of HTML
            // with a 200 on it.
            drogon::app().setCustomErrorHandler(
                [index](drogon::HttpStatusCode code, const drogon::HttpRequestPtr& req) {
                    const std::string path = req ? std::string{req->path()} : std::string{};
                    const bool isApi = path.rfind("/api", 0) == 0 ||
                                       path.rfind("/files", 0) == 0 ||
                                       path.rfind("/healthz", 0) == 0;
                    if (code == drogon::k404NotFound && !isApi) {
                        auto resp = drogon::HttpResponse::newFileResponse(index);
                        resp->setStatusCode(drogon::k200OK);
                        return resp;
                    }
                    Json::Value body;
                    body["error"] = "not found";
                    auto resp = drogon::HttpResponse::newHttpJsonResponse(body);
                    resp->setStatusCode(code);
                    return resp;
                });
            LOG_INFO << "serving frontend from " << webRoot;
        } else {
            LOG_INFO << "no ARCHIVE_WEB_ROOT set - API only";
        }

        drogon::app().registerHandler(
            "/healthz",
            [&db](const drogon::HttpRequestPtr&,
                  std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
                Json::Value body;
                body["status"] = "ok";
                body["schema"] = db.schemaVersion();
                callback(drogon::HttpResponse::newHttpJsonResponse(body));
            },
            {drogon::Get});

        // Binds loopback by default. TLS, HTTP/2 and certificate renewal are Caddy's
        // job; this process only ever speaks plaintext HTTP over the loopback
        // interface. See docs/DESIGN.md for why that split is deliberate.
        LOG_INFO << "listening on " << bind << ":" << port;
        // Drogon 1.9 buffers each request body in memory rather than streaming it, so
        // this bounds per-request memory. tus chunks are what land here, not whole
        // files — a 3 GB upload arrives as many bounded PATCHes.
        drogon::app().setClientMaxBodySize(kMaxRequestBody);

        // Drogon announces itself as `Server: drogon/<version>` on every response by
        // default, which turns "is this host running something with a known hole" into a
        // lookup rather than an investigation. Withholding it fixes nothing underneath,
        // but there is no reason to volunteer it, and Caddy passes the upstream's header
        // straight through.
        drogon::app().enableServerHeader(false);

        // Drogon builds a 256-directory scratch tree for multipart uploads at startup,
        // relative to the working directory. Nothing here uses its multipart handling —
        // tus writes files directly — but it creates the tree regardless, and logs 256
        // permission errors when the working directory is not writable, which it is not
        // for a container running as a non-root user.
        const std::filesystem::path uploadScratch =
            std::filesystem::temp_directory_path() / "thearchive-upload-scratch";
        std::filesystem::create_directories(uploadScratch);
        drogon::app().setUploadPath(uploadScratch.string());

        drogon::app()
            .addListener(bind, port)
            .setThreadNum(0)  // one event loop per hardware thread
            .run();

        archive::thumbnail::shutdown();
        return 0;
    } catch (const std::exception& e) {
        // Startup failures must be loud: a container that stays up with a broken
        // database is worse than one that restarts.
        LOG_FATAL << "startup failed: " << e.what();
        return 1;
    }
}
