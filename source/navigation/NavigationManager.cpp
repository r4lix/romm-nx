#include "NavigationManager.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/DownloadManager.hpp"
#include "../ui/MainMenuLayout.hpp"
#include "../ui/LibraryLayout.hpp"
#include "../ui/GameGrid.hpp"
#include "../model/UpdateManager.hpp"
#include "../ui/DetailLayout.hpp"
#include "../ui/SettingsLayout.hpp"
#include "../ui/InstalledLayout.hpp"
#include "../ui/QueueLayout.hpp"
#include "../ui/FileBrowserLayout.hpp"
#include "../ui/LibraryMenuModal.hpp"
#include "../ui/AlphabetBar.hpp"
#include "../ui/MainApplication.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/PlatformCatalog.hpp"
#include "../nso/NsoSnesInstaller.hpp"
#include "../i18n/I18n.hpp"
#include <sstream>
#include <chrono>
#include <vector>

namespace romm::navigation {

    using romm::ui::SettingsFocusArea;

    /*
    static std::string KeyToString(const u64 key) {
        std::vector<std::string> parts;
        if (key & HidNpadButton_A) parts.push_back("A");
        if (key & HidNpadButton_B) parts.push_back("B");
        if (key & HidNpadButton_X) parts.push_back("X");
        if (key & HidNpadButton_Y) parts.push_back("Y");
        if (key & HidNpadButton_Up) parts.push_back("D-pad Up");
        if (key & HidNpadButton_Down) parts.push_back("D-pad Down");
        if (key & HidNpadButton_Left) parts.push_back("D-pad Left");
        if (key & HidNpadButton_Right) parts.push_back("D-pad Right");
        if (key & HidNpadButton_StickLUp) parts.push_back("StickL Up");
        if (key & HidNpadButton_StickLDown) parts.push_back("StickL Down");
        if (key & HidNpadButton_StickLLeft) parts.push_back("StickL Left");
        if (key & HidNpadButton_StickLRight) parts.push_back("StickL Right");
        if (key & HidNpadButton_Plus) parts.push_back("Plus");
        
        if (parts.empty()) {
            std::stringstream ss;
            ss << "Unknown(" << key << ")";
            return ss.str();
        }
        
        std::string res = parts.front();
        for (size_t i = 1; i < parts.size(); ++i) {
            res += " | " + parts.at(i);
        }
        return res;
    }
    */

    static std::string GetMenuEntryName(size_t idx) {
        switch (idx) {
            case 0: return "Games";
            case 1: return "Installed";
            case 2: return "Queue";
            case 3: return "Saves";
            case 4: return "File browser";
            case 5: return "Settings";
            default: return "Unknown";
        }
    }

    // GetOptionsCountForCategory removed, using SettingsLayout::GetOptionsCount instead

    std::string NavigationManager::ShowKeyboard(const std::string& header, const std::string& subtext, const std::string& initial_text) {
        SwkbdConfig kbd;
        char out_string[512] = {0};
        Result rc = swkbdCreate(&kbd, 0);
        if (R_SUCCEEDED(rc)) {
            swkbdConfigMakePresetDefault(&kbd);
            swkbdConfigSetHeaderText(&kbd, header.c_str());
            swkbdConfigSetSubText(&kbd, subtext.c_str());
            swkbdConfigSetInitialText(&kbd, initial_text.c_str());
            swkbdConfigSetStringLenMax(&kbd, 256);
            
            rc = swkbdShow(&kbd, out_string, sizeof(out_string));
            swkbdClose(&kbd);
            
            if (R_SUCCEEDED(rc)) {
                return std::string(out_string);
            }
        }
        return initial_text;
    }

    bool NavigationManager::PromptForSearch() {
        // Prefilled with the active query so refining a search doesn't mean
        // retyping it, and clearing to empty is how you cancel the filter.
        const std::string entered = ShowKeyboard(romm::i18n::tr("keyboard.search.header"),
                                                 romm::i18n::tr("keyboard.search.subtext"),
                                                 search_query_display);
        const std::string lowered = romm::model::ToLowerAscii(entered);
        if (lowered == search_query) {
            return false;
        }
        search_query = lowered;
        search_query_display = entered;
        // The filtered list is about to change size; a stale index could point
        // past the end of it.
        selected_game_idx = 0;
        std::cout << "[NAV] Search query set to \"" << search_query_display << "\"" << std::endl;
        return true;
    }

    void NavigationManager::ClearSearch() {
        if (search_query.empty()) return;
        search_query.clear();
        search_query_display.clear();
        selected_game_idx = 0;
    }

    void NavigationManager::ToggleBulkSelection(int rom_id) {
        if (rom_id <= 0) return;
        auto it = bulk_selection.find(rom_id);
        if (it != bulk_selection.end()) {
            bulk_selection.erase(it);
        } else {
            bulk_selection.insert(rom_id);
        }
    }

    NavigationManager::NavigationManager(pu::ui::Application* app, std::shared_ptr<romm::model::DataModel> model)
        : app(app), model(model), current_screen(Screen::MainMenu), library_focus(LibraryFocus::Sidebar),
          selected_menu_idx(0), selected_platform_idx(0), loaded_platform_idx(0), selected_game_idx(0),
          selected_letter_idx(0), detail_focus(DetailFocus::Tabs),
          selected_detail_tab_idx(0), selected_detail_action_idx(0),
          repeat_held_button(0),
          repeat_start_time(std::chrono::high_resolution_clock::now()),
          repeat_last_time(std::chrono::high_resolution_clock::now()),
          main_menu_layout(nullptr), library_layout(nullptr), detail_layout(nullptr), settings_layout(nullptr) {}

    void NavigationManager::Initialize() {
        // Instantiate layouts exactly once as required
        main_menu_layout = std::make_shared<romm::ui::MainMenuLayout>(shared_from_this());
        library_layout = std::make_shared<romm::ui::LibraryLayout>(shared_from_this());
        detail_layout = std::make_shared<romm::ui::DetailLayout>(shared_from_this());
        settings_layout = std::make_shared<romm::ui::SettingsLayout>(shared_from_this());
        installed_layout = std::make_shared<romm::ui::InstalledLayout>(shared_from_this());
        queue_layout = std::make_shared<romm::ui::QueueLayout>(shared_from_this());
        file_browser_layout = nullptr;
        fullscreen_image_layout = std::make_shared<romm::ui::FullscreenImageLayout>();

        std::cout << "[NAV] [LAYOUT TRANSITION] Loading MainMenuLayout as default screen" << std::endl;
        app->LoadLayout(main_menu_layout);
        
        UpdateLayoutSelection();
    }

    void NavigationManager::UpdateLayoutSelection() {
        if (current_screen == Screen::MainMenu && main_menu_layout) {
            main_menu_layout->OnSelectionUpdated();
        } else if (current_screen == Screen::Library && library_layout) {
            library_layout->OnSelectionUpdated();
        } else if (current_screen == Screen::Detail && detail_layout) {
            detail_layout->OnSelectionUpdated();
        } else if (current_screen == Screen::Settings && settings_layout) {
            settings_layout->OnSelectionUpdated();
            settings_layout->UpdateFooterHints(settings_focus);
        } else if (current_screen == Screen::Installed && installed_layout) {
            installed_layout->OnSelectionUpdated();
        } else if (current_screen == Screen::Queue && queue_layout) {
            queue_layout->OnSelectionUpdated();
        } else if (current_screen == Screen::FileBrowser && file_browser_layout) {
            file_browser_layout->OnSelectionUpdated();
        }
    }

    // The single refresh path for a language change. Layouts are created once
    // in Initialize() and reused for the whole session, and Plutonium elements
    // bake text into textures at construction, so switching languages has to
    // push through every live layout — there is deliberately no second
    // mechanism (no rebuild-on-navigate, no per-frame string comparison).
    //
    // Each layout re-applies its own strings and keeps its current selection;
    // nothing is torn down, so the user stays exactly where they were.
    void NavigationManager::RefreshTranslations() {
        if (main_menu_layout) main_menu_layout->RefreshTranslations();
        if (library_layout) library_layout->RefreshTranslations();
        if (detail_layout) detail_layout->RefreshTranslations();
        if (settings_layout) settings_layout->RefreshTranslations();
        if (installed_layout) installed_layout->RefreshTranslations();
        if (queue_layout) queue_layout->RefreshTranslations();
        // Created lazily on first visit, so it may legitimately not exist yet.
        if (file_browser_layout) file_browser_layout->RefreshTranslations();
        // FullscreenImageLayout renders its status line from tr() every frame.

        UpdateLayoutSelection();
    }

