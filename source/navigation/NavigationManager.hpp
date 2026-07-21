#pragma once

#include <pu/Plutonium>
#include "../model/DataModel.hpp"
#include <memory>
#include <iostream>
#include <chrono>
#include "../ui/SettingsLayout.hpp"

namespace romm::ui {
    class MainMenuLayout;
    class LibraryLayout;
    class DetailLayout;
    class SettingsLayout;
    class InstalledLayout;
    class QueueLayout;
    class FileBrowserLayout;
    class FullscreenImageLayout;
}

namespace romm::navigation {

    enum class Screen {
        MainMenu,
        Library,
        Detail,
        Settings,
        Installed,
        Queue,
        FileBrowser,
        FullscreenImage
    };

    enum class LibraryFocus {
        Sidebar,
        Alphabet,
        Grid
    };

    enum class DetailFocus {
        Tabs,
        Actions,
        Cover
    };

    enum class CoverSize {
        Small,
        Large
    };

    struct UninstallModalPayload {
        bool active = false;
        int rom_id = 0;
        std::string platform_slug;
        std::string title;
        std::string filename;
        std::string cover_path;
        Screen source_screen;
    };

    class NavigationManager : public std::enable_shared_from_this<NavigationManager> {
    private:
        pu::ui::Application* app;
        std::shared_ptr<romm::model::DataModel> model;
        
        Screen current_screen;
        LibraryFocus library_focus;
        
        size_t selected_menu_idx;     // Main Menu active card index (0 to 6)
        size_t selected_platform_idx; // Library active platform index (focused in sidebar)
        size_t loaded_platform_idx;   // Library loaded platform index (whose ROMs are visible)
        size_t selected_game_idx;     // Library active game index
        
        size_t selected_letter_idx;   // Alphabet slider selection (0 = ALL, 1-26 = A-Z)
        
        DetailFocus detail_focus;
        size_t selected_detail_tab_idx;    // Active detail tab (0 = DETAILS, 1 = SAVE DATA, 2 = MODS, 3 = CHEATS)
        size_t selected_detail_action_idx; // Active detail action (0 = Download)

        // D-pad hold-repeat state
        u64 repeat_held_button;
        std::chrono::high_resolution_clock::time_point repeat_start_time;
        std::chrono::high_resolution_clock::time_point repeat_last_time;
        std::chrono::high_resolution_clock::time_point input_cooldown_until;

        // Settings variables
        bool show_alphabet_filter = false;
        CoverSize cover_size = CoverSize::Large;
        size_t selected_settings_idx = 0;
        size_t selected_settings_category_idx = 0;
        size_t selected_settings_option_idx = 0;
        romm::ui::SettingsFocusArea settings_focus = romm::ui::SettingsFocusArea::CategoryList;

        // ROM Paths redesigned state
        size_t selected_rom_path_platform_idx = 0;
        size_t selected_rom_path_row_idx = 0;
        bool rom_path_rows_focused = false;

        // Library "Y-Menu" state (Search / Sort / View Mode)
        bool library_menu_active = false;
        size_t library_menu_selected_idx = 0;  // 0=Search, 1=Sort, 2=View Mode

        // Persistent layouts created once
        std::shared_ptr<romm::ui::MainMenuLayout> main_menu_layout;
        std::shared_ptr<romm::ui::LibraryLayout> library_layout;
        std::shared_ptr<romm::ui::DetailLayout> detail_layout;
        std::shared_ptr<romm::ui::SettingsLayout> settings_layout;
        std::shared_ptr<romm::ui::InstalledLayout> installed_layout;
        std::shared_ptr<romm::ui::QueueLayout> queue_layout;
        std::shared_ptr<romm::ui::FileBrowserLayout> file_browser_layout;
        std::shared_ptr<romm::ui::FullscreenImageLayout> fullscreen_image_layout;

        void UpdateLayoutSelection();

    public:
        NavigationManager(pu::ui::Application* app, std::shared_ptr<romm::model::DataModel> model);
        
        void Initialize();
        void HandleInput(const u64 keys_down, const u64 keys_held);
        static std::string ShowKeyboard(const std::string& header, const std::string& subtext, const std::string& initial_text);
        
        Screen GetScreen() const { return current_screen; }
        void SetCurrentScreen(Screen s) { current_screen = s; }
        LibraryFocus GetLibraryFocus() const { return library_focus; }
        size_t GetSelectedMenuIdx() const { return selected_menu_idx; }
        size_t GetSelectedPlatformIdx() const { return selected_platform_idx; }
        size_t GetLoadedPlatformIdx() const { return loaded_platform_idx; }
        void SetLoadedPlatformIdx(size_t idx) { loaded_platform_idx = idx; }
        void SetSelectedPlatformIdx(size_t idx) { selected_platform_idx = idx; }
        size_t GetSelectedGameIdx() const { return selected_game_idx; }
        
