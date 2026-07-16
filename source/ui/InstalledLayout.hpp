#pragma once

#include <pu/Plutonium>
#include <memory>
#include <vector>
#include <string>
#include <map>
#include "CoverCache.hpp"
#include "CoverProfile.hpp"
#include "../model/DataModel.hpp"
#include "../model/DownloadManager.hpp"

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    // -----------------------------------------------------------------------
    // InstalledTabBar — horizontal platform switcher
    // -----------------------------------------------------------------------
    class InstalledTabBar : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        bool focused = true;

        struct Tab {
            std::string slug;          // normalised slug
            std::string display_name;
            int game_count = 0;
            pu::sdl2::Texture label_tex_active   = nullptr;
            pu::sdl2::Texture label_tex_inactive = nullptr;
        };

        std::vector<Tab> tabs;
        size_t active_tab = 0;

    public:
        InstalledTabBar(s32 x, s32 y, s32 w, s32 h);
        ~InstalledTabBar() override;

        void SetTabs(const std::vector<std::pair<std::string,int>>& slug_counts); // slug → count
        size_t GetActiveTab() const { return active_tab; }
        std::string GetActiveSlug() const;
        size_t TabCount() const { return tabs.size(); }

        void SelectPrev();
        void SelectNext();
        void SetActiveTab(size_t idx) { if (idx < tabs.size()) active_tab = idx; }
        void SetFocused(bool f) { focused = f; }

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth()  override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref& drawer,
                      const s32 x_coord, const s32 y_coord) override;
        void OnInput(const u64, const u64, const u64, const pu::ui::TouchPoint) override {}

        PU_SMART_CTOR(InstalledTabBar)
    };

    // -----------------------------------------------------------------------
    // InstalledListPanel — vertical game list and right info panel
    // -----------------------------------------------------------------------
    class InstalledListPanel : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::vector<romm::model::InstalledIndexEntry> games;
        std::string platform_slug; // normalised

        size_t selected_idx = 0;
        int scroll_offset = 0;
        int64_t failed_logged_rom_id = 0;
        uint64_t current_generation_id = 0;
        bool cover_resolved = false;
        std::string resolved_cover_source;
        bool resolved_is_local = false;
        bool resolved_is_url = false;

        pu::sdl2::Texture sd_card_icon = nullptr;
        pu::sdl2::Texture cover_placeholder_tex = nullptr;

        // Static label texture caches
        pu::sdl2::Texture label_platform_tex = nullptr;
        pu::sdl2::Texture label_size_tex = nullptr;
        pu::sdl2::Texture label_location_tex = nullptr;
        pu::sdl2::Texture label_file_tex = nullptr;
        pu::sdl2::Texture label_status_tex = nullptr;

        // Informational field texture caches (values)
        pu::sdl2::Texture info_title_tex = nullptr;
        pu::sdl2::Texture info_platform_tex = nullptr;
        pu::sdl2::Texture info_size_tex = nullptr;
        pu::sdl2::Texture info_location_tex = nullptr;
        pu::sdl2::Texture info_file_tex = nullptr;
        pu::sdl2::Texture info_status_tex = nullptr;
        size_t info_cached_idx = SIZE_MAX;

        void ClearInfoTextures();
        void RebuildInfoStrip();

    public:
        InstalledListPanel(s32 x, s32 y, s32 w, s32 h,
                           std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~InstalledListPanel() override;

        void SetGames(const std::vector<romm::model::InstalledIndexEntry>& entries,
                      const std::string& norm_slug);
        void ResetSelection();
        void SetSelectedIdx(size_t idx);

        size_t GetSelectedIdx() const { return selected_idx; }
        const romm::model::InstalledIndexEntry* GetSelected() const;
        void SetFocused(bool) {}

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth()  override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref& drawer,
                      const s32 x_coord, const s32 y_coord) override;
        void OnInput(const u64, const u64, const u64, const pu::ui::TouchPoint) override {}

        // Returns true when A was pressed on a valid entry
        bool HandleInput(const u64 keys_down);

        PU_SMART_CTOR(InstalledListPanel)
    };

    // -----------------------------------------------------------------------
    // InstalledLayout
    // -----------------------------------------------------------------------
    class InstalledLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<InstalledTabBar>   tab_bar;
        std::shared_ptr<InstalledListPanel> list_panel;
        pu::ui::elm::TextBlock::Ref hint_text;

        // Full grouped data; re-built on ForceRefresh
        std::map<std::string, std::vector<romm::model::InstalledIndexEntry>> grouped;
        std::vector<std::pair<std::string,int>> tab_slugs; // ordered

        void RebuildData();
        void SwitchToTab(size_t idx);

    public:
        static std::string DisplayName(const std::string& slug);
        static CoverProfile ProfileForSlug(const std::string& slug);
        static int PlatformOrder(const std::string& slug);

        InstalledLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);

        void OnSelectionUpdated();
        void ForceRefresh();
        void UpdateDownloadStatus() {}
        void HandleInput(const u64 keys_down, const u64 keys_up,
                         const u64 keys_held, const pu::ui::TouchPoint touch_pos);

        PU_SMART_CTOR(InstalledLayout)
    };

} // namespace romm::ui