    void NavigationManager::HandleUninstallModalInput(u64 keys_down) {
        if (!uninstall_modal.active) return;
        if (keys_down & HidNpadButton_B) {
            HideUninstallModal();
        } else if (keys_down & HidNpadButton_A) {
            auto& dl_mgr = romm::model::DownloadManager::Instance();
            dl_mgr.UninstallGame(uninstall_modal.platform_slug, uninstall_modal.filename, uninstall_modal.cover_path);

            // UninstallGame doesn't touch the install-state cache, and
            // GetCachedInstallState answers from it, so anything asking "is this
            // installed?" would keep hearing yes. The Detail card happened to
            // dodge this by re-checking on ForceRefresh, but the library's
            // Detail-mode panel reads the cache directly every frame.
            dl_mgr.RefreshInstallCache(uninstall_modal.platform_slug, uninstall_modal.filename);

            // Force refresh on the active layouts
            if (detail_layout && detail_layout->GetCard()) detail_layout->GetCard()->ForceRefresh();
            if (installed_layout) installed_layout->ForceRefresh();

            HideUninstallModal();
        }
    }

    // Ask mode, resolved at the only moment it can be: after the user asks for
    // the download and before the task exists.
    bool NavigationManager::MaybePromptNsoInjection(int rom_id, const std::string& platform_slug,
                                                    const std::string& title) {
        const std::string canonical = romm::model::ResolvePlatformIdentity(platform_slug, "");
        if (!romm::nso::PlatformSupportsInjection(canonical)) return false;

        auto& config = romm::model::ConfigManager::Instance();
        if (config.GetNsoInjectionMode(canonical) != romm::model::NsoInjectionMode::Ask) return false;

        // Re-detect rather than trusting a cached answer: asking "install this
        // into Switch Online?" when there is nothing to install into is a
        // question with no useful answer, so in that case the download just
        // proceeds as normal.
        auto& installer = romm::nso::NsoSnesInstaller::Instance();
        installer.RefreshDetection();
        const auto detection = installer.GetDetection();
        if (!detection.found) {
            std::cout << "[NSO] " << title << ": platform is set to ask, but no Switch Online target for "
                      << canonical << "; downloading without injection" << std::endl;
            return false;
        }

        nso_inject_modal = NsoInjectModalPayload();
        nso_inject_modal.rom_id = rom_id;
        nso_inject_modal.platform_slug = platform_slug;
        nso_inject_modal.canonical_id = canonical;
        nso_inject_modal.platform_name = romm::model::GetPlatformDisplayName(canonical, "");
        nso_inject_modal.title = title;
        nso_inject_modal.has_exefs_mod = detection.has_exefs_mod;
        nso_inject_modal.selected_row = (size_t)NsoInjectChoiceRow::InjectOnce;
        nso_inject_modal.source_screen = current_screen;
        nso_inject_modal.active = true;
        std::cout << "[NSO] Asking about injection for rom_id=" << rom_id
                  << " platform=" << canonical << std::endl;
        return true;
    }

    void NavigationManager::HandleNsoInjectModalInput(u64 keys_down) {
        if (!nso_inject_modal.active) return;

        constexpr size_t kRowCount = (size_t)NsoInjectChoiceRow::Count;

        if (keys_down & HidNpadButton_B) {
            // Cancel means cancel: nothing is downloaded and nothing is
            // written. Skipping only the injection is the "Download only" row.
            nso_inject_modal.active = false;
            std::cout << "[NSO] Injection prompt cancelled for rom_id=" << nso_inject_modal.rom_id << std::endl;
            return;
        }
        if (keys_down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
            if (nso_inject_modal.selected_row > 0) nso_inject_modal.selected_row--;
            return;
        }
        if (keys_down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
            if (nso_inject_modal.selected_row + 1 < kRowCount) nso_inject_modal.selected_row++;
            return;
        }
        if (!(keys_down & HidNpadButton_A)) return;

        const auto row = (NsoInjectChoiceRow)nso_inject_modal.selected_row;
        const bool inject = (row == NsoInjectChoiceRow::InjectOnce || row == NsoInjectChoiceRow::AlwaysInject);

        // "Always" / "Never" are the don't-ask-again affordance: they answer
        // this download AND rewrite the per-platform setting, so the same
        // question isn't asked for the next game on that platform.
        if (row == NsoInjectChoiceRow::AlwaysInject || row == NsoInjectChoiceRow::NeverInject) {
            const auto mode = (row == NsoInjectChoiceRow::AlwaysInject) ? romm::model::NsoInjectionMode::Always
                                                                       : romm::model::NsoInjectionMode::Off;
            auto& config = romm::model::ConfigManager::Instance();
            config.SetNsoInjectionMode(nso_inject_modal.canonical_id, mode);
            config.Save();
            std::cout << "[NSO] platform=" << nso_inject_modal.canonical_id << " injection="
                      << romm::model::ConfigManager::NsoInjectionModeToString(mode)
                      << " (set from the download prompt)" << std::endl;
        }

        const int rom_id = nso_inject_modal.rom_id;
        const std::string slug = nso_inject_modal.platform_slug;
        const std::string title = nso_inject_modal.title;
        nso_inject_modal.active = false;

        // The detail is re-read rather than captured: the modal outlives the
        // frame that raised it, and the cache is what every other download path
        // reads too. It is missing only if the model was rebuilt underneath us,
        // in which case there is nothing to enqueue.
        const auto* detail = model ? model->GetCachedDetail(rom_id) : nullptr;
        if (!detail) {
            std::cerr << "[NSO] Injection prompt answered but detail for rom " << rom_id
                      << " is no longer cached; nothing enqueued" << std::endl;
            return;
        }

        romm::model::DownloadManager::Instance().EnqueueDownload(
            *detail, slug, title,
            inject ? romm::model::InjectChoice::Yes : romm::model::InjectChoice::No);
        UpdateLayoutSelection();
    }

    void NavigationManager::HandleLibraryMenuInput(u64 keys_down) {
        if (!library_menu_active) return;

        if (keys_down & HidNpadButton_B) {
            library_menu_active = false;
            std::cout << "[NAV] [Y MENU] Closed" << std::endl;
        }
        else if (keys_down & HidNpadButton_Up) {
            if (library_menu_selected_idx > 0) library_menu_selected_idx--;
        }
        else if (keys_down & HidNpadButton_Down) {
            if (library_menu_selected_idx + 1 < romm::ui::LibraryMenuModal::GetRowCount()) library_menu_selected_idx++;
        }
        else if (keys_down & HidNpadButton_A) {
            if (library_menu_selected_idx == 0) { // Search: prompt, then close
                // Close before prompting: swkbd is a blocking applet, and
                // leaving the menu drawn underneath it means it's still up when
                // the user comes back having just acted.
                library_menu_active = false;
                PromptForSearch();
            }
            else if (library_menu_selected_idx == 2) { // View Mode: press to cycle in place
                // Cycles only the CURRENT platform's view mode (per-platform
                // override) — the global default (Settings > General) is a
                // separate value, changed only from Settings.
                auto& config = romm::model::ConfigManager::Instance();
                std::string platform_slug;
                if (model) {
                    const auto& platforms = model->GetPlatforms();
                    if (loaded_platform_idx < platforms.size()) {
                        platform_slug = platforms[loaded_platform_idx].slug;
                    }
                }
                if (!platform_slug.empty()) {
                    romm::model::GridViewMode mode = config.GetGridViewMode(platform_slug);
                    romm::model::GridViewMode next =
                        (mode == romm::model::GridViewMode::Default) ? romm::model::GridViewMode::Big :
                        (mode == romm::model::GridViewMode::Big)     ? romm::model::GridViewMode::Detail :
                                                                        romm::model::GridViewMode::Default;
                    config.SetGridViewModeForPlatform(platform_slug, next);
                    config.Save();
                    std::cout << "[NAV] [Y MENU] View mode for platform=" << platform_slug
                              << " set to " << config.GetGridViewModeString(platform_slug) << std::endl;

                    // Force the grid to recompute its column layout immediately —
                    // it otherwise only refreshes CoverProfile on a platform change.
                    UpdateLayoutSelection();
                }
            }
            // Search / Sort are stubs for now — no-op until implemented.
        }
    }

