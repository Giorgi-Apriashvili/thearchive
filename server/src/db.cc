#include "db.h"

#include <array>
#include <stdexcept>

namespace archive {
namespace {

struct Migration {
    int version;
    const char* sql;
};

// Times are unix epoch seconds. Sizes are bytes.
constexpr const char* kSchemaV1 = R"SQL(
CREATE TABLE users (
    id            INTEGER PRIMARY KEY,
    username      TEXT    NOT NULL UNIQUE,
    password_hash TEXT    NOT NULL,          -- argon2id
    created_at    INTEGER NOT NULL,
    quota_bytes   INTEGER NOT NULL DEFAULT 53687091200,  -- 50 GiB
    is_admin      INTEGER NOT NULL DEFAULT 0
);

-- Registration is invite-only. An open signup form on a public IP becomes a
-- phishing host within days; see docs/DESIGN.md.
CREATE TABLE invites (
    code       TEXT    PRIMARY KEY,
    created_by INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    used_by    INTEGER          REFERENCES users(id) ON DELETE SET NULL,
    created_at INTEGER NOT NULL,
    expires_at INTEGER
);

CREATE TABLE sessions (
    token      TEXT    PRIMARY KEY,          -- 256 random bits, base64url
    user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    created_at INTEGER NOT NULL,
    expires_at INTEGER NOT NULL
);
CREATE INDEX idx_sessions_expires ON sessions(expires_at);

-- Content-addressed storage. Four people uploading the same video from the same
-- night store it once; refcount tracks how many live shares point at it.
CREATE TABLE blobs (
    sha256     TEXT    PRIMARY KEY,
    size       INTEGER NOT NULL,
    refcount   INTEGER NOT NULL DEFAULT 0,
    created_at INTEGER NOT NULL
);
-- Partial index: the GC sweep only ever looks for unreferenced blobs.
CREATE INDEX idx_blobs_orphaned ON blobs(sha256) WHERE refcount = 0;

CREATE TABLE shares (
    id             INTEGER PRIMARY KEY,
    token          TEXT    NOT NULL UNIQUE,  -- 128 random bits, base64url
    owner_id       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    title          TEXT,
    created_at     INTEGER NOT NULL,
    expires_at     INTEGER NOT NULL,
    password_hash  TEXT,                     -- optional, argon2id
    max_downloads  INTEGER,                  -- optional cap
    download_count INTEGER NOT NULL DEFAULT 0,
    deleted_at     INTEGER
);
CREATE INDEX idx_shares_expires ON shares(expires_at) WHERE deleted_at IS NULL;
CREATE INDEX idx_shares_owner   ON shares(owner_id);

CREATE TABLE share_files (
    id           INTEGER PRIMARY KEY,
    share_id     INTEGER NOT NULL REFERENCES shares(id) ON DELETE CASCADE,
    blob_sha256  TEXT    NOT NULL REFERENCES blobs(sha256),
    filename     TEXT    NOT NULL,
    content_type TEXT,
    size         INTEGER NOT NULL
);
CREATE INDEX idx_share_files_share ON share_files(share_id);

-- In-progress tus uploads. offset_bytes is what a HEAD returns as Upload-Offset,
-- so a client that lost its connection knows where to resume.
CREATE TABLE uploads (
    id           TEXT    PRIMARY KEY,
    owner_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    filename     TEXT,
    content_type TEXT,
    total_size   INTEGER NOT NULL,
    offset_bytes INTEGER NOT NULL DEFAULT 0,
    created_at   INTEGER NOT NULL,
    expires_at   INTEGER NOT NULL
);
CREATE INDEX idx_uploads_expires ON uploads(expires_at);
)SQL";

// Records where a finished upload landed, so share creation can reference completed
// uploads by id without the client being trusted to supply a content hash.
constexpr const char* kSchemaV2 = R"SQL(
ALTER TABLE uploads ADD COLUMN blob_sha256 TEXT REFERENCES blobs(sha256);
ALTER TABLE uploads ADD COLUMN completed_at INTEGER;
)SQL";

// Provenance and identity metadata, captured now because it is only available at upload
// time. A future archival mode would need to know who contributed a file, when the file
// itself was created (not when it was uploaded), and what it actually is — none of which
// can be recovered later from bytes on disk.
//
// `pinned` is the hook that makes such a mode possible without reworking expiry: a
// pinned blob is never collected, however many shares referencing it have lapsed.
constexpr const char* kSchemaV3 = R"SQL(
-- Content-level facts: true of the bytes regardless of who shared them, and therefore
-- worth surviving the expiry of every share that pointed at them.
ALTER TABLE blobs ADD COLUMN content_type TEXT;          -- sniffed from magic bytes
ALTER TABLE blobs ADD COLUMN first_uploader INTEGER REFERENCES users(id);
ALTER TABLE blobs ADD COLUMN last_referenced_at INTEGER;
ALTER TABLE blobs ADD COLUMN pinned INTEGER NOT NULL DEFAULT 0;

