#include "SettingsLayout.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/RomPathManager.hpp"
#include "../model/CacheManager.hpp"
#include "../model/DownloadManager.hpp"
#include "../model/ScreenWakeManager.hpp"
#include "../Version.hpp"
#include "../model/UpdateManager.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../navigation/HttpClient.hpp"
#include "LibraryLayout.hpp"
#include "MainApplication.hpp"
#include "GlobalProgressBar.hpp"
#include "../model/AudioManager.hpp"
#include "../model/PlatformCatalog.hpp"
#include "../model/DataModel.hpp"
#include "../i18n/I18n.hpp"
#include <cstdlib>
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <algorithm>

namespace romm::ui {

    std::string TranslateViewMode(const std::string& stored_mode) {
        if (stored_mode == "Big") return romm::i18n::tr("settings.view_mode.big");
        if (stored_mode == "Detail") return romm::i18n::tr("settings.view_mode.detail");
        return romm::i18n::tr("settings.view_mode.default");
    }

    std::string TranslateCoversQuality(const std::string& stored_quality) {
        if (stored_quality == "SD") return romm::i18n::tr("settings.covers_quality.sd");
        if (stored_quality == "HD") return romm::i18n::tr("settings.covers_quality.hd");
        return romm::i18n::tr("settings.covers_quality.balanced");
    }

    namespace {
        // "auto" -> "en" -> "fr" -> "auto". Matches the row's displayed order.
        const char* NextLanguageSetting(const std::string& current) {
            if (current == romm::i18n::kSettingAuto) return romm::i18n::kSettingEnglish;
            if (current == romm::i18n::kSettingEnglish) return romm::i18n::kSettingFrench;
            return romm::i18n::kSettingAuto;
        }

        std::string LanguageSettingLabel(const std::string& setting) {
            if (setting == romm::i18n::kSettingEnglish) return romm::i18n::tr("settings.general.language.en");
            if (setting == romm::i18n::kSettingFrench) return romm::i18n::tr("settings.general.language.fr");
            return romm::i18n::tr("settings.general.language.auto");
        }

        std::string truncatePath(const std::string& path, size_t max_len = 45) {
            if (path.size() <= max_len) return path;
            
            size_t last_slash = path.find_last_of('/');
            if (last_slash != std::string::npos && last_slash > 10 && path.size() - last_slash < 25) {
                std::string start = path.substr(0, 12);
                std::string end = path.substr(last_slash);
                return start + "/..." + end;
            }
            
            return path.substr(0, max_len - 3) + "...";
        }

        const std::vector<PlatformPathDescriptor> SUPPORTED_PLATFORMS = {
            {"PS1", "PlayStation", "psx", "ps1", true, true, false, false, false},
            {"PS2", "PlayStation 2", "ps2", "ps2", true, true, false, false, false},
            {"PSP", "PSP", "psp", "psp", true, true, false, false, false},
            {"DS", "Nintendo DS", "nds", "nds", true, true, false, false, false},
            {"GB", "Game Boy", "gb", "gb", true, true, false, false, false},
            {"GBC", "Game Boy Color", "gbc", "gbc", true, true, false, false, false},
            {"GBA", "Game Boy Advance", "gba", "gba", true, true, false, false, false},
            {"3DS", "Nintendo 3DS", "3ds", "3ds", true, true, false, false, false}
        };

        std::string GetCoverCachePath(const std::string& internal_slug) {
            if (internal_slug == "psx") {
                return "sdmc:/switch/romm-nx/cache/covers/small/";
            }
            return "sdmc:/switch/romm-nx/cache/covers/" + internal_slug + "/";
        }

        // Ordered so cycling (A to advance) has a stable, predictable order —
        // must match ConfigManager's validation lists.
        const std::vector<std::string> kStartupSoundKeys = {"none", "ps1", "ps2", "ps3", "ps3-old", "ps4", "ps5", "psp"};
        const std::vector<std::string> kThemeSoundKeys = {"none", "ps2-remix", "ps4", "ps5", "psvita"};

        // Sound-pack names are product names ("PlayStation 3 (New)") and a
        // credit ("PS2 Remix (by Meech)") — proper nouns, identical in every
        // language. Only the "none" entry is a real UI word.
        std::string StartupSoundDisplayName(const std::string& key) {
            if (key == "none") return romm::i18n::tr("common.none");
            if (key == "ps1") return "PlayStation 1";
            if (key == "ps2") return "PlayStation 2";
            if (key == "ps3") return "PlayStation 3 (New)";
            if (key == "ps3-old") return "PlayStation 3 (Old)";
            if (key == "ps4") return "PlayStation 4";
            if (key == "ps5") return "PlayStation 5";
            if (key == "psp") return "PlayStation Portable";
            return key;
        }

        std::string ThemeSoundDisplayName(const std::string& key) {
            if (key == "none") return romm::i18n::tr("common.none");
            if (key == "ps2-remix") return "PS2 Remix (by Meech)";
            if (key == "ps4") return "PlayStation 4";
            if (key == "ps5") return "PlayStation 5";
            if (key == "psvita") return "PlayStation Vita";
            return key;
        }

        // OTA update channels, in the order the Channel row cycles through
        // them. Stable first — it's the default and where most users stay.
        const std::vector<std::string> kUpdateChannelKeys = {
            romm::model::ConfigManager::kChannelStable,
            romm::model::ConfigManager::kChannelTesting,
        };

        // Splits `text` into lines of at most `max_chars` characters, breaking
        // on a space when there is one and mid-word otherwise (URLs have none).
        //
        // Budgeted by character count rather than measured in pixels: the
        // settings screen re-renders every visible string each frame, so
        // measuring candidate widths would multiply an already hot cost. The
        // budgets passed in are conservative for the panel width. Counts
        // characters, not bytes, so an accented French string is never cut in
        // the middle of a codepoint and drawn as a broken glyph.
        std::vector<std::string> WrapText(const std::string& text, size_t max_chars) {
            std::vector<std::string> lines;
            if (max_chars == 0 || text.empty()) return lines;

            size_t line_start = 0;
            size_t chars = 0;
            size_t last_space = std::string::npos;

            size_t i = 0;
            while (i < text.size()) {
                const unsigned char c = static_cast<unsigned char>(text[i]);
                if (c == ' ') last_space = i;

                if (chars >= max_chars) {
                    const bool break_on_space = (last_space != std::string::npos && last_space > line_start);
                    const size_t brk = break_on_space ? last_space : i;
                    lines.push_back(text.substr(line_start, brk - line_start));
                    line_start = break_on_space ? brk + 1 : brk;
                    chars = 0;
                    last_space = std::string::npos;
                    i = line_start;
                    continue;
                }

                // Advance one UTF-8 codepoint.
                size_t adv = 1;
                if (c >= 0xF0) adv = 4;
                else if (c >= 0xE0) adv = 3;
                else if (c >= 0xC0) adv = 2;

                chars++;
                i += adv;
            }
            if (line_start < text.size()) {
                lines.push_back(text.substr(line_start));
            }
            return lines;
        }

        std::string UpdateChannelDisplayName(const std::string& channel) {
            if (channel == romm::model::ConfigManager::kChannelTesting) {
                return romm::i18n::tr("settings.updates.channel.testing");
            }
            return romm::i18n::tr("settings.updates.channel.stable");
        }

        // The full catalogue of downloadable packs (startup chimes + ambience
        // loops) — "Download Sound Pack" fetches every one of these that
        // isn't already cached, in a single batch. "none" is excluded —
        // there's nothing to download for it.
        struct SoundPackEntry { std::string kind; std::string key; };
        const std::vector<SoundPackEntry> kSoundPacks = {
            {"startup", "ps1"}, {"startup", "ps2"}, {"startup", "ps3"}, {"startup", "ps3-old"},
            {"startup", "ps4"}, {"startup", "ps5"}, {"startup", "psp"},
            {"theme", "ps2-remix"}, {"theme", "ps4"}, {"theme", "ps5"}, {"theme", "psvita"},
        };

        // Startup Sound / Menu Ambience only ever cycle through packs that
        // are actually on the SD card (plus "none", always available) — an
        // undownloaded pack is filtered out rather than shown as a dead end
        // that silently does nothing when picked.
        std::vector<std::string> SelectableKeys(const std::vector<std::string>& all_keys, const std::string& kind) {
            auto& audio_mgr = romm::model::AudioManager::Instance();
            std::vector<std::string> out;
            for (const auto& k : all_keys) {
                if (k == "none" || audio_mgr.IsCached(kind, k)) {
                    out.push_back(k);
                }
            }
            return out;
        }

        // Advances `current` to the entry `direction` steps away in `keys`
        // (1 = next, -1 = previous), wrapping around in either direction.
        std::string CycleKey(const std::vector<std::string>& keys, const std::string& current, int direction = 1) {
            auto it = std::find(keys.begin(), keys.end(), current);
            long idx = (it == keys.end()) ? 0 : (long)(it - keys.begin());
            long n = (long)keys.size();
            idx = ((idx + direction) % n + n) % n;
            return keys[(size_t)idx];
        }

        int ClampVolume(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }

        // Single source of truth for the category list — OnRender's list and
        // NavigationManager's Down-navigation clamp (via GetCategoriesCount())
        // both read this instead of independently hardcoding the same count.
        // Index order is load-bearing: the per-category branches below and in
        // HandleOptionAction()/GetOptionsCount() key off it, which is why these
        // are i18n keys rather than display strings.
        const std::vector<std::string> kSettingsCategoryKeys = {
            "settings.category.general",
            "settings.category.theme",
            "settings.category.connection",
            "settings.category.rom_paths",
            "settings.category.platforms",
            "settings.category.advanced",
            "settings.category.updates",
            "settings.category.debug"
        };

        // --- Settings > Platforms ------------------------------------------
        // One row per canonical platform: everything romm-nx ships knowledge of
        // (GetPlatformCatalog) merged with whatever the connected server
        // returned, de-duplicated by NormalizePlatformId so "ps1", "psx" and
        // "PlayStation" can never appear as three rows.
        struct PlatformVisibilityRow {
            std::string canonical_id;
            std::string display_name;
        };

        // The two fixed action rows that sit above the platform list.
        constexpr size_t kPlatformActionRows = 2; // 0 = Show All, 1 = Reset Defaults

        std::vector<PlatformVisibilityRow> g_platform_rows;

        void RebuildPlatformRows(const std::shared_ptr<romm::model::DataModel>& model) {
            std::vector<PlatformVisibilityRow> rows;

            auto push = [&rows](const std::string& id, const std::string& name) {
                if (id.empty()) return;
                for (const auto& existing : rows) {
                    if (existing.canonical_id == id) return; // alias of a row we already have
                }
                rows.push_back({id, name});
            };

            for (const auto& entry : romm::model::GetPlatformCatalog()) {
                push(entry.id, entry.display_name);
            }
            if (model) {
                // GetAllPlatforms(), not GetPlatforms(): the filtered list is
                // precisely the one that can't show you what to un-hide.
                for (const auto& plat : model->GetAllPlatforms()) {
                    const std::string id = romm::model::ResolvePlatformIdentity(plat.slug, plat.name);
                    push(id, romm::model::GetPlatformDisplayName(id, plat.name));
                }
            }

            g_platform_rows = std::move(rows);
        }

