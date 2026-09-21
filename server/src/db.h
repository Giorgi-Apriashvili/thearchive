#pragma once

#include <sqlite3.h>

#include <cstdint>
#include <filesystem>
#include <string>

namespace archive {

// A prepared statement with bound parameters. Parameters are 1-indexed, matching
// SQLite's own convention, and bound values are copied so callers need not keep
// temporaries alive.
class Stmt {
public:
    Stmt(sqlite3* db, const std::string& sql);
    ~Stmt();

    Stmt(const Stmt&) = delete;
    Stmt& operator=(const Stmt&) = delete;
    Stmt(Stmt&& other) noexcept;

    Stmt& bind(int index, std::int64_t value);
    Stmt& bind(int index, const std::string& value);
    Stmt& bindNull(int index);

    // True when a row is available. Call repeatedly to iterate.
    bool step();
    // For statements returning no rows; throws if the statement fails.
    void run();

    std::int64_t columnInt(int index) const;
    std::string columnText(int index) const;
    bool columnIsNull(int index) const;

private:
    sqlite3* db_ = nullptr;
    sqlite3_stmt* stmt_ = nullptr;
};

// A single SQLite connection, opened in WAL mode and migrated to the current schema.
//
// SQLite rather than Postgres: this is one machine serving a handful of people. WAL
// gives concurrent readers alongside a single writer, which is far more than a friend
// group generates, and it removes an entire service from the deployment.
class Database {
public:
    explicit Database(const std::filesystem::path& file);
    ~Database();

    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    // Applies every migration newer than the stored user_version, each in its own
    // transaction. Throws on failure rather than continuing: a half-migrated schema
    // is not a state worth serving requests from.
    void migrate();

    int schemaVersion() const;

    Stmt prepare(const std::string& sql) const { return Stmt{db_, sql}; }
    std::int64_t lastInsertId() const;

    // Rows touched by the most recent statement. Used to make a guarded UPDATE both
    // check a condition and act on it atomically, rather than reading then writing.
    int changes() const;

    void exec(const std::string& sql) const;

    // The connection is opened SQLITE_OPEN_FULLMUTEX, so SQLite serialises concurrent
    // use across Drogon's event loop threads internally. That is the right trade at
    // this scale: correctness for free, and the contention is irrelevant for a handful
    // of users.
    sqlite3* handle() const { return db_; }

private:
    int queryInt(const std::string& sql) const;

    sqlite3* db_ = nullptr;
};

// RAII transaction: rolls back unless commit() is called. Creating a share bumps blob
// refcounts and inserts rows together, and an exception between the two would leave
// either an unreferenced blob awaiting collection or a share pointing at nothing.
class Transaction {
public:
    explicit Transaction(const Database& db);
    ~Transaction();

    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    void commit();

private:
    const Database& db_;
    bool finished_ = false;
};

}  // namespace archive
