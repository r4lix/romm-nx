#pragma once

#include <string>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace romm::model {
    
    enum class CoversQuality {
        SD,
        Balanced,
        HD
    };

    enum class GridViewMode {
        Default,
        Big,
        Detail // Single-column title list with a live detail panel beside it.
    };

    // How the library's left sidebar draws the platform list.
    enum class PlatformSelectorStyle {
        Text,   // Scrolling list of platform names (the original, and default)
        Banners // Column of platform logo art, selection pinned to a fixed slot
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

        bool IsValid() const { return is_valid; }
        const std::string& GetErrorMessage() const { return error_message; }

        std::string GetMaskedApiKey() const;

        // General settings.
        // UI language: "auto" (follow the console), "en" or "fr". A config file
        // written before this setting existed has no key at all, which is
        // exactly the "auto" case — see the default below.
        const std::string& GetLanguage() const { return language; }
        void SetLanguage(const std::string& lang) { language = lang; }

        const std::string& GetTheme() const { return theme; }
        void SetTheme(const std::string& t) { theme = t; }

        // Library sidebar style (Settings > Theme). Text is the default so an
        // existing config — which has no key at all — keeps the list it had.
        PlatformSelectorStyle GetPlatformSelectorStyle() const { return platform_selector_style; }
        void SetPlatformSelectorStyle(PlatformSelectorStyle s) { platform_selector_style = s; }
        std::string GetPlatformSelectorStyleString() const;

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

        // --- OTA update channels ------------------------------------------
        // Two independent manifests sit side by side under the base URL, one
        // directory per channel: <base>/<channel>/manifest.json, with that
        // channel's NRO next to it (nro.url is relative to the manifest).
        static constexpr const char* kChannelStable = "stable";
        static constexpr const char* kChannelTesting = "testing";
        // Anything that isn't a known channel — a hand-edited config, a value
        // written by a future build — falls back to stable.
        static std::string NormalizeUpdateChannel(const std::string& channel);

        // Channel the user is tracking (Settings > Updates).
        const std::string& GetUpdateChannel() const { return update_channel; }
        void SetUpdateChannel(const std::string& channel) { update_channel = NormalizeUpdateChannel(channel); }

        // Channel the running NRO was installed from. Differs from the tracked
        // channel only between a channel switch and the install that follows
        // it — that gap is what lets UpdateManager offer a build whose
        // version_code is *lower* than the running one (testing -> stable).
        const std::string& GetInstalledUpdateChannel() const { return installed_update_channel; }
        void SetInstalledUpdateChannel(const std::string& channel) { installed_update_channel = NormalizeUpdateChannel(channel); }

        // Manifest URL for the tracked channel, or for a named one. Returns
        // the override verbatim when one is set.
        std::string GetUpdateManifestUrl() const;
        std::string GetUpdateManifestUrlForChannel(const std::string& channel) const;

        const std::string& GetUpdateBaseUrl() const { return update_base_url; }
        void SetUpdateBaseUrl(const std::string& url);

        // Pins the manifest URL, bypassing the channel layout entirely. Only
        // reachable by hand-editing config.json with a URL that doesn't follow
        // the <base>/<channel>/manifest.json convention (see Load).
        const std::string& GetUpdateManifestUrlOverride() const { return update_manifest_url_override; }
        void SetUpdateManifestUrlOverride(const std::string& url) { update_manifest_url_override = url; }

        bool CheckUpdatesOnStartup() const { return check_updates_on_startup; }
        void SetCheckUpdatesOnStartup(bool check) { check_updates_on_startup = check; }

        // Version the user explicitly dismissed ("Later") on the startup
        // update popup — suppresses re-showing it for that exact version on
        // future launches. A newer version still triggers the popup again.
        const std::string& GetDismissedUpdateVersion() const { return dismissed_update_version; }
        void SetDismissedUpdateVersion(const std::string& version) { dismissed_update_version = version; }

        // Single configurable base directory. Games always install to
        // <base>/roms/<system>/, so an override per platform is unnecessary.
        const std::string& GetRomsBaseDir() const { return roms_base_dir; }
        void SetRomsBaseDir(const std::string& dir);
        std::string GetRomPath(const std::string& platform) const;

        // --- Platform visibility (Settings > Platforms) -------------------
        // Purely a UI filter over the platform browser: nothing here touches
        // ROM files, installed_index.json, download paths, covers or cache.
        // Keyed by NormalizePlatformId(), so aliases of the same platform
        // share one entry; accepts a raw RomM slug or a display name.
        bool IsPlatformVisible(const std::string& slug) const;
        void SetPlatformVisible(const std::string& slug, bool visible);
        // Restores the shipped default-visible / default-hidden lists. Any
        // platform outside the catalogue goes back to hidden.
        void ResetPlatformVisibilityDefaults();
        // Unhides everything currently known, catalogue or server-detected.
        void ShowAllPlatforms();

        // Records the platforms the server just returned. Ones seen for the
        // first time take their catalogue default (unknown => hidden), which
        // is what makes "hidden by default" stick without re-hiding a platform
        // the user has since enabled. Returns true if anything changed, so the
        // caller can Save() exactly once.
        bool RegisterDetectedPlatforms(const std::vector<std::string>& slugs);

        const std::set<std::string>& GetHiddenPlatformIds() const { return hidden_platforms; }
        const std::set<std::string>& GetKnownPlatformIds() const { return known_platforms; }

    private:
        ConfigManager();

        // Serializes the config.json write. UpdateManager records the installed
        // channel from its worker thread, which can otherwise interleave with a
        // save triggered from the UI and leave a truncated file behind.
        std::mutex save_mutex;

        std::string romm_host;
        std::string api_key;
        bool is_valid = false;
        std::string error_message;

        std::string language = "auto";
        std::string theme = "romm_brand";
        PlatformSelectorStyle platform_selector_style = PlatformSelectorStyle::Text;
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

        // Root under which every platform's games live, one subfolder per
        // system ("roms/" + slug). Always ends in '/'.
        std::string roms_base_dir = "sdmc:/romm-nx/";

        // Directory holding the per-channel subdirectories; always ends in '/'.
        std::string update_base_url = "https://romm-nx.aaaoz.fr/romm-nx/";
        std::string update_manifest_url_override;
        std::string update_channel = kChannelStable;
        std::string installed_update_channel = kChannelStable;
        bool check_updates_on_startup = true;
        std::string dismissed_update_version;

        std::map<std::string, GridViewMode> platform_grid_view_mode;

        // Canonical ids the user has hidden from the platform browser, and
        // every canonical id romm-nx has ever seen. The second list is what
        // lets a *newly* detected platform default to hidden without also
        // re-hiding one the user deliberately enabled earlier.
        std::set<std::string> hidden_platforms;
        std::set<std::string> known_platforms;
    };

}

