#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace archive::crypto {

// CSPRNG. Throws rather than returning weak bytes: every caller here is minting a
// capability token, so silently degrading entropy is not an acceptable failure mode.
std::vector<uint8_t> randomBytes(std::size_t count);

// Unpadded base64url, suitable for tokens that end up in URLs and cookies.
std::string base64url(const std::vector<uint8_t>& data);

// Standard base64 decode, tolerant of missing padding and of the URL-safe alphabet.
// Returns nothing on invalid input: tus metadata arrives from the network and a
// malformed header must not become a silently truncated filename.
std::optional<std::string> base64Decode(const std::string& text);

// `bytes` of entropy, base64url encoded. 16 bytes = 128 bits for share links,
// 32 bytes = 256 bits for session tokens.
std::string randomToken(std::size_t bytes);

// argon2id, returning the self-describing encoded form ($argon2id$v=19$m=...$...)
// so the parameters travel with the hash and can be raised later without a migration.
std::string hashPassword(const std::string& password);
bool verifyPassword(const std::string& encoded, const std::string& password);

// Compares two equal-length strings without an early exit.
bool constantTimeEquals(const std::string& a, const std::string& b);

std::string sha256Hex(const std::string& data);

// Incremental SHA-256, for hashing a file without reading it into memory.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    void update(const char* data, std::size_t length);
    std::string hex();  // finalises; the object must not be reused afterwards

private:
    void* ctx_;  // EVP_MD_CTX*, kept opaque to keep OpenSSL out of this header
};

}  // namespace archive::crypto