-- Contribution-level facts: which person put this file into this particular share,
-- under what name, and what the file's own timestamp was on their device.
ALTER TABLE share_files ADD COLUMN uploaded_by INTEGER REFERENCES users(id);
ALTER TABLE share_files ADD COLUMN client_mtime INTEGER;
ALTER TABLE share_files ADD COLUMN relative_path TEXT;

ALTER TABLE uploads ADD COLUMN client_mtime INTEGER;
ALTER TABLE uploads ADD COLUMN relative_path TEXT;
ALTER TABLE uploads ADD COLUMN user_agent TEXT;

CREATE INDEX idx_blobs_pinned ON blobs(pinned) WHERE pinned = 1;
)SQL";

// Marks that a lapsed share has already given up its blob references. Without it the
// sweep cannot tell "expired" from "expired and already accounted for", and a second
// pass would decrement every refcount again — quietly deleting live blobs.
constexpr const char* kSchemaV4 = R"SQL(
ALTER TABLE shares ADD COLUMN released_at INTEGER;
CREATE INDEX idx_shares_unreleased ON shares(expires_at) WHERE released_at IS NULL;
)SQL";

// Distinguishes "never yet shared" from "shared, and since released".
//
// The grace period exists only to protect the first case: a completed upload waiting to
// be put into a share. A blob that has already been in a share is past that window by
// definition, so once nothing references it there is no reason to keep it for another
// day. Without this column the sweep cannot tell the two apart.
//
// last_referenced_at cannot serve here: it is stamped at upload time too, so it is
// non-NULL for blobs that have never belonged to any share.
constexpr const char* kSchemaV5 = R"SQL(
ALTER TABLE blobs ADD COLUMN released_at INTEGER;
)SQL";

// ZIP entries carry a CRC-32 in their local header, ahead of the data. A streaming
// archive therefore needs it before it has read the file, so it is computed during
// upload in the same pass as the SHA-256 and kept here. Null for blobs that predate this
// column; those are backfilled on first use.
constexpr const char* kSchemaV6 = R"SQL(
ALTER TABLE blobs ADD COLUMN crc32 INTEGER;
)SQL";

// Whether a preview has been rendered for this blob. Recorded rather than inferred
// from the filesystem so the share listing does not stat two files per row, and so
// "not an image" stays distinguishable from "an image we failed to render".
constexpr const char* kSchemaV7 = R"SQL(
ALTER TABLE blobs ADD COLUMN thumb INTEGER NOT NULL DEFAULT 0;  -- 0 none, 1 ready, 2 failed
)SQL";

// Roles replace the is_admin boolean. `privileged` currently behaves exactly like
// `user`; it exists so a future tier can be granted without another migration over live
// data. The old column is dropped rather than left in place: two representations of the
// same fact is precisely how they drift apart.
constexpr const char* kSchemaV8 = R"SQL(
ALTER TABLE users ADD COLUMN role TEXT NOT NULL DEFAULT 'user';
UPDATE users SET role = 'admin' WHERE is_admin = 1;
ALTER TABLE users DROP COLUMN is_admin;

-- Disabling blocks sign-in and drops live sessions while leaving shares and uploads
-- intact, so a contribution stays attributed to whoever made it.
ALTER TABLE users ADD COLUMN disabled_at INTEGER;
)SQL";

// Who may open a link. New shares default to `private` — only signed-in members — and
// the column default reflects that.
//
// Existing rows are explicitly set `public`: they were created under link-is-enough
// semantics and handed to people who may have no account. Silently tightening them
// would break links already in circulation, which is not a migration's job.
constexpr const char* kSchemaV9 = R"SQL(
ALTER TABLE shares ADD COLUMN visibility TEXT NOT NULL DEFAULT 'private';
UPDATE shares SET visibility = 'public';
)SQL";

constexpr std::array<Migration, 9> kMigrations{{
    {1, kSchemaV1},
    {2, kSchemaV2},
    {3, kSchemaV3},
    {4, kSchemaV4},
    {5, kSchemaV5},
    {6, kSchemaV6},
    {7, kSchemaV7},
    {8, kSchemaV8},
    {9, kSchemaV9},
}};

}  // namespace

Stmt::Stmt(sqlite3* db, const std::string& sql) : db_(db) {
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt_, nullptr) != SQLITE_OK) {
        throw std::runtime_error("cannot prepare '" + sql + "': " + sqlite3_errmsg(db_));
    }
}

Stmt::~Stmt() {
    sqlite3_finalize(stmt_);
}