        const std::vector<PlatformVisibilityRow>& PlatformRows() {
            // GetOptionsCount() is static and can be asked for the row count
            // before any refresh has run; the catalogue alone is a valid answer.
            if (g_platform_rows.empty()) {
                RebuildPlatformRows(nullptr);
            }
            return g_platform_rows;
        }

        // Which ROM Paths rows are selectable (BIOS/Save data are "Coming
        // later" placeholders) — OnRender's rows_data and NavigationManager's
        // row-navigation clamp both read this instead of independently
        // hardcoding the same count. Assumes enabled rows are a contiguous
        // prefix (true today: ROMs, Cover cache enabled; BIOS, Save data not).
        const std::vector<bool> kRomPathRowDisabled = {false, false, true, true};
    }

    // --- SettingsConfirmModal Implementation ---

    SettingsConfirmModal::SettingsConfirmModal() : Element::Element(), x(0), y(0), w(1920), h(1080) {}

    void SettingsConfirmModal::Show(const std::string& title_text, const std::string& msg_text, ConfirmAction act, std::function<void()> confirm_cb) {
        title = title_text;
        message = msg_text;
        action = act;
        on_confirm = confirm_cb;
        active = true;
    }

    void SettingsConfirmModal::Hide() {
        active = false;
        action = ConfirmAction::None;
    }

