#pragma once

#include <pu/Plutonium>
#include "../navigation/NavigationManager.hpp"

namespace romm::ui {

    // Shown once per session on the Main Menu when a background update check
    // (Settings > Updates > Check on startup) lands on UpdateAvailable and the
    // user hasn't already dismissed that exact version. Mirrors
    // UninstallConfirmModal's pattern: a stateless render-only overlay reading
    // its visibility/content from NavigationManager, with input handled
    // centrally in NavigationManager::HandleUpdateModalInput.
    class UpdateAvailableModal : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;
    public:
        UpdateAvailableModal(std::shared_ptr<romm::navigation::NavigationManager> nav)
            : Element::Element(), x(0), y(0), w(1920), h(1080), nav_mgr(nav) {}
        ~UpdateAvailableModal() {}

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(UpdateAvailableModal)
    };

}
