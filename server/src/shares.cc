#include "shares.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "crypto.h"
#include "httputil.h"
#include "mimetype.h"
#include "storage.h"
#include "thumbnail.h"
#include "zipstream.h"

namespace archive {
namespace {

namespace fs = std::filesystem;

constexpr int kDefaultExpiryDays = 30;
constexpr int kMaxExpiryDays = 365;
constexpr int kMaxFilesPerShare = 500;

struct ShareRow {
    std::int64_t id = 0;
    std::int64_t ownerId = 0;
    std::string title;
    std::int64_t createdAt = 0;
    std::int64_t expiresAt = 0;
    std::string passwordHash;
    bool hasMaxDownloads = false;
    std::int64_t maxDownloads = 0;
    std::int64_t downloadCount = 0;
    std::string visibility = "private";
};

// Looks up a live share. Expired, revoked and never-existed are deliberately
// indistinguishable: a share token is a capability, and confirming that one used to
// exist leaks more than it helps.
ShareRow loadLiveShare(Database& db, const std::string& token) {
    auto stmt = db.prepare(
        "SELECT id, owner_id, COALESCE(title, ''), created_at, expires_at, "
        "COALESCE(password_hash, ''), max_downloads, download_count, visibility "
        "FROM shares WHERE token = ? AND deleted_at IS NULL AND expires_at > ?");
    stmt.bind(1, token).bind(2, nowSeconds());
    if (!stmt.step()) {
        throw HttpError{404, "this link has expired or does not exist"};
    }
    ShareRow row;
    row.id = stmt.columnInt(0);
    row.ownerId = stmt.columnInt(1);
    row.title = stmt.columnText(2);
    row.createdAt = stmt.columnInt(3);
    row.expiresAt = stmt.columnInt(4);
    row.passwordHash = stmt.columnText(5);
    row.hasMaxDownloads = !stmt.columnIsNull(6);
    row.maxDownloads = stmt.columnInt(6);
    row.downloadCount = stmt.columnInt(7);
    row.visibility = stmt.columnText(8);
    return row;
}

std::string suppliedPassword(const drogon::HttpRequestPtr& req) {
    const std::string header = req->getHeader("X-Share-Password");
    return header.empty() ? req->getParameter("p") : header;
}

// Resolves a share and authorises the requester: existence, then visibility, then the
// optional password. Every public entry point goes through here — a check that has to be
// remembered at five call sites is a check that will be forgotten at one of them.
ShareRow authoriseShare(Database& db, const Auth& auth, const std::string& token,
                        const drogon::HttpRequestPtr& req);

void requireSharePassword(const ShareRow& share, const drogon::HttpRequestPtr& req) {
    if (share.passwordHash.empty()) {
        return;
    }
    const std::string supplied = suppliedPassword(req);
    if (supplied.empty() || !crypto::verifyPassword(share.passwordHash, supplied)) {
        // 401 rather than 403: the client can usefully retry with a credential.
        throw HttpError{401, "this link requires a password"};
    }
}

ShareRow authoriseShare(Database& db, const Auth& auth, const std::string& token,
                        const drogon::HttpRequestPtr& req) {
    const ShareRow share = loadLiveShare(db, token);
    const auto viewer = auth.userForSession(req->getCookie(kSessionCookie));

    if (share.visibility != "public" && !viewer) {
        // 401, not 403: signing in genuinely resolves this, so the client should be told
        // to authenticate rather than that the door is permanently shut. The reason lets
        // the download page offer a sign-in prompt instead of a password box — both
        // cases are otherwise an indistinguishable 401.
        throw HttpError{401, "this link is only available to members", "members_only"};
    }

    // The owner set the password, and an admin has every other key already.
    const bool privileged =
        viewer && (viewer->id == share.ownerId || viewer->isAdmin());
    if (!privileged) {
        requireSharePassword(share, req);
    }
    return share;
}

// Containers a browser will play from a <video> element. Deliberately narrow: this
// decides what may be served without an attachment disposition.
bool isInlinePlayableVideo(const std::string& contentType) {
    return contentType == "video/mp4" || contentType == "video/webm" ||
           contentType == "video/quicktime";
}

// What /inline may serve. An allowlist rather than mime::isRiskyToRender's denylist,
// because this is the only endpoint that omits `attachment`: anything not named here is
// refused outright, so a type we failed to anticipate cannot execute in our origin.
bool isInlineSafe(const std::string& contentType) {
    static constexpr std::array<std::string_view, 6> kImages{
        "image/jpeg", "image/png", "image/gif", "image/webp", "image/avif", "image/bmp",
    };
    for (const std::string_view type : kImages) {
        if (contentType == type) {
            return true;
        }
    }
    return isInlinePlayableVideo(contentType);
}

// A file within a live share, resolved and password-checked. Shared by both preview
// endpoints, which differ only in what they then serve.
struct PreviewTarget {
    std::string hash;
    std::string filename;
    std::string contentType;
    int thumbState = 0;
};

Json::Value filesOf(Database& db, std::int64_t shareId) {
    Json::Value files{Json::arrayValue};
    auto stmt = db.prepare(
        "SELECT sf.id, sf.filename, sf.size, "
        "       COALESCE(b.content_type, sf.content_type, 'application/octet-stream'), "
        "       COALESCE(sf.relative_path, ''), sf.client_mtime, COALESCE(u.username, ''), "
        "       b.thumb "
        "FROM share_files sf "
        "JOIN blobs b ON b.sha256 = sf.blob_sha256 "
        "LEFT JOIN users u ON u.id = sf.uploaded_by "
        "WHERE sf.share_id = ? ORDER BY sf.id");
    stmt.bind(1, shareId);
    while (stmt.step()) {
        Json::Value file;
        file["id"] = static_cast<Json::Int64>(stmt.columnInt(0));
        file["filename"] = stmt.columnText(1);
        file["size"] = static_cast<Json::Int64>(stmt.columnInt(2));
        file["content_type"] = stmt.columnText(3);
        if (const std::string rel = stmt.columnText(4); !rel.empty()) {
            file["relative_path"] = rel;
        }
        if (!stmt.columnIsNull(5)) {
            file["client_mtime"] = static_cast<Json::Int64>(stmt.columnInt(5));
        }
        if (const std::string who = stmt.columnText(6); !who.empty()) {
            file["uploaded_by"] = who;
        }
        // A single field rather than making the client parse MIME types: "image" when a
        // preview exists or can still be made, "video" when the browser can play the
        // original inline, absent when there is nothing to show.
        //
        // thumb = 0 counts as previewable. A blob predating the column would otherwise
        // never be advertised, so the client would never request /thumb, so the lazy
        // backfill there could never fire — the preview would be unreachable forever.
        const int thumbState = static_cast<int>(stmt.columnInt(7));
        if (thumbState == 1 ||
            (thumbState == 0 && thumbnail::isThumbnailable(stmt.columnText(3)))) {
            file["preview"] = "image";
        } else if (isInlinePlayableVideo(stmt.columnText(3))) {
            file["preview"] = "video";
        }
        files.append(file);
    }
    return files;
}

// RFC 6266: the plain filename= is a fallback for ancient clients, filename*= carries
// the real UTF-8 name. Quotes and backslashes are stripped from the fallback so a
// crafted filename cannot break out of the quoted string and inject header directives.
std::string contentDisposition(const std::string& filename) {
    std::string fallback;
    for (const char c : filename) {
        fallback += (c == '"' || c == '\\' || static_cast<unsigned char>(c) < 0x20) ? '_' : c;
    }

    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string encoded;
    for (const unsigned char c : filename) {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '.' ||
                                c == '_' || c == '~';
        if (unreserved) {
            encoded += static_cast<char>(c);
        } else {
            encoded += '%';
            encoded += kHex[c >> 4];
            encoded += kHex[c & 0x0F];
        }
    }
    return "attachment; filename=\"" + fallback + "\"; filename*=UTF-8''" + encoded;
}

enum class RangeResult { None, Ok, Unsatisfiable };

struct RangeRequest {
    RangeResult result = RangeResult::None;
    std::size_t offset = 0;
    std::size_t length = 0;
};

// Single-range RFC 7233 parsing. Drogon's newFileResponse does not look at the Range
// header itself — its request parameter is only used to build a 404 — so the caller
// must work out the byte window and hand it explicit offset/length.
//
// Multi-range ("bytes=0-9,20-29") is legal but needs a multipart/byteranges body; a
// server is permitted to ignore it and return the whole representation, which is what
// falling through to None does here.
RangeRequest parseRange(const std::string& header, std::size_t fileSize) {
    RangeRequest out;
    constexpr std::string_view kUnit = "bytes=";
    if (header.rfind(kUnit, 0) != 0) {
        return out;
    }
    const std::string spec = header.substr(kUnit.size());
    if (spec.find(',') != std::string::npos) {
        return out;
    }
    const std::size_t dash = spec.find('-');
    if (dash == std::string::npos) {
        return out;
    }

    const std::string first = spec.substr(0, dash);
    const std::string last = spec.substr(dash + 1);
    try {
        if (first.empty()) {
            // Suffix form: "bytes=-500" means the final 500 bytes.
            if (last.empty() || fileSize == 0) {
                return out;
            }
            const std::size_t wanted = std::stoull(last);
            if (wanted == 0) {
                out.result = RangeResult::Unsatisfiable;
                return out;
            }
            out.length = std::min(wanted, fileSize);
            out.offset = fileSize - out.length;
        } else {
            const std::size_t start = std::stoull(first);
            if (start >= fileSize) {
                out.result = RangeResult::Unsatisfiable;
                return out;
            }
            std::size_t end = last.empty() ? fileSize - 1 : std::stoull(last);
            end = std::min(end, fileSize - 1);
            if (end < start) {
                out.result = RangeResult::Unsatisfiable;
                return out;
            }
            out.offset = start;
            out.length = end - start + 1;
        }
    } catch (const std::exception&) {
        return out;  // malformed: RFC says ignore the header entirely
    }
    out.result = RangeResult::Ok;
    return out;
}

// ZIP readers vary in how they handle duplicate paths, and a share can legitimately
// contain two files called IMG_1234.jpg from different phones. Disambiguate rather than
// let one silently overwrite the other on extraction.
std::string uniqueName(const std::string& wanted, std::set<std::string>& used) {
    if (used.insert(wanted).second) {
        return wanted;
    }
    const std::size_t dot = wanted.find_last_of('.');
    const std::string stem = dot == std::string::npos ? wanted : wanted.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : wanted.substr(dot);
    for (int n = 2; n < 10000; ++n) {
        std::string candidate = stem + " (" + std::to_string(n) + ")" + ext;
        if (used.insert(candidate).second) {
            return candidate;
        }
    }
    return wanted;  // absurd collision count; let the reader decide
}

// Blobs stored before the crc32 column existed have none. Compute it once and keep it,
// so this cost is paid at most a single time per blob.
std::uint32_t backfillCrc32(Database& db, const fs::path& path, const std::string& hash) {
    crypto::Crc32 crc;
    if (std::FILE* fp = std::fopen(path.c_str(), "rb")) {
        std::vector<char> buffer(1 << 20);
        std::size_t got = 0;
        while ((got = std::fread(buffer.data(), 1, buffer.size(), fp)) > 0) {
            crc.update(buffer.data(), got);
        }
        std::fclose(fp);
    }
    auto stmt = db.prepare("UPDATE blobs SET crc32 = ? WHERE sha256 = ?");
    stmt.bind(1, static_cast<std::int64_t>(crc.value())).bind(2, hash).run();
    return crc.value();
}

// Filename for the archive itself. Falls back to the token so it is never empty, and
// never carries a path separator into Content-Disposition.
std::string archiveName(const std::string& title, const std::string& token) {
    std::string base;
    for (const char c : title) {
        const bool safe = std::isalnum(static_cast<unsigned char>(c)) || c == ' ' ||
                          c == '-' || c == '_';
        base += safe ? c : '-';
    }
    while (!base.empty() && base.back() == ' ') {
        base.pop_back();
    }
    if (base.empty()) {
        base = "thearchive-" + token;
    }
    return base + ".zip";
}

int clampExpiryDays(const Json::Value& json) {
    if (!json.isMember("expires_days")) {
        return kDefaultExpiryDays;
    }
    if (!json["expires_days"].isIntegral()) {
        throw HttpError{400, "expires_days must be a number"};
    }
    const int days = json["expires_days"].asInt();
    if (days < 1 || days > kMaxExpiryDays) {
        throw HttpError{400, "expires_days must be between 1 and 365"};
    }
    return days;
}

}  // namespace

std::string publicBaseUrl() {
    const char* value = std::getenv("ARCHIVE_PUBLIC_URL");
    std::string base = (value != nullptr && *value != '\0') ? value : "http://localhost:8080";
    while (!base.empty() && base.back() == '/') {
        base.pop_back();
    }
    return base;
}

void registerShareRoutes(Database& db, Auth& auth, const fs::path& dataDir) {
    auto& app = drogon::app();

    // ---- create -----------------------------------------------------------------
    app.registerHandler(
        "/api/shares",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const User user = requireUser(req, auth);

                if (req->method() == drogon::Get) {
                    Json::Value out{Json::arrayValue};
                    auto stmt = db.prepare(
                        "SELECT s.token, COALESCE(s.title, ''), s.created_at, s.expires_at, "
                        "       s.download_count, s.max_downloads, "
                        "       s.password_hash IS NOT NULL, "
                        "       COUNT(sf.id), COALESCE(SUM(sf.size), 0), s.visibility "
                        "FROM shares s LEFT JOIN share_files sf ON sf.share_id = s.id "
                        "WHERE s.owner_id = ? AND s.deleted_at IS NULL "
                        "GROUP BY s.id ORDER BY s.created_at DESC");
                    stmt.bind(1, user.id);
                    while (stmt.step()) {
                        Json::Value share;
                        share["token"] = stmt.columnText(0);
                        share["url"] = publicBaseUrl() + "/d/" + stmt.columnText(0);
                        share["title"] = stmt.columnText(1);
                        share["created_at"] = static_cast<Json::Int64>(stmt.columnInt(2));
                        share["expires_at"] = static_cast<Json::Int64>(stmt.columnInt(3));
                        share["download_count"] = static_cast<Json::Int64>(stmt.columnInt(4));
                        if (!stmt.columnIsNull(5)) {
                            share["max_downloads"] =
                                static_cast<Json::Int64>(stmt.columnInt(5));
                        }
                        share["password_protected"] = stmt.columnInt(6) != 0;
                        share["visibility"] = stmt.columnText(9);
                        share["file_count"] = static_cast<Json::Int64>(stmt.columnInt(7));
                        share["total_bytes"] = static_cast<Json::Int64>(stmt.columnInt(8));
                        out.append(share);
                    }
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                std::shared_ptr<Json::Value> holder;
                const Json::Value& body = requireJson(req, holder);
                if (!body.isMember("uploads") || !body["uploads"].isArray() ||
                    body["uploads"].empty()) {
                    throw HttpError{400, "uploads must be a non-empty array of upload ids"};
                }
                if (body["uploads"].size() > kMaxFilesPerShare) {
                    throw HttpError{400, "too many files in one share"};
                }

                const int days = clampExpiryDays(body);
                const std::string title = optionalString(body, "title");
                const std::string password = optionalString(body, "password");
                const bool hasMaxDownloads =
                    body.isMember("max_downloads") && body["max_downloads"].isIntegral();
                const std::int64_t maxDownloads =
                    hasMaxDownloads ? body["max_downloads"].asInt64() : 0;
                if (hasMaxDownloads && maxDownloads < 1) {
                    throw HttpError{400, "max_downloads must be at least 1"};
                }

                // Private unless explicitly opted out of. Defaulting the other way
                // would mean a slip of attention publishes a link to the internet.
                const bool isPublic =
                    body.isMember("public") && body["public"].isBool() &&
                    body["public"].asBool();

                const std::int64_t now = nowSeconds();
                const std::string token = crypto::randomToken(16);
                const std::string passwordHash =
                    password.empty() ? std::string{} : crypto::hashPassword(password);

                // One transaction: refcounts and rows move together, or not at all.
                Transaction tx{db};

                auto insertShare = db.prepare(
                    "INSERT INTO shares (token, owner_id, title, created_at, expires_at, "
                    "password_hash, max_downloads, visibility) "
                    "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
                insertShare.bind(1, token).bind(2, user.id);
                title.empty() ? insertShare.bindNull(3) : insertShare.bind(3, title);
                insertShare.bind(4, now).bind(5, now + std::int64_t{days} * 86400);
                passwordHash.empty() ? insertShare.bindNull(6)
                                     : insertShare.bind(6, passwordHash);
                hasMaxDownloads ? insertShare.bind(7, maxDownloads) : insertShare.bindNull(7);
                insertShare.bind(8, std::string{isPublic ? "public" : "private"});
                insertShare.run();
                const std::int64_t shareId = db.lastInsertId();

                std::int64_t totalBytes = 0;
                for (const auto& entry : body["uploads"]) {
                    if (!entry.isString()) {
                        throw HttpError{400, "upload ids must be strings"};
                    }
                    const std::string uploadId = entry.asString();

                    auto find = db.prepare(
                        "SELECT blob_sha256, COALESCE(filename, 'download'), "
                        "       content_type, total_size, client_mtime, relative_path "
                        "FROM uploads WHERE id = ? AND owner_id = ? "
                        "AND completed_at IS NOT NULL");
                    find.bind(1, uploadId).bind(2, user.id);
                    if (!find.step()) {
                        throw HttpError{400, "upload '" + uploadId +
                                                 "' is unknown, unfinished, or not yours"};
                    }
                    const std::string hash = find.columnText(0);
                    const std::string filename = find.columnText(1);
                    const std::string declaredType = find.columnText(2);
                    const std::int64_t size = find.columnInt(3);
                    const bool hasMtime = !find.columnIsNull(4);
                    const std::int64_t mtime = find.columnInt(4);
                    const std::string relativePath = find.columnText(5);
                    totalBytes += size;

                    auto insertFile = db.prepare(
                        "INSERT INTO share_files (share_id, blob_sha256, filename, "
                        "content_type, size, uploaded_by, client_mtime, relative_path) "
                        "VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
                    insertFile.bind(1, shareId).bind(2, hash).bind(3, filename);
                    declaredType.empty() ? insertFile.bindNull(4)
                                         : insertFile.bind(4, declaredType);
                    insertFile.bind(5, size).bind(6, user.id);
                    hasMtime ? insertFile.bind(7, mtime) : insertFile.bindNull(7);
                    relativePath.empty() ? insertFile.bindNull(8)
                                         : insertFile.bind(8, relativePath);
                    insertFile.run();

                    auto bump = db.prepare(
                        "UPDATE blobs SET refcount = refcount + 1, last_referenced_at = ? "
                        "WHERE sha256 = ?");
                    bump.bind(1, now).bind(2, hash).run();

                    // Consuming the upload row makes a double-reference impossible: the
                    // same id cannot be spent on two shares and inflate the refcount.
                    auto consume = db.prepare("DELETE FROM uploads WHERE id = ?");
                    consume.bind(1, uploadId).run();
                }

                tx.commit();

                Json::Value out;
                out["token"] = token;
                out["url"] = publicBaseUrl() + "/d/" + token;
                out["expires_at"] = static_cast<Json::Int64>(now + std::int64_t{days} * 86400);
                out["file_count"] = static_cast<Json::Int64>(body["uploads"].size());
                out["total_bytes"] = static_cast<Json::Int64>(totalBytes);
                out["visibility"] = isPublic ? "public" : "private";
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                resp->setStatusCode(drogon::k201Created);
                LOG_INFO << "share " << token << " by " << user.username << ": "
                         << body["uploads"].size() << " file(s), " << totalBytes
                         << " bytes, " << days << "d, "
                         << (isPublic ? "public" : "private");
                return resp;
            }));
        },
        {drogon::Get, drogon::Post});

