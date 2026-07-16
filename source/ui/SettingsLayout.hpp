#pragma once

#include <pu/Plutonium>
#include <memory>
#include <functional>
#include <string>

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    enum class SettingsFocusArea {
        CategoryList,
        OptionList
    };

    enum class ConfirmAction {
        None,
        ClearCoverCache,
        ClearAllCache,
        ResetRomPath,
        RebuildIndex,
        InstallUpdate,
        RestoreBackup
    };

    enum class PathStatus {
        Valid,
        Missing,
        Invalid,
        ReadOnly,
        ComingLater
    };

    struct OptionRenderEntry {
        std::string label;
        std::string value;
        bool is_action = false;
    };

    struct PlatformPathStatus {
        PathStatus roms_status = PathStatus::Missing;
        PathStatus cover_status = PathStatus::ReadOnly;
        PathStatus bios_status = PathStatus::ComingLater;
        PathStatus save_status = PathStatus::ComingLater;
    };

    struct PlatformPathDescriptor {
        std::string tab_label;
        std::string display_name;
        std::string internal_slug;
        std::string display_slug;
        bool download_supported;
        bool rom_path_editable;
        bool cover_path_editable;
        bool bios_path_supported;
        bool save_path_supported;
    };

    class SettingsConfirmModal : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        bool active = false;
        ConfirmAction action = ConfirmAction::None;
        std::string title;
        std::string message;
        std::function<void()> on_confirm;

    public:
        SettingsConfirmModal();
        ~SettingsConfirmModal() override = default;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void Show(const std::string& title_text, const std::string& msg_text, ConfirmAction act, std::function<void()> confirm_cb);
        void Hide();
        bool IsActive() const { return active; }

        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        void HandleInput(u64 keys_down);

        PU_SMART_CTOR(SettingsConfirmModal)
    };

    class SettingsCard : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        // Cache size calculation state
        bool calculating_cache = false;
        long long cache_cover_bytes = 0;
        long long cache_total_bytes = 0;
        int cache_cover_count = 0;

        // Connection test state
        std::string connection_test_status = "Not tested";
        pu::ui::Color connection_status_color;

        // ROM Paths status cache
        std::vector<PlatformPathStatus> cached_statuses;

    public:
        SettingsCard(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~SettingsCard() override = default;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void RefreshConfigTextures();
        void TriggerConnectionTest();
        void TriggerRecalculateCache();
        void RecalculateCacheSize();
        void RefreshPathStatuses();

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(SettingsCard)
    };

    class SettingsLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<SettingsCard> card;
        std::shared_ptr<SettingsConfirmModal> confirm_modal;
        pu::ui::elm::TextBlock::Ref settings_title_text;
        pu::ui::elm::TextBlock::Ref hint_text;

    public:
        SettingsLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);

        void OnSelectionUpdated();
        void RefreshConfig();
        void UpdateFooterHints(SettingsFocusArea focus);

        bool IsConfirmModalActive() const { return confirm_modal && confirm_modal->IsActive(); }
        void HandleConfirmModalInput(u64 keys_down) { if (confirm_modal) confirm_modal->HandleInput(keys_down); }
        void HandleOptionAction(size_t cat_idx, size_t opt_idx);
        static size_t GetOptionsCount(size_t cat_idx);
        static size_t GetSupportedPlatformsCount();

        void EditSelectedRomPath();
        void ValidateOrCreateSelectedPath();
        void ResetSelectedRomPath();

        PU_SMART_CTOR(SettingsLayout)
    };

}
