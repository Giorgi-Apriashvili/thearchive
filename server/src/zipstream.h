#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace archive::zip {

struct Entry {
    std::string name;                 // path inside the archive, UTF-8
    std::filesystem::path source;     // file on disk to copy verbatim
    std::uint64_t size = 0;
    std::uint32_t crc32 = 0;
    std::int64_t mtime = 0;           // unix seconds; 0 means "now"
};

// A streaming, store-only ZIP writer.
//
// Store rather than deflate because the payload is photos, video and archives, which do
// not compress — deflating them spends CPU to produce a slightly larger file. It also
// makes the output size exactly predictable, which is the point: `totalSize()` is known
// before a single byte is produced, so the response can carry a real Content-Length and
// the browser shows a true progress bar rather than an indeterminate spinner.
//
// Because sizes and CRCs are known up front, entries need no data descriptors — the
// local header is complete when written. Zip64 fields appear only on the entries and in
// the trailer that actually need them, so archives that fit the classic limits stay
// readable by tools with shaky Zip64 support.
class Streamer {
public:
    explicit Streamer(std::vector<Entry> entries);

    // Exact byte count of the archive this will produce.
    std::uint64_t totalSize() const { return totalSize_; }

    // Fills up to `length` bytes and returns how many were written. Returns 0 once the
    // archive is complete. Called repeatedly by the HTTP layer as the socket drains.
    std::size_t read(char* out, std::size_t length);

private:
    enum class Phase { LocalHeader, FileData, Trailer, Done };

    void beginEntry();
    void openCurrentFile();
    void buildTrailer();

    std::vector<Entry> entries_;
    std::vector<std::uint64_t> offsets_;   // local header offset per entry

    std::size_t index_ = 0;
    bool started_ = false;
    Phase phase_ = Phase::LocalHeader;

    std::string pending_;                  // header or trailer bytes awaiting output
    std::size_t pendingPos_ = 0;

    std::string central_;                  // accumulated central directory
    int fd_ = -1;
    std::uint64_t fileRemaining_ = 0;

    std::uint64_t written_ = 0;
    std::uint64_t totalSize_ = 0;
};

}  // namespace archive::zip