        size_t GetSelectedLetterIdx() const { return selected_letter_idx; }
        DetailFocus GetDetailFocus() const { return detail_focus; }
        size_t GetSelectedDetailTabIdx() const { return selected_detail_tab_idx; }
        size_t GetSelectedDetailActionIdx() const { return selected_detail_action_idx; }
        
        // Settings accessors
        bool ShowAlphabetFilter() const;
        void SetShowAlphabetFilter(bool show) { show_alphabet_filter = show; }
        CoverSize GetCoverSize() const { return cover_size; }
        void SetCoverSize(CoverSize size) { cover_size = size; }
        size_t GetSelectedSettingsIdx() const { return selected_settings_idx; }
        void SetSelectedSettingsIdx(size_t idx) { selected_settings_idx = idx; }
        size_t GetSelectedSettingsCategoryIdx() const { return selected_settings_category_idx; }
        void SetSelectedSettingsCategoryIdx(size_t idx) { selected_settings_category_idx = idx; }
        size_t GetSelectedSettingsOptionIdx() const { return selected_settings_option_idx; }
        void SetSelectedSettingsOptionIdx(size_t idx) { selected_settings_option_idx = idx; }
        romm::ui::SettingsFocusArea GetSettingsFocus() const { return settings_focus; }
        void SetSettingsFocus(romm::ui::SettingsFocusArea focus) { settings_focus = focus; }

        size_t GetSelectedRomPathPlatformIdx() const { return selected_rom_path_platform_idx; }
        void SetSelectedRomPathPlatformIdx(size_t idx) { selected_rom_path_platform_idx = idx; }
        size_t GetSelectedRomPathRowIdx() const { return selected_rom_path_row_idx; }
        void SetSelectedRomPathRowIdx(size_t idx) { selected_rom_path_row_idx = idx; }
        bool IsRomPathRowsFocused() const { return rom_path_rows_focused; }
        void SetRomPathRowsFocused(bool focused) { rom_path_rows_focused = focused; }

        std::shared_ptr<romm::ui::MainMenuLayout> GetMainMenuLayout() { return main_menu_layout; }
        std::shared_ptr<romm::ui::LibraryLayout> GetLibraryLayout() { return library_layout; }
        std::shared_ptr<romm::ui::DetailLayout> GetDetailLayout() { return detail_layout; }
        std::shared_ptr<romm::ui::SettingsLayout> GetSettingsLayout() { return settings_layout; }
        std::shared_ptr<romm::ui::InstalledLayout> GetInstalledLayout() const { return installed_layout; }
        std::shared_ptr<romm::ui::QueueLayout> GetQueueLayout() const { return queue_layout; }
        std::shared_ptr<romm::ui::FileBrowserLayout> GetFileBrowserLayout() { return file_browser_layout; }
        std::shared_ptr<romm::ui::FullscreenImageLayout> GetFullscreenImageLayout() { return fullscreen_image_layout; }
        std::shared_ptr<romm::model::DataModel> GetModel() { return model; }
        pu::ui::Application* GetApp() { return app; }

        // Modal Controls
        void ShowUninstallModal(const UninstallModalPayload& payload) { uninstall_modal = payload; uninstall_modal.active = true; }
        void HideUninstallModal() { uninstall_modal.active = false; }
        const UninstallModalPayload& GetUninstallModalState() const { return uninstall_modal; }
        void HandleUninstallModalInput(u64 keys_down);

        // Library Y-Menu controls
        bool IsLibraryMenuActive() const { return library_menu_active; }
        size_t GetLibraryMenuSelectedIdx() const { return library_menu_selected_idx; }
        void HandleLibraryMenuInput(u64 keys_down);

        // Startup update-available popup
        bool IsUpdateModalActive() const { return update_modal_active; }
        void HandleUpdateModalInput(u64 keys_down);
        // Call once per rendered frame (works even while the user isn't
        // pressing anything, unlike HandleInput) — shows the popup the first
        // time a background update check lands on UpdateAvailable while the
        // user is on the Main Menu, once per session, unless already
        // dismissed for that exact version.
        void PollUpdateNotification();

    private:
        UninstallModalPayload uninstall_modal;
        bool update_modal_active = false;
        bool update_popup_shown_this_session = false;
    };

}
