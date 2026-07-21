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

        // Global default grid view mode — set from Settings > General, applies
        // to any platform without its own override below.
        GridViewMode GetGridViewMode() const { return grid_view_mode; }
        void SetGridViewMode(GridViewMode m) { grid_view_mode = m; }
        std::string GetGridViewModeString() const;

        // Effective view mode for a specific platform: its own override if one
        // was set (via the in-game Y-Menu), otherwise the global default.
        GridViewMode GetGridViewMode(const std::string& platform_slug) const;
        std::string GetGridViewModeString(const std::string& platform_slug) const;
        // Per-platform override, set from the in-game Y-Menu — affects only
        // this platform, independent of the global default above.
        void SetGridViewModeForPlatform(const std::string& platform_slug, GridViewMode m);

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

        // File browser write policy: false (default) restricts create/rename/
        // delete to the ROMs folders; true allows writes anywhere on the SD.
        bool FileBrowserWriteAnywhere() const { return filebrowser_write_anywhere; }
        void SetFileBrowserWriteAnywhere(bool enabled) { filebrowser_write_anywhere = enabled; }

        // Boot chime + looping menu background track (Settings > Theme).
        // Values are romfs:/audio/{startup,theme}/<key>.mp3 keys, or "none".
        const std::string& GetStartupSound() const { return startup_sound; }
        void SetStartupSound(const std::string& key) { startup_sound = key; }
        const std::string& GetThemeSound() const { return theme_sound; }
        void SetThemeSound(const std::string& key) { theme_sound = key; }

        // 0-100, independent per track like HorizonOS's own volume sliders —
        // AudioManager applies the matching one whenever it starts playback.
        int GetStartupVolume() const { return startup_volume; }
        void SetStartupVolume(int volume) { startup_volume = volume; }
        int GetAmbientVolume() const { return ambient_volume; }
        void SetAmbientVolume(int volume) { ambient_volume = volume; }

        // Host serving <base>/startup/<key>.mp3 and <base>/theme/<key>.mp3 —
        // tracks aren't bundled in the NRO, they're downloaded on first use
        // and cached to SD (see AudioManager).
        const std::string& GetAudioBaseUrl() const { return audio_base_url; }
        void SetAudioBaseUrl(const std::string& url) { audio_base_url = url; }

        std::string GetUpdateManifestUrl() const { return update_manifest_url; }
        void SetUpdateManifestUrl(const std::string& url) { update_manifest_url = url; }

        std::string GetUpdateChannel() const { return update_channel; }
        void SetUpdateChannel(const std::string& channel) { update_channel = channel; }

        bool CheckUpdatesOnStartup() const { return check_updates_on_startup; }
        void SetCheckUpdatesOnStartup(bool check) { check_updates_on_startup = check; }

        // Version the user explicitly dismissed ("Later") on the startup
        // update popup — suppresses re-showing it for that exact version on
        // future launches. A newer version still triggers the popup again.
        const std::string& GetDismissedUpdateVersion() const { return dismissed_update_version; }
        void SetDismissedUpdateVersion(const std::string& version) { dismissed_update_version = version; }

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
        bool filebrowser_write_anywhere = false;
        std::string startup_sound = "none";
        std::string theme_sound = "none";
        int startup_volume = 100;
        int ambient_volume = 100;
        // Best-guess default following the same host/path convention as the
        // existing update_manifest_url — confirm or correct via Settings.
        std::string audio_base_url = "https://romm-nx.aaaoz.fr/romm-nx/audio/";

        std::string update_manifest_url = "https://romm-nx.aaaoz.fr/romm-nx/stable/manifest.json";
        std::string update_channel = "stable";
        bool check_updates_on_startup = true;
        std::string dismissed_update_version;

        std::map<std::string, std::string> rom_paths;
        std::map<std::string, GridViewMode> platform_grid_view_mode;
    };

}

