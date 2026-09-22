#include "tus.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <system_error>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>

#include "crypto.h"
#include "mimetype.h"
#include "storage.h"
#include "thumbnail.h"

namespace archive {
namespace {

namespace fs = std::filesystem;

constexpr const char* kTusVersion = "1.0.0";
constexpr std::int64_t kUploadTtlSeconds = 24 * 3600;

Json::Value errorBody(const std::string& message) {
    Json::Value body;
    body["error"] = message;
    return body;
}

drogon::HttpResponsePtr tusResponse(int status) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->addHeader("Tus-Resumable", kTusVersion);
    return resp;
}

drogon::HttpResponsePtr tusError(int status, const std::string& message) {
    auto resp = drogon::HttpResponse::newHttpJsonResponse(errorBody(message));
    resp->setStatusCode(static_cast<drogon::HttpStatusCode>(status));
    resp->addHeader("Tus-Resumable", kTusVersion);
    return resp;
}

// Distinct from the shared guarded(): every tus response, errors included, must carry
// Tus-Resumable or a conforming client treats it as a non-tus server.
template <typename Fn>
drogon::HttpResponsePtr guardedTus(Fn&& fn) {
    try {
        return fn();
    } catch (const HttpError& e) {
        return tusError(e.status(), e.what());
    } catch (const std::exception& e) {
        LOG_ERROR << "upload handler: " << e.what();
        return tusError(500, "internal error");
    }
}

std::int64_t parseInt64(const std::string& text, const char* what) {
    if (text.empty()) {
        throw HttpError{400, std::string{"missing "} + what};
    }
    try {
        std::size_t consumed = 0;
        const std::int64_t value = std::stoll(text, &consumed);
        if (consumed != text.size() || value < 0) {
            throw HttpError{400, std::string{"malformed "} + what};
        }
        return value;
    } catch (const HttpError&) {
        throw;
    } catch (const std::exception&) {
        throw HttpError{400, std::string{"malformed "} + what};
    }
}

// tus Upload-Metadata is "key <base64>,key2 <base64>". Keys without a value are legal.
std::map<std::string, std::string> parseMetadata(const std::string& header) {
    std::map<std::string, std::string> out;
    std::istringstream stream{header};
    std::string pair;
    while (std::getline(stream, pair, ',')) {
        const std::size_t start = pair.find_first_not_of(' ');
        if (start == std::string::npos) {
            continue;
        }
        const std::size_t space = pair.find(' ', start);
        if (space == std::string::npos) {
            out[pair.substr(start)] = "";
            continue;
        }
        const std::string key = pair.substr(start, space - start);
        if (auto decoded = crypto::base64Decode(pair.substr(space + 1))) {
            out[key] = *decoded;
        }
    }
    return out;
}

// Reads the first key present, so several client spellings are accepted: the tus spec
// says filename/filetype, Uppy also emits name/type/relativePath.
std::string metaValue(const std::map<std::string, std::string>& meta,
                      std::initializer_list<const char*> keys) {
    for (const char* key : keys) {
        const auto it = meta.find(key);
        if (it != meta.end() && !it->second.empty()) {
            return it->second;
        }
    }
    return {};
}

// A browser's File.lastModified is milliseconds; a shell client is likelier to send
// seconds. Anything past ~2001 in milliseconds is implausible as a second count, so the
// magnitude disambiguates reliably.
std::int64_t parseClientMtime(const std::string& raw) {
    if (raw.empty()) {
        return 0;
    }
    try {
        const std::int64_t value = std::stoll(raw);
        if (value <= 0) {
            return 0;
        }
        return (value > 100000000000LL) ? value / 1000 : value;
    } catch (const std::exception&) {
        return 0;
    }
}

// Keeps a folder upload's shape for a future archive while refusing anything that could
// escape a directory. Purely descriptive metadata — it never becomes a real path.
std::string sanitiseRelativePath(const std::string& raw) {
    std::string out;
    for (const auto& part : fs::path{raw}) {
        const std::string component = part.string();
        if (component.empty() || component == "." || component == ".." ||
            component == "/" || component == "\\") {
            continue;
        }
        if (!out.empty()) {
            out += '/';
        }
        out += component;
    }
    if (out.size() > 1024) {
        out.resize(1024);
    }
    return out;
}

// Strips any directory component. The filename is client-supplied and is only ever a
// display label and a Content-Disposition value — it must never influence a path.
std::string sanitiseFilename(const std::string& raw) {
    std::string name = fs::path{raw}.filename().string();
    if (name.empty() || name == "." || name == "..") {
        return "download";
    }
    if (name.size() > 255) {
        name.resize(255);
    }
    return name;
}

struct UploadRow {
    std::string id;
    std::int64_t ownerId = 0;
    std::string filename;
    std::string contentType;
    std::int64_t totalSize = 0;
    std::int64_t offset = 0;
    bool complete = false;
    std::string blobSha256;
};

UploadRow loadUpload(Database& db, const std::string& id, const User& user) {
    auto stmt = db.prepare(
        "SELECT id, owner_id, filename, content_type, total_size, offset_bytes, "
        "completed_at, COALESCE(blob_sha256, '') FROM uploads WHERE id = ?");
    stmt.bind(1, id);
    if (!stmt.step()) {
        throw HttpError{404, "no such upload"};
    }
    UploadRow row;
    row.id = stmt.columnText(0);
    row.ownerId = stmt.columnInt(1);
    row.filename = stmt.columnText(2);
    row.contentType = stmt.columnText(3);
    row.totalSize = stmt.columnInt(4);
    row.offset = stmt.columnInt(5);
    row.complete = !stmt.columnIsNull(6);
    row.blobSha256 = stmt.columnText(7);

    // 404 rather than 403: a user who does not own an upload should not be able to
    // confirm that the id exists at all.
    if (row.ownerId != user.id) {
        throw HttpError{404, "no such upload"};
    }
    return row;
}

// Hashes the finished file and moves it into the blob store. Returns the hash.
std::string promoteToBlob(Database& db, const fs::path& dataDir, const std::string& id,
                          std::int64_t size, std::int64_t uploaderId) {
    const fs::path source = storage::incomingPath(dataDir, id);

    // The hash is computed by re-reading the file rather than by carrying incremental
    // state across PATCHes: chunks may arrive on different connections and threads, and
    // a sequential NVMe read is far cheaper than persisting digest state correctly.
    crypto::Sha256 hasher;
    crypto::Crc32 crc;
    {
        const int fd = ::open(source.c_str(), O_RDONLY);
        if (fd < 0) {
            throw std::runtime_error("cannot reopen upload: " + std::string{std::strerror(errno)});
        }
        std::array<char, 1 << 20> buffer{};
        ssize_t got = 0;
        while ((got = ::read(fd, buffer.data(), buffer.size())) > 0) {
            hasher.update(buffer.data(), static_cast<std::size_t>(got));
            // Same pass: the ZIP writer needs this before it can emit the entry header.
            crc.update(buffer.data(), static_cast<std::size_t>(got));
        }
        const int readError = (got < 0) ? errno : 0;
        ::close(fd);
        if (readError != 0) {
            throw std::runtime_error("read failed: " + std::string{std::strerror(readError)});
        }
    }
    const std::string hash = hasher.hex();
    const fs::path target = storage::blobPath(dataDir, hash);

    // Sniffed while the file is still in place, and from its actual bytes rather than
    // the client's claim — this is the type the download endpoint will serve.
    const std::string detectedType = mime::sniff(source);

    if (fs::exists(target)) {
        // Deduplication: the identical bytes are already stored, so drop the copy.
        fs::remove(source);
    } else {
        fs::create_directories(target.parent_path());
        fs::rename(source, target);  // same filesystem, so this is atomic
    }

    // refcount starts at 0; share creation is what takes a reference. The GC must
    // therefore skip blobs younger than its grace period, or it would delete a blob
    // between upload completion and the share being created.
    //
    // On a dedupe hit the row already exists, and only the recency is refreshed — the
    // original uploader and first-seen time are the interesting provenance and are kept.
    auto insert = db.prepare(
        "INSERT INTO blobs (sha256, size, refcount, created_at, content_type, "
        "first_uploader, last_referenced_at, crc32) VALUES (?, ?, 0, ?, ?, ?, ?, ?) "
        "ON CONFLICT(sha256) DO UPDATE SET last_referenced_at = excluded.last_referenced_at, "
        "crc32 = COALESCE(blobs.crc32, excluded.crc32)");
    const std::int64_t now = nowSeconds();
    insert.bind(1, hash)
        .bind(2, size)
        .bind(3, now)
        .bind(4, detectedType)
        .bind(5, uploaderId)
        .bind(6, now)
        .bind(7, static_cast<std::int64_t>(crc.value()))
        .run();

    // Render previews now, while the bytes are hot in page cache. This is the one
    // blocking call added to the upload path — roughly 50-150 ms for a 12 MP photo — and
    // a failure is recorded rather than propagated: an image libvips cannot read is not
    // a reason to reject an upload that otherwise succeeded.
    //
    // Skipped entirely on a dedupe hit that already has previews: four people uploading
    // the same photo from the same night should pay the rendering cost once.
    bool alreadyRendered = false;
    {
        auto state = db.prepare("SELECT thumb FROM blobs WHERE sha256 = ?");
        state.bind(1, hash);
        alreadyRendered = state.step() && state.columnInt(0) != 0;
    }
    if (!alreadyRendered && thumbnail::isThumbnailable(detectedType)) {
        const bool rendered = thumbnail::generate(target, dataDir, hash);
        auto mark = db.prepare("UPDATE blobs SET thumb = ? WHERE sha256 = ?");
        mark.bind(1, static_cast<std::int64_t>(rendered ? 1 : 2)).bind(2, hash).run();
        if (!rendered) {
            LOG_WARN << "thumbnail failed for " << hash << " (" << detectedType << ")";
        }
    }

    auto complete = db.prepare(
        "UPDATE uploads SET blob_sha256 = ?, completed_at = ? WHERE id = ?");
    complete.bind(1, hash).bind(2, nowSeconds()).bind(3, id).run();

    return hash;
}

}  // namespace

