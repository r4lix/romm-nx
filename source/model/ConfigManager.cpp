#include "ConfigManager.hpp"
#include "RomPathManager.hpp"
#include "JsonUtil.hpp"
#include <cstdio>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>

namespace romm::model {

    namespace {
        std::string extractPlatformPath(const std::string& content, const std::string& block_name, const std::string& platform_key) {
            size_t block_pos = content.find("\"" + block_name + "\"");
            if (block_pos != std::string::npos) {
                size_t plat_pos = content.find("\"" + platform_key + "\"", block_pos);
                if (plat_pos != std::string::npos) {
                    size_t colon_pos = content.find(':', plat_pos);
                    if (colon_pos != std::string::npos) {
                        size_t quote1 = content.find('"', colon_pos);
                        if (quote1 != std::string::npos) {
                            size_t quote2 = content.find('"', quote1 + 1);
                            if (quote2 != std::string::npos) {
                                return content.substr(quote1 + 1, quote2 - quote1 - 1);
                            }
                        }
                    }
                }
            }
            return "";
        }
    }

    ConfigManager& ConfigManager::Instance() {
        static ConfigManager inst;
        return inst;
    }

    ConfigManager::ConfigManager() {
        // Initialize default ROM paths
        rom_paths["psx"] = "sdmc:/roms/ps1/";
        rom_paths["psp"] = "sdmc:/roms/psp/";
        rom_paths["nds"] = "sdmc:/roms/nds/";
        rom_paths["gb"] = "sdmc:/roms/gb/";
        rom_paths["gbc"] = "sdmc:/roms/gbc/";
        rom_paths["gba"] = "sdmc:/roms/gba/";
    }

    void ConfigManager::SetPsxDownloadDir(const std::string& dir) {
        psx_download_dir = dir;
        // Normalize psx_download_dir to have trailing slash
        if (!psx_download_dir.empty() && psx_download_dir.back() != '/') {
            psx_download_dir += "/";
        }
        rom_paths["psx"] = psx_download_dir;
    }

    std::string ConfigManager::GetRomPath(const std::string& platform) const {
        auto it = rom_paths.find(platform);
        if (it != rom_paths.end()) {
            return it->second;
        }
        return "sdmc:/roms/" + platform + "/";
    }

    void ConfigManager::SetRomPath(const std::string& platform, const std::string& path) {
        std::string p = path;
        if (!p.empty() && p.back() != '/') {
            p += "/";
        }
        rom_paths[platform] = p;
        if (platform == "psx" || platform == "playstation" || platform == "ps1") {
            psx_download_dir = p;
        }
    }

