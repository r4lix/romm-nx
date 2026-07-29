#pragma once

#include <pu/Plutonium>
#include <memory>
#include "CoverProfile.hpp"
#include "CoverCache.hpp"

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    enum class DetailCoverState {
        None,
        UsingSmallFallback,
        LoadingLarge,
        UsingLarge,
        UsingMiximage,
        FailedLargeKeepingSmall,
        Placeholder,
        Failed
    };

    enum class DetailDisplayState {
        Placeholder,
        LoadingPlaceholder,
        CoverFallback,
        MiximageReady
    };

    enum class DetailRequestState {
        Idle,
        RequestingCover,
        RequestingMiximage,
        FailedTransient,
        FailedPermanent
    };

    struct FullscreenKeys {
        CoverCacheKey miximage_key;
        CoverCacheKey large_key;
        CoverCacheKey small_key;
    };

    enum class DownloadActionState {
        Download,
        AddToQueue,
        Queued,
        Downloading,
        Failed,
        Uninstall
    };

    // Derives which action the A button should perform for a ROM, from install
    // state plus the download queue. Free function rather than a DetailCard
    // method because the Detail *view mode* panel in the library needs the same
    // answer, and two copies of this ladder would drift the moment one of the
    // DownloadState cases changed.
    //
    // Requires the ROM's GameDetail (for the multi-disc install identity);
    // returns Download when detail is null, which is the safe default for a
    // panel still waiting on its debounced fetch.
    DownloadActionState ComputeDownloadActionState(int rom_id,
                                                   const std::string& platform_slug,
                                                   const romm::model::GameDetail* detail);

    struct DetailGameContext {
        int rom_id = -1;
        std::string platform_name;
        std::string platform_slug;
        std::string title;
        std::string cover_path;
        std::string cover_path_large;
        bool is_ps1 = false;
        size_t platform_idx = 0;

        uint64_t generation = 0;
        std::string cover_variant;
    };

    struct DetailCoverExpectation {
        CoverCacheKey cache_key;
        uint64_t generation = 0;
    };

    class DetailCard : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        pu::sdl2::Texture details_tex = nullptr;
        pu::sdl2::Texture save_data_tex = nullptr;
        pu::sdl2::Texture mods_tex = nullptr;
        pu::sdl2::Texture cheats_tex = nullptr;
        pu::sdl2::Texture cover_placeholder_tex = nullptr;
        pu::sdl2::Texture loading_tex = nullptr;

        pu::sdl2::Texture tex_btn_download = nullptr;
        pu::sdl2::Texture tex_btn_preparing = nullptr;
        pu::sdl2::Texture tex_btn_downloaded = nullptr;
        pu::sdl2::Texture tex_btn_failed = nullptr;
        pu::sdl2::Texture tex_btn_unsupported = nullptr;
        
        pu::sdl2::Texture tex_btn_uninstall = nullptr;
        pu::sdl2::Texture tex_btn_confirm_uninstall = nullptr;
        pu::sdl2::Texture tex_btn_add_to_queue = nullptr;
        pu::sdl2::Texture tex_btn_remove_from_queue = nullptr;
        
        pu::sdl2::Texture dynamic_download_tex = nullptr;
        std::string current_dynamic_text;
        bool file_exists_checked = false;
        bool final_file_exists = false;
        bool part_file_exists = false;
        int checked_rom_id = -1;
        DownloadActionState current_action_state = DownloadActionState::Download;

        size_t cached_plat_idx = 999999;
        CoverProfile currentCoverProfile;

        DetailCoverState cover_state = DetailCoverState::None;
        DetailDisplayState display_state = DetailDisplayState::Placeholder;
        DetailRequestState request_state = DetailRequestState::Idle;
        pu::sdl2::Texture cover_tex = nullptr;
        int current_cover_rom_id = -1;

        s32 actual_cover_x = 0;
        s32 actual_cover_y = 0;
        s32 actual_cover_w = 0;
        s32 actual_cover_h = 0;

        int descriptionScrollOffset = 0;
        int maxDescriptionScrollOffset = 0;
        std::vector<std::string> descriptionLines;

        // Marquee scrolling members
        enum class TitleScrollState { WaitStart, Scrolling, WaitEnd };
        TitleScrollState title_scroll_state = TitleScrollState::WaitStart;
        float title_scroll_x = 0.0f;
        float title_wait_timer = 0.0f;
        std::chrono::steady_clock::time_point last_title_scroll_update_time;
        std::string cached_title;
        pu::sdl2::Texture title_tex = nullptr;

        // Stale cover validation members
        bool is_active = false;
        DetailCoverExpectation expected_identity;
        CoverCacheKey displayed_key;
        uint64_t logged_complete_gen_val = 0;
        uint64_t logged_apply_gen_val = 0;
        CoverState last_known_cache_state = CoverState::Unknown;

        void InitTextures();
        void ClearTextures();
        void GetVariantDimensions(const std::string& variant, int& w, int& h);

    public:
        void ResolveDetailImageState(bool allow_download);

    public:
        DetailGameContext ctx;
        void SetContext(const DetailGameContext& context);
        void OnLeave();

        // Re-rasterises the button/tab/placeholder labels after a language
        // change — they're all pre-rendered at construction.
        void RefreshTranslations();

        // Single source of truth for the tab count (Details/Save Data/Mods/
        // Cheats) — OnRender's tab strip and NavigationManager's Right-
        // navigation clamp both size against this instead of independently
        // hardcoding the same number.
        static constexpr size_t GetTabCount() { return 4; }

        DetailCard(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~DetailCard() override;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        bool FinalFileExists() const { return final_file_exists; }
        bool PartFileExists() const { return part_file_exists; }

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override;

        DownloadActionState GetActionState() const { return current_action_state; }
        void ForceRefresh() { checked_rom_id = -1; }

        DetailCoverExpectation& GetExpectedIdentity() { return expected_identity; }
        pu::sdl2::Texture GetCoverTexture() { return cover_tex; }
        DetailCoverState GetCoverState() { return cover_state; }
        FullscreenKeys GetFullscreenKeys();

        PU_SMART_CTOR(DetailCard)
    };

    class DetailLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<DetailCard> card;
        pu::ui::elm::TextBlock::Ref game_title_text;
        pu::ui::elm::TextBlock::Ref platform_text;
        pu::ui::elm::TextBlock::Ref meta_text;
        pu::ui::elm::TextBlock::Ref desc_title_text;
        pu::ui::elm::TextBlock::Ref desc_text;
        pu::ui::elm::TextBlock::Ref trailer_title_text;
        pu::ui::elm::TextBlock::Ref download_status_text;
        pu::ui::elm::TextBlock::Ref hint_text;

        int descriptionScrollOffset = 0;
        int maxDescriptionScrollOffset = 0;
        std::vector<std::string> descriptionLines;

    public:
        DetailGameContext ctx;
        void SetContext(const DetailGameContext& context);
        void OnLeave();

        DetailLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);

        void OnSelectionUpdated();
        void RefreshTranslations();
        void ScrollDescription(int direction);
        void UpdateDownloadStatus();
        void UpdateFooterHints();
        std::shared_ptr<DetailCard> GetCard() { return card; }

        PU_SMART_CTOR(DetailLayout)
    };

    enum class FullscreenMode {
        MixImage,
        LargeCover
    };

    class FullscreenImageElement : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        FullscreenKeys keys;
        FullscreenMode current_mode;

        // Cached so the fallback message isn't re-rasterised every frame; only
        // re-rendered when the text actually changes.
        pu::sdl2::Texture status_tex = nullptr;
        std::string status_str;
        void UpdateStatusText(const std::string& s);
    public:
        FullscreenImageElement(s32 x, s32 y, s32 w, s32 h);
        ~FullscreenImageElement();
        void SetKeys(const FullscreenKeys& new_keys);
        void CycleMode(int direction);
        FullscreenMode GetMode() { return current_mode; }
        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }
        void OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}
        PU_SMART_CTOR(FullscreenImageElement)
    };

    class FullscreenImageLayout : public pu::ui::Layout {
    private:
        std::shared_ptr<FullscreenImageElement> img_el;
    public:
        FullscreenImageLayout();
        std::shared_ptr<FullscreenImageElement> GetImageElement() { return img_el; }
        void SetKeys(const FullscreenKeys& new_keys) { img_el->SetKeys(new_keys); }
        PU_SMART_CTOR(FullscreenImageLayout)
    };

}
