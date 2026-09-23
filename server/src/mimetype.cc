#include "mimetype.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>

namespace archive::mime {
namespace {

bool startsWith(std::string_view data, std::string_view prefix) {
    return data.size() >= prefix.size() && data.compare(0, prefix.size(), prefix) == 0;
}

// Matches at a fixed offset, for container formats that put their brand after a size
// field (the ISO base media family: MP4, MOV, HEIC, AVIF).
bool matchesAt(std::string_view data, std::size_t offset, std::string_view needle) {
    return data.size() >= offset + needle.size() &&
           data.compare(offset, needle.size(), needle) == 0;
}

std::string sniffIsoBmff(std::string_view data) {
    if (!matchesAt(data, 4, "ftyp")) {
        return {};
    }
    // The major brand sits immediately after "ftyp".
    const std::string_view brand = data.substr(8, 4);
    if (brand == "heic" || brand == "heix" || brand == "hevc" || brand == "heim") {
        return "image/heic";
    }
    if (brand == "mif1" || brand == "msf1") {
        return "image/heif";
    }
    if (brand == "avif" || brand == "avis") {
        return "image/avif";
    }
    if (brand == "qt  ") {
        return "video/quicktime";
    }
    // isom, mp41, mp42, M4V, dash and friends are all MP4 as far as a browser cares.
    return "video/mp4";
}

}  // namespace

std::string sniff(const std::filesystem::path& file) {
    std::array<char, 64> head{};
    std::size_t got = 0;
    if (std::FILE* fp = std::fopen(file.c_str(), "rb")) {
        got = std::fread(head.data(), 1, head.size(), fp);
        std::fclose(fp);
    }
    return sniffBytes(std::string_view{head.data(), got});
}

std::string sniffBytes(std::string_view data) {
    if (data.empty()) {
        return "application/octet-stream";
    }

    // Images
    if (startsWith(data, "\xFF\xD8\xFF")) return "image/jpeg";
    if (startsWith(data, "\x89PNG\r\n\x1A\n")) return "image/png";
    if (startsWith(data, "GIF87a") || startsWith(data, "GIF89a")) return "image/gif";
    if (startsWith(data, "BM")) return "image/bmp";
    if (startsWith(data, "RIFF") && matchesAt(data, 8, "WEBP")) return "image/webp";
    if (startsWith(data, "II*\0") || startsWith(data, "MM\0*")) return "image/tiff";

    // ISO base media containers: MP4, MOV, HEIC, AVIF
    if (auto iso = sniffIsoBmff(data); !iso.empty()) return iso;

    // Other video / audio
    if (startsWith(data, "\x1A\x45\xDF\xA3")) return "video/webm";  // also Matroska
    if (startsWith(data, "RIFF") && matchesAt(data, 8, "AVI ")) return "video/x-msvideo";
    if (startsWith(data, "RIFF") && matchesAt(data, 8, "WAVE")) return "audio/wav";
    if (startsWith(data, "fLaC")) return "audio/flac";
    if (startsWith(data, "OggS")) return "audio/ogg";
    if (startsWith(data, "ID3") || startsWith(data, "\xFF\xFB")) return "audio/mpeg";

    // Documents and archives
    if (startsWith(data, "%PDF-")) return "application/pdf";
    if (startsWith(data, "PK\x03\x04")) return "application/zip";
    if (startsWith(data, "\x1F\x8B")) return "application/gzip";
    if (startsWith(data, "7z\xBC\xAF\x27\x1C")) return "application/x-7z-compressed";
    if (startsWith(data, "Rar!\x1A\x07")) return "application/vnd.rar";

    return "application/octet-stream";
}

bool isRiskyToRender(const std::string& contentType) {
    static constexpr std::array<std::string_view, 6> kRisky{
        "text/html", "application/xhtml+xml", "image/svg+xml",
        "text/xml",  "application/xml",       "application/javascript",
    };
    return std::any_of(kRisky.begin(), kRisky.end(),
                       [&](std::string_view t) { return contentType == t; });
}

}  // namespace archive::mime
