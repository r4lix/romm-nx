#include "JsonUtil.hpp"
#include <cctype>
#include <iostream>

namespace romm::model {

namespace {

size_t skipWhitespace(const std::string& json, size_t pos) {
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    return pos;
}

size_t findMatching(const std::string& json, size_t start, char open, char close) {
    if (start >= json.size() || json[start] != open) {
        return std::string::npos;
    }

    int depth = 0;
    bool inString = false;
    for (size_t i = start; i < json.size(); ++i) {
        const char ch = json[i];
        if (ch == '"' && (i == 0 || json[i - 1] != '\\')) {
            inString = !inString;
            continue;
        }
        if (inString) {
            continue;
        }
        if (ch == open) {
            ++depth;
        } else if (ch == close) {
            --depth;
            if (depth == 0) {
                return i;
            }
        }
    }
    return std::string::npos;
}

} // namespace

bool jsonExtractLongLong(const std::string& json, const std::string& key, long long& out) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) {
        return false;
    }
    pos = skipWhitespace(json, pos + 1);

    bool negative = false;
    if (pos < json.size() && json[pos] == '-') {
        negative = true;
        ++pos;
    }

    long long value = 0;
    bool found = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + (json[pos] - '0');
        ++pos;
        found = true;
    }

    if (!found) {
        return false;
    }

    out = negative ? -value : value;
    return true;
}

bool jsonExtractString(const std::string& json, const std::string& key, std::string& out) {
    const std::string marker = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(marker, pos)) != std::string::npos) {
        pos = json.find(':', pos + marker.size());
        if (pos == std::string::npos) {
            return false;
        }
        pos = skipWhitespace(json, pos + 1);
        if (pos >= json.size() || json[pos] != '"') {
            pos += 1;
            continue; // Not a string type (could be null or int), try next key occurrence
        }
        ++pos;

        std::string value;
        while (pos < json.size()) {
            const char ch = json[pos++];
            if (ch == '"') {
                out = value;
                return true;
            }
            if (ch == '\\' && pos < json.size()) {
                value.push_back(json[pos++]);
            } else {
                value.push_back(ch);
            }
        }
    }
    return false;
}

bool jsonExtractInt(const std::string& json, const std::string& key, int& out) {
    long long val = 0;
    if (jsonExtractLongLong(json, key, val)) {
        out = static_cast<int>(val);
        return true;
    }
    return false;
}

bool jsonExtractStringArray(const std::string& json, const std::string& key, std::vector<std::string>& out) {
    out.clear();
    const std::string marker = "\"" + key + "\"";
    size_t pos = json.find(marker);
    if (pos == std::string::npos) return false;

    pos = json.find(':', pos + marker.size());
    if (pos == std::string::npos) return false;

    pos = skipWhitespace(json, pos + 1);
    if (pos >= json.size() || json[pos] != '[') return false;

    size_t end = findMatching(json, pos, '[', ']');
    if (end == std::string::npos) return false;

    pos = pos + 1;
    while (pos < end) {
        pos = skipWhitespace(json, pos);
        if (pos >= end || json[pos] == ']') break;
        if (json[pos] == '"') {
            ++pos;
            std::string value;
            while (pos < end) {
                const char ch = json[pos++];
                if (ch == '"') {
                    out.push_back(value);
                    break;
                }
                if (ch == '\\' && pos < end) {
                    value.push_back(json[pos++]);
                } else {
                    value.push_back(ch);
                }
            }
        } else {
            ++pos;
        }
        pos = skipWhitespace(json, pos);
        if (pos < end && json[pos] == ',') ++pos;
    }
    return true;
}

bool jsonParsePlatformList(const std::string& json, std::vector<PlatformEntry>& out) {
    out.clear();
    size_t start = skipWhitespace(json, 0);
    if (start >= json.size() || json[start] != '[') {
        return false;
    }

    const size_t end = findMatching(json, start, '[', ']');
    if (end == std::string::npos) {
        return false;
    }

    size_t pos = start + 1;
    while (pos < end) {
        pos = skipWhitespace(json, pos);
        if (pos >= end || json[pos] == ']') {
            break;
        }
        if (json[pos] != '{') {
            return false;
        }

        const size_t objectEnd = findMatching(json, pos, '{', '}');
        if (objectEnd == std::string::npos || objectEnd > end) {
            return false;
        }

        std::string block = json.substr(pos, objectEnd - pos + 1);
        PlatformEntry platform;
        if (jsonExtractInt(block, "id", platform.id)) {
            if (!jsonExtractString(block, "name", platform.name)) {
                platform.name = "Platform #" + std::to_string(platform.id);
            }
            jsonExtractString(block, "slug", platform.slug);
            out.push_back(platform);
        }

        pos = objectEnd + 1;
        pos = skipWhitespace(json, pos);
        if (pos < end && json[pos] == ',') {
            ++pos;
        }
    }

    return true;
}

