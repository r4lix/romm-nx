#pragma once

#include <pu/Plutonium>
#include <memory>
#include <functional>
#include <string>

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    // ConfigManager stores these as stable English tokens because they are
    // written to config.json — never translate them at the source. These two
    // map a stored token to its display word for the current language, and are
    // the only place that mapping happens (Settings, and the library Y-Menu).
    std::string TranslateViewMode(const std::string& stored_mode);
    std::string TranslateCoversQuality(const std::string& stored_quality);

    // The settings tabs, by name.
    //
    // These used to be bare integers compared inline in five different places
    // (the option builder, the custom-draw branches, the action dispatcher, the
    // row-count table, and NavigationManager's input handling). Inserting or
    // removing a tab shifted every index after it, and nothing complained —
    // rows simply became unreachable, which is how General's last two rows and
    // the whole Theme tab were silently broken once already.
    //
    // Everything now switches on this enum with no `default:` label, so -Wswitch
    // turns "you forgot a tab somewhere" into a compile-time error instead of a
    // bug someone finds months later.
    enum class SettingsCategory {
        General,
        Theme,
        Connection,
        Platforms,
        // Nintendo Switch Online: the setup checklist for the injection
        // feature, plus restoring a backup and the manual install screen.
        // Sits next to Platforms because that is where injection is turned on
        // per platform.
        //
        // The enum name is the feature; the tab is labelled "Nintendo Classic"
        // in both languages (measured 283px in Orbitron@30 against 340px of
        // capsule — the literal "Nintendo Switch Online" is 390px and overflows,
        // and a translated "Classiques Nintendo" is 344px, so the French keeps
        // the English label as a proper noun).
        SwitchOnline,
        Advanced,
        Updates,
        Debug,
        Count
    };

    // Display order of the tabs; index <-> category conversion goes through
    // these two so the order lives in exactly one place.
    SettingsCategory CategoryAt(size_t index);
    size_t CategoryIndex(SettingsCategory category);

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
        RestoreBackup,
        DownloadSoundPack,
        // Restores the SNES Switch Online LayeredFS from romm-nx's own backup —
        // unrelated to RestoreBackup above, which rolls the NRO back.
        RestoreNsoBackup,
        // Removes every game romm-nx injected, in one pass.
        RemoveAllNsoGames
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
        int volume_percent = -1; // >= 0 draws a HorizonOS-style slider bar instead of `value` text
        // Overrides the default value colour. Used by the Switch Online
        // checklist, where present-vs-missing is the whole point of the row.
        bool tint_value = false;
        pu::ui::Color value_tint = pu::ui::Color(0, 0, 0, 255);
    };

    // One row of the platform list: a canonical id plus the name to show.
    struct PlatformVisibilityRow {
        std::string canonical_id;
        std::string display_name;
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

        // Connection test state. Set from the constructor rather than inline so
        // it is a translated string, not a hardcoded English one.
        std::string connection_test_status;
        pu::ui::Color connection_status_color;

        // ROM Paths status cache
        std::vector<PlatformPathStatus> cached_statuses;
        // ROM Paths platform tab bar: index of the first visible tab. Tabs
        // stay a fixed, legible width — this scrolls the visible window
        // instead of shrinking tabs to force-fit an arbitrary platform count
        // (which just overlaps again once the count grows further).
        size_t rom_path_tab_scroll_offset = 0;
        // Settings > Platforms row list: index of the first visible row. The
        // list is open-ended (catalogue + server-detected), so it scrolls
        // rather than assuming it fits the panel.
        size_t platform_scroll_offset = 0;

    public:
        SettingsCard(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~SettingsCard() override = default;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void RefreshConfigTextures();
        // Back to "Not tested" in the current language — used on construction
        // and after a language switch, since the stored result would otherwise
        // stay in the previous one.
        void ResetConnectionStatus();
        void TriggerConnectionTest();
        void TriggerRecalculateCache();
        void RecalculateCacheSize();
        void RefreshPathStatuses();

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        PU_SMART_CTOR(SettingsCard)
    };

    class NsoSnesModal;

    class SettingsLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<SettingsCard> card;
        std::shared_ptr<SettingsConfirmModal> confirm_modal;
        // Settings > Nintendo Classic > injection screen (experimental).
        std::shared_ptr<NsoSnesModal> nso_snes_modal;

        pu::ui::elm::TextBlock::Ref settings_title_text;
        pu::ui::elm::TextBlock::Ref hint_text;

        // Which tab OnSelectionUpdated last saw. The Switch Online tab re-runs
        // detection when it is opened — a directory scan plus a JSON parse,
        // cheap once but not something to repeat on every cursor move.
        SettingsCategory last_seen_category = SettingsCategory::Count;

    public:
        SettingsLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);

        void OnSelectionUpdated();
        void RefreshTranslations();
        void RefreshConfig();
        void UpdateFooterHints(SettingsFocusArea focus);

        bool IsConfirmModalActive() const { return confirm_modal && confirm_modal->IsActive(); }
        void HandleConfirmModalInput(u64 keys_down) { if (confirm_modal) confirm_modal->HandleInput(keys_down); }

        // Same input-forwarding contract as the confirm modal above: while this
        // is up, NavigationManager sends every button here and nothing reaches
        // the settings list underneath.
        bool IsNsoSnesModalActive() const;
        void HandleNsoSnesModalInput(u64 keys_down);
        void HandleOptionAction(size_t cat_idx, size_t opt_idx);
        void CycleStartupSound(int direction);
        void CycleThemeSound(int direction);
        void CycleStartupVolume(int direction);
        void CycleAmbientVolume(int direction);

        // --- Settings > Platforms -----------------------------------------
        // Rebuilds the row list from the catalogue + the live model, then
        // clamps the cursor if the list shrank.
        void RefreshPlatformRows();
        // Left/Right in the platform list: sets a platform's visibility
        // outright, or cycles the sort order on the row that owns it.
        void AdjustSelectedPlatformRow(int direction);
        // Name -> Brand -> Custom. Switching to Custom seeds the stored order
        // from what is currently on screen, if the user has none yet.
        void CyclePlatformSortMode(int direction);
        // L/R on a platform row, Custom sort only: moves it one slot up (-1) or
        // down (+1) and persists the new order.
        void MoveSelectedPlatform(int direction);
        void SetPlatformVisibility(size_t platform_row_idx, bool visible);
        void ApplyPlatformVisibility();

        // The platform whose rows are on screen, or nullptr when the cursor is
        // on an action row or the list shrank under it.
        const PlatformVisibilityRow* SelectedPlatformRow() const;

        static size_t GetOptionsCount(size_t cat_idx);
        static size_t GetCategoriesCount();
        static size_t GetSelectableRomPathRowCount();
        // Rows above the platform list (Show All, Reset Defaults), which act in
        // place rather than opening a platform.
        static size_t GetPlatformActionRowCount();

        // A on a row of the per-platform screen.
        void ActivateSelectedPlatformRow();
        void EditSelectedRomPath();
        void ValidateOrCreateSelectedPath();
        void ResetSelectedRomPath();

        PU_SMART_CTOR(SettingsLayout)
    };

}
