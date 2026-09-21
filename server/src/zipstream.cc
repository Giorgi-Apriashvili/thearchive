#include "zipstream.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <ctime>

namespace archive::zip {
namespace {

constexpr std::uint32_t kLocalSig = 0x04034b50;
constexpr std::uint32_t kCentralSig = 0x02014b50;
constexpr std::uint32_t kEocdSig = 0x06054b50;
constexpr std::uint32_t kZip64EocdSig = 0x06064b50;
constexpr std::uint32_t kZip64LocatorSig = 0x07064b50;

// Above this a 32-bit field cannot hold the value and the Zip64 extra field is required.
constexpr std::uint64_t kMax32 = 0xFFFFFFFFull;
constexpr std::size_t kMaxEntries16 = 0xFFFF;

// Version 4.5 signals Zip64; 2.0 is the baseline for stored entries.
constexpr std::uint16_t kVersionZip64 = 45;
constexpr std::uint16_t kVersionBase = 20;

// Bit 11 declares the filename is UTF-8, which is the only sane reading of arbitrary
// names from phones. Without it, readers fall back to code page 437.
constexpr std::uint16_t kFlagUtf8 = 1 << 11;

void put16(std::string& out, std::uint16_t v) {
    out.push_back(static_cast<char>(v & 0xFF));
    out.push_back(static_cast<char>((v >> 8) & 0xFF));
}

void put32(std::string& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

void put64(std::string& out, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
    }
}

// MS-DOS packed date/time, which is all a ZIP header can carry.
std::pair<std::uint16_t, std::uint16_t> dosTime(std::int64_t unixSeconds) {
    std::time_t when = unixSeconds > 0 ? static_cast<std::time_t>(unixSeconds)
                                       : std::time(nullptr);
    std::tm tm{};
    gmtime_r(&when, &tm);

    // The format cannot represent anything before 1980.
    int year = tm.tm_year + 1900;
    if (year < 1980) {
        year = 1980;
        tm.tm_mon = 0;
        tm.tm_mday = 1;
        tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
    }
    const auto date = static_cast<std::uint16_t>(((year - 1980) << 9) |
                                                 ((tm.tm_mon + 1) << 5) | tm.tm_mday);
    const auto time = static_cast<std::uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) |
                                                 (tm.tm_sec / 2));
    return {date, time};
}

bool needsZip64Local(const Entry& e) {
    return e.size > kMax32;
}

std::uint16_t localExtraLen(const Entry& e) {
    return needsZip64Local(e) ? 20 : 0;  // header id + size + 2 × 8-byte fields
}

// The central record's Zip64 extra carries only the fields that overflow, in a fixed
// order: uncompressed, compressed, then local header offset.
std::uint16_t centralExtraLen(const Entry& e, std::uint64_t offset) {
    std::uint16_t payload = 0;
    if (e.size > kMax32) payload += 16;  // uncompressed + compressed
    if (offset > kMax32) payload += 8;
    return payload == 0 ? 0 : static_cast<std::uint16_t>(4 + payload);
}

}  // namespace

Streamer::Streamer(std::vector<Entry> entries) : entries_(std::move(entries)) {
    // A single forward pass fixes every offset, and therefore the exact output size,
    // before any byte is produced. Each entry's local header length depends only on its
    // own size, and its offset only on the entries before it.
    offsets_.reserve(entries_.size());
    std::uint64_t offset = 0;
    std::uint64_t centralSize = 0;

    for (const Entry& e : entries_) {
        offsets_.push_back(offset);
        offset += 30 + e.name.size() + localExtraLen(e) + e.size;
        centralSize += 46 + e.name.size() + centralExtraLen(e, offsets_.back());
    }

    const bool zip64Trailer = offset > kMax32 || centralSize > kMax32 ||
                              entries_.size() > kMaxEntries16;

    totalSize_ = offset + centralSize + (zip64Trailer ? 56 + 20 : 0) + 22;
}

void Streamer::openCurrentFile() {
    const Entry& e = entries_[index_];
    fd_ = ::open(e.source.c_str(), O_RDONLY);
    fileRemaining_ = e.size;
}