    void SettingsConfirmModal::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x_coord, const s32 y_coord) {
        if (!active) return;

        // Dim background
        drawer->RenderRectangleFill(pu::ui::Color(0, 0, 0, 180), 0, 0, 1920, 1080);

        // Center modal box
        s32 modal_w = 750;
        s32 modal_h = 340;
        s32 modal_x = (1920 - modal_w) / 2;
        s32 modal_y = (1080 - modal_h) / 2;

        pu::ui::Color border_color(45, 50, 62, 255); // Slate Border Grey (#2D323E)
        pu::ui::Color bg_color(16, 18, 22, 255);     // Web Dark Slate (#101216)

        drawer->RenderRoundedRectangleFill(border_color, modal_x, modal_y, modal_w, modal_h, 16);
        drawer->RenderRoundedRectangleFill(bg_color, modal_x + 4, modal_y + 4, modal_w - 8, modal_h - 8, 12);

        pu::ui::Color text_color(237, 229, 251, 255); // Cream / White (#EDE5FB)

        // Render Title
        auto title_tex = pu::ui::render::RenderText("Orbitron@30", title, text_color);
        if (title_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(title_tex);
            drawer->RenderTexture(title_tex, modal_x + (modal_w - tw) / 2, modal_y + 35);
            pu::ui::render::DeleteTexture(title_tex);
        }

        // Render Message - Use registered "Ubuntu@20"
        auto msg_tex = pu::ui::render::RenderText("Ubuntu@20", message, pu::ui::Color(180, 180, 180, 255), modal_w - 80);
        if (msg_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(msg_tex);
            drawer->RenderTexture(msg_tex, modal_x + (modal_w - tw) / 2, modal_y + 115);
            pu::ui::render::DeleteTexture(msg_tex);
        }

        // Render Buttons Hint
        auto btn_tex = pu::ui::render::RenderText("Orbitron@24", romm::i18n::tr("hint.modal.confirm_cancel"), text_color);
        if (btn_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(btn_tex);
            drawer->RenderTexture(btn_tex, modal_x + (modal_w - tw) / 2, modal_y + modal_h - 75);
            pu::ui::render::DeleteTexture(btn_tex);
        }
    }

    void SettingsConfirmModal::HandleInput(u64 keys_down) {
        if (!active) return;
        if (keys_down & HidNpadButton_B) {
            Hide();
        } else if (keys_down & HidNpadButton_A) {
            if (on_confirm) {
                on_confirm();
            }
            Hide();
        }
    }


    // --- SettingsCard Implementation ---

    SettingsCard::SettingsCard(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element::Element(), x(x), y(y), w(w), h(h), nav_mgr(nav), connection_status_color(190, 180, 225, 255) {
        cached_statuses.resize(SUPPORTED_PLATFORMS.size());
        ResetConnectionStatus();
        TriggerRecalculateCache();
        RefreshPathStatuses();
    }

    void SettingsCard::RefreshConfigTextures() {
        TriggerRecalculateCache();
    }

    void SettingsCard::ResetConnectionStatus() {
        connection_test_status = romm::i18n::tr("settings.connection.not_tested");
        connection_status_color = pu::ui::Color(190, 180, 225, 255);
    }

    void SettingsCard::TriggerConnectionTest() {
        connection_test_status = romm::i18n::tr("settings.connection.testing");
        connection_status_color = pu::ui::Color(230, 199, 167, 255); // Cream (#E6C7A7)

        auto& config = romm::model::ConfigManager::Instance();
        std::string host = config.GetRommHost();
        std::string key = config.GetApiKey();

        if (host.empty() || key.empty()) {
            connection_test_status = romm::i18n::tr("settings.connection.failed_missing");
            connection_status_color = pu::ui::Color(231, 76, 60, 255); // Red
            return;
        }

        std::string url = host + "/api/platforms";
        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + key},
            {"Accept", "application/json"}
        };

        std::cout << "[CONNECTION] Testing connection status..." << std::endl;

        HttpClient::runAsync([this, url, headers]() {
            auto http_res = HttpClient::getSync(url, headers);
            if (http_res.success && http_res.statusCode == 200) {
                connection_test_status = romm::i18n::tr("settings.connection.connected");
                connection_status_color = pu::ui::Color(46, 204, 113, 255); // Green
            } else {
                // curl/HTTP diagnostics: a protocol code or a library string,
                // substituted into the localized wrapper rather than translated.
                std::string err = http_res.error;
                if (err.empty()) {
                    err = "HTTP " + std::to_string(http_res.statusCode);
                }
                connection_test_status = romm::i18n::format("settings.connection.failed", {{"error", err}});
                connection_status_color = pu::ui::Color(231, 76, 60, 255); // Red
            }
        });
    }

    void SettingsCard::TriggerRecalculateCache() {
        calculating_cache = true;

        HttpClient::runAsync([this]() {
            auto stats = romm::model::CacheManager::Instance().CalculateSize();
            cache_cover_bytes = stats.cover_size;
            cache_cover_count = stats.cover_count;
            cache_total_bytes = stats.total_size;
            calculating_cache = false;
        });
    }

    void SettingsCard::RecalculateCacheSize() {
        TriggerRecalculateCache();
    }

    void SettingsCard::RefreshPathStatuses() {
        auto& config = romm::model::ConfigManager::Instance();
        for (size_t i = 0; i < SUPPORTED_PLATFORMS.size(); ++i) {
            const auto& plat = SUPPORTED_PLATFORMS[i];
            
            // ROMs / Downloads
            std::string rom_path = config.GetRomPath(plat.internal_slug);
            bool pattern_ok = romm::model::RomPathManager::ValidatePath(rom_path);
            if (!pattern_ok) {
                cached_statuses[i].roms_status = PathStatus::Invalid;
            } else {
                struct stat st;
                bool exists = (stat(rom_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                if (exists) {
                    cached_statuses[i].roms_status = PathStatus::Valid;
                } else {
                    cached_statuses[i].roms_status = PathStatus::Missing;
                }
            }

            // Cover cache
            std::string cover_path = GetCoverCachePath(plat.internal_slug);
            struct stat st_cov;
            bool cov_exists = (stat(cover_path.c_str(), &st_cov) == 0 && S_ISDIR(st_cov.st_mode));
            if (cov_exists) {
                cached_statuses[i].cover_status = PathStatus::ReadOnly;
            } else {
                cached_statuses[i].cover_status = PathStatus::ReadOnly;
            }

            // BIOS
            cached_statuses[i].bios_status = PathStatus::ComingLater;

            // Save data
            cached_statuses[i].save_status = PathStatus::ComingLater;
        }
    }

    void SettingsCard::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        size_t active_cat = nav->GetSelectedSettingsCategoryIdx();
        size_t active_opt = nav->GetSelectedSettingsOptionIdx();
        bool is_cat_focused = (nav->GetSettingsFocus() == romm::ui::SettingsFocusArea::CategoryList);

        // 1. Draw Left Category Panel
        s32 cat_panel_w = 420;
        pu::ui::Color border_color(45, 50, 62, 255); // Slate Border Grey
        pu::ui::Color fill_color(30, 34, 43, 255);   // Web Charcoal Grey

        drawer->RenderRoundedRectangleFill(border_color, x_coord, y_coord, cat_panel_w, h, 16);
        drawer->RenderRoundedRectangleFill(fill_color, x_coord + 4, y_coord + 4, cat_panel_w - 8, h - 8, 12);

        const std::vector<std::string>& categories = kSettingsCategoryKeys;
        s32 start_y = y_coord + 40;
        s32 row_h = 75;
        s32 row_spacing = 20;

        for (size_t i = 0; i < categories.size(); ++i) {
            s32 ry = start_y + i * (row_h + row_spacing);
            bool is_selected = (i == active_cat);

            pu::ui::Color r_bg;
            pu::ui::Color r_border;
            s32 r_border_w = 0;

            if (is_selected) {
                if (is_cat_focused) {
                    r_bg = pu::ui::Color(85, 63, 152, 255); // Violet highlight capsule (#553F98)
                    r_border = pu::ui::Color(230, 199, 167, 255); // Cream border
                    r_border_w = 3;
                } else {
                    r_bg = pu::ui::Color(85, 63, 152, 120); // Subdued violet capsule (no active focus)
                    r_border = pu::ui::Color(45, 50, 62, 255); // Standard border
                    r_border_w = 2;
                }
            } else {
                r_bg = pu::ui::Color(16, 18, 22, 255); // Web Dark Slate (#101216)
                r_border = pu::ui::Color(45, 50, 62, 255); // Slate Border Grey (#2D323E)
                r_border_w = 2;
            }

            s32 rx = x_coord + 25;
            s32 rw = cat_panel_w - 50;

            drawer->RenderRoundedRectangleFill(r_border, rx, ry, rw, row_h, 8);
            drawer->RenderRoundedRectangleFill(r_bg, rx + r_border_w, ry + r_border_w, rw - (r_border_w * 2), row_h - (r_border_w * 2), 6);

            pu::ui::Color text_color(237, 229, 251, 255);
            auto tex = pu::ui::render::RenderText("Orbitron@30", romm::i18n::tr(categories[i]), text_color);
            if (tex) {
                s32 th = pu::ui::render::GetTextureHeight(tex);
                drawer->RenderTexture(tex, rx + 30, ry + (row_h - th) / 2);
                pu::ui::render::DeleteTexture(tex);
            }

            // Keyed off the category's identity, not its translated label.
            if (categories[i] == "settings.category.updates" &&
                romm::model::UpdateManager::Instance().GetState() == romm::model::UpdateState::UpdateAvailable) {
                s32 dot_radius = 7;
                drawer->RenderCircleFill(pu::ui::Color(231, 76, 60, 255), rx + rw - dot_radius - 12, ry + dot_radius + 12, dot_radius);
            }
        }

        // 2. Draw Right Option Panel
        s32 opt_panel_x = x_coord + cat_panel_w + 30;
        s32 opt_panel_w = w - cat_panel_w - 30;

        drawer->RenderRoundedRectangleFill(border_color, opt_panel_x, y_coord, opt_panel_w, h, 16);
        drawer->RenderRoundedRectangleFill(fill_color, opt_panel_x + 4, y_coord + 4, opt_panel_w - 8, h - 8, 12);

        // Populate options entries exactly as requested (no placeholder rows)
        std::vector<OptionRenderEntry> options;
        auto& config = romm::model::ConfigManager::Instance();

        // ON/OFF is a UI word pair, so it comes from the dictionary too.
        const std::string on_text = romm::i18n::tr("common.on");
        const std::string off_text = romm::i18n::tr("common.off");
        auto on_off = [&](bool value) { return value ? on_text : off_text; };

        if (active_cat == 0) { // General
            options.push_back({romm::i18n::tr("settings.general.language"),
                               LanguageSettingLabel(config.GetLanguage())});
            options.push_back({romm::i18n::tr("settings.general.show_build_version"), on_off(config.ShowBuildVersion())});
            options.push_back({romm::i18n::tr("settings.general.confirm_uninstall"), on_off(config.ConfirmBeforeUninstall())});
            options.push_back({romm::i18n::tr("settings.general.show_installed_badge"), on_off(config.ShowInstalledBadge())});
            options.push_back({romm::i18n::tr("settings.general.screen_always_on"), on_off(config.ScreenAlwaysOn())});
            options.push_back({romm::i18n::tr("settings.general.covers_quality"),
                               TranslateCoversQuality(config.GetCoversQualityString())});
            options.push_back({romm::i18n::tr("settings.general.filebrowser_writes"),
                               romm::i18n::tr(config.FileBrowserWriteAnywhere()
                                                  ? "settings.general.filebrowser_writes.everywhere"
                                                  : "settings.general.filebrowser_writes.roms_only")});
            options.push_back({romm::i18n::tr("settings.general.default_view_mode"),
                               romm::i18n::format("settings.general.default_view_mode.value",
                                                  {{"mode", TranslateViewMode(config.GetGridViewModeString())}})});
        }
        else if (active_cat == 1) { // Theme
            auto& audio_mgr = romm::model::AudioManager::Instance();
            // "RomM Brand" is the theme's proper name, not a UI word.
            options.push_back({romm::i18n::tr("settings.theme.theme"), "RomM Brand"});

            if (audio_mgr.IsBatchDownloading()) {
                options.push_back({romm::i18n::tr("settings.theme.download_pack"),
                    romm::i18n::format("settings.theme.pack_downloading",
                                       {{"remaining", std::to_string(audio_mgr.GetBatchRemaining())}})});
            } else {
                size_t downloaded = 0;
                for (const auto& pack : kSoundPacks) {
                    if (audio_mgr.IsCached(pack.kind, pack.key)) downloaded++;
                }
                std::string status = (downloaded == kSoundPacks.size())
                    ? romm::i18n::tr("settings.theme.pack_all_downloaded")
                    : romm::i18n::format("settings.theme.pack_progress", {
                          {"downloaded", std::to_string(downloaded)},
                          {"total", std::to_string(kSoundPacks.size())}
                      });
                options.push_back({romm::i18n::tr("settings.theme.download_pack"), status});
            }

            // Only "none" plus already-downloaded packs are reachable here —
            // fetch new ones via "Download Sound Pack" above first.
            options.push_back({romm::i18n::tr("settings.theme.startup_sound"), StartupSoundDisplayName(config.GetStartupSound())});
            options.push_back({romm::i18n::tr("settings.theme.startup_volume"), "", false, config.GetStartupVolume()});
            options.push_back({romm::i18n::tr("settings.theme.menu_ambience"), ThemeSoundDisplayName(config.GetThemeSound())});
            options.push_back({romm::i18n::tr("settings.theme.ambience_volume"), "", false, config.GetAmbientVolume()});
            options.push_back({romm::i18n::tr("settings.theme.platform_selector"),
                               romm::i18n::tr(config.GetPlatformSelectorStyle() == romm::model::PlatformSelectorStyle::Banners
                                                  ? "settings.theme.platform_selector.banners"
                                                  : "settings.theme.platform_selector.text")});
        }
        else if (active_cat == 2) { // Connection
            // Host URL and masked key are configuration values, shown as-is.
            options.push_back({romm::i18n::tr("settings.connection.server_url"), truncatePath(config.GetRommHost())});
            options.push_back({romm::i18n::tr("settings.connection.api_key"), config.GetMaskedApiKey()});
            options.push_back({romm::i18n::tr("settings.connection.test"), connection_test_status, true});
        }
        else if (active_cat == 3) { // ROM Paths
            // Custom drawing logic handles this
        }
        else if (active_cat == 4) { // Platforms
            // Custom drawing logic handles this (needs its own scroll window)
        }
        else if (active_cat == 5) { // Advanced
            std::string cover_sz = romm::i18n::tr("settings.advanced.calculating");
            std::string total_sz = cover_sz;
            if (!calculating_cache) {
                char cover_buf[64];
                std::snprintf(cover_buf, sizeof(cover_buf), "%.2f", (double)cache_cover_bytes / (1024.0 * 1024.0));
                cover_sz = romm::i18n::format("settings.advanced.cover_size_value", {
                    {"size", cover_buf},
                    {"count", std::to_string(cache_cover_count)}
                });

                char total_buf[64];
                std::snprintf(total_buf, sizeof(total_buf), "%.2f", (double)cache_total_bytes / (1024.0 * 1024.0));
                total_sz = romm::i18n::format("settings.advanced.size_value", {{"size", total_buf}});
            }
            const std::string trigger_clear = romm::i18n::tr("settings.advanced.trigger_clear");
            options.push_back({romm::i18n::tr("settings.advanced.cover_cache_size"), cover_sz});
            options.push_back({romm::i18n::tr("settings.advanced.total_cache_size"), total_sz});
            options.push_back({romm::i18n::tr("settings.advanced.clear_cover_cache"), trigger_clear, true});
            options.push_back({romm::i18n::tr("settings.advanced.clear_all_cache"), trigger_clear, true});
            options.push_back({romm::i18n::tr("settings.advanced.auto_clear"), on_off(config.IsAutoClearEnabled())});
            options.push_back({romm::i18n::tr("settings.advanced.max_size"),
                               romm::i18n::format("settings.advanced.size_value", {{"size", std::to_string(config.GetMaxSizeMb())}})});
            options.push_back({romm::i18n::tr("settings.advanced.max_age"),
                               romm::i18n::format("settings.advanced.age_value", {{"days", std::to_string(config.GetMaxAgeDays())}})});
        }
        else if (active_cat == 6) { // Updates
            // Rows are the things you can act on, and nothing else — every row
            // value here is a short word or a version. The read-only detail
            // (build, installed channel, manifest URL, status, changelog) is
            // drawn in the bounded info panel below the list, where it can wrap
            // instead of running into its own label.
            auto& um = romm::model::UpdateManager::Instance();
            const auto state = um.GetState();

            options.push_back({romm::i18n::tr("settings.updates.channel"),
                               UpdateChannelDisplayName(config.GetUpdateChannel())});
            options.push_back({romm::i18n::tr("settings.updates.check_on_startup"), on_off(config.CheckUpdatesOnStartup())});
            options.push_back({romm::i18n::tr("settings.updates.check_now"), romm::i18n::tr("settings.updates.trigger_check"), true});

            if (state == romm::model::UpdateState::UpdateAvailable) {
                auto manifest = um.GetRemoteManifest();
                const bool is_switch = um.IsOfferedBuildChannelSwitch();
                options.push_back({romm::i18n::tr(is_switch ? "settings.updates.switch_available"
                                                            : "settings.updates.available"),
                                   romm::i18n::format(is_switch ? "settings.updates.available.switch"
                                                                : "settings.updates.available.install", {
                                       {"version", manifest.version},
                                       {"code", std::to_string(manifest.version_code)}
                                   }), true});
            }

            if (um.CanRestoreBackup()) {
                options.push_back({romm::i18n::tr("settings.updates.restore_backup"),
                                   romm::i18n::tr("settings.updates.trigger_restore"), true});
            }
        }
        else if (active_cat == 7) { // Debug
            options.push_back({romm::i18n::tr("settings.debug.build_version"),
                               "v" + romm::ROMM_NX_VERSION + " (" + std::to_string(romm::ROMM_NX_VERSION_CODE) + ")"});
            // Both channels, unconditionally: which one is selected and which
            // one the running NRO actually came from is the first thing worth
            // knowing when an update behaves unexpectedly.
            options.push_back({romm::i18n::tr("settings.debug.update_channel"),
                               UpdateChannelDisplayName(config.GetUpdateChannel())});
            options.push_back({romm::i18n::tr("settings.debug.installed_channel"),
                               UpdateChannelDisplayName(config.GetInstalledUpdateChannel())});
            options.push_back({romm::i18n::tr("settings.debug.config_path"), truncatePath("sdmc:/switch/romm-nx/config.json")});
            options.push_back({romm::i18n::tr("settings.debug.index_entries"), std::to_string(romm::model::DownloadManager::Instance().GetInstalledIndex().size())});

            // Populating active download title
            std::string active_title = romm::i18n::tr("common.none");
            auto active_task = romm::model::DownloadManager::Instance().GetActiveDownloadSnapshot();
            if (active_task.rom_id > 0 && (active_task.state == romm::model::DownloadState::DownloadingGame || active_task.state == romm::model::DownloadState::DownloadingCover || active_task.state == romm::model::DownloadState::SyncingCover || active_task.state == romm::model::DownloadState::Preparing)) {
                active_title = active_task.title;
                if (active_title.size() > 22) {
                    active_title = active_title.substr(0, 19) + "...";
                }
            }
            options.push_back({romm::i18n::tr("settings.debug.active_download"), active_title});
            options.push_back({romm::i18n::tr("settings.debug.queue_count"), std::to_string(romm::model::DownloadManager::Instance().GetQueueSnapshot().size())});
            options.push_back({romm::i18n::tr("settings.debug.export"), romm::i18n::tr("settings.debug.trigger_export"), true});
        }

        if (active_cat != 3 && active_cat != 4) {
            // Draw Options List (real rows only)
            s32 opt_start_y = y_coord + 40;
            s32 opt_row_h = 75;
            s32 opt_spacing = 15;
            s32 opt_row_w = opt_panel_w - 60;

            for (size_t j = 0; j < options.size(); ++j) {
                s32 ry = opt_start_y + j * (opt_row_h + opt_spacing);
                bool is_selected = (j == active_opt && !is_cat_focused); // Highlight right row only when OptionList is focused

                pu::ui::Color r_bg;
                pu::ui::Color r_border;
                s32 r_border_w = 0;

                if (is_selected) {
                    r_bg = pu::ui::Color(85, 63, 152, 255); // Violet highlight capsule
                    r_border = pu::ui::Color(230, 199, 167, 255); // Cream border
                    r_border_w = 3;
                } else {
                    r_bg = pu::ui::Color(16, 18, 22, 255); // Web Dark Slate
                    r_border = pu::ui::Color(45, 50, 62, 255); // Slate Border Grey
                    r_border_w = 2;
                }

                s32 rx = opt_panel_x + 30;

                drawer->RenderRoundedRectangleFill(r_border, rx, ry, opt_row_w, opt_row_h, 8);
                drawer->RenderRoundedRectangleFill(r_bg, rx + r_border_w, ry + r_border_w, opt_row_w - (r_border_w * 2), opt_row_h - (r_border_w * 2), 6);

                // 1. Draw Option Label - Use registered "Ubuntu@24"
                pu::ui::Color text_color(237, 229, 251, 255);
                auto label_tex = pu::ui::render::RenderText("Ubuntu@24", options[j].label, text_color);
                if (label_tex) {
                    s32 th = pu::ui::render::GetTextureHeight(label_tex);
                    drawer->RenderTexture(label_tex, rx + 25, ry + (opt_row_h - th) / 2);
                    pu::ui::render::DeleteTexture(label_tex);
                }

                // 2. Draw Option Value - Use registered "Ubuntu@24"
                if (options[j].volume_percent >= 0) {
                    // HorizonOS-style slider: thin track, proportional fill,
                    // round handle riding the fill/track boundary, percentage
                    // printed after it.
                    int percent = options[j].volume_percent;
                    pu::ui::Color track_color(70, 74, 84, 255);    // Visible grey line
                    pu::ui::Color fill_color(230, 199, 167, 255);  // Cream accent
                    pu::ui::Color handle_color(237, 229, 251, 255); // Bright lavender-white
                    pu::ui::Color pct_color(190, 180, 225, 255);

                    std::string pct_text = std::to_string(percent) + "%";
                    auto pct_tex = pu::ui::render::RenderText("Ubuntu@24", pct_text, pct_color);
                    s32 pct_w = pct_tex ? pu::ui::render::GetTextureWidth(pct_tex) : 0;
                    s32 pct_x = rx + opt_row_w - 25 - pct_w;

                    s32 bar_w = 480;
                    s32 bar_h = 6;
                    s32 handle_d = 26;
                    s32 assembly_right = pct_x - 20;
                    s32 bar_x = assembly_right - handle_d / 2 - bar_w;
                    s32 bar_y = ry + (opt_row_h - bar_h) / 2;

                    drawer->RenderRoundedRectangleFill(track_color, bar_x, bar_y, bar_w, bar_h, bar_h / 2);
                    s32 fill_w = (bar_w * percent) / 100;
                    if (fill_w > 0) {
                        s32 fill_radius = std::min(bar_h / 2, fill_w / 2);
                        drawer->RenderRoundedRectangleFill(fill_color, bar_x, bar_y, fill_w, bar_h, fill_radius);
                    }

                    s32 handle_cx = bar_x + fill_w;
                    s32 handle_cy = ry + opt_row_h / 2;
                    drawer->RenderRoundedRectangleFill(handle_color, handle_cx - handle_d / 2, handle_cy - handle_d / 2, handle_d, handle_d, handle_d / 2);

                    if (pct_tex) {
                        s32 th = pu::ui::render::GetTextureHeight(pct_tex);
                        drawer->RenderTexture(pct_tex, pct_x, ry + (opt_row_h - th) / 2);
                        pu::ui::render::DeleteTexture(pct_tex);
                    }
                }
                else if (!options[j].value.empty()) {
                    pu::ui::Color val_color = options[j].is_action ? pu::ui::Color(230, 199, 167, 255) : pu::ui::Color(190, 180, 225, 255);

                    // Category + row index rather than the label text, which is
                    // now language-dependent (Connection tab, "Test Connection").
                    if (active_cat == 2 && j == 2) {
                        val_color = connection_status_color;
                    }

                    auto val_tex = pu::ui::render::RenderText("Ubuntu@24", options[j].value, val_color);
                    if (val_tex) {
                        s32 tw = pu::ui::render::GetTextureWidth(val_tex);
                        s32 th = pu::ui::render::GetTextureHeight(val_tex);
                        drawer->RenderTexture(val_tex, rx + opt_row_w - tw - 25, ry + (opt_row_h - th) / 2);
                        pu::ui::render::DeleteTexture(val_tex);
                    }
                }
            }

            if (active_cat == 6) {
                // Update info panel: everything read-only, in the space left
                // under the rows. Its top follows the row count and every line
                // is gated on a hard bottom limit, so it can neither sit on top
                // of a row (the old fixed y+550 did, once there were more than
                // five) nor spill past the card however long the changelog is.
                auto& um = romm::model::UpdateManager::Instance();
                const auto state = um.GetState();

                const s32 info_x = opt_panel_x + 30;
                const s32 info_w = opt_panel_w - 60;
                const s32 line_h = 30;
                const s32 block_top = opt_start_y + (s32)options.size() * (opt_row_h + opt_spacing) + 5;
                const s32 block_bottom = y_coord + h - 25;

                if (block_top + line_h < block_bottom) {
                    const pu::ui::Color panel_border(45, 50, 62, 255);
                    const pu::ui::Color panel_bg(16, 18, 22, 255);
                    const pu::ui::Color label_color(150, 143, 180, 255);
                    const pu::ui::Color value_color(237, 229, 251, 255);
                    const pu::ui::Color dim_color(140, 134, 170, 255);
                    const pu::ui::Color accent_color(230, 199, 167, 255);

                    drawer->RenderRoundedRectangleFill(panel_border, info_x - 12, block_top,
                                                       info_w + 24, block_bottom - block_top, 8);
                    drawer->RenderRoundedRectangleFill(panel_bg, info_x - 10, block_top + 2,
                                                       info_w + 20, block_bottom - block_top - 4, 6);

                    s32 cy = block_top + 18;
                    const s32 value_x = info_x + 220;

                    // Every draw below is a no-op once the cursor runs out of
                    // panel, which is what keeps the block inside the card.
                    auto draw_text = [&](const std::string& text, const char* font,
                                         pu::ui::Color color, s32 x) {
                        if (cy + line_h > block_bottom || text.empty()) return;
                        auto tex = pu::ui::render::RenderText(font, text, color);
                        if (tex) {
                            drawer->RenderTexture(tex, x, cy);
                            pu::ui::render::DeleteTexture(tex);
                        }
                    };
                    auto draw_field = [&](const std::string& label, const std::string& value,
                                          pu::ui::Color vcolor) {
                        if (cy + line_h > block_bottom) return;
                        draw_text(label, "Ubuntu@20", label_color, info_x);
                        draw_text(value, "Ubuntu@20", vcolor, value_x);
                        cy += line_h;
                    };

                    draw_field(romm::i18n::tr("settings.updates.current_version"),
                               "v" + romm::ROMM_NX_VERSION + " (" + std::to_string(romm::ROMM_NX_VERSION_CODE) + ")",
                               value_color);
                    // Which channel the running NRO came from. Read next to the
                    // Channel row above, a difference between the two is the
                    // pending-switch state, spelled out rather than encoded.
                    draw_field(romm::i18n::tr("settings.updates.installed_channel"),
                               UpdateChannelDisplayName(config.GetInstalledUpdateChannel()),
                               value_color);
                    draw_field(romm::i18n::tr("settings.updates.manifest_url"),
                               config.GetUpdateManifestUrl(), dim_color);

                    cy += 12;

                    // Status, colour-coded, wrapped: an error carries curl's
                    // diagnostic and is far too long for a single line.
                    std::string status_str = romm::i18n::tr("settings.updates.status.idle");
                    pu::ui::Color status_color(190, 180, 225, 255);
                    if (state == romm::model::UpdateState::Checking) {
                        status_str = romm::i18n::tr("settings.updates.status.checking");
                    } else if (state == romm::model::UpdateState::NoUpdateAvailable) {
                        status_str = romm::i18n::tr("settings.updates.status.up_to_date");
                        status_color = pu::ui::Color(126, 200, 145, 255); // Green
                    } else if (state == romm::model::UpdateState::UpdateAvailable) {
                        // "Update" would be wrong for a channel switch: moving
                        // from testing back to stable installs an older build.
                        status_str = um.IsOfferedBuildChannelSwitch()
                            ? romm::i18n::format("settings.updates.status.switch_available",
                                                 {{"channel", UpdateChannelDisplayName(config.GetUpdateChannel())}})
                            : romm::i18n::tr("settings.updates.status.available");
                        status_color = accent_color;
                    } else if (state == romm::model::UpdateState::Downloading) {
                        char cur_buf[32], tot_buf[32];
                        std::snprintf(cur_buf, sizeof(cur_buf), "%.2f", (double)um.GetDownloadedBytes() / (1024.0 * 1024.0));
                        std::snprintf(tot_buf, sizeof(tot_buf), "%.2f", (double)um.GetTotalBytes() / (1024.0 * 1024.0));
                        status_str = romm::i18n::format("settings.updates.status.downloading",
                                                        {{"downloaded", cur_buf}, {"total", tot_buf}});
                    } else if (state == romm::model::UpdateState::Verifying) {
                        status_str = romm::i18n::tr("settings.updates.status.verifying");
                    } else if (state == romm::model::UpdateState::Installing) {
                        status_str = romm::i18n::tr("settings.updates.status.installing");
                    } else if (state == romm::model::UpdateState::InstalledRestartRequired) {
                        status_str = romm::i18n::tr("settings.updates.status.restart_required");
                        status_color = pu::ui::Color(126, 200, 145, 255); // Green
                    } else if (state == romm::model::UpdateState::Error) {
                        // The wrapper is localized; the diagnostic inside comes
                        // from UpdateManager and stays in English (see there).
                        status_str = romm::i18n::format("settings.updates.status.error", {{"error", um.GetLatestError()}});
                        status_color = pu::ui::Color(232, 118, 118, 255); // Red
                    }

                    for (const auto& line : WrapText(status_str, 84)) {
                        if (cy + line_h > block_bottom) break;
                        draw_text(line, "Ubuntu@24", status_color, info_x);
                        cy += line_h;
                    }

                    if (state == romm::model::UpdateState::UpdateAvailable) {
                        auto manifest = um.GetRemoteManifest();
                        if (!manifest.changelog.empty() && cy + line_h * 2 <= block_bottom) {
                            cy += 12;
                            draw_text(romm::i18n::tr("settings.updates.changelog"), "Ubuntu@20", accent_color, info_x);
                            cy += line_h;

                            size_t shown = 0;
                            for (const auto& entry : manifest.changelog) {
                                bool out_of_room = false;
                                for (const auto& line : WrapText("- " + entry, 104)) {
                                    if (cy + line_h > block_bottom) { out_of_room = true; break; }
                                    draw_text(line, "Ubuntu@20", dim_color, info_x + 16);
                                    cy += line_h;
                                }
                                if (out_of_room) break;
                                shown++;
                            }

                            if (shown < manifest.changelog.size() && cy + line_h <= block_bottom) {
                                draw_text(romm::i18n::format("settings.updates.changelog_more",
                                                             {{"count", std::to_string(manifest.changelog.size() - shown)}}),
                                          "Ubuntu@20", dim_color, info_x + 16);
                                cy += line_h;
                            }
                        }
                    }
                }
            }
        } else if (active_cat == 4) {
            // Draw Custom Platforms layout.
            //
            // Same row visuals as the generic option list, but with its own
            // scroll window: the row count is open-ended (catalogue + whatever
            // the server reports) and can't be assumed to fit the panel.
            s32 rows_start_y = y_coord + 40;
            s32 row_h = 75;
            s32 row_spacing = 15;
            s32 row_w = opt_panel_w - 60;
            s32 rx = opt_panel_x + 30;

            const auto& plat_rows = PlatformRows();
            const size_t total_rows = kPlatformActionRows + plat_rows.size();

            s32 available_h = h - 80;
            size_t visible_count = (size_t)std::max(1, (available_h + row_spacing) / (row_h + row_spacing));
            if (visible_count > total_rows) visible_count = total_rows;

            size_t selected_row = std::min(active_opt, total_rows > 0 ? total_rows - 1 : 0);
            if (selected_row < platform_scroll_offset) {
                platform_scroll_offset = selected_row;
            } else if (selected_row >= platform_scroll_offset + visible_count) {
                platform_scroll_offset = selected_row - visible_count + 1;
            }
            if (platform_scroll_offset + visible_count > total_rows) {
                platform_scroll_offset = (total_rows > visible_count) ? (total_rows - visible_count) : 0;
            }

            for (size_t i = platform_scroll_offset; i < platform_scroll_offset + visible_count; ++i) {
                s32 ry = rows_start_y + (s32)(i - platform_scroll_offset) * (row_h + row_spacing);
                bool is_selected = (i == selected_row && !is_cat_focused);

                pu::ui::Color r_bg;
                pu::ui::Color r_border;
                s32 r_border_w = 2;

                if (is_selected) {
                    r_bg = pu::ui::Color(85, 63, 152, 255);       // Violet highlight capsule
                    r_border = pu::ui::Color(230, 199, 167, 255); // Cream border
                    r_border_w = 3;
                } else {
                    r_bg = pu::ui::Color(16, 18, 22, 255);        // Web Dark Slate
                    r_border = pu::ui::Color(45, 50, 62, 255);    // Slate Border Grey
                }

                drawer->RenderRoundedRectangleFill(r_border, rx, ry, row_w, row_h, 8);
                drawer->RenderRoundedRectangleFill(r_bg, rx + r_border_w, ry + r_border_w, row_w - (r_border_w * 2), row_h - (r_border_w * 2), 6);

                std::string label;
                std::string value;
                pu::ui::Color value_color(190, 180, 225, 255);

                if (i == 0) {
                    label = romm::i18n::tr("settings.platforms.show_all");
                    value = romm::i18n::tr("common.trigger");
                    value_color = pu::ui::Color(230, 199, 167, 255); // Cream = action
                } else if (i == 1) {
                    label = romm::i18n::tr("settings.platforms.reset_defaults");
                    value = romm::i18n::tr("common.trigger");
                    value_color = pu::ui::Color(230, 199, 167, 255);
                } else {
                    const auto& row = plat_rows[i - kPlatformActionRows];
                    const bool shown = config.IsPlatformVisible(row.canonical_id);
                    // Platform name: catalogue/RomM identity, never translated.
                    label = row.display_name;
                    value = romm::i18n::tr(shown ? "settings.platforms.shown" : "settings.platforms.hidden");
                    value_color = shown ? pu::ui::Color(46, 204, 113, 255)   // Green
                                        : pu::ui::Color(140, 140, 150, 255); // Muted grey
                }

                auto label_tex = pu::ui::render::RenderText("Ubuntu@24", label, pu::ui::Color(237, 229, 251, 255));
                if (label_tex) {
                    s32 th = pu::ui::render::GetTextureHeight(label_tex);
                    drawer->RenderTexture(label_tex, rx + 25, ry + (row_h - th) / 2);
                    pu::ui::render::DeleteTexture(label_tex);
                }

                auto val_tex = pu::ui::render::RenderText("Ubuntu@24", value, value_color);
                if (val_tex) {
                    s32 tw = pu::ui::render::GetTextureWidth(val_tex);
                    s32 th = pu::ui::render::GetTextureHeight(val_tex);
                    drawer->RenderTexture(val_tex, rx + row_w - tw - 25, ry + (row_h - th) / 2);
                    pu::ui::render::DeleteTexture(val_tex);
                }
            }

            // Scroll affordances, only when there's more off-screen.
            {
                pu::ui::Color arrow_clr(190, 180, 225, 200);
                if (platform_scroll_offset > 0) {
                    auto up_tex = pu::ui::render::RenderText("Orbitron@24", "^", arrow_clr);
                    if (up_tex) {
                        s32 tw = pu::ui::render::GetTextureWidth(up_tex);
                        drawer->RenderTexture(up_tex, opt_panel_x + opt_panel_w - 22 - tw, y_coord + 12);
                        pu::ui::render::DeleteTexture(up_tex);
                    }
                }
                if (platform_scroll_offset + visible_count < total_rows) {
                    auto down_tex = pu::ui::render::RenderText("Orbitron@24", "v", arrow_clr);
                    if (down_tex) {
                        s32 tw = pu::ui::render::GetTextureWidth(down_tex);
                        drawer->RenderTexture(down_tex, opt_panel_x + opt_panel_w - 22 - tw, y_coord + h - 42);
                        pu::ui::render::DeleteTexture(down_tex);
                    }
                }
            }
        } else {
            // Draw Custom ROM Paths layout
            // 1. Draw Platform Tab Bar
            s32 tab_y = y_coord + 40;
            s32 tab_h = 60;
            s32 tab_w = 160;
            s32 tab_spacing = 20;
            s32 tab_start_x = opt_panel_x + 50;

            size_t selected_tab = nav->GetSelectedRomPathPlatformIdx();
            bool is_rows_focused = nav->IsRomPathRowsFocused();
            bool is_option_list_focused = (!is_cat_focused);

            // The platform count is open-ended (started at 7, now 8 with
            // 3DS, more will follow), and a fixed-width tab bar can't grow
            // to fit an unbounded list without eventually overlapping the
            // panel edge or shrinking tabs into illegibility. Instead this
            // scrolls a fixed-size window — same approach as the file
            // browser's and queue's row lists — so tabs always stay a
            // legible, constant 160px regardless of how many platforms
            // exist; L/R past the visible edge scrolls the window with the
            // selection, and small arrow glyphs in the side margins signal
            // there's more off-screen.
            size_t plat_count = SUPPORTED_PLATFORMS.size();
            s32 available_w = opt_panel_w - 100; // symmetric 50px margins
            size_t visible_count = (size_t)std::max(1, (available_w + tab_spacing) / (tab_w + tab_spacing));
            if (visible_count > plat_count) visible_count = plat_count;

            if (selected_tab < rom_path_tab_scroll_offset) {
                rom_path_tab_scroll_offset = selected_tab;
            } else if (selected_tab >= rom_path_tab_scroll_offset + visible_count) {
                rom_path_tab_scroll_offset = selected_tab - visible_count + 1;
            }
            // Clamp defensively (e.g. platform count shrank under us).
            if (rom_path_tab_scroll_offset + visible_count > plat_count) {
                rom_path_tab_scroll_offset = (plat_count > visible_count) ? (plat_count - visible_count) : 0;
            }

            for (size_t i = rom_path_tab_scroll_offset; i < rom_path_tab_scroll_offset + visible_count; ++i) {
                s32 tx = tab_start_x + (s32)(i - rom_path_tab_scroll_offset) * (tab_w + tab_spacing);
                bool is_current_tab = (i == selected_tab);

                pu::ui::Color t_bg;
                pu::ui::Color t_border;
                s32 t_border_w = 2;

                if (is_current_tab) {
                    if (is_option_list_focused && !is_rows_focused) {
                        t_bg = pu::ui::Color(85, 63, 152, 255); // Violet highlight capsule
                        t_border = pu::ui::Color(230, 199, 167, 255); // Cream border
                        t_border_w = 3;
                    } else {
                        t_bg = pu::ui::Color(85, 63, 152, 120); // Subdued violet capsule
                        t_border = pu::ui::Color(45, 50, 62, 255);
                    }
                } else {
                    t_bg = pu::ui::Color(16, 18, 22, 255);
                    t_border = pu::ui::Color(45, 50, 62, 255);
                }

                drawer->RenderRoundedRectangleFill(t_border, tx, tab_y, tab_w, tab_h, 8);
                drawer->RenderRoundedRectangleFill(t_bg, tx + t_border_w, tab_y + t_border_w, tab_w - (t_border_w * 2), tab_h - (t_border_w * 2), 6);

                pu::ui::Color text_color(237, 229, 251, 255);
                auto t_tex = pu::ui::render::RenderText("Orbitron@24", SUPPORTED_PLATFORMS[i].tab_label, text_color);
                if (t_tex) {
                    s32 tw = pu::ui::render::GetTextureWidth(t_tex);
                    s32 th = pu::ui::render::GetTextureHeight(t_tex);
                    drawer->RenderTexture(t_tex, tx + (tab_w - tw) / 2, tab_y + (tab_h - th) / 2);
                    pu::ui::render::DeleteTexture(t_tex);
                }
            }

            // Scroll arrows in the side margins, only when there's more off-screen
            {
                pu::ui::Color arrow_clr(190, 180, 225, 200);
                s32 arrow_y = tab_y + (tab_h / 2) - 12;
                if (rom_path_tab_scroll_offset > 0) {
                    auto lt = pu::ui::render::RenderText("Orbitron@24", "<", arrow_clr);
                    if (lt) {
                        drawer->RenderTexture(lt, opt_panel_x + 15, arrow_y);
                        pu::ui::render::DeleteTexture(lt);
                    }
                }
                if (rom_path_tab_scroll_offset + visible_count < plat_count) {
                    auto rt = pu::ui::render::RenderText("Orbitron@24", ">", arrow_clr);
                    if (rt) {
                        s32 rtw = pu::ui::render::GetTextureWidth(rt);
                        drawer->RenderTexture(rt, opt_panel_x + opt_panel_w - 15 - rtw, arrow_y);
                        pu::ui::render::DeleteTexture(rt);
                    }
                }
            }

            // 2. Draw Selected Platform Name
            s32 plat_title_y = tab_y + tab_h + 30;
            const auto& active_plat = SUPPORTED_PLATFORMS[selected_tab];
            auto plat_name_tex = pu::ui::render::RenderText("Orbitron@30", active_plat.display_name, pu::ui::Color(237, 229, 251, 255));
            if (plat_name_tex) {
                drawer->RenderTexture(plat_name_tex, opt_panel_x + 50, plat_title_y);
                pu::ui::render::DeleteTexture(plat_name_tex);
            }

            // 3. Draw Path Rows
            s32 rows_start_y = plat_title_y + 60;
            s32 row_h = 85;
            s32 row_spacing = 15;
            s32 row_w = opt_panel_w - 100;

            struct RowData {
                std::string label;
                std::string path;
                PathStatus status;
                bool disabled;
            };

            // Row labels are localized; the paths beside them are filesystem
            // values shown verbatim (only truncated to fit).
            const std::string coming_later = romm::i18n::tr("settings.rom_paths.coming_later");
            std::vector<RowData> rows_data = {
                {romm::i18n::tr("settings.rom_paths.roms"), truncatePath(config.GetRomPath(active_plat.internal_slug)), cached_statuses[selected_tab].roms_status, kRomPathRowDisabled[0]},
                {romm::i18n::tr("settings.rom_paths.cover_cache"), truncatePath(GetCoverCachePath(active_plat.internal_slug)), cached_statuses[selected_tab].cover_status, kRomPathRowDisabled[1]},
                {romm::i18n::tr("settings.rom_paths.bios"), coming_later, cached_statuses[selected_tab].bios_status, kRomPathRowDisabled[2]},
                {romm::i18n::tr("settings.rom_paths.save_data"), coming_later, cached_statuses[selected_tab].save_status, kRomPathRowDisabled[3]}
            };

            size_t selected_row = nav->GetSelectedRomPathRowIdx();

            for (size_t j = 0; j < rows_data.size(); ++j) {
                s32 ry = rows_start_y + j * (row_h + row_spacing);
                bool is_current_row = (j == selected_row && is_rows_focused && is_option_list_focused);

                pu::ui::Color r_bg;
                pu::ui::Color r_border;
                s32 r_border_w = 2;

                if (rows_data[j].disabled) {
                    r_bg = pu::ui::Color(10, 11, 13, 255);
                    r_border = pu::ui::Color(30, 34, 43, 255);
                } else if (is_current_row) {
                    r_bg = pu::ui::Color(85, 63, 152, 255);
                    r_border = pu::ui::Color(230, 199, 167, 255);
                    r_border_w = 3;
                } else {
                    r_bg = pu::ui::Color(16, 18, 22, 255);
                    r_border = pu::ui::Color(45, 50, 62, 255);
                }

                s32 rx = opt_panel_x + 50;

                drawer->RenderRoundedRectangleFill(r_border, rx, ry, row_w, row_h, 8);
                drawer->RenderRoundedRectangleFill(r_bg, rx + r_border_w, ry + r_border_w, row_w - (r_border_w * 2), row_h - (r_border_w * 2), 6);

                // Draw Label
                pu::ui::Color label_color = rows_data[j].disabled ? pu::ui::Color(110, 110, 120, 255) : pu::ui::Color(237, 229, 251, 255);
                auto l_tex = pu::ui::render::RenderText("Ubuntu@24", rows_data[j].label, label_color);
                if (l_tex) {
                    drawer->RenderTexture(l_tex, rx + 25, ry + 12);
                    pu::ui::render::DeleteTexture(l_tex);
                }

                // Draw Path
                pu::ui::Color path_color = rows_data[j].disabled ? pu::ui::Color(80, 80, 90, 255) : pu::ui::Color(190, 180, 225, 255);
                auto p_tex = pu::ui::render::RenderText("Ubuntu@20", rows_data[j].path, path_color);
                if (p_tex) {
                    drawer->RenderTexture(p_tex, rx + 25, ry + 46);
                    pu::ui::render::DeleteTexture(p_tex);
                }

                // Draw Status Indicator
                std::string status_text;
                pu::ui::Color status_color;

                switch (rows_data[j].status) {
                    case PathStatus::Valid:
                        status_text = romm::i18n::tr("settings.path_status.valid");
                        status_color = pu::ui::Color(46, 204, 113, 255);
                        break;
                    case PathStatus::Missing:
                        status_text = romm::i18n::tr("settings.path_status.missing");
                        status_color = pu::ui::Color(241, 196, 15, 255);
                        break;
                    case PathStatus::Invalid:
                        status_text = romm::i18n::tr("settings.path_status.invalid");
                        status_color = pu::ui::Color(231, 76, 60, 255);
                        break;
                    case PathStatus::ReadOnly:
                        status_text = romm::i18n::tr("settings.path_status.readonly");
                        status_color = pu::ui::Color(150, 150, 160, 255);
                        break;
                    case PathStatus::ComingLater:
                        status_text = romm::i18n::tr("settings.path_status.coming_later");
                        status_color = pu::ui::Color(100, 100, 110, 255);
                        break;
                }

                auto s_tex = pu::ui::render::RenderText("Ubuntu@24", status_text, status_color);
                if (s_tex) {
                    s32 sw = pu::ui::render::GetTextureWidth(s_tex);
                    s32 sh = pu::ui::render::GetTextureHeight(s_tex);
                    drawer->RenderTexture(s_tex, rx + row_w - sw - 30, ry + (row_h - sh) / 2);
                    pu::ui::render::DeleteTexture(s_tex);
                }
            }
        }
    }


    // --- SettingsLayout Implementation ---

    SettingsLayout::SettingsLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {

        this->SetBackgroundColor(pu::ui::Color(16, 18, 22, 255));

        card = SettingsCard::New(80, 160, 1760, 800, nav);
        this->Add(card);

        settings_title_text = pu::ui::elm::TextBlock::New(0, 65, romm::i18n::tr("settings.title"));
        settings_title_text->SetFont("Orbitron@45");
        settings_title_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        settings_title_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(settings_title_text);

        hint_text = pu::ui::elm::TextBlock::New(0, 1080 - 65, romm::i18n::tr("hint.settings.categories"));
        hint_text->SetFont("Ubuntu@30");
        hint_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        hint_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(hint_text);

        confirm_modal = SettingsConfirmModal::New();
        this->Add(confirm_modal);

        auto global_progress = romm::ui::GlobalProgressBar::New(850, 15, 400, 60, nav);
        this->Add(global_progress);

        std::cout << "[SETTINGS] Opened" << std::endl;
    }

    void SettingsLayout::OnSelectionUpdated() {
        auto nav = nav_mgr.lock();
        if (!nav) return;
        if (card && nav->GetSelectedSettingsCategoryIdx() == 3) {
            card->RefreshPathStatuses();
        }
        if (nav->GetSelectedSettingsCategoryIdx() == 4) {
            RefreshPlatformRows();
        }
    }

    void SettingsLayout::RefreshConfig() {
        if (card) {
            card->RefreshConfigTextures();
            card->RefreshPathStatuses();
        }
        RefreshPlatformRows();
    }

    // Rebuilt from the live model rather than cached once: platforms can land
    // after the Settings screen is already open (the fetch is async), and a
    // server the user just re-pointed at can report a different set entirely.
    void SettingsLayout::RefreshPlatformRows() {
        auto nav = nav_mgr.lock();
        RebuildPlatformRows(nav ? nav->GetModel() : nullptr);

        // The list just changed size under a cursor that may have been near its
        // end — clamp before anything indexes with it.
        if (nav && nav->GetSelectedSettingsCategoryIdx() == 4) {
            const size_t count = kPlatformActionRows + PlatformRows().size();
            if (count > 0 && nav->GetSelectedSettingsOptionIdx() >= count) {
                nav->SetSelectedSettingsOptionIdx(count - 1);
            }
        }
    }

    // A on a platform row toggles; Left/Right set it outright, matching how the
    // other Left/Right rows in Settings behave (deterministic per direction
    // rather than direction-insensitive cycling).
    void SettingsLayout::SetPlatformVisibility(size_t platform_row_idx, bool visible) {
        const auto& plat_rows = PlatformRows();
        if (platform_row_idx >= plat_rows.size()) return;

        auto& config = romm::model::ConfigManager::Instance();
        const auto& row = plat_rows[platform_row_idx];
        if (config.IsPlatformVisible(row.canonical_id) == visible) return; // nothing to write

        config.SetPlatformVisible(row.canonical_id, visible);
        config.Save();
        std::cout << "[PLATFORMS] platform=" << row.canonical_id
                  << " visible=" << (visible ? "true" : "false") << std::endl;
        ApplyPlatformVisibility();
    }

    void SettingsLayout::ToggleSelectedPlatform(bool visible) {
        auto nav = nav_mgr.lock();
        if (!nav) return;
        const size_t opt_idx = nav->GetSelectedSettingsOptionIdx();
        if (opt_idx < kPlatformActionRows) return; // Show All / Reset Defaults aren't toggles
        SetPlatformVisibility(opt_idx - kPlatformActionRows, visible);
    }

    // Re-filters the platform browser immediately, so leaving Settings shows
    // the new list without a refetch. Config is already saved by the caller.
    void SettingsLayout::ApplyPlatformVisibility() {
        auto nav = nav_mgr.lock();
        if (nav) {
            nav->ApplyPlatformVisibilityChange();
        }
    }

    void SettingsLayout::UpdateFooterHints(SettingsFocusArea focus) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        if (nav->GetSelectedSettingsCategoryIdx() == 3) {
            if (!nav->IsRomPathRowsFocused()) {
                hint_text->SetText(romm::i18n::tr("hint.settings.rompath.tabs"));
            } else {
                if (nav->GetSelectedRomPathRowIdx() == 0) {
                    hint_text->SetText(romm::i18n::tr("hint.settings.rompath.roms"));
                } else if (nav->GetSelectedRomPathRowIdx() == 1) {
                    hint_text->SetText(romm::i18n::tr("hint.settings.rompath.cover"));
                } else {
                    hint_text->SetText(romm::i18n::tr("hint.settings.rompath.other"));
                }
            }
            return;
        }

        if (nav->GetSelectedSettingsCategoryIdx() == 4 && focus != SettingsFocusArea::CategoryList) {
            hint_text->SetText(romm::i18n::tr("hint.settings.platforms"));
            return;
        }

        if (focus == SettingsFocusArea::CategoryList) {
            hint_text->SetText(romm::i18n::tr("hint.settings.categories"));
        } else {
            hint_text->SetText(romm::i18n::tr("hint.settings.options"));
        }
    }

    void SettingsLayout::RefreshTranslations() {
        if (settings_title_text) settings_title_text->SetText(romm::i18n::tr("settings.title"));
        auto nav = nav_mgr.lock();
        if (nav) UpdateFooterHints(nav->GetSettingsFocus());
        // The category and option rows are rendered from tr() every frame, so
        // they pick up the new language on the next redraw with nothing to
        // rebuild here. The connection-test result is a stored string, though,
        // and would otherwise stay in the old language until re-run.
        if (card) card->ResetConnectionStatus();
    }

    void SettingsLayout::HandleOptionAction(size_t cat_idx, size_t opt_idx) {
        auto& config = romm::model::ConfigManager::Instance();

        if (cat_idx == 0) { // General
            if (opt_idx == 0) {
                // System default -> English -> Français -> System default.
                // Persist the setting, apply it, then push the new strings
                // through every layout so the change is visible immediately —
                // no restart, and the user stays on this row.
                const std::string next = NextLanguageSetting(config.GetLanguage());
                config.SetLanguage(next);
                config.Save();
                romm::i18n::SetLanguageSetting(next);
                if (auto nav = nav_mgr.lock()) {
                    nav->RefreshTranslations();
                }
            } else if (opt_idx == 1) {
                config.SetShowBuildVersion(!config.ShowBuildVersion());
                config.Save();
            } else if (opt_idx == 2) {
                config.SetConfirmBeforeUninstall(!config.ConfirmBeforeUninstall());
                config.Save();
            } else if (opt_idx == 3) {
                config.SetShowInstalledBadge(!config.ShowInstalledBadge());
                config.Save();
            } else if (opt_idx == 4) {
                config.SetScreenAlwaysOn(!config.ScreenAlwaysOn());
                config.Save();
                romm::model::ScreenWakeManager::Instance().RequestUpdate();
            } else if (opt_idx == 5) {
                auto current = config.GetCoversQuality();
                if (current == romm::model::CoversQuality::Balanced) {
                    config.SetCoversQuality(romm::model::CoversQuality::HD);
                } else if (current == romm::model::CoversQuality::HD) {
                    config.SetCoversQuality(romm::model::CoversQuality::SD);
                } else {
                    config.SetCoversQuality(romm::model::CoversQuality::Balanced);
                }
                config.Save();
            } else if (opt_idx == 6) {
                config.SetFileBrowserWriteAnywhere(!config.FileBrowserWriteAnywhere());
                config.Save();
            } else if (opt_idx == 7) {
                // Global default only — platforms with their own Y-Menu
                // override are unaffected.
                auto current = config.GetGridViewMode();
                romm::model::GridViewMode next =
                    (current == romm::model::GridViewMode::Default) ? romm::model::GridViewMode::Big :
                    (current == romm::model::GridViewMode::Big)     ? romm::model::GridViewMode::Detail :
                                                                        romm::model::GridViewMode::Default;
                config.SetGridViewMode(next);
                config.Save();
            }
        }
        else if (cat_idx == 1) { // Theme
            if (opt_idx == 1) { // Download Sound Pack
                // Downloads the whole catalogue in one go (whatever's not
                // already cached) rather than one track at a time. Once it
                // lands, those packs become selectable in Startup Sound /
                // Menu Ambience below. No-op if a batch is already running
                // or everything's already downloaded.
                auto& audio_mgr = romm::model::AudioManager::Instance();
                if (!audio_mgr.IsBatchDownloading()) {
                    size_t missing = 0;
                    for (const auto& pack : kSoundPacks) {
                        if (!audio_mgr.IsCached(pack.kind, pack.key)) missing++;
                    }
                    if (missing > 0) {
                        confirm_modal->Show(
                            romm::i18n::tr("settings.confirm.sound_pack.title"),
                            romm::i18n::format("settings.confirm.sound_pack.message", {{"count", std::to_string(missing)}}),
                            ConfirmAction::DownloadSoundPack,
                            []() {
                                std::vector<std::pair<std::string, std::string>> packs;
                                for (const auto& p : kSoundPacks) packs.push_back({p.kind, p.key});
                                romm::model::AudioManager::Instance().DownloadMissingSoundPacks(packs);
                            }
                        );
                    }
                }
            }
            else if (opt_idx == 6) { // Platform Selector
                // The library sidebar reads this every frame, so no refresh
                // hop is needed — it picks the change up on the way back.
                const auto current = config.GetPlatformSelectorStyle();
                config.SetPlatformSelectorStyle(current == romm::model::PlatformSelectorStyle::Text
                                                    ? romm::model::PlatformSelectorStyle::Banners
                                                    : romm::model::PlatformSelectorStyle::Text);
                config.Save();
            }
            // opt_idx 2 (Startup Sound), 3 (Startup Volume), 4 (Menu
            // Ambience), 5 (Menu Ambience Volume): all four are Left/Right-
            // only (NavigationManager) — A does nothing on any of them.
        }
        else if (cat_idx == 2) { // Connection
            if (opt_idx == 0) {
                std::string current = config.GetRommHost();
                std::string val = romm::navigation::NavigationManager::ShowKeyboard(
                    romm::i18n::tr("keyboard.host.header"), romm::i18n::tr("keyboard.host.subtext"), current);
                if (!val.empty() && val != current) {
                    config.SetRommHost(val);
                    config.Save();
                    auto nav = nav_mgr.lock();
                    if (nav) {
                        auto main_app = static_cast<romm::ui::MainApplication*>(nav->GetApp());
                        if (main_app) main_app->TriggerFetchPlatforms();
                    }
                }
            } else if (opt_idx == 1) {
                std::string current = config.GetApiKey();
                std::string val = romm::navigation::NavigationManager::ShowKeyboard(
                    romm::i18n::tr("keyboard.api_key.header"), romm::i18n::tr("keyboard.api_key.subtext"), current);
                if (!val.empty() && val != current) {
                    config.SetApiKey(val);
                    config.Save();
                    auto nav = nav_mgr.lock();
                    if (nav) {
                        auto main_app = static_cast<romm::ui::MainApplication*>(nav->GetApp());
                        if (main_app) main_app->TriggerFetchPlatforms();
                    }
                }
            } else if (opt_idx == 2) {
                if (card) {
                    card->TriggerConnectionTest();
                }
            }
        }
        else if (cat_idx == 3) { // ROM Paths
            // Handled via explicit actions EditSelectedRomPath(), ValidateOrCreateSelectedPath(), and ResetSelectedRomPath()
        }
        else if (cat_idx == 4) { // Platforms
            const auto& plat_rows = PlatformRows();
            if (opt_idx == 0) { // Show All
                config.ShowAllPlatforms();
                config.Save();
                std::cout << "[PLATFORMS] Show All: every known platform is now visible" << std::endl;
                ApplyPlatformVisibility();
            } else if (opt_idx == 1) { // Reset Defaults
                config.ResetPlatformVisibilityDefaults();
                config.Save();
                std::cout << "[PLATFORMS] Reset to default visibility" << std::endl;
                ApplyPlatformVisibility();
            } else if (opt_idx - kPlatformActionRows < plat_rows.size()) {
                SetPlatformVisibility(opt_idx - kPlatformActionRows,
                                      !config.IsPlatformVisible(plat_rows[opt_idx - kPlatformActionRows].canonical_id));
            }
        }
        else if (cat_idx == 5) { // Advanced
            if (opt_idx == 2) {
                confirm_modal->Show(
                    romm::i18n::tr("settings.confirm.clear_cover.title"),
                    romm::i18n::tr("settings.confirm.clear_cover.message"),
                    ConfirmAction::ClearCoverCache,
                    [this]() {
                        long long b = 0;
                        int c = 0;
                        romm::model::CacheManager::Instance().ClearCache(b, c);
                        if (card) card->TriggerRecalculateCache();
                    }
                );
            } else if (opt_idx == 3) {
                confirm_modal->Show(
                    romm::i18n::tr("settings.confirm.clear_all.title"),
                    romm::i18n::tr("settings.confirm.clear_all.message"),
                    ConfirmAction::ClearAllCache,
                    [this]() {
                        long long b = 0;
                        int c = 0;
                        romm::model::CacheManager::Instance().ClearCache(b, c);
                        if (card) card->TriggerRecalculateCache();
                    }
                );
            } else if (opt_idx == 4) {
                config.SetAutoClearEnabled(!config.IsAutoClearEnabled());
                config.Save();
                std::cout << "[CACHE] Auto prune enabled=" << (config.IsAutoClearEnabled() ? "true" : "false")
                          << " max_mb=" << config.GetMaxSizeMb()
                          << " max_days=" << config.GetMaxAgeDays() << std::endl;
            } else if (opt_idx == 5) {
                std::string val = romm::navigation::NavigationManager::ShowKeyboard(
                    romm::i18n::tr("keyboard.max_cache_size.header"),
                    romm::i18n::tr("keyboard.max_cache_size.subtext"),
                    std::to_string(config.GetMaxSizeMb()));
                if (!val.empty()) {
                    char* endptr = nullptr;
                    long mb = std::strtol(val.c_str(), &endptr, 10);
                    if (endptr != val.c_str() && mb > 0) {
                        config.SetMaxSizeMb(static_cast<int>(mb));
                        config.Save();
                    }
                }
            } else if (opt_idx == 6) {
                std::string val = romm::navigation::NavigationManager::ShowKeyboard(
                    romm::i18n::tr("keyboard.max_cache_age.header"),
                    romm::i18n::tr("keyboard.max_cache_age.subtext"),
                    std::to_string(config.GetMaxAgeDays()));
                if (!val.empty()) {
                    char* endptr = nullptr;
                    long days = std::strtol(val.c_str(), &endptr, 10);
                    if (endptr != val.c_str() && days > 0) {
                        config.SetMaxAgeDays(static_cast<int>(days));
                        config.Save();
                    }
                }
            }
        }
        else if (cat_idx == 6) { // Updates
            // Dispatched by row index rather than by rebuilding a shadow copy
            // of the option list and matching on its labels — those labels are
            // translated now, so string comparison would silently stop matching
            // in French. Indices 0-2 are the fixed rows drawn in OnRender;
            // "Update available" and "Restore backup" follow, in that order,
            // when their conditions hold. Everything read-only lives in the
            // info panel below the rows and is not addressable here.
            auto& um = romm::model::UpdateManager::Instance();
            const bool has_update = (um.GetState() == romm::model::UpdateState::UpdateAvailable);
            const bool can_restore = um.CanRestoreBackup();

            constexpr size_t kRowChannel = 0;
            constexpr size_t kRowCheckOnStartup = 1;
            constexpr size_t kRowCheckForUpdates = 2;
            constexpr size_t kFixedRowCount = 3;

            const size_t update_row = has_update ? kFixedRowCount : SIZE_MAX;
            const size_t restore_row = can_restore ? (has_update ? kFixedRowCount + 1 : kFixedRowCount)
                                                   : SIZE_MAX;

            if (opt_idx == kRowChannel) {
                const std::string next = CycleKey(kUpdateChannelKeys, config.GetUpdateChannel());
                config.SetUpdateChannel(next);
                // A dismissal ("Later" on the startup popup) applies to the
                // version it was shown for on the channel it came from; the
                // other channel's build deserves its own prompt.
                config.SetDismissedUpdateVersion("");
                config.Save();

                // Anything learned from the old channel — manifest, changelog,
                // "update available" — describes a build we're no longer
                // tracking, so it goes before the new check starts. The check
                // is skipped when the updater is mid-install or waiting for a
                // restart; the new channel is still recorded and takes effect
                // on the next check.
                if (um.ResetForChannelChange()) {
                    um.CheckForUpdates();
                }
                std::cout << "[SETTINGS] [UPDATES] Channel set to " << next
                          << " (manifest " << config.GetUpdateManifestUrl() << ")" << std::endl;
            }
            else if (opt_idx == kRowCheckOnStartup) {
                config.SetCheckUpdatesOnStartup(!config.CheckUpdatesOnStartup());
                config.Save();
            }
            else if (opt_idx == kRowCheckForUpdates) {
                um.CheckForUpdates();
            }
            else if (opt_idx == update_row) {
                // The switch wording doesn't promise a newer version — going
                // back to stable usually means installing an older build.
                const bool is_switch = um.IsOfferedBuildChannelSwitch();
                confirm_modal->Show(
                    romm::i18n::tr(is_switch ? "settings.confirm.switch_channel.title"
                                             : "settings.confirm.install_update.title"),
                    is_switch ? romm::i18n::format("settings.confirm.switch_channel.message",
                                                   {{"channel", UpdateChannelDisplayName(config.GetUpdateChannel())}})
                              : romm::i18n::tr("settings.confirm.install_update.message"),
                    ConfirmAction::InstallUpdate,
                    [&um]() {
                        um.StartDownloadAndInstall();
                    }
                );
            }
            else if (opt_idx == restore_row) {
                confirm_modal->Show(
                    romm::i18n::tr("settings.confirm.restore_backup.title"),
                    romm::i18n::tr("settings.confirm.restore_backup.message"),
                    ConfirmAction::RestoreBackup,
                    [&um]() {
                        um.RestoreBackup();
                    }
                );
            }
        }
        else if (cat_idx == 7) { // Debug
            // Rows 0-6 are read-only diagnostics (build, both update channels,
            // config path, index count, active download, queue); Export is last.
            if (opt_idx == 7) {
                std::string debug_path = "sdmc:/switch/romm-nx/debug.txt";
                FILE* f = fopen(debug_path.c_str(), "w");
                if (f) {
                    fprintf(f, "Build Version: v%s (code %d)\n", romm::ROMM_NX_VERSION.c_str(), romm::ROMM_NX_VERSION_CODE);
                    fprintf(f, "Update Channel (selected): %s\n", config.GetUpdateChannel().c_str());
                    fprintf(f, "Update Channel (installed): %s\n", config.GetInstalledUpdateChannel().c_str());
                    fprintf(f, "Update Manifest URL: %s\n", config.GetUpdateManifestUrl().c_str());
                    fprintf(f, "RomM Host: %s\n", config.GetRommHost().c_str());
                    fprintf(f, "Config Path: sdmc:/switch/romm-nx/config.json\n");
                    fprintf(f, "Installed Index Path: sdmc:/switch/romm-nx/installed_index.json\n");
                    fprintf(f, "Installed Index Count: %zu\n", romm::model::DownloadManager::Instance().GetInstalledIndex().size());
                    fprintf(f, "PlayStation ROM Path: %s\n", config.GetRomPath("psx").c_str());
                    fprintf(f, "PlayStation 2 ROM Path: %s\n", config.GetRomPath("ps2").c_str());
                    fprintf(f, "PSP ROM Path: %s\n", config.GetRomPath("psp").c_str());
                    fprintf(f, "Nintendo DS ROM Path: %s\n", config.GetRomPath("nds").c_str());
                    fprintf(f, "Download Queue Count: %zu\n", romm::model::DownloadManager::Instance().GetQueueSnapshot().size());
                    
                    auto stats = romm::model::CacheManager::Instance().CalculateSize();
                    fprintf(f, "Cache Total Size: %lld bytes\n", stats.total_size);
                    fprintf(f, "Cache Cover Size: %lld bytes\n", stats.cover_size);
                    fclose(f);
                    std::cout << "[DEBUG] Exported debug info to " << debug_path << std::endl;
                }
            }
        }
    }

    // Left/Right on the Startup Sound / Menu Ambience rows (NavigationManager)
    // — browses only already-downloaded packs (plus "none") and saves
    // immediately, same as the ROM Paths platform-tab pattern. Fetching a
    // pack that isn't downloaded yet happens separately, via the "Download
    // Sound Pack" row's confirm-then-prefetch flow.
    void SettingsLayout::CycleStartupSound(int direction) {
        auto& config = romm::model::ConfigManager::Instance();
        auto selectable = SelectableKeys(kStartupSoundKeys, "startup");
        config.SetStartupSound(CycleKey(selectable, config.GetStartupSound(), direction));
        config.Save();
    }

    void SettingsLayout::CycleThemeSound(int direction) {
        auto& config = romm::model::ConfigManager::Instance();
        auto selectable = SelectableKeys(kThemeSoundKeys, "theme");
        config.SetThemeSound(CycleKey(selectable, config.GetThemeSound(), direction));
        config.Save();
        // Ambience is the "current background state" — swapping it live is
        // the whole point, so apply immediately (already-downloaded, so this
        // never needs to fetch anything).
        romm::model::AudioManager::Instance().ApplyThemeAmbienceFromConfig();
    }

    void SettingsLayout::CycleStartupVolume(int direction) {
        auto& config = romm::model::ConfigManager::Instance();
        config.SetStartupVolume(ClampVolume(config.GetStartupVolume() + direction * 10));
        config.Save();
        romm::model::AudioManager::Instance().RefreshVolume();
    }

    void SettingsLayout::CycleAmbientVolume(int direction) {
        auto& config = romm::model::ConfigManager::Instance();
        config.SetAmbientVolume(ClampVolume(config.GetAmbientVolume() + direction * 10));
        config.Save();
        romm::model::AudioManager::Instance().RefreshVolume();
    }

    // NavigationManager clamps Down-navigation to this count — it does NOT
    // derive from the options vector built in OnRender, so adding/removing a
    // row there and forgetting to update the matching case here silently
    // makes the new row(s) unreachable (confirmed twice: General's last two
    // rows, then all of Theme). When you add a row to a tab, update its case
    // here in the same change.
    size_t SettingsLayout::GetOptionsCount(size_t cat_idx) {
        switch (cat_idx) {
            case 0: return 8; // General
            case 1: return 7; // Theme
            case 2: return 3; // Connection
            case 3: return 2; // ROM Paths (ROMs and Cover cache)
            // Platforms: Show All + Reset Defaults + one row per canonical
            // platform. Grows with whatever the server reports, so it's derived
            // rather than a literal.
            case 4: return kPlatformActionRows + PlatformRows().size();
            case 5: return 7; // Advanced
            case 6: { // Updates
                size_t count = 3; // Channel, Check on startup, Check for updates
                auto state = romm::model::UpdateManager::Instance().GetState();
                if (state == romm::model::UpdateState::UpdateAvailable) {
                    count += 1; // Update available
                }
                if (romm::model::UpdateManager::Instance().CanRestoreBackup()) {
                    count += 1; // Restore backup
                }
                return count;
            }
            case 7: return 8; // Debug (build, 2 channels, config path, index, active dl, queue, export)
            default: return 0;
        }
    }
    
    size_t SettingsLayout::GetCategoriesCount() {
        return kSettingsCategoryKeys.size();
    }

    size_t SettingsLayout::GetSelectableRomPathRowCount() {
        size_t count = 0;
        for (bool disabled : kRomPathRowDisabled) {
            if (disabled) break; // enabled rows are a contiguous prefix
            count++;
        }
        return count;
    }

    size_t SettingsLayout::GetSupportedPlatformsCount() {
        return SUPPORTED_PLATFORMS.size();
    }

    void SettingsLayout::EditSelectedRomPath() {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        size_t plat_idx = nav->GetSelectedRomPathPlatformIdx();
        size_t row_idx = nav->GetSelectedRomPathRowIdx();
        const auto& plat = SUPPORTED_PLATFORMS[plat_idx];
        auto& config = romm::model::ConfigManager::Instance();

        if (row_idx == 0) { // ROMs / Downloads is editable
            // Platform name and the example path are values; only the wording
            // around them is localized.
            std::string current = config.GetRomPath(plat.internal_slug);
            std::string title = romm::i18n::format("keyboard.rom_path.header", {{"platform", plat.display_name}});
            std::string prompt = romm::i18n::format("keyboard.rom_path.subtext",
                                                    {{"path", "sdmc:/roms/" + plat.display_slug + "/"}});
            std::string val = romm::navigation::NavigationManager::ShowKeyboard(title, prompt, current);
            if (!val.empty() && val != current) {
                if (romm::model::RomPathManager::ValidatePath(val)) {
                    config.SetRomPath(plat.internal_slug, val);
                    config.Save();
                    std::cout << "[ROM_PATH] platform=" << plat.internal_slug << " path=" << val << std::endl;
                }
            }
        }
        
        // Refresh statuses after editing
        if (card) {
            card->RefreshPathStatuses();
        }
        UpdateFooterHints(nav->GetSettingsFocus());
    }

    void SettingsLayout::ValidateOrCreateSelectedPath() {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        size_t plat_idx = nav->GetSelectedRomPathPlatformIdx();
        size_t row_idx = nav->GetSelectedRomPathRowIdx();
        const auto& plat = SUPPORTED_PLATFORMS[plat_idx];
        auto& config = romm::model::ConfigManager::Instance();

        if (row_idx == 0) { // ROMs / Downloads
            std::string p = config.GetRomPath(plat.internal_slug);
            bool pattern_ok = romm::model::RomPathManager::ValidatePath(p);
            if (!pattern_ok) {
                std::cout << "[ROM_PATH] Validate failed: Invalid pattern for " << plat.display_name << std::endl;
            } else {
                struct stat st;
                bool exists = (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
                if (exists) {
                    std::cout << "[ROM_PATH] Validated: " << p << " exists." << std::endl;
                } else {
                    std::cout << "[ROM_PATH] Missing directory " << p << ". Creating folder..." << std::endl;
                    bool ok = romm::model::RomPathManager::CreateFolderIfMissing(p);
                    if (ok) {
                        std::cout << "[ROM_PATH] Successfully created " << p << std::endl;
                    } else {
                        std::cout << "[ROM_PATH] Failed to create folder " << p << std::endl;
                    }
                }
            }
        } else if (row_idx == 1) { // Cover Cache
            std::string cover_path = GetCoverCachePath(plat.internal_slug);
            struct stat st_cov;
            bool exists = (stat(cover_path.c_str(), &st_cov) == 0 && S_ISDIR(st_cov.st_mode));
            std::cout << "[ROM_PATH] Cover cache validation for " << plat.display_name 
                      << " path=" << cover_path << " exists=" << (exists ? "yes" : "no") << std::endl;
        }

        // Refresh statuses
        if (card) {
            card->RefreshPathStatuses();
        }
        UpdateFooterHints(nav->GetSettingsFocus());
    }

    void SettingsLayout::ResetSelectedRomPath() {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        size_t plat_idx = nav->GetSelectedRomPathPlatformIdx();
        size_t row_idx = nav->GetSelectedRomPathRowIdx();
        const auto& plat = SUPPORTED_PLATFORMS[plat_idx];
        auto& config = romm::model::ConfigManager::Instance();

        if (row_idx == 0) { // ROMs / Downloads
            std::string default_path = romm::model::RomPathManager::GetDefaultPath(plat.internal_slug);
            confirm_modal->Show(
                romm::i18n::format("settings.confirm.reset_path.title", {{"platform", plat.display_name}}),
                romm::i18n::format("settings.confirm.reset_path.message", {
                    {"platform", plat.display_name},
                    {"path", default_path}
                }),
                ConfirmAction::ResetRomPath,
                [this, &config, plat, default_path]() {
                    config.SetRomPath(plat.internal_slug, default_path);
                    config.Save();
                    std::cout << "[ROM_PATH] platform=" << plat.internal_slug << " path=" << default_path << std::endl;
                    if (card) {
                        card->RefreshPathStatuses();
                    }
                }
            );
        }
    }

}
