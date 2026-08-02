#pragma once

#include <pu/Plutonium>

#include <memory>
#include <string>
#include <vector>

#include "../model/DataModel.hpp"
#include "../model/RommApi.hpp"

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    // Settings > Advanced > SNES Online Injection Test (experimental).
    //
    // A self-contained full-screen overlay rather than a new Screen: the whole
    // feature is a lab bench for one pipeline, and threading it through
    // NavigationManager's screen/focus state machine would spread experimental
    // code across the app's core navigation for no benefit. It borrows the same
    // input-forwarding hook SettingsConfirmModal already uses.
    class NsoSnesModal : public pu::ui::elm::Element {
    public:
        enum class Page {
            Overview,
            Picker,
            Progress
        };

        explicit NsoSnesModal(std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~NsoSnesModal() override = default;

        s32 GetX() override { return 0; }
        s32 GetY() override { return 0; }
        s32 GetWidth() override { return 1920; }
        s32 GetHeight() override { return 1080; }

        void Show();
        // Opens straight onto the progress page with a restore already running.
        // Settings > Switch Online > "Restore last SNES Online backup" uses
        // this so the restore reports its steps instead of running headless.
        void ShowRestore();
        // Same idea for the bulk removal: it runs on the pipeline worker and
        // the progress page is where it reports.
        void ShowUninstallAll();
        void Hide();
        bool IsActive() const { return active; }

        void HandleInput(u64 keys_down);

        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(NsoSnesModal)

    private:
        void RefreshDetection();
        void LoadSnesLibrary();
        void PollLibraryFetch();
        void StartInstallForSelection();

        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        bool active = false;
        Page page = Page::Overview;

        size_t overview_row = 0;
        size_t picker_row = 0;
        size_t picker_scroll = 0;

        // Local copy of the SNES library. Deliberately not written back into
        // DataModel: opening this screen must not disturb whatever platform the
        // library browser has loaded.
        std::vector<romm::model::Game> snes_games;
        std::string library_status;
        std::shared_ptr<romm::model::RomFetchResult> pending_fetch;
        int fetch_request_id = 0;
    };

}
