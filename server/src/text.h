#pragma once

#include <drogon/drogon.h>

#include <cstddef>
#include <optional>
#include <string>

namespace archive {

// Short free text one member writes for others to read: a display name, a bio, the note
// sent with a link. Trimmed, length-checked in characters, and free of control characters
// (a line break is allowed when `multiline`) and of Unicode direction controls — which
// reorder the text around them as displayed, and so could rewrite the @username shown
// beside whatever a member wrote. Empty yields nothing, which callers store as NULL.
// Throws a 400 naming `field`.
std::optional<std::string> cleanText(const Json::Value& value, const char* field,
                                     std::size_t maxChars, bool multiline);

// Characters, not bytes: counting bytes would give someone writing in Georgian or Cyrillic
// a third of the room an English writer gets.
std::size_t codePoints(const std::string& text);

}  // namespace archive
