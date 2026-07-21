#pragma once

#include <pu/Plutonium>
#include "../navigation/NavigationManager.hpp"
#include <memory>
#include <vector>

namespace romm::ui {

    class AlphabetBar : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::vector<pu::sdl2::Texture> selected_texs;
        std::vector<pu::sdl2::Texture> unselected_texs;
        std::vector<std::string> letters;

        void InitTextures();
        void ClearTextures();

    public:
        AlphabetBar(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~AlphabetBar() override;

        // Single source of truth for the letter count ("ALL" + A-Z) — the
        // constructor's populate loop and NavigationManager's Right-
        // navigation clamp both size against this instead of independently
        // hardcoding the same number.
        static constexpr size_t GetLetterCount() { return 27; }

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(AlphabetBar)
    };

}
