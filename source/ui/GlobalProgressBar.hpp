#pragma once

#include <pu/Plutonium>
#include <memory>
#include <string>
#include <memory>
#include "../navigation/NavigationManager.hpp"

namespace romm::ui {

    class GlobalProgressBar : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;
        pu::sdl2::Texture text_tex = nullptr;
        pu::sdl2::Texture speed_tex = nullptr;
        pu::sdl2::Texture cover_tex = nullptr;
        std::string current_text;
        std::string current_speed;
        int current_rom_id = -1;
        
        void UpdateTextures(const std::string& text, const std::string& speed);

    public:
        GlobalProgressBar(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~GlobalProgressBar() override;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(GlobalProgressBar)
    };

}
