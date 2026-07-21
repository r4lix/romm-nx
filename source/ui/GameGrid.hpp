#pragma once

#include <pu/Plutonium>
#include "../navigation/NavigationManager.hpp"
#include "../model/ConfigManager.hpp"
#include <memory>
#include <vector>
#include "CoverProfile.hpp"

namespace romm::ui {

    class GameGrid : public pu::ui::elm::Element {
    public:
        // -----------------------------------------------------------------------
        // Grid layout constants
        // -----------------------------------------------------------------------
        static constexpr int INFO_STRIP_H = 100;

    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        // Current platform cover profile
        CoverProfile current_profile;

        // Status message (loading / error / idle)
        pu::sdl2::Texture status_tex = nullptr;

        // Bottom info strip textures — refreshed only when selected game changes
        pu::sdl2::Texture info_title_tex = nullptr;
        pu::sdl2::Texture info_sub_tex   = nullptr;
        size_t info_cached_game_idx      = 999999;
        size_t info_cached_total         = 999999;

        // Virtualized game list & scroll state
        std::vector<romm::model::Game> filtered_games;
        size_t current_visible_start_idx  = 999999;
        size_t current_visible_count      = 0;
        int    scroll_row_offset          = 0;
        int    last_scroll_row_offset     = 0;
        int    last_scroll_direction      = 0; // 1 = down, -1 = up, 0 = stationary
        size_t cached_selected_platform_idx = 999999;
        size_t cached_selected_letter_idx   = 999999;
        romm::model::GridViewMode cached_view_mode = romm::model::GridViewMode::Default;

        // Marquee scrolling members
        enum class InfoScrollState { WaitStart, Scrolling, WaitEnd };
        InfoScrollState info_scroll_state = InfoScrollState::WaitStart;
        float info_scroll_x = 0.0f;
        float info_wait_timer = 0.0f;
        std::chrono::steady_clock::time_point last_info_scroll_update_time;
        size_t cached_info_game_idx = 999999;

        void ClearStatusTex();
        void ClearInfoTextures();
        void RebuildInfoStrip(const romm::model::Game& game, const std::string& platform_name, size_t game_idx, size_t total);
        void AdjustProfileForHeight(CoverProfile& profile);

    public:
        GameGrid(s32 x, s32 y, s32 w, s32 h,
                 std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~GameGrid() override;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override  { return w; }
        s32 GetHeight() override { return h; }

        void SetX(s32 new_x) { x = new_x; }
        void SetY(s32 new_y) { y = new_y; }
        void SetWidth(s32 new_w)  { w = new_w; }
        void SetHeight(s32 new_h) { h = new_h; }

        // Called by LibraryLayout whenever selection/state changes
        void OnSelectionUpdated();

        // Authoritative column count for the currently-rendered grid — the
        // single source NavigationManager's row/col navigation math reads,
        // instead of independently re-deriving it from CoverProfile.
        int GetColumns() const { return current_profile.columns; }

        void OnRender(pu::ui::render::Renderer::Ref& drawer,
                      const s32 x, const s32 y) override;
        void OnInput(const u64, const u64, const u64, const pu::ui::TouchPoint) override {}

        PU_SMART_CTOR(GameGrid)
    };

} // namespace romm::ui