std::int64_t maxUploadBytes() {
    const char* value = std::getenv("ARCHIVE_MAX_UPLOAD_BYTES");
    if (value != nullptr && *value != '\0') {
        try {
            return std::stoll(value);
        } catch (const std::exception&) {
            // fall through to the default
        }
    }
    return std::int64_t{20} * 1024 * 1024 * 1024;  // 20 GiB
}

void registerUploadRoutes(Database& db, Auth& auth, const fs::path& dataDir) {
    auto& app = drogon::app();

    // Drogon answers OPTIONS itself, before routing, with CORS and Allow headers — a
    // handler registered for Options never runs. tus requires OPTIONS on the collection
    // for capability discovery, so the response has to be installed ahead of the router.
    app.registerSyncAdvice([](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
        if (req->method() != drogon::Options || req->path() != "/files") {
            return {};  // null means "carry on routing"
        }
        auto resp = tusResponse(204);
        resp->addHeader("Tus-Version", kTusVersion);
        resp->addHeader("Tus-Extension", "creation,termination");
        resp->addHeader("Tus-Max-Size", std::to_string(maxUploadBytes()));
        return resp;
    });

    app.registerHandler(
        "/files",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guardedTus([&]() -> drogon::HttpResponsePtr {
                const User user = requireUser(req, auth);
                const std::int64_t length =
                    parseInt64(req->getHeader("Upload-Length"), "Upload-Length");
                if (length == 0) {
                    throw HttpError{400, "Upload-Length must be greater than zero"};
                }
                if (length > maxUploadBytes()) {
                    throw HttpError{413, "upload exceeds the maximum permitted size"};
                }

                const auto metadata = parseMetadata(req->getHeader("Upload-Metadata"));
                const std::string filename =
                    sanitiseFilename(metaValue(metadata, {"filename", "name"}));
                const std::string declaredType = metaValue(metadata, {"filetype", "type"});
                const std::string relativePath =
                    sanitiseRelativePath(metaValue(metadata, {"relativePath", "relative_path"}));
                const std::int64_t clientMtime =
                    parseClientMtime(metaValue(metadata, {"lastModified", "filetime", "mtime"}));
                const std::string userAgent = req->getHeader("User-Agent");

                const std::string id = crypto::randomToken(16);
                const fs::path path = storage::incomingPath(dataDir, id);

                // Create the (empty) file up front so a PATCH never has to decide
                // whether an absent file means "new" or "lost".
                const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
                if (fd < 0) {
                    throw std::runtime_error("cannot create upload file: " +
                                             std::string{std::strerror(errno)});
                }
                ::close(fd);

                const std::int64_t now = nowSeconds();
                auto stmt = db.prepare(
                    "INSERT INTO uploads (id, owner_id, filename, content_type, "
                    "total_size, offset_bytes, created_at, expires_at, "
                    "client_mtime, relative_path, user_agent) "
                    "VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?)");
                stmt.bind(1, id).bind(2, user.id);
                filename.empty() ? stmt.bindNull(3) : stmt.bind(3, filename);
                declaredType.empty() ? stmt.bindNull(4) : stmt.bind(4, declaredType);
                stmt.bind(5, length).bind(6, now).bind(7, now + kUploadTtlSeconds);
                clientMtime == 0 ? stmt.bindNull(8) : stmt.bind(8, clientMtime);
                relativePath.empty() ? stmt.bindNull(9) : stmt.bind(9, relativePath);
                userAgent.empty() ? stmt.bindNull(10) : stmt.bind(10, userAgent);
                stmt.run();

                auto resp = tusResponse(201);
                resp->addHeader("Location", "/files/" + id);
                return resp;
            }));
        },
        {drogon::Post});

    app.registerHandler(
        "/files/{id}",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              const std::string& id) {
            callback(guardedTus([&]() -> drogon::HttpResponsePtr {
                const User user = requireUser(req, auth);
                UploadRow upload = loadUpload(db, id, user);

                // Drogon dispatches a HEAD request through the GET route and strips the
                // body afterwards, so a handler registered only for Head never matches.
                if (req->method() == drogon::Head || req->method() == drogon::Get) {
                    auto resp = tusResponse(200);
                    resp->addHeader("Upload-Offset", std::to_string(upload.offset));
                    resp->addHeader("Upload-Length", std::to_string(upload.totalSize));
                    // Proxies must never serve a stale offset, or a client resumes
                    // from the wrong place and corrupts the file.
                    resp->addHeader("Cache-Control", "no-store");
                    return resp;
                }

                if (req->method() == drogon::Delete) {
                    // tus termination: discard the upload now rather than leaving it to
                    // expire. Without this the client can stop sending, but the partial
                    // file occupies disk until its TTL elapses.
                    auto del = db.prepare("DELETE FROM uploads WHERE id = ?");
                    del.bind(1, id).run();

                    std::error_code ec;
                    fs::remove(storage::incomingPath(dataDir, id), ec);

                    // A completed upload already produced a blob. It is unreferenced
                    // unless a share took it, and the upload row that pinned it is now
                    // gone, so this is the moment to reclaim it.
                    const bool blobRemoved =
                        storage::deleteIfOrphaned(db, dataDir, upload.blobSha256);
                    LOG_INFO << "upload " << id << " terminated"
                             << (blobRemoved ? ", blob reclaimed" : "");
                    return tusResponse(204);
                }

                // PATCH
                if (upload.complete) {
                    throw HttpError{409, "upload is already complete"};
                }
                if (req->getHeader("Content-Type") != "application/offset+octet-stream") {
                    throw HttpError{415, "Content-Type must be application/offset+octet-stream"};
                }
                const std::int64_t clientOffset =
                    parseInt64(req->getHeader("Upload-Offset"), "Upload-Offset");
                if (clientOffset != upload.offset) {
                    // The whole point of tus: tell the client where we actually are
                    // rather than blindly appending and corrupting the file.
                    auto resp = tusError(409, "offset mismatch");
                    resp->addHeader("Upload-Offset", std::to_string(upload.offset));
                    return resp;
                }

                const std::string_view body = req->getBody();
                if (upload.offset + static_cast<std::int64_t>(body.size()) > upload.totalSize) {
                    throw HttpError{413, "chunk would exceed the declared Upload-Length"};
                }

                const fs::path path = storage::incomingPath(dataDir, id);
                const int fd = ::open(path.c_str(), O_WRONLY);
                if (fd < 0) {
                    throw std::runtime_error("cannot open upload file: " +
                                             std::string{std::strerror(errno)});
                }
                std::size_t written = 0;
                while (written < body.size()) {
                    const ssize_t n = ::pwrite(fd, body.data() + written, body.size() - written,
                                               upload.offset + static_cast<off_t>(written));
                    if (n <= 0) {
                        const int err = errno;
                        ::close(fd);
                        throw std::runtime_error("write failed: " +
                                                 std::string{std::strerror(err)});
                    }
                    written += static_cast<std::size_t>(n);
                }
                // Durability before the offset is recorded. If the database says bytes
                // are on disk, they must actually be on disk, or a resume after a crash
                // silently skips a hole.
                const bool syncFailed = (::fsync(fd) != 0);
                const int syncError = syncFailed ? errno : 0;
                ::close(fd);
                if (syncFailed) {
                    throw std::runtime_error("fsync failed: " +
                                             std::string{std::strerror(syncError)});
                }

                const std::int64_t newOffset =
                    upload.offset + static_cast<std::int64_t>(body.size());
                auto update = db.prepare("UPDATE uploads SET offset_bytes = ? WHERE id = ?");
                update.bind(1, newOffset).bind(2, id).run();

                auto resp = tusResponse(204);
                resp->addHeader("Upload-Offset", std::to_string(newOffset));

                if (newOffset == upload.totalSize) {
                    const std::string hash =
                        promoteToBlob(db, dataDir, id, upload.totalSize, user.id);
                    LOG_INFO << "upload " << id << " complete: " << hash << " ("
                             << upload.totalSize << " bytes)";
                    resp->addHeader("X-Archive-Blob", hash);
                }
                return resp;
            }));
        },
        {drogon::Get, drogon::Head, drogon::Patch, drogon::Delete});
}

}  // namespace archive
