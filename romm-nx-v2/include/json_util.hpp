#pragma once

#include <string>
#include <vector>

struct PlatformEntry {
    int id = 0;
    std::string name;
};

struct RomEntry {
    int id = 0;
    std::string name;
    std::string fsName;
};

bool jsonExtractString(const std::string& json, const std::string& key, std::string& out);
bool jsonExtractInt(const std::string& json, const std::string& key, int& out);

bool jsonParsePlatformList(const std::string& json, std::vector<PlatformEntry>& out);
bool jsonParseRomItems(const std::string& json, std::vector<RomEntry>& out);
bool jsonExtractFirstFileName(const std::string& json, std::string& out);
