#include "json_util.hpp"

#include <cctype>

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

bool parseObjectFields(const std::string& objectJson, PlatformEntry& platform) {
    if (!jsonExtractInt(objectJson, "id", platform.id)) {
        return false;
    }
    if (!jsonExtractString(objectJson, "name", platform.name)) {
        platform.name = "Platform #" + std::to_string(platform.id);
    }
    return true;
}

bool parseRomObject(const std::string& objectJson, RomEntry& rom) {
    if (!jsonExtractInt(objectJson, "id", rom.id)) {
        return false;
    }

    if (!jsonExtractString(objectJson, "name", rom.name)) {
        jsonExtractString(objectJson, "fs_name", rom.name);
    }
    jsonExtractString(objectJson, "fs_name", rom.fsName);
    if (rom.name.empty()) {
        rom.name = rom.fsName.empty() ? ("ROM #" + std::to_string(rom.id)) : rom.fsName;
    }
    return true;
}

} // namespace

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
            return false;
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

    long value = 0;
    bool found = false;
    while (pos < json.size() && std::isdigit(static_cast<unsigned char>(json[pos]))) {
        value = value * 10 + (json[pos] - '0');
        ++pos;
        found = true;
    }

    if (!found) {
        return false;
    }

    out = static_cast<int>(negative ? -value : value);
    return true;
}

bool jsonParsePlatformList(const std::string& json, std::vector<PlatformEntry>& out) {
    out.clear();
    const size_t start = skipWhitespace(json, 0);
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

        PlatformEntry platform;
        if (parseObjectFields(json.substr(pos, objectEnd - pos + 1), platform)) {
            out.push_back(platform);
        }

        pos = objectEnd + 1;
        pos = skipWhitespace(json, pos);
        if (pos < end && json[pos] == ',') {
            ++pos;
        }
    }

    return !out.empty();
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

        RomEntry rom;
        if (parseRomObject(json.substr(pos, objectEnd - pos + 1), rom)) {
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

bool jsonExtractFirstFileName(const std::string& json, std::string& out) {
    const std::string keys[] = {"file_name", "filename", "name"};
    for (const auto& key : keys) {
        if (jsonExtractString(json, key, out) && !out.empty()) {
            return true;
        }
    }

    const size_t filesPos = json.find("\"files\"");
    if (filesPos == std::string::npos) {
        return jsonExtractString(json, "fs_name", out);
    }

    const size_t arrayStart = json.find('[', filesPos);
    if (arrayStart == std::string::npos) {
        return jsonExtractString(json, "fs_name", out);
    }

    const size_t objectStart = json.find('{', arrayStart);
    if (objectStart == std::string::npos) {
        return jsonExtractString(json, "fs_name", out);
    }

    const size_t objectEnd = findMatching(json, objectStart, '{', '}');
    if (objectEnd == std::string::npos) {
        return jsonExtractString(json, "fs_name", out);
    }

    const std::string firstFile = json.substr(objectStart, objectEnd - objectStart + 1);
    if (jsonExtractString(firstFile, "file_name", out) ||
        jsonExtractString(firstFile, "filename", out) ||
        jsonExtractString(firstFile, "name", out)) {
        return !out.empty();
    }

    return jsonExtractString(json, "fs_name", out);
}
