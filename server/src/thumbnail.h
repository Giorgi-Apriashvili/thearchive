#pragma once

#include <filesystem>
#include <string>

namespace archive::thumbnail {

// Longest-edge sizes. `sm` backs the row icons on the download page, `lg` is what the
// viewer displays — large enough to look right full-screen on a laptop, ~50x smaller
// than the phone original it came from.
inline constexpr int kSmall = 320;
inline constexpr int kLarge = 1600;

// Must be called once before any other function here, and shut down at exit. libvips
// keeps a global operation cache which is left deliberately small: this process is long
// running, and the default would let it grow without bound.
void startup(const char* argv0);
void shutdown();

// Whether a sniffed content type is one we attempt to thumbnail at all.
bool isThumbnailable(const std::string& contentType);

// Renders both sizes beside the blob. Returns false on any failure — an unreadable or
// unsupported image must never fail the upload that produced it, so callers record the
// outcome and carry on.
bool generate(const std::filesystem::path& source, const std::filesystem::path& dataDir,
              const std::string& hash);

// thumbs/ab/cd/<sha256>-sm.webp — mirrors the blob store's fan-out so the two trees stay
// navigable side by side.
std::filesystem::path path(const std::filesystem::path& dataDir, const std::string& hash,
                           bool large);

// Removes both sizes and prunes the fan-out directories once empty.
void remove(const std::filesystem::path& dataDir, const std::string& hash);

}  // namespace archive::thumbnail