Stmt::Stmt(Stmt&& other) noexcept : db_(other.db_), stmt_(other.stmt_) {
    other.stmt_ = nullptr;
}

Stmt& Stmt::bind(int index, std::int64_t value) {
    if (sqlite3_bind_int64(stmt_, index, value) != SQLITE_OK) {
        throw std::runtime_error(std::string{"bind failed: "} + sqlite3_errmsg(db_));
    }
    return *this;
}

Stmt& Stmt::bind(int index, const std::string& value) {
    // SQLITE_TRANSIENT: SQLite copies the value, so callers may pass temporaries.
    if (sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        throw std::runtime_error(std::string{"bind failed: "} + sqlite3_errmsg(db_));
    }
    return *this;
}

Stmt& Stmt::bindNull(int index) {
    if (sqlite3_bind_null(stmt_, index) != SQLITE_OK) {
        throw std::runtime_error(std::string{"bind failed: "} + sqlite3_errmsg(db_));
    }
    return *this;
}

bool Stmt::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw std::runtime_error(std::string{"step failed: "} + sqlite3_errmsg(db_));
}

void Stmt::run() {
    step();
}

std::int64_t Stmt::columnInt(int index) const {
    return sqlite3_column_int64(stmt_, index);
}

std::string Stmt::columnText(int index) const {
    const auto* text = sqlite3_column_text(stmt_, index);
    if (text == nullptr) {
        return {};
    }
    return std::string{reinterpret_cast<const char*>(text),
                       static_cast<std::size_t>(sqlite3_column_bytes(stmt_, index))};
}

bool Stmt::columnIsNull(int index) const {
    return sqlite3_column_type(stmt_, index) == SQLITE_NULL;
}

std::int64_t Database::lastInsertId() const {
    return sqlite3_last_insert_rowid(db_);
}

int Database::changes() const {
    return sqlite3_changes(db_);
}

// BEGIN IMMEDIATE rather than plain BEGIN: it takes the write lock up front, so a
// concurrent writer fails here instead of midway through, where SQLITE_BUSY on COMMIT
// would be far harder to recover from.
Transaction::Transaction(const Database& db) : db_(db) {
    db_.exec("BEGIN IMMEDIATE");
}

Transaction::~Transaction() {
    if (!finished_) {
        try {
            db_.exec("ROLLBACK");
        } catch (const std::exception&) {
            // Already unwinding in the common case; there is nothing useful to do and
            // throwing from a destructor would terminate.
        }
    }
}

void Transaction::commit() {
    db_.exec("COMMIT");
    finished_ = true;
}

Database::Database(const std::filesystem::path& file) {
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX;
    if (sqlite3_open_v2(file.c_str(), &db_, flags, nullptr) != SQLITE_OK) {
        const std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open " + file.string() + ": " + msg);
    }

    // WAL keeps readers from blocking behind the writer. busy_timeout covers the brief
    // contention when the expiry sweep and an upload commit land together.
    exec("PRAGMA journal_mode=WAL");
    exec("PRAGMA busy_timeout=5000");

    // Foreign keys are off by default in SQLite and are per-connection, so this cannot
    // be confirmed by inspecting the file later. The pragma is also a silent no-op
    // inside a transaction, which would leave enforcement off while orphaned rows
    // accumulated unnoticed — so read it back rather than assume it took.
    exec("PRAGMA foreign_keys=ON");
    if (queryInt("PRAGMA foreign_keys") != 1) {
        throw std::runtime_error("could not enable foreign key enforcement");
    }
    // NORMAL is the right durability trade under WAL: a power cut can lose the last
    // transaction, but the database cannot corrupt. The blobs themselves are fsynced.
    exec("PRAGMA synchronous=NORMAL");
}

Database::~Database() {
    if (db_) {
        sqlite3_close(db_);
    }
}

void Database::exec(const std::string& sql) const {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        const std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("sql failed: " + msg);
    }
}

int Database::queryInt(const std::string& sql) const {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
        throw std::runtime_error("cannot prepare '" + sql +
                                 "': " + sqlite3_errmsg(db_));
    }
    int value = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        value = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return value;
}

int Database::schemaVersion() const {
    return queryInt("PRAGMA user_version");
}

void Database::migrate() {
    const int current = schemaVersion();
    for (const auto& m : kMigrations) {
        if (m.version <= current) {
            continue;
        }
        exec("BEGIN IMMEDIATE");
        try {
            exec(m.sql);
            // PRAGMA user_version is transactional, so the bump commits atomically
            // with the DDL above it.
            exec("PRAGMA user_version=" + std::to_string(m.version));
            exec("COMMIT");
        } catch (...) {
            exec("ROLLBACK");
            throw;
        }
    }
}

}  // namespace archive