bool jsonParseRomItems(const std::string& json, std::vector<RomEntry>& out) {
    out.clear();

    const std::string marker = "\"items\"";
    size_t itemsPos = json.find(marker);
    if (itemsPos == std::string::npos) {
        return false;
    }

    itemsPos = json.find('[', itemsPos);
    if (itemsPos == std::string::npos) {
        return false;
    }

    const size_t itemsEnd = findMatching(json, itemsPos, '[', ']');
    if (itemsEnd == std::string::npos) {
        return false;
    }

    size_t pos = itemsPos + 1;
    while (pos < itemsEnd) {
        pos = skipWhitespace(json, pos);
        if (pos >= itemsEnd || json[pos] == ']') {
            break;
        }
        if (json[pos] != '{') {
            return false;
        }

        const size_t objectEnd = findMatching(json, pos, '{', '}');
        if (objectEnd == std::string::npos || objectEnd > itemsEnd) {
            return false;
        }

        std::string block = json.substr(pos, objectEnd - pos + 1);
        RomEntry rom;
        if (jsonExtractInt(block, "id", rom.id)) {
            if (!jsonExtractString(block, "name", rom.name)) {
                rom.name = "ROM #" + std::to_string(rom.id);
            }
            jsonExtractInt(block, "platform_id", rom.platform_id);
            jsonExtractString(block, "fs_name", rom.fs_name);
            jsonExtractLongLong(block, "fs_size_bytes", rom.fs_size_bytes);
            if (jsonExtractString(block, "path_cover_small", rom.path_cover_small)) {
                // Strip query params (e.g., "?ts=2026-02-10 22:22:55")
                auto q = rom.path_cover_small.find('?');
                if (q != std::string::npos) {
                    rom.path_cover_small = rom.path_cover_small.substr(0, q);
                }
            }
            if (jsonExtractString(block, "path_cover_large", rom.path_cover_large)) {
                auto q = rom.path_cover_large.find('?');
                if (q != std::string::npos) {
                    rom.path_cover_large = rom.path_cover_large.substr(0, q);
                }
            }
            out.push_back(rom);
        }

        pos = objectEnd + 1;
        pos = skipWhitespace(json, pos);
        if (pos < itemsEnd && json[pos] == ',') {
            ++pos;
        }
    }

    return true;
}

    bool jsonExtractBool(const std::string& json, const std::string& key, bool& out) {
        const std::string marker = "\"" + key + "\"";
        size_t pos = json.find(marker);
        if (pos == std::string::npos) return false;
        pos = json.find(':', pos + marker.size());
        if (pos == std::string::npos) return false;
        pos = skipWhitespace(json, pos + 1);
        if (pos < json.size()) {
            if (json.compare(pos, 4, "true") == 0) {
                out = true;
                return true;
            } else if (json.compare(pos, 5, "false") == 0) {
                out = false;
                return true;
            }
        }
        return false;
    }

    namespace {

        // Appends one Unicode code point as UTF-8. Only used by the \uXXXX
        // escape path; dictionaries are authored as literal UTF-8, so this is
        // purely defensive against a hand-edited or tool-generated file.
        void appendUtf8(std::string& out, unsigned int cp) {
            if (cp < 0x80) {
                out.push_back(static_cast<char>(cp));
            } else if (cp < 0x800) {
                out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            } else {
                out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            }
        }

        // Reads a JSON string starting at the opening quote. Returns false if
        // it is unterminated; `pos` ends just past the closing quote.
        bool readJsonString(const std::string& json, size_t& pos, std::string& out) {
            if (pos >= json.size() || json[pos] != '"') return false;
            ++pos;
            out.clear();
            while (pos < json.size()) {
                const char ch = json[pos++];
                if (ch == '"') return true;
                if (ch != '\\') {
                    out.push_back(ch);
                    continue;
                }
                if (pos >= json.size()) return false;
                const char esc = json[pos++];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 't': out.push_back('\t'); break;
                    case 'r': out.push_back('\r'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'u': {
                        unsigned int cp = 0;
                        if (pos + 4 > json.size()) return false;
                        for (int i = 0; i < 4; ++i) {
                            const char h = json[pos + i];
                            cp <<= 4;
                            if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                            else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                            else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                            else return false;
                        }
                        pos += 4;
                        appendUtf8(out, cp);
                        break;
                    }
                    default: out.push_back(esc); break; // covers \" \\ \/
                }
            }
            return false;
        }

    } // namespace

    bool jsonParseFlatStringMap(const std::string& json, std::map<std::string, std::string>& out) {
        out.clear();

        size_t pos = skipWhitespace(json, 0);
        if (pos >= json.size() || json[pos] != '{') {
            return false;
        }
        const size_t end = findMatching(json, pos, '{', '}');
        if (end == std::string::npos) {
            return false;
        }

        ++pos;
        while (pos < end) {
            pos = skipWhitespace(json, pos);
            if (pos >= end || json[pos] == '}') break;

            if (json[pos] != '"') {
                ++pos; // stray token; skip rather than abandon the whole file
                continue;
            }

            std::string key;
            if (!readJsonString(json, pos, key)) return false;

            pos = skipWhitespace(json, pos);
            if (pos >= end || json[pos] != ':') return false;
            pos = skipWhitespace(json, pos + 1);

            if (pos < end && json[pos] == '"') {
                std::string value;
                if (!readJsonString(json, pos, value)) return false;
                out[key] = value;
            } else {
                // Non-string value (number/bool/null/nested): not a dictionary
                // entry, so skip to the next comma at this depth.
                while (pos < end && json[pos] != ',') ++pos;
            }

            pos = skipWhitespace(json, pos);
            if (pos < end && json[pos] == ',') ++pos;
        }

        return true;
    }

} // namespace romm::model