    void NavigationManager::HandleUpdateModalInput(u64 keys_down) {
        if (!update_modal_active) return;

        if (keys_down & HidNpadButton_B) {
            // "Later" — remember this exact version so it won't nag again
            // until a newer one ships. Badges (Settings card / Updates row)
            // stay up regardless; only the popup itself is suppressed.
            auto manifest = romm::model::UpdateManager::Instance().GetRemoteManifest();
            auto& config = romm::model::ConfigManager::Instance();
            config.SetDismissedUpdateVersion(manifest.version);
            config.Save();
            update_modal_active = false;
            std::cout << "[NAV] [UPDATE POPUP] Dismissed for version " << manifest.version << std::endl;
        }
        else if (keys_down & HidNpadButton_A) {
            // Jump straight to Settings > Updates rather than installing
            // directly from the popup — reuses the existing progress/
            // changelog/confirm UI there instead of duplicating it here.
            update_modal_active = false;
            current_screen = Screen::Settings;
            selected_settings_category_idx = romm::ui::CategoryIndex(romm::ui::SettingsCategory::Updates);
            settings_focus = romm::ui::SettingsFocusArea::OptionList;
            // Land on the install row, not row 0 — row 0 cycles the update
            // channel, and the popup's A button means "take me to this
            // update". The install row sits right after the three fixed rows
            // (Channel, Check on startup, Check for updates) whenever one is
            // being offered, which is exactly the case that raised the popup.
            selected_settings_option_idx =
                (romm::model::UpdateManager::Instance().GetState() == romm::model::UpdateState::UpdateAvailable) ? 3 : 0;
            if (settings_layout) {
                app->LoadLayout(settings_layout);
            }
            std::cout << "[NAV] [UPDATE POPUP] Opening Settings > Updates" << std::endl;
            UpdateLayoutSelection();
        }
    }

    void NavigationManager::ApplyPlatformVisibilityChange() {
        if (!model) return;

        // Anchor on platform ids, not indices: the filter reorders the list, so
        // an index that was valid a moment ago can point at a different
        // platform (or nothing) afterwards.
        std::string loaded_id;
        std::string selected_id;
        {
            const auto& before = model->GetPlatforms();
            if (loaded_platform_idx < before.size()) loaded_id = before[loaded_platform_idx].id;
            if (selected_platform_idx < before.size()) selected_id = before[selected_platform_idx].id;
        }

        model->RebuildVisiblePlatforms();

        const auto& after = model->GetPlatforms();
        if (after.empty()) {
            // Everything hidden: park on 0 and let the sidebar/grid draw their
            // empty states. Nothing indexes into the list in that state.
            loaded_platform_idx = 0;
            selected_platform_idx = 0;
            selected_game_idx = 0;
            selected_letter_idx = 0;
            ClearSearch();
            ClearBulkSelection();
        } else {
            // Nearest still-visible entry when the anchor itself disappeared —
            // indices only shift downwards under a filter, so clamping to the
            // last valid index is the closest surviving neighbour.
            auto resolve = [&after](const std::string& id, size_t fallback) -> size_t {
                for (size_t i = 0; i < after.size(); ++i) {
                    if (after[i].id == id) return i;
                }
                return (fallback < after.size()) ? fallback : after.size() - 1;
            };

            const size_t new_loaded = resolve(loaded_id, loaded_platform_idx);
            selected_platform_idx = resolve(selected_id, selected_platform_idx);

            const bool loaded_platform_survived = (!loaded_id.empty() && after[new_loaded].id == loaded_id);
            loaded_platform_idx = new_loaded;
            if (!loaded_platform_survived) {
                // A different platform's games are about to be shown; the
                // per-platform selection, filter and marks no longer apply.
                selected_game_idx = 0;
                selected_letter_idx = 0;
                ClearSearch();
                ClearBulkSelection();
            }
        }

        if (library_layout) {
            // The grid caches by platform index, which can survive this change
            // unchanged while pointing at a different platform.
            if (auto grid = library_layout->GetGameGrid()) {
                grid->InvalidatePlatformCache();
            }
            library_layout->OnSelectionUpdated();
        }
    }

    void NavigationManager::PollUpdateNotification() {
        if (update_modal_active || update_popup_shown_this_session) return;
        if (current_screen != Screen::MainMenu) return; // don't yank a popup over whatever else the user's doing

        auto& um = romm::model::UpdateManager::Instance();
        if (um.GetState() != romm::model::UpdateState::UpdateAvailable) return;

        auto manifest = um.GetRemoteManifest();
        if (manifest.version.empty()) return;

        auto& config = romm::model::ConfigManager::Instance();
        if (manifest.version == config.GetDismissedUpdateVersion()) return;

        update_modal_active = true;
        update_popup_shown_this_session = true;
        std::cout << "[NAV] [UPDATE POPUP] Showing for version " << manifest.version << std::endl;
    }