    // ---- revoke -----------------------------------------------------------------
    app.registerHandler(
        "/api/shares/{token}",
        [&db, &auth](const drogon::HttpRequestPtr& req,
                     std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                     const std::string& token) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                if (req->method() == drogon::Delete) {
                    const User user = requireUser(req, auth);
                    // Soft delete: the row stays so the GC can release its blob
                    // references on its next pass, in one place rather than two.
                    auto stmt = db.prepare(
                        "UPDATE shares SET deleted_at = ? WHERE token = ? AND owner_id = ? "
                        "AND deleted_at IS NULL");
                    stmt.bind(1, nowSeconds()).bind(2, token).bind(3, user.id).run();
                    if (db.changes() == 0) {
                        throw HttpError{404, "no such share"};
                    }
                    Json::Value out;
                    out["revoked"] = true;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                if (req->method() == drogon::Patch) {
                    const User user = requireUser(req, auth);

                    std::shared_ptr<Json::Value> holder;
                    const std::string wanted =
                        requireString(requireJson(req, holder), "visibility");
                    // Validated rather than stored as given: authoriseShare treats
                    // anything that is not exactly "public" as private, so a typo would
                    // fail closed — safely, but silently, and the owner would believe
                    // they had published something they had not.
                    if (wanted != "private" && wanted != "public") {
                        throw HttpError{400, "visibility must be private or public"};
                    }

                    // Owner-scoped, as with the delete above: the WHERE clause is the
                    // authorisation, and one 404 covers both "no such share" and "not
                    // yours" rather than confirming someone else's token exists.
                    auto stmt = db.prepare(
                        "UPDATE shares SET visibility = ? WHERE token = ? "
                        "AND owner_id = ? AND deleted_at IS NULL");
                    stmt.bind(1, wanted).bind(2, token).bind(3, user.id).run();
                    if (db.changes() == 0) {
                        throw HttpError{404, "no such share"};
                    }
                    Json::Value out;
                    out["visibility"] = wanted;
                    return drogon::HttpResponse::newHttpJsonResponse(out);
                }

                const ShareRow share = authoriseShare(db, auth, token, req);
                const auto viewer = auth.userForSession(req->getCookie(kSessionCookie));
                const bool isOwner = viewer && viewer->id == share.ownerId;

                Json::Value out;
                out["token"] = token;
                out["is_owner"] = isOwner;
                out["title"] = share.title;
                out["created_at"] = static_cast<Json::Int64>(share.createdAt);
                out["expires_at"] = static_cast<Json::Int64>(share.expiresAt);
                out["download_count"] = static_cast<Json::Int64>(share.downloadCount);
                out["visibility"] = share.visibility;
                if (share.hasMaxDownloads) {
                    out["max_downloads"] = static_cast<Json::Int64>(share.maxDownloads);
                }
                out["files"] = filesOf(db, share.id);
                auto resp = drogon::HttpResponse::newHttpJsonResponse(out);
                // A share link is a capability; caches must not retain its contents.
                resp->addHeader("Cache-Control", "no-store");
                return resp;
            }));
        },
        {drogon::Get, drogon::Delete, drogon::Patch});

    // ---- remove one file from a share -------------------------------------------
    app.registerHandler(
        "/api/shares/{token}/files/{fileId}",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                              std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                              const std::string& token, const std::string& fileId) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const User user = requireUser(req, auth);
                const ShareRow share = loadLiveShare(db, token);
                if (share.ownerId != user.id) {
                    throw HttpError{404, "no such share"};
                }

                std::int64_t wanted = 0;
                try {
                    wanted = std::stoll(fileId);
                } catch (const std::exception&) {
                    throw HttpError{404, "no such file in this share"};
                }

                std::string hash;
                {
                    auto find = db.prepare(
                        "SELECT blob_sha256 FROM share_files WHERE id = ? AND share_id = ?");
                    find.bind(1, wanted).bind(2, share.id);
                    if (!find.step()) {
                        throw HttpError{404, "no such file in this share"};
                    }
                    hash = find.columnText(0);
                }

                std::int64_t remaining = 0;
                bool revoked = false;
                {
                    Transaction tx{db};
                    auto del = db.prepare(
                        "DELETE FROM share_files WHERE id = ? AND share_id = ?");
                    del.bind(1, wanted).bind(2, share.id).run();

                    // released_at, as in the sweep, marks the blob as having been through
                    // a share so it is no longer held by the grace period.
                    auto decrement = db.prepare(
                        "UPDATE blobs SET refcount = MAX(0, refcount - 1), released_at = ? "
                        "WHERE sha256 = ?");
                    decrement.bind(1, nowSeconds()).bind(2, hash).run();

                    auto count = db.prepare(
                        "SELECT COUNT(*) FROM share_files WHERE share_id = ?");
                    count.bind(1, share.id);
                    remaining = count.step() ? count.columnInt(0) : 0;

                    // A share with no files is a link that resolves to an empty list.
                    // Revoking it is less confusing than leaving it reachable.
                    if (remaining == 0) {
                        auto revoke = db.prepare(
                            "UPDATE shares SET deleted_at = ? WHERE id = ?");
                        revoke.bind(1, nowSeconds()).bind(2, share.id).run();
                        revoked = true;
                    }
                    tx.commit();
                }

                const bool reclaimed = storage::deleteIfOrphaned(db, dataDir, hash);

                Json::Value out;
                out["removed"] = true;
                out["files_remaining"] = static_cast<Json::Int64>(remaining);
                out["share_revoked"] = revoked;
                out["blob_reclaimed"] = reclaimed;
                return drogon::HttpResponse::newHttpJsonResponse(out);
            }));
        },
        {drogon::Delete});

    // ---- previews ----------------------------------------------------------------
    //
    // Neither of these touches download_count. Browsing a gallery of forty photos would
    // otherwise exhaust a share's max_downloads before the recipient had downloaded
    // anything — the same reasoning that already exempts ranged requests.
    const auto resolvePreview = [&db, &auth](const std::string& token,
                                             const std::string& fileId,
                                             const drogon::HttpRequestPtr& req) {
        const ShareRow share = authoriseShare(db, auth, token, req);

        std::int64_t wanted = 0;
        try {
            wanted = std::stoll(fileId);
        } catch (const std::exception&) {
            throw HttpError{404, "no such file in this share"};
        }

        auto stmt = db.prepare(
            "SELECT sf.blob_sha256, sf.filename, "
            "       COALESCE(b.content_type, 'application/octet-stream'), b.thumb "
            "FROM share_files sf JOIN blobs b ON b.sha256 = sf.blob_sha256 "
            "WHERE sf.id = ? AND sf.share_id = ?");
        stmt.bind(1, wanted).bind(2, share.id);
        if (!stmt.step()) {
            throw HttpError{404, "no such file in this share"};
        }
        return PreviewTarget{stmt.columnText(0), stmt.columnText(1), stmt.columnText(2),
                             static_cast<int>(stmt.columnInt(3))};
    };

    app.registerHandler(
        "/d/{token}/{fileId}/thumb",
        [&db, dataDir, resolvePreview](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            const std::string& token, const std::string& fileId) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                PreviewTarget target = resolvePreview(token, fileId, req);

                // Blobs stored before previews existed have thumb = 0. Render on first
                // request, as the crc32 column is backfilled, so old shares gain
                // previews without a migration pass over the whole store.
                if (target.thumbState == 0 && thumbnail::isThumbnailable(target.contentType)) {
                    const bool rendered = thumbnail::generate(
                        storage::blobPath(dataDir, target.hash), dataDir, target.hash);
                    auto mark = db.prepare("UPDATE blobs SET thumb = ? WHERE sha256 = ?");
                    mark.bind(1, static_cast<std::int64_t>(rendered ? 1 : 2))
                        .bind(2, target.hash)
                        .run();
                    target.thumbState = rendered ? 1 : 2;
                }
                if (target.thumbState != 1) {
                    throw HttpError{404, "no preview for this file"};
                }

                const bool large = req->getParameter("s") != "sm";
                const fs::path path = thumbnail::path(dataDir, target.hash, large);
                if (!fs::exists(path)) {
                    throw HttpError{404, "no preview for this file"};
                }

                auto resp = drogon::HttpResponse::newFileResponse(
                    path.string(), "", drogon::CT_CUSTOM, "image/webp", req);
                resp->addHeader("Content-Disposition", "inline");
                resp->addHeader("X-Content-Type-Options", "nosniff");
                // Content-addressed and immutable, but reachable only via the share
                // token, so private rather than public.
                resp->addHeader("Cache-Control", "private, max-age=3600");
                return resp;
            }));
        },
        {drogon::Get, drogon::Head});

    app.registerHandler(
        "/d/{token}/{fileId}/inline",
        [dataDir, resolvePreview](
            const drogon::HttpRequestPtr& req,
            std::function<void(const drogon::HttpResponsePtr&)>&& callback,
            const std::string& token, const std::string& fileId) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const PreviewTarget target = resolvePreview(token, fileId, req);
                if (!isInlineSafe(target.contentType)) {
                    throw HttpError{415, "this file cannot be previewed inline"};
                }

                const fs::path path = storage::blobPath(dataDir, target.hash);
                std::error_code ec;
                const std::size_t fileSize = fs::file_size(path, ec);
                if (ec) {
                    throw HttpError{404, "file is no longer available"};
                }

                // Range support matters here specifically: it is what lets a <video>
                // element seek without refetching from the start.
                const RangeRequest range = parseRange(req->getHeader("Range"), fileSize);
                if (range.result == RangeResult::Unsatisfiable) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k416RequestedRangeNotSatisfiable);
                    resp->addHeader("Content-Range", "bytes */" + std::to_string(fileSize));
                    return resp;
                }

                const bool ranged = (range.result == RangeResult::Ok);
                auto resp = drogon::HttpResponse::newFileResponse(
                    path.string(), ranged ? range.offset : 0, ranged ? range.length : 0,
                    /*setContentRange=*/ranged, "", drogon::CT_CUSTOM, target.contentType,
                    req);
                resp->addHeader("Content-Disposition", "inline");
                resp->addHeader("X-Content-Type-Options", "nosniff");
                resp->addHeader("Accept-Ranges", "bytes");
                resp->addHeader("Cache-Control", "private, max-age=3600");
                return resp;
            }));
        },
        {drogon::Get, drogon::Head});

    // ---- download everything as one archive --------------------------------------
    app.registerHandler(
        "/d/{token}/all.zip",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::string& token) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const ShareRow share = authoriseShare(db, auth, token, req);

                std::vector<zip::Entry> entries;
                std::set<std::string> used;
                {
                    auto stmt = db.prepare(
                        "SELECT sf.blob_sha256, sf.filename, sf.size, sf.client_mtime, "
                        "       COALESCE(sf.relative_path, ''), b.crc32, b.created_at "
                        "FROM share_files sf JOIN blobs b ON b.sha256 = sf.blob_sha256 "
                        "WHERE sf.share_id = ? ORDER BY sf.id");
                    stmt.bind(1, share.id);
                    while (stmt.step()) {
                        zip::Entry entry;
                        const std::string hash = stmt.columnText(0);
                        const std::string relative = stmt.columnText(4);
                        entry.name = uniqueName(
                            relative.empty() ? stmt.columnText(1) : relative, used);
                        entry.source = storage::blobPath(dataDir, hash);
                        entry.size = static_cast<std::uint64_t>(stmt.columnInt(2));
                        entry.mtime = stmt.columnIsNull(3) ? stmt.columnInt(6)
                                                           : stmt.columnInt(3);
                        entry.crc32 =
                            stmt.columnIsNull(5)
                                ? backfillCrc32(db, entry.source, hash)
                                : static_cast<std::uint32_t>(stmt.columnInt(5));
                        entries.push_back(std::move(entry));
                    }
                }
                if (entries.empty()) {
                    throw HttpError{404, "this link has no files"};
                }

                // One archive counts as one download, however many files it holds.
                auto count = db.prepare(
                    "UPDATE shares SET download_count = download_count + 1 WHERE id = ? "
                    "AND (max_downloads IS NULL OR download_count < max_downloads)");
                count.bind(1, share.id).run();
                if (db.changes() == 0) {
                    throw HttpError{410, "this link has reached its download limit"};
                }

                auto streamer = std::make_shared<zip::Streamer>(std::move(entries));
                const std::uint64_t total = streamer->totalSize();

                auto resp = drogon::HttpResponse::newStreamResponse(
                    [streamer](char* buffer, std::size_t length) -> std::size_t {
                        // Drogon passes nullptr once the send has finished or been
                        // interrupted, purely so we can release our state.
                        return buffer == nullptr ? 0 : streamer->read(buffer, length);
                    },
                    "", drogon::CT_CUSTOM, "application/zip");

                // Known up front because the archive is stored, not deflated. This is
                // what turns the browser's indeterminate spinner into a real progress
                // bar with an ETA.
                resp->addHeader("Content-Length", std::to_string(total));
                resp->addHeader("Content-Disposition",
                                contentDisposition(archiveName(share.title, token)));
                resp->addHeader("X-Content-Type-Options", "nosniff");
                resp->addHeader("Cache-Control", "private, no-store");
                LOG_INFO << "zip stream for share " << token << ": " << total << " bytes";
                return resp;
            }));
        },
        {drogon::Get, drogon::Head});

    // ---- download ---------------------------------------------------------------
    app.registerHandler(
        "/d/{token}/{fileId}",
        [&db, &auth, dataDir](const drogon::HttpRequestPtr& req,
                       std::function<void(const drogon::HttpResponsePtr&)>&& callback,
                       const std::string& token, const std::string& fileId) {
            callback(guarded([&]() -> drogon::HttpResponsePtr {
                const ShareRow share = authoriseShare(db, auth, token, req);

                std::int64_t wanted = 0;
                try {
                    wanted = std::stoll(fileId);
                } catch (const std::exception&) {
                    throw HttpError{404, "no such file in this share"};
                }

                auto stmt = db.prepare(
                    "SELECT sf.blob_sha256, sf.filename, sf.size, "
                    "       COALESCE(b.content_type, 'application/octet-stream') "
                    "FROM share_files sf JOIN blobs b ON b.sha256 = sf.blob_sha256 "
                    "WHERE sf.id = ? AND sf.share_id = ?");
                stmt.bind(1, wanted).bind(2, share.id);
                if (!stmt.step()) {
                    throw HttpError{404, "no such file in this share"};
                }
                const std::string hash = stmt.columnText(0);
                const std::string filename = stmt.columnText(1);
                std::string contentType = stmt.columnText(3);

                const fs::path path = storage::blobPath(dataDir, hash);
                std::error_code ec;
                const std::size_t fileSize = fs::file_size(path, ec);
                if (ec) {
                    LOG_ERROR << "blob missing for share " << token << ": " << hash;
                    throw HttpError{500, "file is no longer available"};
                }

                const RangeRequest range = parseRange(req->getHeader("Range"), fileSize);
                if (range.result == RangeResult::Unsatisfiable) {
                    auto resp = drogon::HttpResponse::newHttpResponse();
                    resp->setStatusCode(drogon::k416RequestedRangeNotSatisfiable);
                    resp->addHeader("Content-Range", "bytes */" + std::to_string(fileSize));
                    return resp;
                }

                // Only a request that starts at byte zero counts as a download. A video
                // player seeking, or a client resuming an interrupted transfer, issues
                // many ranged requests for one logical download and would otherwise burn
                // through max_downloads in seconds.
                //
                // This makes max_downloads a courtesy limit rather than a hard control:
                // a determined client could request from byte 1 onwards. That is an
                // acceptable trade for links shared among friends.
                const bool counts =
                    (range.result == RangeResult::None) || (range.offset == 0);
                if (counts) {
                    // Condition and increment in one statement: two concurrent downloads
                    // of the last permitted copy would otherwise both read the old count
                    // and both succeed.
                    auto count = db.prepare(
                        "UPDATE shares SET download_count = download_count + 1 WHERE id = ? "
                        "AND (max_downloads IS NULL OR download_count < max_downloads)");
                    count.bind(1, share.id).run();
                    if (db.changes() == 0) {
                        throw HttpError{410, "this link has reached its download limit"};
                    }
                } else if (share.hasMaxDownloads &&
                           share.downloadCount >= share.maxDownloads) {
                    throw HttpError{410, "this link has reached its download limit"};
                }

                // Anything a browser might execute in our origin is downgraded. Combined
                // with the unconditional attachment disposition and nosniff below, an
                // uploaded HTML or SVG file cannot become stored XSS.
                if (mime::isRiskyToRender(contentType)) {
                    contentType = "application/octet-stream";
                }

                const bool ranged = (range.result == RangeResult::Ok);
                auto resp = drogon::HttpResponse::newFileResponse(
                    path.string(), ranged ? range.offset : 0, ranged ? range.length : 0,
                    /*setContentRange=*/ranged, "", drogon::CT_CUSTOM, contentType, req);
                resp->addHeader("Content-Disposition", contentDisposition(filename));
                resp->addHeader("X-Content-Type-Options", "nosniff");
                resp->addHeader("Accept-Ranges", "bytes");
                resp->addHeader("Cache-Control", "private, no-store");
                return resp;
            }));
        },
        {drogon::Get, drogon::Head});
}

}  // namespace archive