    bool ConfigManager::Load() {
        std::string path = "sdmc:/switch/romm-nx/config.json";
        FILE* f = fopen(path.c_str(), "r");
        if (!f) {
            // Fallback to local config for testing
            path = "./config.json";
            f = fopen(path.c_str(), "r");
        }

        if (!f) {
            is_valid = false;
            error_message = "Config file sdmc:/switch/romm-nx/config.json not found";
            std::cerr << "[CONFIG] Error: " << error_message << std::endl;
            return false;
        }

        std::string content;
        char buf[512];
        while (size_t read_bytes = fread(buf, 1, sizeof(buf), f)) {
            content.append(buf, read_bytes);
        }
        fclose(f);

        std::string host;
        std::string key;
        // Check nested server_url first, fallback to root romm_host
        bool has_host = jsonExtractString(content, "server_url", host);
        if (!has_host || host.empty()) {
            has_host = jsonExtractString(content, "romm_host", host);
        }
        bool has_key = jsonExtractString(content, "api_key", key);

        if (!has_host || host.empty()) {
            is_valid = false;
            error_message = "romm_host/server_url is missing or empty in config";
            std::cerr << "[CONFIG] Error: " << error_message << std::endl;
            return false;
        }

        if (!has_key || key.empty()) {
            is_valid = false;
            error_message = "api_key is missing or empty in config";
            std::cerr << "[CONFIG] Error: " << error_message << std::endl;
            return false;
        }

        // Normalize host: strip trailing slash
        if (host.back() == '/') {
            host = host.substr(0, host.size() - 1);
        }
        romm_host = host;
        api_key = key;

        // Load other standard fields
        jsonExtractString(content, "language", language);
        jsonExtractString(content, "theme", theme);
        
        std::string quality_str;
        if (jsonExtractString(content, "covers_quality", quality_str)) {
            if (quality_str == "SD" || quality_str == "sd") covers_quality = CoversQuality::SD;
            else if (quality_str == "HD" || quality_str == "hd") covers_quality = CoversQuality::HD;
            else covers_quality = CoversQuality::Balanced;
        } else {
            covers_quality = CoversQuality::Balanced;
        }

        std::string view_mode_str;
        if (jsonExtractString(content, "grid_view_mode", view_mode_str)) {
            if (view_mode_str == "Big" || view_mode_str == "big") grid_view_mode = GridViewMode::Big;
            else if (view_mode_str == "Detail" || view_mode_str == "detail") grid_view_mode = GridViewMode::Detail;
            else grid_view_mode = GridViewMode::Default;
        } else {
            grid_view_mode = GridViewMode::Default;
        }

        jsonExtractBool(content, "auto_clear_enabled", auto_clear_enabled);
        jsonExtractInt(content, "max_size_mb", max_size_mb);
        jsonExtractInt(content, "max_age_days", max_age_days);
        jsonExtractBool(content, "show_build_version", show_build_version);
        jsonExtractString(content, "log_level", log_level);
        jsonExtractBool(content, "confirm_before_uninstall", confirm_before_uninstall);
        jsonExtractBool(content, "show_installed_badge", show_installed_badge);
        jsonExtractBool(content, "screen_always_on", screen_always_on);

        jsonExtractString(content, "update_manifest_url", update_manifest_url);
        jsonExtractString(content, "update_channel", update_channel);
        jsonExtractBool(content, "check_updates_on_startup", check_updates_on_startup);

        // Load PS1 download dir
        std::string psx_path = extractPlatformPath(content, "rom_paths", "psx");
        if (psx_path.empty()) {
            psx_path = extractPlatformPath(content, "download_dirs", "psx");
        }

        if (!psx_path.empty()) {
            psx_download_dir = psx_path;
        }

        // Normalize psx_download_dir to have trailing slash
        if (!psx_download_dir.empty() && psx_download_dir.back() != '/') {
            psx_download_dir += "/";
        }
        // Force valid root if tampered
        if (!romm::model::RomPathManager::ValidatePath(psx_download_dir)) {
            psx_download_dir = "sdmc:/roms/ps1/";
        }
        rom_paths["psx"] = psx_download_dir;

        // Extract any other platforms if they exist
        std::vector<std::string> known_platforms = {"ps2", "psp", "nds", "gb", "gbc", "gba"};
        for (const auto& plat : known_platforms) {
            std::string plat_path = extractPlatformPath(content, "rom_paths", plat);
            if (plat_path.empty()) {
                plat_path = extractPlatformPath(content, "download_dirs", plat);
            }
            if (!plat_path.empty()) {
                if (plat_path.back() != '/') plat_path += "/";
                if (!romm::model::RomPathManager::ValidatePath(plat_path)) {
                    plat_path = (plat == "psp" ? "sdmc:/roms/psp/" : "sdmc:/roms/" + plat + "/");
                }
                rom_paths[plat] = plat_path;
            } else {
                rom_paths[plat] = (plat == "psp" ? "sdmc:/roms/psp/" : "sdmc:/roms/" + plat + "/");
            }
        }

        is_valid = true;
        error_message = "";

        std::cout << "[CONFIG] Loaded config from " << path << std::endl;
        std::cout << "[CONFIG] host=" << romm_host << " api_key=" << GetMaskedApiKey() << std::endl;
        return true;
    }

    bool ConfigManager::Save(const std::string& host, const std::string& key) {
        romm_host = host;
        api_key = key;
        return Save();
    }

