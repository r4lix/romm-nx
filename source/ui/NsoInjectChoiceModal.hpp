#pragma once

#include <pu/Plutonium>
#include "../navigation/NavigationManager.hpp"

namespace romm::ui {

    // Settings > Platforms > Switch Online = "Ask each time", asked at download
    // time. Same shape as UninstallConfirmModal: a render-only overlay that
    // reads its whole state from NavigationManager, which also owns the input
    // handling — a layout that can raise a modal but not draw it would soft-lock
    // the screen, so this is added to every layout that can raise it.
    class NsoInjectChoiceModal : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;
    public:
        NsoInjectChoiceModal(std::shared_ptr<romm::navigation::NavigationManager> nav)
            : Element::Element(), x(0), y(0), w(1920), h(1080), nav_mgr(nav) {}
        ~NsoInjectChoiceModal() {}

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(NsoInjectChoiceModal)
    };

}
