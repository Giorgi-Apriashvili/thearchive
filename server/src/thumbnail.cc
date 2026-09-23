#include "thumbnail.h"

#include <vips/vips.h>

#include <array>
#include <string_view>
#include <system_error>

namespace archive::thumbnail {
namespace {

namespace fs = std::filesystem;

// Types libvips can decode here. HEIC and AVIF are the interesting entries: browsers
// cannot display them, so without thumbnailing an iPhone upload would be invisible to
// everyone who received the link.
constexpr std::array<std::string_view, 8> kSupported{
    "image/jpeg", "image/png",  "image/webp", "image/avif",
    "image/heic", "image/heif", "image/tiff", "image/bmp",
};

bool renderOne(const fs::path& source, const fs::path& target, int size) {
    VipsImage* image = nullptr;
    // vips_thumbnail shrinks during decode rather than loading the full raster, so a
    // 12 MP JPEG never materialises in memory. `auto_rotate` applies the EXIF
    // orientation tag, without which portrait phone photos arrive on their side.
    if (vips_thumbnail(source.c_str(), &image, size, "height", size, "auto_rotate", TRUE,
                       nullptr) != 0) {
        return false;
    }

    // Strip metadata: EXIF on a thumbnail serves no purpose here and would hand every
    // recipient of a link the GPS coordinates the photo was taken at.
    const int rc = vips_image_write_to_file(image, target.c_str(), "Q", 80, "strip", TRUE,
                                            nullptr);
    g_object_unref(image);
    return rc == 0;
}

}  // namespace

void startup(const char* argv0) {
    if (VIPS_INIT(argv0) != 0) {
        vips_error_clear();
    }
    // Long-running process: cap the operation cache rather than letting it accumulate.
    vips_cache_set_max_mem(64 * 1024 * 1024);
    vips_cache_set_max(100);
    // One worker per thumbnail call. Drogon already runs a thread per core, so letting
    // libvips fan out too would oversubscribe badly under concurrent uploads.
    vips_concurrency_set(1);
}

void shutdown() {
    vips_shutdown();
}

bool isThumbnailable(const std::string& contentType) {
    for (const std::string_view supported : kSupported) {
        if (contentType == supported) {
            return true;
        }
    }
    return false;
}

fs::path path(const fs::path& dataDir, const std::string& hash, bool large) {
    return dataDir / "thumbs" / hash.substr(0, 2) / hash.substr(2, 2) /
           (hash + (large ? "-lg.webp" : "-sm.webp"));
}

bool generate(const fs::path& source, const fs::path& dataDir, const std::string& hash) {
    const fs::path large = path(dataDir, hash, true);

    std::error_code ec;
    fs::create_directories(large.parent_path(), ec);
    if (ec) {
        return false;
    }

    if (!renderOne(source, large, kLarge)) {
        vips_error_clear();
        return false;
    }
    // The small size is derived from the large one: it is already decoded, already
    // rotated, and a fraction of the original's pixels.
    if (!renderOne(large, path(dataDir, hash, false), kSmall)) {
        vips_error_clear();
        fs::remove(large, ec);
        return false;
    }
    return true;
}

void remove(const fs::path& dataDir, const std::string& hash) {
    std::error_code ec;
    const fs::path large = path(dataDir, hash, true);
    fs::remove(large, ec);
    fs::remove(path(dataDir, hash, false), ec);
    fs::remove(large.parent_path(), ec);
    fs::remove(large.parent_path().parent_path(), ec);
}

bool renderAvatar(std::string_view image, const fs::path& target) {
    // libvips takes a non-const pointer for buffers it only ever reads.
    void* data = const_cast<char*>(image.data());

    // Header first. Opening from a buffer reads only the header, so this costs nothing
    // for a real photo and stops a tiny file that declares 100,000 x 100,000 pixels
    // before any of them are decoded. 50 MP is well past any phone camera.
    {
        VipsImage* header = vips_image_new_from_buffer(data, image.size(), "", nullptr);
        if (header == nullptr) {
            vips_error_clear();
            return false;
        }
        const double pixels = static_cast<double>(vips_image_get_width(header)) *
                              static_cast<double>(vips_image_get_height(header));
        g_object_unref(header);
        if (pixels > 50e6) {
            return false;
        }
    }

    VipsImage* square = nullptr;
    // Cropped from the centre rather than libvips' "attention" heuristic: the result is
    // then what a person expects from a square crop, not a guess at what mattered.
    if (vips_thumbnail_buffer(data, image.size(), &square, kAvatar, "height", kAvatar, "crop",
                              VIPS_INTERESTING_CENTRE, "auto_rotate", TRUE, nullptr) != 0) {
        vips_error_clear();
        return false;
    }
    const int rc = vips_image_write_to_file(square, target.c_str(), "Q", 85, "strip", TRUE,
                                            nullptr);
    g_object_unref(square);
    if (rc != 0) {
        vips_error_clear();
        std::error_code ec;
        fs::remove(target, ec);  // never leave a half-written picture behind
        return false;
    }
    return true;
}

}  // namespace archive::thumbnail