    bool ConfigManager::Save() {
        // Attempt to create switch and romm-nx directories on SD card
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/romm-nx", 0777);

        std::string path = "sdmc:/switch/romm-nx/config.json";
        FILE* f = fopen(path.c_str(), "w");
        if (!f) {
            // Fallback to local config for testing
            path = "./config.json";
            f = fopen(path.c_str(), "w");
        }

        if (!f) {
            std::cerr << "[CONFIG] Error: Could not open config file for writing: " << path << std::endl;
            return false;
        }

        // Normalize host: strip trailing slash
        if (!romm_host.empty() && romm_host.back() == '/') {
            romm_host = romm_host.substr(0, romm_host.size() - 1);
        }

        // Generate JSON output
        std::string content = "{\n";
        content += "  \"romm_host\": \"" + romm_host + "\",\n";
        content += "  \"api_key\": \"" + api_key + "\",\n";
        content += "  \"language\": \"" + language + "\",\n";
        content += "  \"theme\": \"" + theme + "\",\n";
        content += "  \"covers_quality\": \"" + GetCoversQualityString() + "\",\n";
        content += "  \"grid_view_mode\": \"" + GetGridViewModeString() + "\",\n";
        content += "  \"connection\": {\n";
        content += "    \"server_url\": \"" + romm_host + "\",\n";
        content += "    \"api_key\": \"" + api_key + "\"\n";
        content += "  },\n";
        content += "  \"rom_paths\": {\n";
        bool first = true;
        for (const auto& pair : rom_paths) {
            if (!first) content += ",\n";
            first = false;
            content += "    \"" + pair.first + "\": \"" + pair.second + "\"";
        }
        content += "\n  },\n";
        // Also keep download_dirs for backwards compatibility
        content += "  \"download_dirs\": {\n";
        first = true;
        for (const auto& pair : rom_paths) {
            if (!first) content += ",\n";
            first = false;
            content += "    \"" + pair.first + "\": \"" + pair.second + "\"";
        }
        content += "\n  },\n";
        content += "  \"cache\": {\n";
        content += "    \"auto_clear_enabled\": " + std::string(auto_clear_enabled ? "true" : "false") + ",\n";
        content += "    \"max_size_mb\": " + std::to_string(max_size_mb) + ",\n";
        content += "    \"max_age_days\": " + std::to_string(max_age_days) + "\n";
        content += "  },\n";
        content += "  \"debug\": {\n";
        content += "    \"show_build_version\": " + std::string(show_build_version ? "true" : "false") + ",\n";
        content += "    \"log_level\": \"" + log_level + "\"\n";
        content += "  },\n";
        content += "  \"confirm_before_uninstall\": " + std::string(confirm_before_uninstall ? "true" : "false") + ",\n";
        content += "  \"show_installed_badge\": " + std::string(show_installed_badge ? "true" : "false") + ",\n";
        content += "  \"screen_always_on\": " + std::string(screen_always_on ? "true" : "false") + ",\n";
        content += "  \"update_manifest_url\": \"" + update_manifest_url + "\",\n";
        content += "  \"update_channel\": \"" + update_channel + "\",\n";
        content += "  \"check_updates_on_startup\": " + std::string(check_updates_on_startup ? "true" : "false") + "\n";
        content += "}\n";

        size_t written = fwrite(content.c_str(), 1, content.size(), f);
        fclose(f);

        if (written != content.size()) {
            std::cerr << "[CONFIG] Error: Failed to write complete config content" << std::endl;
            return false;
        }

        is_valid = (!romm_host.empty() && !api_key.empty());
        if (is_valid) {
            error_message = "";
        } else {
            error_message = "Config has empty fields";
        }

        std::cout << "[CONFIG] Saved config to " << path << std::endl;
        return true;
    }

    std::string ConfigManager::GetCoversQualityString() const {
        switch (covers_quality) {
            case CoversQuality::SD: return "SD";
            case CoversQuality::HD: return "HD";
            default: return "Balanced";
        }
    }

    std::string ConfigManager::GetGridViewModeString() const {
        switch (grid_view_mode) {
            case GridViewMode::Big: return "Big";
            case GridViewMode::Detail: return "Detail";
            default: return "Default";
        }
    }

    std::string ConfigManager::GetMaskedApiKey() const {
        if (api_key.empty()) return "Not Configured";
        if (api_key.size() >= 12) {
            return api_key.substr(0, 8) + "..." + api_key.substr(api_key.size() - 4);
        }
        return "...";
    }

}
