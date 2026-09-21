#pragma once

#include <filesystem>
#include <string>

namespace archive::mime {

// Identifies a file from its leading bytes. The client's declared type is a hint from
// an untrusted source, and getting this wrong matters twice over: video seeking needs
// an accurate type, and echoing a client-chosen type back on download is a classic
// stored-XSS vector.
//
// Returns "application/octet-stream" for anything unrecognised, which is the safe
// default rather than a guess.
std::string sniff(const std::filesystem::path& file);

// True for types a browser will happily execute in the origin's context. Downloads are
// always served as attachments regardless, but this keeps the decision explicit.
bool isRiskyToRender(const std::string& contentType);

}  // namespace archive::mime
