#pragma once

#include <pu/Plutonium>
#include "../navigation/NavigationManager.hpp"

namespace romm::ui {

    class LibraryMenuModal : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

    public:
        LibraryMenuModal(std::shared_ptr<romm::navigation::NavigationManager> nav)
            : Element::Element(), x(0), y(0), w(1920), h(1080), nav_mgr(nav) {}
        ~LibraryMenuModal() {}

        // Single source of truth for the row count (Search/Sort/View Mode) —
        // OnRender's `Row rows[N]` and NavigationManager's Down-navigation
        // clamp both size against this instead of independently hardcoding
        // the same number (the exact bug class that left Settings > Theme
        // unable to scroll: two hardcoded copies of the same fact, one of
        // them forgotten when a row was added).
        static constexpr size_t GetRowCount() { return 3; }

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(LibraryMenuModal)
    };

}
