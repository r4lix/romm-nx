#include "config.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>

namespace {

bool ensureParentDir(const std::string& path) {
    std::filesystem::path fsPath(path);
    std::filesystem::path dir = fsPath.parent_path();
    if (dir.empty()) {
        return true;
    }
    if (std::filesystem::exists(dir)) {
        return true;
    }
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return !ec;
}

std::string trim(const std::string& value) {
    const auto start = value.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) {
        return "";
    }
    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(start, end - start + 1);
}

std::string unquote(const std::string& value) {
    if (value.size() >= 2 && value.front() == '"' && value.back() == '"') {
        return value.substr(1, value.size() - 2);
    }
    return value;
}

bool readSimpleJsonField(const std::string& path, const std::string& key, std::string& out) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return false;
    }

    const std::string marker = "\"" + key + "\"";
    std::string line;
    while (std::getline(file, line)) {
        const auto pos = line.find(marker);
        if (pos == std::string::npos) {
            continue;
        }
        const auto colon = line.find(':', pos + marker.size());
        if (colon == std::string::npos) {
            continue;
        }
        out = unquote(trim(line.substr(colon + 1)));
        return true;
    }

    return false;
}

} // namespace

Config& Config::instance() {
    static Config inst;
    return inst;
}

Config::Config() {
    load();
}

std::string Config::configPath() const {
#ifdef __SWITCH__
    return "sdmc:/config/romm-nx/config.json";
#else
    return "./config.json";
#endif
}

void Config::load() {
    std::string path = configPath();
    if (!std::filesystem::exists(path)) {
        const std::string localPath = "./config.json";
        if (std::filesystem::exists(localPath)) {
            path = localPath;
        } else {
            save();
            return;
        }
    }

    std::string host;
    std::string token;
    if (readSimpleJsonField(path, "host_url", host)) {
        hostUrl_ = host;
    }
    if (readSimpleJsonField(path, "access_token", token)) {
        accessToken_ = token;
    }

    std::cout << "[config] loaded host=" << hostUrl_ << " token_len=" << accessToken_.size() << std::endl;
}

void Config::save() {
    const std::string path = configPath();
    ensureParentDir(path);

    std::ofstream file(path);
    if (!file.is_open()) {
        std::cerr << "[config] failed to write " << path << std::endl;
        return;
    }

    file << "{\n";
    file << "  \"host_url\": \"" << hostUrl_ << "\",\n";
    file << "  \"access_token\": \"" << accessToken_ << "\"\n";
    file << "}\n";
}
