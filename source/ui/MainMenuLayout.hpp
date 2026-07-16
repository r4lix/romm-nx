#pragma once

#include <pu/Plutonium>
#include <memory>
#include <vector>

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    class MenuGrid : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::vector<pu::sdl2::Texture> selected_texs;
        std::vector<pu::sdl2::Texture> unselected_texs;
        std::vector<std::string> item_names;

        s32 card_w;
        s32 card_h;
        s32 col_spacing;
        s32 row_spacing;
        s32 offset_x;
        s32 offset_y;

        void InitTextures();
        void ClearTextures();

    public:
        MenuGrid(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~MenuGrid() override;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(MenuGrid)
    };

    class MainMenuLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<MenuGrid> grid;
        pu::ui::elm::TextBlock::Ref header_text;
        pu::ui::elm::TextBlock::Ref hint_text;

    public:
        MainMenuLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);

        void OnSelectionUpdated();

        PU_SMART_CTOR(MainMenuLayout)
    };

}
