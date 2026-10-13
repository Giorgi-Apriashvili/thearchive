#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "auth.h"
#include "db.h"

namespace archive::quota {

// Storage limits, per person. Each role has a default — user 20 GB, privileged 100 GB,
// admin none — and an administrator can set any one account's limit apart from its role
// (users.storage_limit: NULL follows the role, -1 means no limit, otherwise bytes).
// Promoting someone raises their limit with no further step.
//
// What counts is what you are keeping on the site: each distinct file in your links that
// still work and in your uploads not yet in a link, once however many of your links hold
// it, and an unfinished upload at its full declared size, since that much is spoken for.
// Links that expire or are removed give their space back.
//
// Checked when an upload is created, against its declared length, so a file that will
// not fit is refused before anything is sent rather than after an hour of sending.
inline constexpr std::int64_t kGiB = std::int64_t{1} << 30;

struct Limit {
    std::optional<std::int64_t> bytes;  // none: no limit
    bool custom = false;                // set for this account rather than by its role
};

std::optional<std::int64_t> roleDefault(const std::string& role);
Limit limitFor(Database& db, std::int64_t userId);
std::int64_t usage(Database& db, std::int64_t userId);

// Throws 413, saying how much is in use, when `adding` more bytes would take `user` over
// their limit.
void requireRoom(Database& db, const User& user, std::int64_t adding);

// "20 GB", "2.5 GB", "800 MB", "100 KB" — for messages a person reads.
std::string human(std::int64_t bytes);

}  // namespace archive::quota
