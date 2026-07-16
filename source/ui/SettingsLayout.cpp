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
#include <cstdlib>
#include <iostream>
#include <cstdio>
#include <sys/stat.h>

namespace romm::ui {

    namespace {
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
            {"GBA", "Game Boy Advance", "gba", "gba", true, true, false, false, false}
        };

        std::string GetCoverCachePath(const std::string& internal_slug) {
            if (internal_slug == "psx") {
                return "sdmc:/switch/romm-nx/cache/covers/small/";
            }
            return "sdmc:/switch/romm-nx/cache/covers/" + internal_slug + "/";
        }
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
        auto btn_tex = pu::ui::render::RenderText("Orbitron@24", "A Confirm        B Cancel", text_color);
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
        TriggerRecalculateCache();
        RefreshPathStatuses();
    }

    void SettingsCard::RefreshConfigTextures() {
        TriggerRecalculateCache();
    }

    void SettingsCard::TriggerConnectionTest() {
        connection_test_status = "Testing...";
        connection_status_color = pu::ui::Color(230, 199, 167, 255); // Cream (#E6C7A7)

        auto& config = romm::model::ConfigManager::Instance();
        std::string host = config.GetRommHost();
        std::string key = config.GetApiKey();

        if (host.empty() || key.empty()) {
            connection_test_status = "Failed: Missing host/key";
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
                connection_test_status = "Connected";
                connection_status_color = pu::ui::Color(46, 204, 113, 255); // Green
            } else {
                std::string err = http_res.error;
                if (err.empty()) {
                    err = "HTTP " + std::to_string(http_res.statusCode);
                }
                connection_test_status = "Failed: " + err;
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

        std::vector<std::string> categories = {"General", "Theme", "Connection", "ROM Paths", "Advanced", "Updates", "Debug"};
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
            auto tex = pu::ui::render::RenderText("Orbitron@30", categories[i], text_color);
            if (tex) {
                s32 th = pu::ui::render::GetTextureHeight(tex);
                drawer->RenderTexture(tex, rx + 30, ry + (row_h - th) / 2);
                pu::ui::render::DeleteTexture(tex);
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

        if (active_cat == 0) { // General
            options.push_back({"Language", config.GetLanguage() == "fr" ? "French (FR)" : "English (EN)"});
            options.push_back({"Show debug build/version", config.ShowBuildVersion() ? "ON" : "OFF"});
            options.push_back({"Confirm before uninstall", config.ConfirmBeforeUninstall() ? "ON" : "OFF"});
            options.push_back({"Show installed badge", config.ShowInstalledBadge() ? "ON" : "OFF"});
            options.push_back({"Screen Always On", config.ScreenAlwaysOn() ? "ON" : "OFF"});
            options.push_back({"Covers Quality", config.GetCoversQualityString()});
        }
        else if (active_cat == 1) { // Theme
            options.push_back({"Theme", "RomM Brand"});
        }
        else if (active_cat == 2) { // Connection
            options.push_back({"RomM Server URL", truncatePath(config.GetRommHost())});
            options.push_back({"API Key", config.GetMaskedApiKey()});
            options.push_back({"Test Connection", connection_test_status, true});
        }
        else if (active_cat == 3) { // ROM Paths
            // Custom drawing logic handles this
        }
        else if (active_cat == 4) { // Advanced
            std::string cover_sz = "Calculating...";
            std::string total_sz = "Calculating...";
            if (!calculating_cache) {
                double cover_mb = (double)cache_cover_bytes / (1024.0 * 1024.0);
                char cover_buf[64];
                std::snprintf(cover_buf, sizeof(cover_buf), "%.2f MB (%d files)", cover_mb, cache_cover_count);
                cover_sz = cover_buf;

                double total_mb = (double)cache_total_bytes / (1024.0 * 1024.0);
                char total_buf[64];
                std::snprintf(total_buf, sizeof(total_buf), "%.2f MB", total_mb);
                total_sz = total_buf;
            }
            options.push_back({"Cover cache size", cover_sz});
            options.push_back({"Total cache size", total_sz});
            options.push_back({"Clear cover cache", "Trigger Clear", true});
            options.push_back({"Clear all cache", "Trigger Clear", true});
            options.push_back({"Auto-clear cache", config.IsAutoClearEnabled() ? "ON" : "OFF"});
            options.push_back({"Max cache size", std::to_string(config.GetMaxSizeMb()) + " MB"});
            options.push_back({"Delete cache older than", std::to_string(config.GetMaxAgeDays()) + " days"});
        }
        else if (active_cat == 5) { // Updates
            options.push_back({"Current version", romm::ROMM_NX_VERSION});
            options.push_back({"Current version code", std::to_string(romm::ROMM_NX_VERSION_CODE)});
            options.push_back({"Update channel", config.GetUpdateChannel()});
            options.push_back({"Manifest URL", config.GetUpdateManifestUrl()});
            options.push_back({"Check for updates", "Trigger Check", true});
            
            auto& um = romm::model::UpdateManager::Instance();
            auto state = um.GetState();
            std::string status_str = "Idle";
            if (state == romm::model::UpdateState::Checking) status_str = "Checking...";
            else if (state == romm::model::UpdateState::NoUpdateAvailable) status_str = "romm-nx is up to date.";
            else if (state == romm::model::UpdateState::UpdateAvailable) status_str = "Update available!";
            else if (state == romm::model::UpdateState::Downloading) {
                long long cur = um.GetDownloadedBytes();
                long long tot = um.GetTotalBytes();
                double cur_mb = (double)cur / (1024.0 * 1024.0);
                double tot_mb = (double)tot / (1024.0 * 1024.0);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "Downloading (%.2f/%.2f MB)", cur_mb, tot_mb);
                status_str = buf;
            }
            else if (state == romm::model::UpdateState::Verifying) status_str = "Verifying...";
            else if (state == romm::model::UpdateState::Installing) status_str = "Installing...";
            else if (state == romm::model::UpdateState::InstalledRestartRequired) status_str = "Success! Please restart app.";
            else if (state == romm::model::UpdateState::Error) status_str = "Error: " + um.GetLatestError();

            options.push_back({"Status", status_str});

            if (state == romm::model::UpdateState::UpdateAvailable) {
                auto manifest = um.GetRemoteManifest();
                options.push_back({"New version", manifest.version + " (code " + std::to_string(manifest.version_code) + ")"});
                options.push_back({"Download & install", "Trigger Install", true});
            }

            if (um.CanRestoreBackup()) {
                options.push_back({"Restore backup", "Trigger Restore", true});
            }
        }
        else if (active_cat == 6) { // Debug
            options.push_back({"Build version", "v" + romm::ROMM_NX_VERSION});
            options.push_back({"Config path", truncatePath("sdmc:/switch/romm-nx/config.json")});
            options.push_back({"Installed index entries", std::to_string(romm::model::DownloadManager::Instance().GetInstalledIndex().size())});
            
            // Populating active download title
            std::string active_title = "None";
            auto active_task = romm::model::DownloadManager::Instance().GetActiveDownloadSnapshot();
            if (active_task.rom_id > 0 && (active_task.state == romm::model::DownloadState::DownloadingGame || active_task.state == romm::model::DownloadState::DownloadingCover || active_task.state == romm::model::DownloadState::SyncingCover || active_task.state == romm::model::DownloadState::Preparing)) {
                active_title = active_task.title;
                if (active_title.size() > 22) {
                    active_title = active_title.substr(0, 19) + "...";
                }
            }
            options.push_back({"Active download", active_title});
            options.push_back({"Queue count", std::to_string(romm::model::DownloadManager::Instance().GetQueueSnapshot().size())});
            options.push_back({"Export debug info", "Trigger Export", true});
        }

        if (active_cat != 3) {
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
                if (!options[j].value.empty()) {
                    pu::ui::Color val_color = options[j].is_action ? pu::ui::Color(230, 199, 167, 255) : pu::ui::Color(190, 180, 225, 255);

                    if (options[j].label == "Test Connection") {
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

            if (active_cat == 5) {
                auto& um = romm::model::UpdateManager::Instance();
                auto state = um.GetState();
                if (state == romm::model::UpdateState::UpdateAvailable) {
                    auto manifest = um.GetRemoteManifest();
                    if (!manifest.changelog.empty()) {
                        s32 cy = y_coord + 550;
                        pu::ui::Color title_color(230, 199, 167, 255); // Cream
                        auto title_tex = pu::ui::render::RenderText("Ubuntu@24", "Changelog:", title_color);
                        if (title_tex) {
                            drawer->RenderTexture(title_tex, opt_panel_x + 30, cy);
                            pu::ui::render::DeleteTexture(title_tex);
                        }
                        cy += 45;
                        pu::ui::Color item_color(190, 180, 225, 255); // Light lavender
                        size_t max_items = std::min(manifest.changelog.size(), (size_t)4);
                        for (size_t k = 0; k < max_items; ++k) {
                            std::string bullet = "- " + manifest.changelog[k];
                            if (bullet.size() > 80) {
                                bullet = bullet.substr(0, 77) + "...";
                            }
                            auto item_tex = pu::ui::render::RenderText("Ubuntu@24", bullet, item_color);
                            if (item_tex) {
                                drawer->RenderTexture(item_tex, opt_panel_x + 50, cy);
                                pu::ui::render::DeleteTexture(item_tex);
                            }
                            cy += 35;
                        }
                        if (manifest.changelog.size() > 4) {
                            std::string extra = "... and " + std::to_string(manifest.changelog.size() - 4) + " more";
                            auto extra_tex = pu::ui::render::RenderText("Ubuntu@24", extra, item_color);
                            if (extra_tex) {
                                drawer->RenderTexture(extra_tex, opt_panel_x + 50, cy);
                                pu::ui::render::DeleteTexture(extra_tex);
                            }
                        }
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

            for (size_t i = 0; i < SUPPORTED_PLATFORMS.size(); ++i) {
                s32 tx = tab_start_x + i * (tab_w + tab_spacing);
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

            std::vector<RowData> rows_data = {
                {"ROMs / Downloads", truncatePath(config.GetRomPath(active_plat.internal_slug)), cached_statuses[selected_tab].roms_status, false},
                {"Cover cache", truncatePath(GetCoverCachePath(active_plat.internal_slug)), cached_statuses[selected_tab].cover_status, false},
                {"BIOS", "Coming later", cached_statuses[selected_tab].bios_status, true},
                {"Save data", "Coming later", cached_statuses[selected_tab].save_status, true}
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
                        status_text = "\u2713 Valid";
                        status_color = pu::ui::Color(46, 204, 113, 255);
                        break;
                    case PathStatus::Missing:
                        status_text = "! Missing";
                        status_color = pu::ui::Color(241, 196, 15, 255);
                        break;
                    case PathStatus::Invalid:
                        status_text = "! Invalid";
                        status_color = pu::ui::Color(231, 76, 60, 255);
                        break;
                    case PathStatus::ReadOnly:
                        status_text = "\u2014 Read-only";
                        status_color = pu::ui::Color(150, 150, 160, 255);
                        break;
                    case PathStatus::ComingLater:
                        status_text = "\u2014 Coming later";
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

        settings_title_text = pu::ui::elm::TextBlock::New(0, 65, "SETTINGS");
        settings_title_text->SetFont("Orbitron@45");
        settings_title_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        settings_title_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(settings_title_text);

        hint_text = pu::ui::elm::TextBlock::New(0, 1080 - 65, "D-Pad Choose Tab   |   A Open   |   B Back");
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
        if (nav && card && nav->GetSelectedSettingsCategoryIdx() == 3) {
            card->RefreshPathStatuses();
        }
    }

    void SettingsLayout::RefreshConfig() {
        if (card) {
            card->RefreshConfigTextures();
            card->RefreshPathStatuses();
        }
    }

    void SettingsLayout::UpdateFooterHints(SettingsFocusArea focus) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        if (nav->GetSelectedSettingsCategoryIdx() == 3) {
            if (!nav->IsRomPathRowsFocused()) {
                hint_text->SetText("D-Pad Left/Right Change Platform   |   A Enter Rows   |   B Back");
            } else {
                if (nav->GetSelectedRomPathRowIdx() == 0) {
                    hint_text->SetText("A Edit Path   |   X Validate/Create   |   Y Reset Default   |   B Tabs");
                } else if (nav->GetSelectedRomPathRowIdx() == 1) {
                    hint_text->SetText("X Validate   |   B Tabs");
                } else {
                    hint_text->SetText("B Tabs");
                }
            }
            return;
        }

        if (focus == SettingsFocusArea::CategoryList) {
            hint_text->SetText("D-Pad Choose Tab   |   A Open   |   B Back");
        } else {
            hint_text->SetText("D-Pad Move   |   A Select   |   B Tabs");
        }
    }

    void SettingsLayout::HandleOptionAction(size_t cat_idx, size_t opt_idx) {
        auto& config = romm::model::ConfigManager::Instance();

        if (cat_idx == 0) { // General
            if (opt_idx == 0) {
                config.SetLanguage(config.GetLanguage() == "fr" ? "en" : "fr");
                config.Save();
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
            }
        }
        else if (cat_idx == 2) { // Connection
            if (opt_idx == 0) {
                std::string current = config.GetRommHost();
                std::string val = romm::navigation::NavigationManager::ShowKeyboard("Edit RomM Host URL", "Example: https://rom.example.com", current);
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
                std::string val = romm::navigation::NavigationManager::ShowKeyboard("Edit API Token", "Enter RomM API Key", current);
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
        else if (cat_idx == 4) { // Advanced
            if (opt_idx == 2) {
                confirm_modal->Show(
                    "Clear cover cache?",
                    "This will remove downloaded cover thumbnails. ROM files will not be affected.",
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
                    "Clear all cache?",
                    "This will delete downloaded covers and temporary files. ROM files will not be deleted.",
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
                std::string val = romm::navigation::NavigationManager::ShowKeyboard("Edit Max Cache Size (MB)", "Enter maximum cache size in MB", std::to_string(config.GetMaxSizeMb()));
                if (!val.empty()) {
                    char* endptr = nullptr;
                    long mb = std::strtol(val.c_str(), &endptr, 10);
                    if (endptr != val.c_str() && mb > 0) {
                        config.SetMaxSizeMb(static_cast<int>(mb));
                        config.Save();
                    }
                }
            } else if (opt_idx == 6) {
                std::string val = romm::navigation::NavigationManager::ShowKeyboard("Edit Max Cache Age (Days)", "Delete cache files older than X days", std::to_string(config.GetMaxAgeDays()));
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
        else if (cat_idx == 5) { // Updates
            std::vector<OptionRenderEntry> options;
            options.push_back({"Current version", romm::ROMM_NX_VERSION});
            options.push_back({"Current version code", std::to_string(romm::ROMM_NX_VERSION_CODE)});
            options.push_back({"Update channel", config.GetUpdateChannel()});
            options.push_back({"Manifest URL", config.GetUpdateManifestUrl()});
            options.push_back({"Check for updates", "Trigger Check", true});
            
            auto& um = romm::model::UpdateManager::Instance();
            auto state = um.GetState();
            std::string status_str = "Idle";
            if (state == romm::model::UpdateState::Checking) status_str = "Checking...";
            else if (state == romm::model::UpdateState::NoUpdateAvailable) status_str = "romm-nx is up to date.";
            else if (state == romm::model::UpdateState::UpdateAvailable) status_str = "Update available!";
            else if (state == romm::model::UpdateState::Downloading) status_str = "Downloading...";
            else if (state == romm::model::UpdateState::Verifying) status_str = "Verifying...";
            else if (state == romm::model::UpdateState::Installing) status_str = "Installing...";
            else if (state == romm::model::UpdateState::InstalledRestartRequired) status_str = "Success! Restart required.";
            else if (state == romm::model::UpdateState::Error) status_str = "Error";
            options.push_back({"Status", status_str});

            if (state == romm::model::UpdateState::UpdateAvailable) {
                auto manifest = um.GetRemoteManifest();
                options.push_back({"New version", manifest.version});
                options.push_back({"Download & install", "Trigger Install", true});
            }

            if (um.CanRestoreBackup()) {
                options.push_back({"Restore backup", "Trigger Restore", true});
            }

            if (opt_idx < options.size()) {
                std::string label = options[opt_idx].label;
                if (label == "Check for updates") {
                    um.CheckForUpdates();
                }
                else if (label == "Download & install") {
                    confirm_modal->Show(
                        "Download & Install Update?",
                        "This will download and replace romm-nx with the newer version. A restart will be required.",
                        ConfirmAction::InstallUpdate,
                        [&um]() {
                            um.StartDownloadAndInstall();
                        }
                    );
                }
                else if (label == "Restore backup") {
                    confirm_modal->Show(
                        "Restore Previous Version?",
                        "This will restore the backup romm-nx.nro.bak to the current version. A restart will be required.",
                        ConfirmAction::RestoreBackup,
                        [&um]() {
                            um.RestoreBackup();
                        }
                    );
                }
            }
        }
        else if (cat_idx == 6) { // Debug
            if (opt_idx == 5) {
                std::string debug_path = "sdmc:/switch/romm-nx/debug.txt";
                FILE* f = fopen(debug_path.c_str(), "w");
                if (f) {
                    fprintf(f, "Build Version: v%s\n", romm::ROMM_NX_VERSION.c_str());
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

    size_t SettingsLayout::GetOptionsCount(size_t cat_idx) {
        switch (cat_idx) {
            case 0: return 6; // General
            case 1: return 1; // Theme
            case 2: return 3; // Connection
            case 3: return 2; // ROM Paths (ROMs and Cover cache)
            case 4: return 7; // Advanced
            case 5: { // Updates
                size_t count = 6; // Current version, code, channel, URL, Check for updates, Status
                auto state = romm::model::UpdateManager::Instance().GetState();
                if (state == romm::model::UpdateState::UpdateAvailable) {
                    count += 2; // New version, Install update
                }
                if (romm::model::UpdateManager::Instance().CanRestoreBackup()) {
                    count += 1; // Restore backup
                }
                return count;
            }
            case 6: return 6; // Debug
            default: return 0;
        }
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
            std::string current = config.GetRomPath(plat.internal_slug);
            std::string title = "Edit " + plat.display_name + " Path";
            std::string prompt = "Example: sdmc:/roms/" + plat.display_slug + "/";
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
            std::string title = "Reset " + plat.display_name + " ROM path?";
            std::string msg = "This will restore " + plat.display_name + " path to the default value " + default_path + ".";
            confirm_modal->Show(
                title,
                msg,
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
