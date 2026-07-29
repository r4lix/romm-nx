#pragma once

#include <pu/Plutonium>
#include "../navigation/NavigationManager.hpp"
#include <memory>
#include <vector>

namespace romm::ui {

    class SidebarList : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::vector<pu::sdl2::Texture> selected_texs;
        std::vector<pu::sdl2::Texture> unselected_texs;
        
        s32 item_height;
        s32 padding_top;
        s32 padding_left;

        romm::model::ApiState cached_state = romm::model::ApiState::Idle;
        size_t cached_platform_count = 999999;
        // Catches visibility changes that leave the count untouched (e.g. Reset
        // Defaults hiding one platform and showing another).
        uint64_t cached_platforms_generation = 0;
        // Status cards ("Loading platforms...", "Failed to connect", ...) are
        // pre-rendered, so a language switch has to invalidate them too.
        uint64_t cached_i18n_generation = 0;
        int sidebar_scroll_offset = 0;
        int visible_platform_count = 13;

        void InitTextures();
        void ClearTextures();

    public:
        SidebarList(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~SidebarList() override;

        void Refresh();
        void RefreshTranslations();

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(SidebarList)
    };

}
