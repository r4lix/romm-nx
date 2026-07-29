#pragma once

#include <map>
#include <string>
#include <vector>

namespace romm::model {

    struct PlatformEntry {
        int id = 0;
        std::string name;
        std::string slug;
    };

    struct RomEntry {
        int id = 0;
        std::string name;
        int platform_id = 0;
        std::string fs_name;
        long long fs_size_bytes = 0;
        std::string path_cover_small; // small thumbnail URL
        std::string path_cover_large; // full-resolution cover URL
    };

    bool jsonParsePlatformList(const std::string& json, std::vector<PlatformEntry>& out);
    bool jsonParseRomItems(const std::string& json, std::vector<RomEntry>& out);
    bool jsonExtractString(const std::string& json, const std::string& key, std::string& out);
    bool jsonExtractInt(const std::string& json, const std::string& key, int& out);
    bool jsonExtractLongLong(const std::string& json, const std::string& key, long long& out);
    bool jsonExtractStringArray(const std::string& json, const std::string& key, std::vector<std::string>& out);
    bool jsonExtractBool(const std::string& json, const std::string& key, bool& out);

    // Parses a top-level object whose values are all strings, i.e. exactly the
    // shape of the i18n dictionaries in romfs:/lang/. Non-string values are
    // skipped rather than treated as a parse failure. Returns false only if the
    // document isn't a JSON object at all.
    bool jsonParseFlatStringMap(const std::string& json, std::map<std::string, std::string>& out);
}