void Streamer::beginEntry() {
    const Entry& e = entries_[index_];
    const auto [date, time] = dosTime(e.mtime);
    const bool zip64 = needsZip64Local(e);

    pending_.clear();
    pendingPos_ = 0;

    put32(pending_, kLocalSig);
    put16(pending_, zip64 ? kVersionZip64 : kVersionBase);
    put16(pending_, kFlagUtf8);
    put16(pending_, 0);  // stored
    put16(pending_, time);
    put16(pending_, date);
    put32(pending_, e.crc32);
    put32(pending_, zip64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(e.size));
    put32(pending_, zip64 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(e.size));
    put16(pending_, static_cast<std::uint16_t>(e.name.size()));
    put16(pending_, localExtraLen(e));
    pending_ += e.name;
    if (zip64) {
        put16(pending_, 0x0001);
        put16(pending_, 16);
        put64(pending_, e.size);
        put64(pending_, e.size);
    }

    // The central directory entry is built now, while the values are at hand, and held
    // until every file has been sent.
    const std::uint64_t offset = offsets_[index_];
    const std::uint16_t extra = centralExtraLen(e, offset);

    put32(central_, kCentralSig);
    put16(central_, zip64 || extra ? kVersionZip64 : kVersionBase);  // version made by
    put16(central_, zip64 || extra ? kVersionZip64 : kVersionBase);  // version needed
    put16(central_, kFlagUtf8);
    put16(central_, 0);
    put16(central_, time);
    put16(central_, date);
    put32(central_, e.crc32);
    put32(central_, e.size > kMax32 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(e.size));
    put32(central_, e.size > kMax32 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(e.size));
    put16(central_, static_cast<std::uint16_t>(e.name.size()));
    put16(central_, extra);
    put16(central_, 0);  // comment length
    put16(central_, 0);  // disk number
    put16(central_, 0);  // internal attributes
    put32(central_, 0);  // external attributes
    put32(central_, offset > kMax32 ? 0xFFFFFFFFu : static_cast<std::uint32_t>(offset));
    central_ += e.name;
    if (extra != 0) {
        put16(central_, 0x0001);
        put16(central_, static_cast<std::uint16_t>(extra - 4));
        if (e.size > kMax32) {
            put64(central_, e.size);
            put64(central_, e.size);
        }
        if (offset > kMax32) {
            put64(central_, offset);
        }
    }

    phase_ = Phase::LocalHeader;
}

void Streamer::buildTrailer() {
    const std::uint64_t centralOffset = written_;  // everything so far was entry data
    const std::uint64_t centralSize = central_.size();
    const std::size_t count = entries_.size();
    const bool zip64 = centralOffset > kMax32 || centralSize > kMax32 ||
                       count > kMaxEntries16;

    pending_ = std::move(central_);
    central_.clear();

    if (zip64) {
        put32(pending_, kZip64EocdSig);
        put64(pending_, 44);  // size of the remainder of this record
        put16(pending_, kVersionZip64);
        put16(pending_, kVersionZip64);
        put32(pending_, 0);
        put32(pending_, 0);
        put64(pending_, count);
        put64(pending_, count);
        put64(pending_, centralSize);
        put64(pending_, centralOffset);

        put32(pending_, kZip64LocatorSig);
        put32(pending_, 0);
        put64(pending_, centralOffset + centralSize);
        put32(pending_, 1);
    }

    put32(pending_, kEocdSig);
    put16(pending_, 0);
    put16(pending_, 0);
    put16(pending_, static_cast<std::uint16_t>(std::min<std::size_t>(count, kMaxEntries16)));
    put16(pending_, static_cast<std::uint16_t>(std::min<std::size_t>(count, kMaxEntries16)));
    put32(pending_, centralSize > kMax32 ? 0xFFFFFFFFu
                                         : static_cast<std::uint32_t>(centralSize));
    put32(pending_, centralOffset > kMax32 ? 0xFFFFFFFFu
                                           : static_cast<std::uint32_t>(centralOffset));
    put16(pending_, 0);  // no archive comment

    pendingPos_ = 0;
    phase_ = Phase::Trailer;
}

std::size_t Streamer::read(char* out, std::size_t length) {
    if (!started_) {
        started_ = true;
        if (entries_.empty()) {
            buildTrailer();  // a valid, empty archive
        } else {
            beginEntry();
        }
    }

    std::size_t produced = 0;

    while (produced < length && phase_ != Phase::Done) {
        // Staged bytes always go first, so the switch below is only ever reached once
        // the current phase has nothing buffered left.
        if (pendingPos_ < pending_.size()) {
            const std::size_t take =
                std::min(length - produced, pending_.size() - pendingPos_);
            std::memcpy(out + produced, pending_.data() + pendingPos_, take);
            pendingPos_ += take;
            produced += take;
            written_ += take;
            continue;
        }

        switch (phase_) {
            case Phase::LocalHeader:
                openCurrentFile();
                phase_ = Phase::FileData;
                break;

            case Phase::FileData: {
                // A file that could not be opened is treated as fully consumed: the
                // body then falls short of the declared Content-Length, which the
                // client detects — better than handing over a plausible-looking
                // archive with silently wrong contents.
                if (fd_ < 0 || fileRemaining_ == 0) {
                    if (fd_ >= 0) {
                        ::close(fd_);
                        fd_ = -1;
                    }
                    ++index_;
                    if (index_ < entries_.size()) {
                        beginEntry();
                    } else {
                        buildTrailer();
                    }
                    break;
                }
                const std::size_t want = static_cast<std::size_t>(
                    std::min<std::uint64_t>(length - produced, fileRemaining_));
                const ssize_t got = ::read(fd_, out + produced, want);
                if (got <= 0) {
                    ::close(fd_);
                    fd_ = -1;
                    fileRemaining_ = 0;
                    break;
                }
                produced += static_cast<std::size_t>(got);
                written_ += static_cast<std::uint64_t>(got);
                fileRemaining_ -= static_cast<std::uint64_t>(got);
                break;
            }

            case Phase::Trailer:
                phase_ = Phase::Done;
                break;

            case Phase::Done:
                break;
        }
    }

    return produced;
}

}  // namespace archive::zip
