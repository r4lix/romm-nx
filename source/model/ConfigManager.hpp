#pragma once

#include <string>
#include <map>

namespace romm::model {
    
    enum class CoversQuality {
        SD,
        Balanced,
        HD
    };

    enum class GridViewMode {
        Default,
        Big,
        Detail // Not implemented yet — grid rendering treats this the same as Default.
    };

    class ConfigManager {
    public:
        static ConfigManager& Instance();

        bool Load();
        bool Save(const std::string& host, const std::string& api_key);
        bool Save(); // General save using current settings

        const std::string& GetRommHost() const { return romm_host; }
        void SetRommHost(const std::string& host) { romm_host = host; }

        const std::string& GetApiKey() const { return api_key; }
        void SetApiKey(const std::string& key) { api_key = key; }
        
        const std::string& GetPsxDownloadDir() const { return psx_download_dir; }
        void SetPsxDownloadDir(const std::string& dir);

        bool IsValid() const { return is_valid; }
        const std::string& GetErrorMessage() const { return error_message; }

        std::string GetMaskedApiKey() const;

        // General settings
        const std::string& GetLanguage() const { return language; }
        void SetLanguage(const std::string& lang) { language = lang; }

        const std::string& GetTheme() const { return theme; }
        void SetTheme(const std::string& t) { theme = t; }

        CoversQuality GetCoversQuality() const { return covers_quality; }
        void SetCoversQuality(CoversQuality q) { covers_quality = q; }
        std::string GetCoversQualityString() const;

        GridViewMode GetGridViewMode() const { return grid_view_mode; }
        void SetGridViewMode(GridViewMode m) { grid_view_mode = m; }
        std::string GetGridViewModeString() const;

        bool IsAutoClearEnabled() const { return auto_clear_enabled; }
        void SetAutoClearEnabled(bool enabled) { auto_clear_enabled = enabled; }

        int GetMaxSizeMb() const { return max_size_mb; }
        void SetMaxSizeMb(int mb) { max_size_mb = mb; }

        int GetMaxAgeDays() const { return max_age_days; }
        void SetMaxAgeDays(int days) { max_age_days = days; }

        bool ShowBuildVersion() const { return show_build_version; }
        void SetShowBuildVersion(bool show) { show_build_version = show; }

        const std::string& GetLogLevel() const { return log_level; }
        void SetLogLevel(const std::string& level) { log_level = level; }

        bool ConfirmBeforeUninstall() const { return confirm_before_uninstall; }
        void SetConfirmBeforeUninstall(bool confirm) { confirm_before_uninstall = confirm; }

        bool ShowInstalledBadge() const { return show_installed_badge; }
        void SetShowInstalledBadge(bool show) { show_installed_badge = show; }

        bool ScreenAlwaysOn() const { return screen_always_on; }
        void SetScreenAlwaysOn(bool enabled) { screen_always_on = enabled; }

        std::string GetUpdateManifestUrl() const { return update_manifest_url; }
        void SetUpdateManifestUrl(const std::string& url) { update_manifest_url = url; }

        std::string GetUpdateChannel() const { return update_channel; }
        void SetUpdateChannel(const std::string& channel) { update_channel = channel; }

        bool CheckUpdatesOnStartup() const { return check_updates_on_startup; }
        void SetCheckUpdatesOnStartup(bool check) { check_updates_on_startup = check; }

        // ROM paths for different platforms
        std::string GetRomPath(const std::string& platform) const;
        void SetRomPath(const std::string& platform, const std::string& path);

    private:
        ConfigManager();

        std::string romm_host;
        std::string api_key;
        std::string psx_download_dir = "sdmc:/roms/ps1/";
        bool is_valid = false;
        std::string error_message;

        std::string language = "en";
        std::string theme = "romm_brand";
        CoversQuality covers_quality = CoversQuality::Balanced;
        GridViewMode grid_view_mode = GridViewMode::Default;
        bool auto_clear_enabled = false;
        int max_size_mb = 500;
        int max_age_days = 30;
        bool show_build_version = true;
        std::string log_level = "info";
        bool confirm_before_uninstall = true;
        bool show_installed_badge = true;
        bool screen_always_on = false;

        std::string update_manifest_url = "https://romm-nx.aaaoz.fr/romm-nx/stable/manifest.json";
        std::string update_channel = "stable";
        bool check_updates_on_startup = false;

        std::map<std::string, std::string> rom_paths;
    };

}

