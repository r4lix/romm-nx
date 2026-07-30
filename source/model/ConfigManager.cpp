#include "ConfigManager.hpp"
#include "RomPathManager.hpp"
#include "JsonUtil.hpp"
#include "DataModel.hpp"
#include "PlatformCatalog.hpp"
#include "../i18n/I18n.hpp"
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

        // Splits "<base>/<channel>/manifest.json" into its base and channel.
        // Returns false for any URL that doesn't follow that convention, which
        // is what marks a hand-written URL as an explicit override.
        bool splitChannelManifestUrl(const std::string& url, std::string& base_out, std::string& channel_out) {
            static const std::string kSuffix = "/manifest.json";
            if (url.size() <= kSuffix.size()) return false;
            if (url.compare(url.size() - kSuffix.size(), kSuffix.size(), kSuffix) != 0) return false;

            const std::string without_file = url.substr(0, url.size() - kSuffix.size());
            const size_t slash = without_file.find_last_of('/');
            if (slash == std::string::npos) return false;

            const std::string channel = without_file.substr(slash + 1);
            if (channel != ConfigManager::kChannelStable && channel != ConfigManager::kChannelTesting) {
                return false;
            }

            base_out = without_file.substr(0, slash + 1);
            channel_out = channel;
            return true;
        }
    }

    std::string ConfigManager::NormalizeUpdateChannel(const std::string& channel) {
        return (channel == kChannelTesting) ? kChannelTesting : kChannelStable;
    }

    void ConfigManager::SetUpdateBaseUrl(const std::string& url) {
        update_base_url = url;
        if (!update_base_url.empty() && update_base_url.back() != '/') {
            update_base_url += '/';
        }
    }

    std::string ConfigManager::GetUpdateManifestUrlForChannel(const std::string& channel) const {
        if (!update_manifest_url_override.empty()) return update_manifest_url_override;
        return update_base_url + NormalizeUpdateChannel(channel) + "/manifest.json";
    }

    std::string ConfigManager::GetUpdateManifestUrl() const {
        return GetUpdateManifestUrlForChannel(update_channel);
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
        rom_paths["3ds"] = "sdmc:/roms/3ds/";

        // Seed the shipped defaults so a first launch (or a config file that
        // predates this setting) already hides the right platforms without
        // waiting for Load() to say so.
        ResetPlatformVisibilityDefaults();
    }

    // --- Platform visibility -------------------------------------------------

    bool ConfigManager::IsPlatformVisible(const std::string& slug) const {
        const std::string id = NormalizePlatformId(slug);
        if (id.empty()) return true; // nothing to key on; never hide it
        return hidden_platforms.find(id) == hidden_platforms.end();
    }

    void ConfigManager::SetPlatformVisible(const std::string& slug, bool visible) {
        const std::string id = NormalizePlatformId(slug);
        if (id.empty()) return;
        known_platforms.insert(id);
        if (visible) {
            hidden_platforms.erase(id);
        } else {
            hidden_platforms.insert(id);
        }
    }

    void ConfigManager::ResetPlatformVisibilityDefaults() {
        hidden_platforms.clear();
        for (const auto& id : GetDefaultHiddenPlatformIds()) {
            hidden_platforms.insert(id);
        }
        // Server-only platforms aren't in the catalogue, so "default" for them
        // is hidden — same rule new detections get.
        for (const auto& id : known_platforms) {
            if (!IsPlatformVisibleByDefault(id)) {
                hidden_platforms.insert(id);
            }
        }
    }

    void ConfigManager::ShowAllPlatforms() {
        hidden_platforms.clear();
    }

    bool ConfigManager::RegisterDetectedPlatforms(const std::vector<std::string>& slugs) {
        bool changed = false;
        for (const auto& slug : slugs) {
            const std::string id = NormalizePlatformId(slug);
            if (id.empty()) continue;
            if (!known_platforms.insert(id).second) continue; // already seen
            changed = true;
            if (!IsPlatformVisibleByDefault(id)) {
                hidden_platforms.insert(id);
                std::cout << "[PLATFORMS] New platform '" << id << "' detected, hidden by default" << std::endl;
            }
        }
        return changed;
    }

    namespace {
        std::string GridViewModeToString(GridViewMode m) {
            switch (m) {
                case GridViewMode::Big: return "Big";
                case GridViewMode::Detail: return "Detail";
                default: return "Default";
            }
        }
        GridViewMode GridViewModeFromString(const std::string& s) {
            if (s == "Big" || s == "big") return GridViewMode::Big;
            if (s == "Detail" || s == "detail") return GridViewMode::Detail;
            return GridViewMode::Default;
        }

        std::string PlatformSelectorStyleToString(PlatformSelectorStyle s) {
            return s == PlatformSelectorStyle::Banners ? "Banners" : "Text";
        }
        PlatformSelectorStyle PlatformSelectorStyleFromString(const std::string& s) {
            if (s == "Banners" || s == "banners") return PlatformSelectorStyle::Banners;
            return PlatformSelectorStyle::Text;
        }
    }

    std::string ConfigManager::GetPlatformSelectorStyleString() const {
        return PlatformSelectorStyleToString(platform_selector_style);
    }

    GridViewMode ConfigManager::GetGridViewMode(const std::string& platform_slug) const {
        std::string norm = NormalizePlatformSlug(platform_slug);
        auto it = platform_grid_view_mode.find(norm);
        if (it != platform_grid_view_mode.end()) {
            return it->second;
        }
        return grid_view_mode;
    }

    std::string ConfigManager::GetGridViewModeString(const std::string& platform_slug) const {
        return GridViewModeToString(GetGridViewMode(platform_slug));
    }

    void ConfigManager::SetGridViewModeForPlatform(const std::string& platform_slug, GridViewMode m) {
        std::string norm = NormalizePlatformSlug(platform_slug);
        platform_grid_view_mode[norm] = m;
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
        // Guard against a hand-edited config naming a language romm-nx doesn't
        // ship. "auto" is the safe answer: it resolves to the console language
        // and, failing that, to English — never a missing dictionary.
        if (language != "auto" && language != "en" && language != "fr") {
            std::cerr << "[CONFIG] Unknown language \"" << language << "\", using \"auto\"" << std::endl;
            language = "auto";
        }
        jsonExtractString(content, "theme", theme);

        std::string selector_style_str;
        if (jsonExtractString(content, "platform_selector_style", selector_style_str)) {
            platform_selector_style = PlatformSelectorStyleFromString(selector_style_str);
        } else {
            platform_selector_style = PlatformSelectorStyle::Text;
        }

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
        jsonExtractBool(content, "filebrowser_write_anywhere", filebrowser_write_anywhere);

        jsonExtractString(content, "startup_sound", startup_sound);
        jsonExtractString(content, "theme_sound", theme_sound);
        jsonExtractString(content, "audio_base_url", audio_base_url);
        {
            // Guard against a hand-edited config.json pointing at a track
            // that doesn't exist in romfs:/audio/ — fall back to silent
            // rather than let AudioManager repeatedly fail to load it.
            static const std::vector<std::string> kValidStartup = {"none", "ps1", "ps2", "ps3", "ps3-old", "ps4", "ps5", "psp"};
            static const std::vector<std::string> kValidTheme = {"none", "ps2-remix", "ps4", "ps5", "psvita"};
            if (std::find(kValidStartup.begin(), kValidStartup.end(), startup_sound) == kValidStartup.end()) {
                startup_sound = "none";
            }
            if (std::find(kValidTheme.begin(), kValidTheme.end(), theme_sound) == kValidTheme.end()) {
                theme_sound = "none";
            }
        }

        jsonExtractInt(content, "startup_volume", startup_volume);
        jsonExtractInt(content, "ambient_volume", ambient_volume);
        startup_volume = std::max(0, std::min(100, startup_volume));
        ambient_volume = std::max(0, std::min(100, ambient_volume));

        // Update channels. The manifest URL isn't stored as such: it's derived
        // from the base URL plus the tracked channel, so switching channels
        // repoints it without any per-channel URL bookkeeping.
        {
            std::string base_url;
            if (jsonExtractString(content, "update_base_url", base_url) && !base_url.empty()) {
                SetUpdateBaseUrl(base_url);
            }

            // update_manifest_url is what every config written before channels
            // existed carries (and what this build still writes, so an older
            // NRO restored from backup keeps working). A URL following the
            // <base>/<channel>/manifest.json convention only tells us the base
            // — the channel itself comes from update_channel below. Anything
            // else is a custom endpoint and gets pinned as an override.
            std::string manifest_url;
            if (jsonExtractString(content, "update_manifest_url", manifest_url) && !manifest_url.empty()) {
                std::string parsed_base, parsed_channel;
                if (splitChannelManifestUrl(manifest_url, parsed_base, parsed_channel)) {
                    if (base_url.empty()) SetUpdateBaseUrl(parsed_base);
                    update_manifest_url_override.clear();
                } else {
                    update_manifest_url_override = manifest_url;
                }
            }

            std::string channel;
            if (jsonExtractString(content, "update_channel", channel)) {
                update_channel = NormalizeUpdateChannel(channel);
            }

            // Absent on every config written before channels existed, and the
            // build those configs belong to shipped from stable — which is
            // exactly the default.
            std::string installed_channel;
            if (jsonExtractString(content, "installed_update_channel", installed_channel)) {
                installed_update_channel = NormalizeUpdateChannel(installed_channel);
            }
        }

        jsonExtractBool(content, "check_updates_on_startup", check_updates_on_startup);
        jsonExtractString(content, "dismissed_update_version", dismissed_update_version);

        // Platform visibility. A config written before this setting existed has
        // neither key — the constructor's defaults then stand, which is exactly
        // the "initialize the default hidden platforms" case. Malformed data
        // (key present but not a string array) makes jsonExtractStringArray
        // return false and is treated the same way. Every id is re-normalized
        // on the way in, so a hand-edited "playstation-2" still matches "ps2".
        {
            std::vector<std::string> hidden_list;
            if (jsonExtractStringArray(content, "hidden_platforms", hidden_list)) {
                hidden_platforms.clear();
                for (const auto& raw : hidden_list) {
                    std::string id = NormalizePlatformId(raw);
                    if (!id.empty()) hidden_platforms.insert(id);
                }
            }

            std::vector<std::string> known_list;
            if (jsonExtractStringArray(content, "known_platforms", known_list)) {
                known_platforms.clear();
                for (const auto& raw : known_list) {
                    std::string id = NormalizePlatformId(raw);
                    if (!id.empty()) known_platforms.insert(id);
                }
            }
            // Seed the known set from what we can prove was known: everything
            // in the catalogue plus anything already hidden. Without this, a
            // config carrying only hidden_platforms would treat every built-in
            // platform as brand new on the next fetch and re-hide it.
            for (const auto& entry : GetPlatformCatalog()) {
                known_platforms.insert(entry.id);
            }
            for (const auto& id : hidden_platforms) {
                known_platforms.insert(id);
            }
        }

        // Per-platform grid view mode overrides (set via the in-game Y-Menu;
        // independent of the global grid_view_mode default above).
        platform_grid_view_mode.clear();
        std::vector<std::string> view_mode_platforms = {"psx", "ps2", "psp", "nds", "gb", "gbc", "gba", "3ds"};
        for (const auto& plat : view_mode_platforms) {
            std::string mode_str = extractPlatformPath(content, "platform_grid_view_mode", plat);
            if (!mode_str.empty()) {
                platform_grid_view_mode[plat] = GridViewModeFromString(mode_str);
            }
        }

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
        std::vector<std::string> known_platforms = {"ps2", "psp", "nds", "gb", "gbc", "gba", "3ds"};
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
        std::lock_guard<std::mutex> lock(save_mutex);

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
        content += "  \"platform_selector_style\": \"" + GetPlatformSelectorStyleString() + "\",\n";
        content += "  \"covers_quality\": \"" + GetCoversQualityString() + "\",\n";
        content += "  \"grid_view_mode\": \"" + GetGridViewModeString() + "\",\n";
        content += "  \"platform_grid_view_mode\": {\n";
        {
            bool first_vm = true;
            for (const auto& pair : platform_grid_view_mode) {
                if (!first_vm) content += ",\n";
                first_vm = false;
                content += "    \"" + pair.first + "\": \"" + GridViewModeToString(pair.second) + "\"";
            }
        }
        content += "\n  },\n";
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
        content += "  \"filebrowser_write_anywhere\": " + std::string(filebrowser_write_anywhere ? "true" : "false") + ",\n";
        content += "  \"startup_sound\": \"" + startup_sound + "\",\n";
        content += "  \"theme_sound\": \"" + theme_sound + "\",\n";
        content += "  \"startup_volume\": " + std::to_string(startup_volume) + ",\n";
        content += "  \"ambient_volume\": " + std::to_string(ambient_volume) + ",\n";
        content += "  \"audio_base_url\": \"" + audio_base_url + "\",\n";
        content += "  \"update_base_url\": \"" + update_base_url + "\",\n";
        // The resolved URL for the tracked channel, kept in the same key older
        // builds read so a backup NRO restored later still checks for updates.
        // Load() turns it back into a base + channel, or keeps it as an
        // override when it doesn't follow the channel convention.
        content += "  \"update_manifest_url\": \"" + GetUpdateManifestUrl() + "\",\n";
        content += "  \"update_channel\": \"" + update_channel + "\",\n";
        content += "  \"installed_update_channel\": \"" + installed_update_channel + "\",\n";
        content += "  \"check_updates_on_startup\": " + std::string(check_updates_on_startup ? "true" : "false") + ",\n";
        content += "  \"dismissed_update_version\": \"" + dismissed_update_version + "\",\n";

        // Settings > Platforms. hidden_platforms is the user-facing list;
        // known_platforms is bookkeeping so a platform detected later can be
        // hidden by default exactly once, instead of every launch.
        auto writeIdArray = [&content](const char* key, const std::set<std::string>& ids, bool last) {
            content += std::string("  \"") + key + "\": [";
            bool first_id = true;
            for (const auto& id : ids) {
                if (!first_id) content += ", ";
                first_id = false;
                content += "\"" + id + "\"";
            }
            content += last ? "]\n" : "],\n";
        };
        writeIdArray("hidden_platforms", hidden_platforms, false);
        writeIdArray("known_platforms", known_platforms, true);

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
        return GridViewModeToString(grid_view_mode);
    }

    std::string ConfigManager::GetMaskedApiKey() const {
        if (api_key.empty()) return romm::i18n::tr("settings.connection.not_configured");
        if (api_key.size() >= 12) {
            return api_key.substr(0, 8) + "..." + api_key.substr(api_key.size() - 4);
        }
        return "...";
    }

}