    void NavigationManager::HandleInput(const u64 keys_down, const u64 keys_held) {
        static auto last_transition_time = std::chrono::high_resolution_clock::time_point();
        auto now = std::chrono::high_resolution_clock::now();

        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_transition_time).count() < 200) {
            return; // Ignore input immediately after screen transitions to prevent bounce
        }

        // Block all background input if modal is active
        if (uninstall_modal.active) {
            HandleUninstallModalInput(keys_down);
            return;
        }

        if (nso_inject_modal.active) {
            HandleNsoInjectModalInput(keys_down);
            return;
        }

        if (library_menu_active) {
            HandleLibraryMenuInput(keys_down);
            return;
        }

        if (update_modal_active) {
            HandleUpdateModalInput(keys_down);
            return;
        }

        Screen old_screen = current_screen;

        // Buttons that support hold-to-repeat (directional only, not A/B/Plus)
        static const u64 DIR_MASK =
            HidNpadButton_Up    | HidNpadButton_Down    | HidNpadButton_Left    | HidNpadButton_Right |
            HidNpadButton_StickLUp | HidNpadButton_StickLDown | HidNpadButton_StickLLeft | HidNpadButton_StickLRight |
            HidNpadButton_StickRUp | HidNpadButton_StickRDown | HidNpadButton_ZL | HidNpadButton_ZR;

        u64 dir_held = keys_held & DIR_MASK;

        // Build keys_effective: fresh presses always included; directionals also fire on repeat
        u64 keys_effective = keys_down;

        if (dir_held == 0) {
            // No directional held — reset repeat state
            repeat_held_button = 0;
        } else if (keys_down & DIR_MASK) {
            // Fresh directional press — handle immediately via keys_down, reset repeat timer
            repeat_held_button = dir_held;
            repeat_start_time = now;
            repeat_last_time = now;
        } else if (repeat_held_button == dir_held) {
            // Same direction still held — check if repeat interval has elapsed
            auto held_ms  = std::chrono::duration_cast<std::chrono::milliseconds>(now - repeat_start_time).count();
            auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - repeat_last_time).count();
            if (held_ms >= 300) {
                // Accelerate after 1 second of continuous hold
                int interval_ms = (held_ms >= 1000) ? 40 : 70;
                if (since_ms >= interval_ms) {
                    repeat_last_time = now;
                    keys_effective |= dir_held; // inject repeat movement
                }
            }
        } else {
            // Direction changed while held (e.g. Down -> Up) — reset timer
            repeat_held_button = dir_held;
            repeat_start_time = now;
            repeat_last_time = now;
        }

        if (keys_effective == 0) {
            if (current_screen == Screen::Detail && detail_layout) {
                // Background updates not strictly necessary since UI polls snapshots on render, but harmless to call a function if needed.
            } else if (current_screen == Screen::Installed && installed_layout) {
                installed_layout->OnSelectionUpdated(); // Keep dynamic lists refreshed
            } else if (current_screen == Screen::Queue && queue_layout) {
                queue_layout->OnSelectionUpdated(); // Keep dynamic lists refreshed
            }
            return; // Nothing to process
        }

        bool state_changed = false;

        if (current_screen == Screen::MainMenu) {
            size_t old_idx = selected_menu_idx;
            // Main Menu grid D-pad/Stick Navigation (6 items: Row 1: 0,1,2,3; Row 2: 4,5)
            if ((keys_effective & HidNpadButton_Left) || (keys_effective & HidNpadButton_StickLLeft)) {
                if (selected_menu_idx == 1 || selected_menu_idx == 2 || selected_menu_idx == 3) {
                    selected_menu_idx--;
                    state_changed = true;
                } else if (selected_menu_idx == 5) {
                    selected_menu_idx = 4;
                    state_changed = true;
                }
            }
            else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                if (selected_menu_idx == 0 || selected_menu_idx == 1 || selected_menu_idx == 2) {
                    selected_menu_idx++;
                    state_changed = true;
                } else if (selected_menu_idx == 4) {
                    selected_menu_idx = 5;
                    state_changed = true;
                }
            }
            else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                if (selected_menu_idx == 0 || selected_menu_idx == 1) {
                    selected_menu_idx = 4; // Select File browser
                    state_changed = true;
                } else if (selected_menu_idx == 2 || selected_menu_idx == 3) {
                    selected_menu_idx = 5; // Select Settings
                    state_changed = true;
                }
            }
            else if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                if (selected_menu_idx == 4) {
                    selected_menu_idx = 0; // Go up to Games (or 1)
                    state_changed = true;
                } else if (selected_menu_idx == 5) {
                    selected_menu_idx = 2; // Go up to Queue (or 3)
                    state_changed = true;
                }
            }
            // Select item (A) - single press only
            else if (keys_down & HidNpadButton_A) {
                std::cout << "[NAV] [A PRESS] Selected Main Menu index: " << selected_menu_idx 
                          << " (" << GetMenuEntryName(selected_menu_idx) << ")" << std::endl;

                if (selected_menu_idx == 0) { // Games
                    current_screen = Screen::Library;
                    library_focus = LibraryFocus::Sidebar; // Reset focus to sidebar
                    state_changed = true;
                    // FailedConnect is otherwise terminal for the session:
                    // PollDeferredNetworkStart gives up after
                    // NETWORK_WAIT_TIMEOUT_SECONDS and sets it, and nothing
                    // retries afterwards — so a connection that drops and comes
                    // back leaves the library stuck on "Failed to connect" until
                    // the app is restarted. Re-entering Games is the natural
                    // "try again" gesture, so treat it as one. Deliberately not
                    // extended to Unauthorized: a bad API key fails identically
                    // every time, and Settings already refetches when it changes.
                    if (model->GetPlatformState() == romm::model::ApiState::FailedConnect) {
                        std::cout << "[NAV] Entering Games after a failed fetch: retrying platforms." << std::endl;
                        static_cast<romm::ui::MainApplication*>(app)->TriggerFetchPlatforms();
                    }
                    app->LoadLayout(library_layout);
                    std::cout << "[NAV] [LAYOUT TRANSITION] Screen transition: Main Menu -> Library Screen" << std::endl;
                } else if (selected_menu_idx == 1) { // Installed
                    current_screen = Screen::Installed;
                    state_changed = true;
                    if (installed_layout) installed_layout->ForceRefresh();
                    app->LoadLayout(installed_layout);
                    std::cout << "[NAV] Opening Installed layout" << std::endl;
                } else if (selected_menu_idx == 2) { // Queue
                    current_screen = Screen::Queue;
                    state_changed = true;
                    if (queue_layout) queue_layout->ForceRefresh();
                    app->LoadLayout(queue_layout);
                    std::cout << "[NAV] Opening Queue layout" << std::endl;
                } else if (selected_menu_idx == 5) { // Settings
                    current_screen = Screen::Settings;
                    selected_settings_category_idx = romm::ui::CategoryIndex(romm::ui::SettingsCategory::General);
                    selected_settings_option_idx = 0;
                    settings_focus = SettingsFocusArea::CategoryList;
                    state_changed = true;
                    if (settings_layout) {
                        settings_layout->RefreshConfig();
                    }
                    app->LoadLayout(settings_layout);
                    std::cout << "[NAV] [LAYOUT TRANSITION] Screen transition: Main Menu -> Settings Screen" << std::endl;
                } else if (selected_menu_idx == 4) { // File browser
                    current_screen = Screen::FileBrowser;
                    state_changed = true;
                    if (!file_browser_layout) {
                        file_browser_layout = std::make_shared<romm::ui::FileBrowserLayout>(shared_from_this());
                    }
                    if (file_browser_layout) file_browser_layout->ForceRefresh();
                    app->LoadLayout(file_browser_layout);
                    std::cout << "[NAV] [LAYOUT TRANSITION] Screen transition: Main Menu -> File Browser Screen" << std::endl;
                } else {
                    std::cout << "[LOG] Not implemented yet" << std::endl;
                }
            }

            if (state_changed && !(keys_effective & HidNpadButton_A)) {
                std::cout << "[NAV] [MAIN MENU INDEX CHANGE] Selection: " 
                          << old_idx << " -> " << selected_menu_idx 
                          << " (" << GetMenuEntryName(selected_menu_idx) << ")" << std::endl;
            }
        }
        else if (current_screen == Screen::Library) {
            const auto& platforms = model->GetPlatforms();
            if (platforms.empty()) {
                // Retry in place. Without this, recovering from a dropped
                // connection means backing out to the main menu and coming back
                // in — which works, but isn't something the screen tells you.
                // Skipped while a fetch is already in flight, where it would
                // just cancel the request that's about to answer the question.
                if ((keys_down & HidNpadButton_A) &&
                    model->GetPlatformState() != romm::model::ApiState::Loading) {
                    std::cout << "[NAV] [A PRESS] Retrying platform fetch from empty Library." << std::endl;
                    static_cast<romm::ui::MainApplication*>(app)->TriggerFetchPlatforms();
                    state_changed = true;
                }
                if (keys_down & HidNpadButton_B) {
                    current_screen = Screen::MainMenu;
                    state_changed = true;
                    app->LoadLayout(main_menu_layout);
                    std::cout << "[NAV] [B PRESS] B pressed in Library (empty platforms): returning to Main Menu" << std::endl;
                }
                if (state_changed) {
                    UpdateLayoutSelection();
                }
                return;
            }
            // Use loaded_platform_idx for game data (what's displayed in the grid)
            // selected_platform_idx is only for sidebar focus highlight
            const auto& current_platform = platforms.at(loaded_platform_idx);

            // The letter-filtered index set, using loaded_platform_idx. Built via
            // the same romm::model::FilterGamesByLetter() predicate GameGrid uses
            // to build its render list, so the two can't silently drift apart.
            // selected_game_idx is always an index into THIS filtered list (it's what
            // GameGrid renders/highlights), so any lookup by selected_game_idx must go
            // through filtered_indices rather than current_platform.games directly.
            std::vector<size_t> filtered_indices = romm::model::FilterGames(current_platform.games, selected_letter_idx, search_query);
            size_t filtered_count = filtered_indices.size();

            // X toggles the game under the cursor in the bulk-download
            // selection. Only meaningful once a game is actually highlighted,
            // so it's ignored while the sidebar or alphabet bar has focus.
            if (keys_down & HidNpadButton_X) {
                const bool on_a_game = (library_focus == LibraryFocus::Grid ||
                                        library_focus == LibraryFocus::Panel);
                if (on_a_game && selected_game_idx < filtered_count) {
                    ToggleBulkSelection(current_platform.games[filtered_indices[selected_game_idx]].id);
                    state_changed = true;
                }
            }
            // ZR queues every selected game. Chosen over A-with-modifier because
            // it can't be hit by accident while browsing.
            else if (keys_down & HidNpadButton_ZR) {
                if (GetBulkSelectionCount() > 0) {
                    auto main_app = static_cast<romm::ui::MainApplication*>(app);
                    const std::string slug = romm::model::NormalizePlatformSlug(current_platform.slug);
                    for (const auto& game : current_platform.games) {
                        if (IsBulkSelected(game.id)) {
                            main_app->EnqueueBulkDownload(game.id, slug, game.title);
                        }
                    }
                    std::cout << "[NAV] [ZR] Bulk download queued for "
                              << GetBulkSelectionCount() << " games" << std::endl;
                    ClearBulkSelection();
                    state_changed = true;
                }
            }
            // Y opens the library menu (Search / Sort / View Mode) regardless of
            // which sub-area (sidebar/alphabet/grid) currently has focus.
            else if (keys_down & HidNpadButton_Y) {
                library_menu_active = true;
                library_menu_selected_idx = 0;
                state_changed = true;
                std::cout << "[NAV] [Y PRESS] Opened Library Menu" << std::endl;
            }
            else if (library_focus == LibraryFocus::Sidebar) {
                // Sidebar Up/Down: only moves focus highlight, does NOT trigger network fetch
                if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                    if (selected_platform_idx > 0) {
                        selected_platform_idx--;
                        state_changed = true;
                        std::cout << "[PERF] Platform focus changed: " << platforms.at(selected_platform_idx).name << std::endl;
                    }
                }
                else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                    if (selected_platform_idx + 1 < platforms.size()) {
                        selected_platform_idx++;
                        state_changed = true;
                        std::cout << "[PERF] Platform focus changed: " << platforms.at(selected_platform_idx).name << std::endl;
                    }
                }
                // Sidebar Enter (A) -> load selected platform ROMs - single press only
                else if (keys_down & HidNpadButton_A) {
                    loaded_platform_idx = selected_platform_idx;
                    selected_game_idx = 0;
                    selected_letter_idx = 0;
                    // The query was scoped to the platform being left; carrying
                    // it over would silently hide most of the new one. Same for
                    // the bulk selection — invisible ids from another platform.
                    ClearSearch();
                    ClearBulkSelection();
                    std::cout << "[PERF] Platform selected: " << platforms.at(selected_platform_idx).name << std::endl;
                    std::cout << "[LIBRARY] Platform changed to " << platforms.at(selected_platform_idx).name << "/" << platforms.at(selected_platform_idx).slug << std::endl;

                    auto main_app = static_cast<romm::ui::MainApplication*>(app);
                    main_app->TriggerFetchRoms(std::stoi(platforms.at(selected_platform_idx).id));

                    if (ShowAlphabetFilter()) {
                        library_focus = LibraryFocus::Alphabet;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Sidebar -> Alphabet Bar" << std::endl;
                    } else {
                        library_focus = LibraryFocus::Grid;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Sidebar -> Game Grid" << std::endl;
                    }
                    state_changed = true;
                }
                // Sidebar Right -> move focus to grid (no refetch)
                else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                    if (ShowAlphabetFilter()) {
                        library_focus = LibraryFocus::Alphabet;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Sidebar -> Alphabet Bar" << std::endl;
                    } else {
                        library_focus = LibraryFocus::Grid;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Sidebar -> Game Grid" << std::endl;
                    }
                    state_changed = true;
                }
                // Sidebar Back (B) -> Return to Main Menu - single press only
                else if (keys_down & HidNpadButton_B) {
                    current_screen = Screen::MainMenu;
                    state_changed = true;
                    app->LoadLayout(main_menu_layout);
                    std::cout << "[NAV] [B PRESS] B pressed in Library sidebar: returning to Main Menu" << std::endl;
                }
            }
            else if (library_focus == LibraryFocus::Alphabet) {
                // Alphabet Left/Right - repeatable
                if ((keys_effective & HidNpadButton_Left) || (keys_effective & HidNpadButton_StickLLeft)) {
                    if (selected_letter_idx > 0) {
                        selected_letter_idx--;
                        selected_game_idx = 0;
                        state_changed = true;
                        char current_letter = (selected_letter_idx == 0) ? ' ' : ('A' + (char)(selected_letter_idx - 1));
                        std::cout << "[NAV] [ALPHABET INDEX CHANGE] Selected letter index: " << selected_letter_idx << " (Letter: " << (current_letter == ' ' ? "ALL" : std::string(1, current_letter)) << ")" << std::endl;
                    } else {
                        // Go back to Sidebar
                        library_focus = LibraryFocus::Sidebar;
                        state_changed = true;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Alphabet Bar -> Sidebar" << std::endl;
                    }
                }
                else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                    if (selected_letter_idx + 1 < romm::ui::AlphabetBar::GetLetterCount()) {
                        selected_letter_idx++;
                        selected_game_idx = 0;
                        state_changed = true;
                        char current_letter = (char)('A' + selected_letter_idx - 1);
                        std::cout << "[NAV] [ALPHABET INDEX CHANGE] Selected letter index: " << selected_letter_idx << " (Letter: " << std::string(1, current_letter) << ")" << std::endl;
                    }
                }
                // Alphabet Down -> Focus Grid (repeatable direction, but transition is idempotent)
                else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown) || (keys_down & HidNpadButton_A)) {
                    if (filtered_count > 0) {
                        library_focus = LibraryFocus::Grid;
                        state_changed = true;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Alphabet Bar -> Game Grid" << std::endl;
                    }
                }
                // Alphabet Back (B) -> Focus Sidebar - single press only
                else if (keys_down & HidNpadButton_B) {
                    library_focus = LibraryFocus::Sidebar;
                    state_changed = true;
                    std::cout << "[NAV] [B PRESS] B pressed in Alphabet Bar: returning to Sidebar" << std::endl;
                    std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Alphabet Bar -> Sidebar" << std::endl;
                }
            }
            else if (library_focus == LibraryFocus::Grid) {
                // Column count comes from GameGrid — the layout that actually
                // rendered the tiles — rather than re-deriving it here, so
                // navigation math can never drift out of sync with what's on
                // screen.
                int cols = library_layout->GetGameGrid()->GetColumns();
                int row = (int)(selected_game_idx / cols);
                int col = (int)(selected_game_idx % cols);

                if ((keys_effective & HidNpadButton_Left) || (keys_effective & HidNpadButton_StickLLeft)) {
                    if (col > 0) {
                        selected_game_idx--;
                        state_changed = true;
                    } else {
                        // Return focus to Sidebar
                        library_focus = LibraryFocus::Sidebar;
                        state_changed = true;
                    }
                }
                else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                    if (selected_game_idx + 1 < filtered_count && col < cols - 1) {
                        selected_game_idx++;
                        state_changed = true;
                    } else if (library_layout->GetGameGrid()->IsDetailList() && filtered_count > 0) {
                        // Single-column list: there's no cell to the right, so
                        // Right crosses into the detail panel's action row.
                        library_focus = LibraryFocus::Panel;
                        state_changed = true;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Game List -> Detail Panel" << std::endl;
                    }
                }
                else if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                    if (row > 0) {
                        selected_game_idx -= cols;
                        state_changed = true;
                    } else {
                        if (ShowAlphabetFilter()) {
                            library_focus = LibraryFocus::Alphabet;
                            state_changed = true;
                        }
                    }
                }
                else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                    if (selected_game_idx + cols < filtered_count) {
                        selected_game_idx += cols;
                        state_changed = true;
                    }
                }
                // Enter game details (A) - single press only
                else if (keys_down & HidNpadButton_A) {
                    // In Detail view mode the panel already shows everything the
                    // Detail screen would, so A moves into its actions instead
                    // of pushing a whole new layout.
                    if (library_layout->GetGameGrid()->IsDetailList()) {
                        if (filtered_count > 0) {
                            library_focus = LibraryFocus::Panel;
                            // Land on the cover, so a second A opens it
                            // fullscreen without any further navigation.
                            panel_on_cover = true;
                            panel_desc_scroll = 0;
                            state_changed = true;
                            std::cout << "[NAV] [A PRESS] Focus: Game List -> Detail Panel" << std::endl;
                        }
                    }
                    else if (selected_game_idx < filtered_count) {
                        current_screen = Screen::Detail;
                        detail_focus = DetailFocus::Tabs;
                        selected_detail_tab_idx = 0;
                        selected_detail_action_idx = 0;
                        state_changed = true;
                        
                        auto& game = current_platform.games[filtered_indices[selected_game_idx]];
                        int rom_id = game.id;

                        if (detail_layout) {
                            romm::ui::DetailGameContext ctx;
                            ctx.rom_id = rom_id;
                            ctx.platform_name = current_platform.name;
                            ctx.platform_slug = current_platform.slug;
                            ctx.platform_slug = romm::model::NormalizePlatformSlug(ctx.platform_slug);
                            ctx.title = game.title;
                            ctx.cover_path = game.cover_path;
                            ctx.cover_path_large = game.cover_path_large;
                            ctx.is_ps1 = (current_platform.slug == "psx" || current_platform.name == "PlayStation");
                            ctx.platform_idx = selected_platform_idx;
                            detail_layout->SetContext(ctx);
                            
                            std::cout << "[DETAIL] Open platform=" << ctx.platform_slug << " rom=" << ctx.rom_id << " title=" << ctx.title << std::endl;
                            std::cout << "[DETAIL] Cover=" << ctx.cover_path << std::endl;
                        }
                        
                        if (model->GetDetailState(rom_id) == romm::model::DetailLoadState::NotLoaded ||
                            model->GetDetailState(rom_id) == romm::model::DetailLoadState::Failed) {
                            static_cast<romm::ui::MainApplication*>(app)->TriggerFetchRomDetail(rom_id, detail_layout->ctx.generation, detail_layout->ctx.platform_slug);
                        }

                        app->LoadLayout(detail_layout);
                        std::cout << "[NAV] [LAYOUT TRANSITION] Screen transition: Library Screen -> Detail Screen" << std::endl;
                    }
                }
                // Back to Alphabet Bar or Sidebar (B) - single press only
                else if (keys_down & HidNpadButton_B) {
                    if (ShowAlphabetFilter()) {
                        library_focus = LibraryFocus::Alphabet;
                        std::cout << "[NAV] [B PRESS] B pressed in grid: returning to Alphabet Bar" << std::endl;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Game Grid -> Alphabet Bar" << std::endl;
                    } else {
                        library_focus = LibraryFocus::Sidebar;
                        std::cout << "[NAV] [B PRESS] B pressed in grid: returning to Sidebar" << std::endl;
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Game Grid -> Sidebar" << std::endl;
                    }
                    state_changed = true;
                }
            }
            else if (library_focus == LibraryFocus::Panel) {
                if ((keys_effective & HidNpadButton_Left) || (keys_effective & HidNpadButton_StickLLeft) ||
                    (keys_down & HidNpadButton_B)) {
                    library_focus = LibraryFocus::Grid;
                    panel_desc_scroll = 0;
                    state_changed = true;
                    std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Detail Panel -> Game List" << std::endl;
                }
                // Down: leave the cover for the action row, then scroll the
                // description. Up reverses it, ending back on the cover.
                else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                    if (panel_on_cover) {
                        panel_on_cover = false;
                    } else {
                        panel_desc_scroll += 28;
                    }
                    state_changed = true;
                }
                else if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                    if (panel_desc_scroll > 0) {
                        panel_desc_scroll -= 28;
                        if (panel_desc_scroll < 0) panel_desc_scroll = 0;
                    } else {
                        panel_on_cover = true;
                    }
                    state_changed = true;
                }
                // A on the cover opens it fullscreen, where L/R cycle miximage
                // and large cover exactly as from the Detail screen. A on the
                // action row runs the download/uninstall action below.
                else if ((keys_down & HidNpadButton_A) && panel_on_cover) {
                    if (selected_game_idx < filtered_count && fullscreen_image_layout) {
                        const auto& game = current_platform.games[filtered_indices[selected_game_idx]];
                        const std::string slug = romm::model::NormalizePlatformSlug(current_platform.slug);
                        const auto* detail = model->GetCachedDetail(game.id);

                        romm::ui::FullscreenKeys keys;
                        if (detail && !detail->miximage_v2_url.empty()) {
                            keys.miximage_key.rom_id = game.id;
                            keys.miximage_key.platform_slug = slug;
                            keys.miximage_key.cover_source = detail->miximage_v2_url;
                            keys.miximage_key.variant = "miximage_v2";
                        }
                        const std::string large_source =
                            (detail && !detail->path_cover_large.empty()) ? detail->path_cover_large
                                                                          : game.cover_path_large;
                        if (!large_source.empty()) {
                            keys.large_key.rom_id = game.id;
                            keys.large_key.platform_slug = slug;
                            keys.large_key.cover_source = large_source;
                            keys.large_key.variant = "big";
                        }
                        if (!game.cover_path.empty()) {
                            keys.small_key.rom_id = game.id;
                            keys.small_key.platform_slug = slug;
                            keys.small_key.cover_source = game.cover_path;
                            keys.small_key.variant = "small";
                        }

                        if (keys.miximage_key.rom_id > 0 || keys.large_key.rom_id > 0 || keys.small_key.rom_id > 0) {
                            fullscreen_return_screen = Screen::Library;
                            current_screen = Screen::FullscreenImage;
                            fullscreen_image_layout->SetKeys(keys);
                            app->LoadLayout(fullscreen_image_layout);
                            state_changed = true;
                            std::cout << "[NAV] [LAYOUT TRANSITION] Library Panel -> Fullscreen Image" << std::endl;
                        }
                    }
                }
                else if (keys_down & HidNpadButton_A) {
                    if (selected_game_idx >= filtered_count) {
                        // Filter changed under us; nothing to act on.
                    } else {
                        const auto& game = current_platform.games[filtered_indices[selected_game_idx]];
                        const int rom_id = game.id;
                        const std::string slug = romm::model::NormalizePlatformSlug(current_platform.slug);
                        const auto* detail = model->GetCachedDetail(rom_id);

                        if (!detail) {
                            // The debounced prefetch hasn't landed yet. Downloading
                            // needs the file list (multi-disc games resolve their
                            // on-disk identity from it), so there's nothing safe to
                            // do but wait — the panel says as much.
                            std::cerr << "[NAV] Panel action ignored: detail not loaded for rom " << rom_id << std::endl;
                        } else {
                            auto& dl_mgr = romm::model::DownloadManager::Instance();
                            const auto action = romm::ui::ComputeDownloadActionState(rom_id, slug, detail);

                            if (action == romm::ui::DownloadActionState::Uninstall) {
                                UninstallModalPayload p;
                                p.rom_id = rom_id;
                                p.platform_slug = slug;
                                p.title = game.title;
                                p.filename = dl_mgr.InstallIdentityFilename(slug, detail->files, detail->file_name);
                                p.cover_path = game.cover_path;
                                p.source_screen = current_screen;
                                ShowUninstallModal(p);
                            } else if (action == romm::ui::DownloadActionState::Queued) {
                                dl_mgr.RemoveFromQueue(rom_id);
                            } else if (action == romm::ui::DownloadActionState::Failed) {
                                dl_mgr.RetryFailed(rom_id);
                            } else if (action == romm::ui::DownloadActionState::Download ||
                                       action == romm::ui::DownloadActionState::AddToQueue) {
                                // Ask mode answers the injection question here;
                                // the modal's own handler does the enqueue.
                                if (!MaybePromptNsoInjection(rom_id, slug, game.title)) {
                                    dl_mgr.EnqueueDownload(*detail, slug, game.title);
                                }
                            }
                            state_changed = true;
                        }
                    }
                }
            }
        }
        else if (current_screen == Screen::Detail) {
            // Scroll description logic (R-Stick or ZL/ZR)
            if ((keys_effective & HidNpadButton_StickRDown) || (keys_effective & HidNpadButton_ZR)) {
                if (detail_layout) detail_layout->ScrollDescription(1);
            }
            else if ((keys_effective & HidNpadButton_StickRUp) || (keys_effective & HidNpadButton_ZL)) {
                if (detail_layout) detail_layout->ScrollDescription(-1);
            }

            // B to exit detail - single press only
            if (keys_down & HidNpadButton_B) {
                if (detail_layout) detail_layout->OnLeave();
                current_screen = Screen::Library;
                state_changed = true;
                app->LoadLayout(library_layout);
                std::cout << "[NAV] [B PRESS] B pressed in detail view: returning to Library" << std::endl;
                std::cout << "[NAV] [LAYOUT TRANSITION] Screen transition: Detail Screen -> Library Screen" << std::endl;
            }
            else if (detail_focus == DetailFocus::Tabs) {
                if ((keys_effective & HidNpadButton_Left) || (keys_effective & HidNpadButton_StickLLeft)) {
                    if (selected_detail_tab_idx > 0) {
                        selected_detail_tab_idx--;
                        state_changed = true;
                        std::cout << "[NAV] [DETAIL TAB CHANGE] Selected detail tab: " << selected_detail_tab_idx << std::endl;
                    } else {
                        // Left on first tab -> go to Cover
                        detail_focus = DetailFocus::Cover;
                        state_changed = true;
                        if (detail_layout) detail_layout->UpdateFooterHints();
                        std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Tabs -> Cover" << std::endl;
                    }
                }
                else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                    if (selected_detail_tab_idx + 1 < romm::ui::DetailCard::GetTabCount()) {
                        selected_detail_tab_idx++;
                        state_changed = true;
                        std::cout << "[NAV] [DETAIL TAB CHANGE] Selected detail tab: " << selected_detail_tab_idx << std::endl;
                    }
                }
                else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                    detail_focus = DetailFocus::Actions;
                    selected_detail_action_idx = 0;
                    state_changed = true;
                    if (detail_layout) detail_layout->UpdateFooterHints();
                    std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Tabs -> Download Button" << std::endl;
                }
            }
            else if (detail_focus == DetailFocus::Cover) {
                if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                    detail_focus = DetailFocus::Actions;
                    selected_detail_action_idx = 0;
                    state_changed = true;
                    if (detail_layout) detail_layout->UpdateFooterHints();
                    std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Cover -> Actions" << std::endl;
                }
                else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                    detail_focus = DetailFocus::Tabs;
                    selected_detail_tab_idx = 0;
                    state_changed = true;
                    if (detail_layout) detail_layout->UpdateFooterHints();
                    std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Cover -> Tabs" << std::endl;
                }
                else if (keys_down & HidNpadButton_A) {
                    if (detail_layout && detail_layout->GetCard()) {
                        auto card = detail_layout->GetCard();
                        bool has_valid_image = (card->GetCoverTexture() != nullptr &&
                                                card->GetCoverState() != romm::ui::DetailCoverState::Placeholder &&
                                                card->GetCoverState() != romm::ui::DetailCoverState::Failed);
                        if (has_valid_image) {
                            auto keys = card->GetFullscreenKeys();
                            fullscreen_return_screen = Screen::Detail;
                            current_screen = Screen::FullscreenImage;
                            fullscreen_image_layout->SetKeys(keys);
                            app->LoadLayout(fullscreen_image_layout);
                            state_changed = true;
                            std::cout << "[NAV] [LAYOUT TRANSITION] Screen transition: Detail Screen -> Fullscreen Image Screen" << std::endl;
                        }
                    }
                }
            }
            else if (detail_focus == DetailFocus::Actions) {
                if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                    detail_focus = DetailFocus::Cover;
                    state_changed = true;
                    if (detail_layout) detail_layout->UpdateFooterHints();
                    std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Actions -> Cover" << std::endl;
                }
                else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                    detail_focus = DetailFocus::Tabs;
                    selected_detail_tab_idx = 0;
                    state_changed = true;
                    if (detail_layout) detail_layout->UpdateFooterHints();
                    std::cout << "[NAV] [FOCUS REGION CHANGE] Focus: Actions -> Tabs" << std::endl;
                }
                // A on download action - single press only
                else if (keys_down & HidNpadButton_A) {
                    std::cout << "[NAV] [A PRESS] Triggered detail action" << std::endl;
                    
                    if (detail_layout) {
                        int rom_id = detail_layout->ctx.rom_id;
                        const auto* detail = model->GetCachedDetail(rom_id);
                        if (detail) {
                            auto& dl_mgr = romm::model::DownloadManager::Instance();
                            auto task_snap = dl_mgr.GetTaskSnapshot(rom_id);

                            if (detail_layout->GetCard()) {
                                romm::ui::DownloadActionState action = detail_layout->GetCard()->GetActionState();
                                
                                if (action == romm::ui::DownloadActionState::Uninstall) {
                                    // Open Modal
                                    UninstallModalPayload p;
                                    p.rom_id = rom_id;
                                    p.platform_slug = detail_layout->ctx.platform_slug;
                                    p.title = detail_layout->ctx.title;
                                    // Multi-disc: pass the root .m3u identity; UninstallGame
                                    // sweeps the subfolder discs and playlist from there.
                                    p.filename = dl_mgr.InstallIdentityFilename(
                                        detail_layout->ctx.platform_slug, detail->files, detail->file_name);
                                    p.cover_path = detail_layout->ctx.cover_path;
                                    p.source_screen = current_screen;
                                    ShowUninstallModal(p);
                                } else if (action == romm::ui::DownloadActionState::Queued) {
                                    dl_mgr.RemoveFromQueue(rom_id);
                                } else if (action == romm::ui::DownloadActionState::Failed) {
                                    dl_mgr.RetryFailed(rom_id);
                                } else if (action == romm::ui::DownloadActionState::Download || action == romm::ui::DownloadActionState::AddToQueue) {
                                    // Same Ask resolution as the library grid.
                                    if (!MaybePromptNsoInjection(rom_id, detail_layout->ctx.platform_slug,
                                                                 detail_layout->ctx.title)) {
                                        dl_mgr.EnqueueDownload(*detail, detail_layout->ctx.platform_slug, detail_layout->ctx.title);
                                    }
                                }
                            }
                        } else {
                            std::cerr << "[NAV] Detail not loaded yet." << std::endl;
                        }
                    }
                }
            }
        }
        else if (current_screen == Screen::FullscreenImage) {
            if (keys_down & HidNpadButton_B) {
                // Return to whichever screen opened the viewer. Hardcoding the
                // Detail screen here would strand a user who reached fullscreen
                // from the library's Detail-mode panel on a screen they never
                // navigated to.
                if (fullscreen_return_screen == Screen::Library && library_layout) {
                    current_screen = Screen::Library;
                    app->LoadLayout(library_layout);
                    std::cout << "[NAV] [B PRESS] Fullscreen -> Library" << std::endl;
                } else {
                    current_screen = Screen::Detail;
                    app->LoadLayout(detail_layout);
                    std::cout << "[NAV] [B PRESS] Fullscreen -> Detail" << std::endl;
                }
                state_changed = true;
            }
            else if (keys_down & (HidNpadButton_R | HidNpadButton_Right)) {
                if (fullscreen_image_layout && fullscreen_image_layout->GetImageElement()) {
                    fullscreen_image_layout->GetImageElement()->CycleMode(1);
                    state_changed = true;
                }
            }
            else if (keys_down & (HidNpadButton_L | HidNpadButton_Left)) {
                if (fullscreen_image_layout && fullscreen_image_layout->GetImageElement()) {
                    fullscreen_image_layout->GetImageElement()->CycleMode(-1);
                    state_changed = true;
                }
            }
        }
        else if (current_screen == Screen::Settings) {
            // The experimental SNES Online screen owns the whole display while
            // it is open, so it gets first refusal on input — ahead of the
            // confirm modal and of the settings list itself.
            if (settings_layout && settings_layout->IsNsoSnesModalActive()) {
                settings_layout->HandleNsoSnesModalInput(keys_down);
                return;
            }

            // Forward input to settings layout if confirmation modal is open
            if (settings_layout && settings_layout->IsConfirmModalActive()) {
                settings_layout->HandleConfirmModalInput(keys_down);
                return; // Block background input
            }

            if (settings_focus == SettingsFocusArea::CategoryList) {
                // B -> return to Main Menu - single press only
                if (keys_down & HidNpadButton_B) {
                    current_screen = Screen::MainMenu;
                    state_changed = true;
                    app->LoadLayout(main_menu_layout);
                    std::cout << "[SETTINGS] Closed" << std::endl;
                }
                // Up/Down to navigate categories
                else if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                    if (selected_settings_category_idx > 0) {
                        selected_settings_category_idx--;
                        selected_settings_option_idx = 0;
                        state_changed = true;
                        std::cout << "[SETTINGS] Category changed to " << selected_settings_category_idx << std::endl;
                    }
                }
                else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                    if (selected_settings_category_idx + 1 < romm::ui::SettingsLayout::GetCategoriesCount()) {
                        selected_settings_category_idx++;
                        selected_settings_option_idx = 0;
                        state_changed = true;
                        std::cout << "[SETTINGS] Category changed to " << selected_settings_category_idx << std::endl;
                    }
                }
                // A -> enter OptionList
                else if (keys_down & HidNpadButton_A) {
                    settings_focus = SettingsFocusArea::OptionList;
                    selected_settings_option_idx = 0;
                    if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Platforms) {
                        // Always land on the platform list, never inside a
                        // platform's rows.
                        rom_path_rows_focused = false;
                        selected_rom_path_row_idx = 0;
                    }
                    state_changed = true;
                    std::cout << "[SETTINGS] Entered OptionList for Category " << selected_settings_category_idx << std::endl;
                }
            }
            else {
                // OptionList Focus
                if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Platforms) {
                    // Platforms is two-level: the platform list, and the rows
                    // for whichever platform is selected. This replaces the old
                    // ROM Paths tab bar, which could only ever address the eight
                    // platforms hardcoded in SUPPORTED_PLATFORMS.
                    if (rom_path_rows_focused) {
                        // Level 2: rows for one platform.
                        if (keys_down & HidNpadButton_B) {
                            rom_path_rows_focused = false;
                            state_changed = true;
                        }
                        else if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                            if (selected_rom_path_row_idx > 0) {
                                selected_rom_path_row_idx--;
                                state_changed = true;
                            }
                        }
                        else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                            if (selected_rom_path_row_idx + 1 < romm::ui::SettingsLayout::GetSelectableRomPathRowCount()) {
                                selected_rom_path_row_idx++;
                                state_changed = true;
                            }
                        }
                        else if (keys_down & HidNpadButton_A) {
                            if (settings_layout) {
                                settings_layout->ActivateSelectedPlatformRow();
                                state_changed = true;
                            }
                        }
                        else if (keys_down & HidNpadButton_X) {
                            if (settings_layout) {
                                settings_layout->ValidateOrCreateSelectedPath();
                                state_changed = true;
                            }
                        }
                        else if (keys_down & HidNpadButton_Y) {
                            if (settings_layout) {
                                settings_layout->ResetSelectedRomPath();
                                state_changed = true;
                            }
                        }
                    }
                    else {
                        // Level 1: the platform list. Show All / Reset Defaults
                        // sit above it and act immediately; a platform row opens
                        // its settings with A and toggles visibility with
                        // Left/Right, so the list stays scannable.
                        const size_t row_count = romm::ui::SettingsLayout::GetOptionsCount(selected_settings_category_idx);
                        if (keys_down & HidNpadButton_B) {
                            settings_focus = SettingsFocusArea::CategoryList;
                            state_changed = true;
                        }
                        else if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                            if (selected_settings_option_idx > 0) {
                                selected_settings_option_idx--;
                                state_changed = true;
                            }
                        }
                        else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                            if (selected_settings_option_idx + 1 < row_count) {
                                selected_settings_option_idx++;
                                state_changed = true;
                            }
                        }
                        else if ((keys_effective & HidNpadButton_Left) || (keys_effective & HidNpadButton_StickLLeft)) {
                            if (settings_layout) {
                                settings_layout->ToggleSelectedPlatform(false);
                                state_changed = true;
                            }
                        }
                        else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                            if (settings_layout) {
                                settings_layout->ToggleSelectedPlatform(true);
                                state_changed = true;
                            }
                        }
                        else if (keys_down & HidNpadButton_A) {
                            if (selected_settings_option_idx < romm::ui::SettingsLayout::GetPlatformActionRowCount()) {
                                // Show All / Reset Defaults act in place.
                                if (settings_layout) {
                                    settings_layout->HandleOptionAction(selected_settings_category_idx,
                                                                        selected_settings_option_idx);
                                }
                            } else {
                                rom_path_rows_focused = true;
                                selected_rom_path_row_idx = 0;
                            }
                            state_changed = true;
                        }
                    }
                }
                else {
                    // General option navigation
                    if (keys_down & HidNpadButton_B) {
                        settings_focus = SettingsFocusArea::CategoryList;
                        state_changed = true;
                        std::cout << "[SETTINGS] Returned to CategoryList" << std::endl;
                    }
                    else if ((keys_effective & HidNpadButton_Up) || (keys_effective & HidNpadButton_StickLUp)) {
                        if (selected_settings_option_idx > 0) {
                            selected_settings_option_idx--;
                            state_changed = true;
                        }
                    }
                    else if ((keys_effective & HidNpadButton_Down) || (keys_effective & HidNpadButton_StickLDown)) {
                        size_t max_opts = romm::ui::SettingsLayout::GetOptionsCount(selected_settings_category_idx);
                        if (selected_settings_option_idx + 1 < max_opts) {
                            selected_settings_option_idx++;
                            state_changed = true;
                        }
                    }
                    // Left/Right on the Startup Sound / Startup Volume / Menu
                    // Ambience / Menu Ambience Volume rows. No-op on every other
                    // row/category (Download Sound Pack is now a single A-press
                    // action, not browsable). Platforms handles its own
                    // Left/Right above and never reaches here.
                    else if ((keys_effective & HidNpadButton_Left) || (keys_effective & HidNpadButton_StickLLeft)) {
                        if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 2 && settings_layout) {
                            settings_layout->CycleStartupSound(-1);
                            state_changed = true;
                        } else if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 3 && settings_layout) {
                            settings_layout->CycleStartupVolume(-1);
                            state_changed = true;
                        } else if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 4 && settings_layout) {
                            settings_layout->CycleThemeSound(-1);
                            state_changed = true;
                        } else if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 5 && settings_layout) {
                            settings_layout->CycleAmbientVolume(-1);
                            state_changed = true;
                        }
                    }
                    else if ((keys_effective & HidNpadButton_Right) || (keys_effective & HidNpadButton_StickLRight)) {
                        if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 2 && settings_layout) {
                            settings_layout->CycleStartupSound(1);
                            state_changed = true;
                        } else if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 3 && settings_layout) {
                            settings_layout->CycleStartupVolume(1);
                            state_changed = true;
                        } else if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 4 && settings_layout) {
                            settings_layout->CycleThemeSound(1);
                            state_changed = true;
                        } else if (romm::ui::CategoryAt(selected_settings_category_idx) == romm::ui::SettingsCategory::Theme && selected_settings_option_idx == 5 && settings_layout) {
                            settings_layout->CycleAmbientVolume(1);
                            state_changed = true;
                        }
                    }
                    else if (keys_down & HidNpadButton_A) {
                        if (settings_layout) {
                            settings_layout->HandleOptionAction(selected_settings_category_idx, selected_settings_option_idx);
                            state_changed = true;
                        }
                    }
                }
            }
        }
        else if (current_screen == Screen::Installed) {
            if (keys_down & HidNpadButton_B) {
                current_screen = Screen::MainMenu;
                state_changed = true;
                app->LoadLayout(main_menu_layout);
            }
            else {
                installed_layout->HandleInput(keys_down, 0, keys_held, pu::ui::TouchPoint());
            }
        }
        else if (current_screen == Screen::Queue) {
            if (keys_down & HidNpadButton_B) {
                current_screen = Screen::MainMenu;
                state_changed = true;
                app->LoadLayout(main_menu_layout);
            }
            else {
                queue_layout->HandleInput(keys_down, 0, keys_held, pu::ui::TouchPoint());
            }
        }
        else if (current_screen == Screen::FileBrowser) {
            if (file_browser_layout) {
                file_browser_layout->HandleInput(keys_down, 0, keys_held, pu::ui::TouchPoint());
            }
        }

        if (current_screen != old_screen) {
            last_transition_time = std::chrono::high_resolution_clock::now();
            if (old_screen == Screen::FileBrowser && file_browser_layout) {
                file_browser_layout->CancelPendingScan();
            }
        }

        if (state_changed) {
            UpdateLayoutSelection();
        }
    }

    bool NavigationManager::ShowAlphabetFilter() const {
        if (model) {
            const auto& platforms = model->GetPlatforms();
            if (loaded_platform_idx < platforms.size()) {
                const auto& plat = platforms.at(loaded_platform_idx);
                if (plat.slug == "nds" || plat.slug == "nintendo-ds" ||
                    plat.slug == "nintendo_ds" || plat.slug == "Nintendo DS" ||
                    plat.slug == "gb" || plat.slug == "game-boy" || plat.slug == "gameboy" || plat.slug == "nintendo-game-boy" ||
                    plat.slug == "gbc" || plat.slug == "game-boy-color" || plat.slug == "gameboy-color" || plat.slug == "nintendo-game-boy-color" ||
                    plat.slug == "gba" || plat.slug == "game-boy-advance" || plat.slug == "gameboy-advance" || plat.slug == "nintendo-game-boy-advance" ||
                    plat.slug == "3ds" || plat.slug == "nintendo-3ds" || plat.slug == "n3ds" || plat.slug == "nintendo_3ds") {
                    return false;
                }
            }
        }
        return show_alphabet_filter;
    }

}
