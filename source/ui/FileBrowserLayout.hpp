#pragma once

#include <pu/Plutonium>
#include <memory>
#include <vector>
#include <unordered_set>
#include <string>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>
#include <unordered_map>

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    std::string GetSafeFont(const std::string& requested, const std::string& fallback);

    enum class FileBrowserFocus {
        Locations,
        Files,
        OptionsMenu,
        DeleteConfirm,
        PropertiesModal
    };

    struct LocationEntry {
        std::string name;
        std::string path;
        bool exists = false;
        pu::sdl2::Texture text_tex_selected = nullptr;
        pu::sdl2::Texture text_tex_unselected = nullptr;
    };

    struct FileEntry {
        std::string name;
        std::string path;
        bool is_dir = false;
        long long size = 0;
        time_t mtime = 0;
        int item_count = -1;
        pu::sdl2::Texture text_tex_selected = nullptr;
        pu::sdl2::Texture text_tex_unselected = nullptr;
        pu::sdl2::Texture details_tex_selected = nullptr;
        pu::sdl2::Texture details_tex_unselected = nullptr;
    };

    struct DetailTextures {
        std::string cached_path;
        pu::sdl2::Texture name_tex = nullptr;
        pu::sdl2::Texture type_tex = nullptr;
        pu::sdl2::Texture path_tex = nullptr;
        pu::sdl2::Texture size_tex = nullptr;
        pu::sdl2::Texture time_tex = nullptr;

        void Clear() {
            if (name_tex) { pu::ui::render::DeleteTexture(name_tex); name_tex = nullptr; }
            if (type_tex) { pu::ui::render::DeleteTexture(type_tex); type_tex = nullptr; }
            if (path_tex) { pu::ui::render::DeleteTexture(path_tex); path_tex = nullptr; }
            if (size_tex) { pu::ui::render::DeleteTexture(size_tex); size_tex = nullptr; }
            if (time_tex) { pu::ui::render::DeleteTexture(time_tex); time_tex = nullptr; }
            cached_path = "";
        }
    };

    struct NetworkInfo {
        std::string status_line1;
        std::string status_line2;
    };

    // Small string→texture cache for text that changes rarely but was being
    // re-rendered with TTF every frame (header stats, footer counter, error
    // line). Re-renders only when the string actually changes.
    struct CachedText {
        std::string str;
        pu::sdl2::Texture tex = nullptr;

        void Clear() {
            if (tex) { pu::ui::render::DeleteTexture(tex); tex = nullptr; }
            str.clear();
        }
    };

    struct StorageInfo {
        double sd_free = 0.0;
        double sd_total = 0.0;
        double sys_free = 0.0;
        double sys_total = 0.0;
    };

    class FileBrowserPane : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        // Navigation state
        FileBrowserFocus active_focus = FileBrowserFocus::Files;
        std::string current_path = "sdmc:/";
        std::string current_scan_path = "";
        uint32_t scan_generation_id = 0;

        // Data containers
        std::vector<LocationEntry> locations;
        std::vector<FileEntry> loaded_items;
        // Entries displaced by a directory scan. Their textures must be freed
        // on the render thread, so the scan thread parks them here (under
        // data_mutex) and OnRender frees them on its next pass.
        std::vector<FileEntry> retired_items;
        // Stable identifiers for the contextual Options rows, parallel to the
        // rendered labels below. Input handling branches on these ids — never on
        // the label, which changes with the UI language.
        enum class FileOption {
            Mount, Open, Properties, Refresh,
            CreateFolder, Rename, Delete, DeleteMarked
        };
        std::vector<FileOption> options_menu_ids;
        std::vector<std::string> options_menu_items;
        // Bulk-operation marks, keyed by absolute path. Cleared whenever the
        // listing changes directory — a mark the user can no longer see must
        // never end up in a delete batch.
        std::unordered_set<std::string> marked_paths;
        bool confirm_is_bulk = false;
        std::string confirm_title_str;

        void ToggleMark(const FileEntry& item);
        // Deletes one path. 0 = ok, 1 = not writable, 2 = directory not empty,
        // 3 = syscall failed.
        int DeleteOnePath(const std::string& path);

        std::vector<std::string> confirm_menu_items;

        // Selected indices
        size_t selected_location_idx = 0;
        size_t selected_file_idx = 0;
        size_t selected_option_idx = 0;
        size_t selected_confirm_idx = 0;

        // Scroll offsets
        int file_scroll_offset = 0;

        // Loading and thread state
        bool is_loading = false;
        bool load_failed = false;
        std::string error_msg = "";
        bool needs_layout_update = false;
        std::mutex data_mutex;
        std::thread scan_thread;
        std::atomic<bool> cancel_scan;

        // Caching system and network statistics
        NetworkInfo cached_net_info;
        StorageInfo cached_store_info;
        std::chrono::steady_clock::time_point last_stats_time;
        size_t current_mount_idx = 0;

        // Navigation and scroll repeat states
        std::string target_select_name = "";
        std::chrono::steady_clock::time_point last_scroll_time;
        bool is_repeating = false;

        // Per-frame text caches (header stats, footer counter, error message,
        // footer button glyphs/labels)
        CachedText net1_cache, net2_cache, sd_cache, sys_cache, pos_cache, error_cache;
        std::unordered_map<std::string, pu::sdl2::Texture> button_text_cache;

        // UI Pre-rendered textures
        DetailTextures detail_texs;
        pu::sdl2::Texture loading_tex = nullptr;
        pu::sdl2::Texture empty_tex = nullptr;
        pu::sdl2::Texture options_title_tex = nullptr;
        pu::sdl2::Texture confirm_title_tex = nullptr;
        pu::sdl2::Texture mount_val_tex_selected = nullptr;
        pu::sdl2::Texture mount_val_tex_unselected = nullptr;
        std::vector<pu::sdl2::Texture> options_texs_selected;
        std::vector<pu::sdl2::Texture> options_texs_unselected;
        std::vector<pu::sdl2::Texture> confirm_texs_selected;
        std::vector<pu::sdl2::Texture> confirm_texs_unselected;
        std::vector<pu::sdl2::Texture> properties_texs;

        // Write safety functions
        bool DirectoryExists(const std::string& path);
        std::string NormalizePath(const std::string& path);
        bool IsWritablePath(const std::string& path);
        bool IsValidName(const std::string& name);

        // Core layout helpers
        void BuildLocationsList();
        void LoadDirectoryAsync(const std::string& path);
        void CreateItemTextures(FileEntry& item);
        void RebuildFileTextures();
        void RebuildDetailTextures();
        void RebuildOptionsTextures();
        void RebuildConfirmTextures();
        void ClearTextures();
        void RefreshStats();
        // (Re)renders the language-dependent textures created at construction.
        void BuildStaticTextures();

        // Dynamic drawing icons and UI helpers
        void DrawFolderIcon(pu::ui::render::Renderer::Ref &drawer, s32 x, s32 y, pu::ui::Color color);
        void DrawFileIcon(pu::ui::render::Renderer::Ref &drawer, s32 x, s32 y, pu::ui::Color color);
        void DrawSwitchButton(pu::ui::render::Renderer::Ref &drawer, const std::string& letter, const std::string& label, s32 &x_pos, s32 y_pos);
        void DrawRectangleBorder(pu::ui::render::Renderer::Ref &drawer, const pu::ui::Color clr, const s32 x, const s32 y, const s32 width, const s32 height, const s32 border_width);

    public:
        FileBrowserPane(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~FileBrowserPane() override;

        void OnSelectionUpdated();
        void RefreshTranslations();
        void ForceRefresh();
        void CancelPendingScan();

        std::string GetCurrentPath() const { return current_path; }
        FileBrowserFocus GetActiveFocus() const { return active_focus; }

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        void HandleInput(const u64 keys_down, const u64 keys_held);

        PU_SMART_CTOR(FileBrowserPane)
    };

    class FileBrowserLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;
        std::shared_ptr<FileBrowserPane> pane;
        pu::ui::elm::TextBlock::Ref header_text;
        pu::ui::elm::TextBlock::Ref hint_text;
        pu::ui::elm::TextBlock::Ref path_text;

    public:
        FileBrowserLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);

        void OnSelectionUpdated();
        void RefreshTranslations();
        void ForceRefresh();
        void CancelPendingScan();
        void HandleInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos);

        PU_SMART_CTOR(FileBrowserLayout)
    };

}
