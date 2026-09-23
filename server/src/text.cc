#include "text.h"

#include <algorithm>
#include <cctype>

#include "httputil.h"

namespace archive {
namespace {

// Bidirectional overrides and isolates (U+202A-202E, U+2066-2069) and the directional
// marks (U+200E, U+200F). They reorder the text *around* them as displayed, so a name
// could visually rewrite the @username shown beside it — and that username is exactly
// what stops one member passing themselves off as another.
bool hasBidiControl(const std::string& text) {
    for (std::size_t i = 0; i + 2 < text.size(); ++i) {
        const auto b0 = static_cast<unsigned char>(text[i]);
        const auto b1 = static_cast<unsigned char>(text[i + 1]);
        const auto b2 = static_cast<unsigned char>(text[i + 2]);
        if (b0 != 0xE2) {
            continue;
        }
        if (b1 == 0x80 && ((b2 >= 0xAA && b2 <= 0xAE) || b2 == 0x8E || b2 == 0x8F)) {
            return true;
        }
        if (b1 == 0x81 && b2 >= 0xA6 && b2 <= 0xA9) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::size_t codePoints(const std::string& text) {
    return static_cast<std::size_t>(std::count_if(text.begin(), text.end(), [](char c) {
        return (static_cast<unsigned char>(c) & 0xC0) != 0x80;
    }));
}

// Trimmed, length-checked and free of control characters; nothing when empty, which
// clears the field. Throws a 400 naming the field.
std::optional<std::string> cleanText(const Json::Value& value, const char* field,
                                     std::size_t maxChars, bool multiline) {
    if (value.isNull()) {
        return std::nullopt;
    }
    if (!value.isString()) {
        throw HttpError{400, std::string{field} + " must be text"};
    }
    std::string text = value.asString();
    // Browsers send \r\n from some textareas; one kind of line break is enough.
    std::erase(text, '\r');

    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    text.erase(text.begin(), std::find_if(text.begin(), text.end(), notSpace));
    text.erase(std::find_if(text.rbegin(), text.rend(), notSpace).base(), text.end());
    if (text.empty()) {
        return std::nullopt;
    }

    for (const char c : text) {
        const auto u = static_cast<unsigned char>(c);
        if ((u < 0x20 && !(multiline && c == '\n')) || u == 0x7F) {
            throw HttpError{400, std::string{field} + " cannot contain control characters"};
        }
    }
    if (hasBidiControl(text)) {
        throw HttpError{400, std::string{field} + " cannot contain text-direction characters"};
    }
    if (codePoints(text) > maxChars) {
        throw HttpError{400, std::string{field} + " is at most " + std::to_string(maxChars) +
                                 " characters"};
    }
    return text;
}


}  // namespace archive
