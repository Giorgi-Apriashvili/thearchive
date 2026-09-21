#include "crypto.h"

#include <argon2.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <zlib.h>

#include <cstring>
#include <stdexcept>

namespace archive::crypto {
namespace {

// OWASP's baseline argon2id parameters: 19 MiB, 2 iterations, 1 lane. Deliberately at
// the recommended floor rather than above it, because hashing happens on a Drogon event
// loop thread — a heavier setting would turn the login endpoint into an amplification
// vector for anyone who can send unauthenticated requests.
constexpr uint32_t kTimeCost = 2;
constexpr uint32_t kMemoryCostKiB = 19456;
constexpr uint32_t kParallelism = 1;
constexpr std::size_t kSaltBytes = 16;
constexpr std::size_t kHashBytes = 32;

constexpr char kAlphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

}  // namespace

std::vector<uint8_t> randomBytes(std::size_t count) {
    std::vector<uint8_t> out(count);
    if (RAND_bytes(out.data(), static_cast<int>(count)) != 1) {
        throw std::runtime_error("CSPRNG failed");
    }
    return out;
}

std::string base64url(const std::vector<uint8_t>& data) {
    std::string out;
    out.reserve((data.size() * 4 + 2) / 3);
    std::size_t i = 0;
    while (i + 3 <= data.size()) {
        const uint32_t v = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8) |
                           uint32_t{data[i + 2]};
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
        out += kAlphabet[v & 0x3F];
        i += 3;
    }
    const std::size_t remaining = data.size() - i;
    if (remaining == 1) {
        const uint32_t v = uint32_t{data[i]} << 16;
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
    } else if (remaining == 2) {
        const uint32_t v = (uint32_t{data[i]} << 16) | (uint32_t{data[i + 1]} << 8);
        out += kAlphabet[(v >> 18) & 0x3F];
        out += kAlphabet[(v >> 12) & 0x3F];
        out += kAlphabet[(v >> 6) & 0x3F];
    }
    return out;
}

std::string randomToken(std::size_t bytes) {
    return base64url(randomBytes(bytes));
}

std::optional<std::string> base64Decode(const std::string& text) {
    auto value = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+' || c == '-') return 62;  // '-' for the URL-safe alphabet
        if (c == '/' || c == '_') return 63;
        return -1;
    };

    std::string out;
    out.reserve(text.size() * 3 / 4);
    uint32_t accumulator = 0;
    int bits = 0;
    for (const char c : text) {
        if (c == '=' || c == '\n' || c == '\r') {
            continue;
        }
        const int decoded = value(c);
        if (decoded < 0) {
            return std::nullopt;
        }
        accumulator = (accumulator << 6) | static_cast<uint32_t>(decoded);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((accumulator >> bits) & 0xFF);
        }
    }
    return out;
}

std::string hashPassword(const std::string& password) {
    const std::vector<uint8_t> salt = randomBytes(kSaltBytes);
    const std::size_t encodedLength = argon2_encodedlen(
        kTimeCost, kMemoryCostKiB, kParallelism,
        static_cast<uint32_t>(kSaltBytes), static_cast<uint32_t>(kHashBytes), Argon2_id);

    std::string encoded(encodedLength, '\0');
    const int rc = argon2id_hash_encoded(
        kTimeCost, kMemoryCostKiB, kParallelism, password.data(), password.size(),
        salt.data(), salt.size(), kHashBytes, encoded.data(), encodedLength);
    if (rc != ARGON2_OK) {
        throw std::runtime_error(std::string{"argon2: "} + argon2_error_message(rc));
    }
    encoded.resize(std::strlen(encoded.c_str()));  // drop the trailing NUL
    return encoded;
}

bool verifyPassword(const std::string& encoded, const std::string& password) {
    return argon2id_verify(encoded.c_str(), password.data(), password.size()) == ARGON2_OK;
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;
    }
    return CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
}

void Crc32::update(const char* data, std::size_t length) {
    crc_ = static_cast<uint32_t>(
        ::crc32_z(crc_, reinterpret_cast<const Bytef*>(data), length));
}

Sha256::Sha256() {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (ctx == nullptr || EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        throw std::runtime_error("cannot initialise SHA-256");
    }
    ctx_ = ctx;
}

Sha256::~Sha256() {
    EVP_MD_CTX_free(static_cast<EVP_MD_CTX*>(ctx_));
}

void Sha256::update(const char* data, std::size_t length) {
    if (EVP_DigestUpdate(static_cast<EVP_MD_CTX*>(ctx_), data, length) != 1) {
        throw std::runtime_error("SHA-256 update failed");
    }
}

std::string Sha256::hex() {
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(static_cast<EVP_MD_CTX*>(ctx_), digest, &length) != 1) {
        throw std::runtime_error("SHA-256 finalise failed");
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(length * 2);
    for (unsigned int i = 0; i < length; ++i) {
        out += kHex[digest[i] >> 4];
        out += kHex[digest[i] & 0x0F];
    }
    return out;
}

std::string sha256Hex(const std::string& data) {
    Sha256 hasher;
    hasher.update(data.data(), data.size());
    return hasher.hex();
}

}  // namespace archive::crypto
