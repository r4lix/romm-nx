#pragma once

#include <pu/Plutonium>
#include <memory>
#include <vector>
#include <string>
#include <chrono>
#include "../model/DownloadManager.hpp"
#include "../model/DataModel.hpp"

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {

    class QueueList : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        struct QueueItem {
            romm::model::DownloadTask task;
            std::string game_title;
            std::string status_str;
            pu::sdl2::Texture text_tex_selected = nullptr;
            pu::sdl2::Texture text_tex_unselected = nullptr;
            pu::sdl2::Texture status_tex_selected = nullptr;
            pu::sdl2::Texture status_tex_unselected = nullptr;
        };

        std::vector<QueueItem> items;
        size_t selected_idx = 0;
        int scroll_offset = 0;
        bool list_built = false;
        std::chrono::steady_clock::time_point last_refresh;

        pu::sdl2::Texture empty_tex = nullptr;

        void ClearTextures();
        void RenderStatusTextures(QueueItem& item);

    public:
        QueueList(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~QueueList() override;

        // Full rebuild: tears down and re-renders every row's textures.
        void BuildList();
        // Throttled incremental refresh: full rebuild only when queue
        // membership/order changed; otherwise re-renders just the status
        // line of rows whose state or progress text actually changed.
        void RefreshList();
        // Footer hint matching the selected row's available action.
        std::string GetContextHint() const;
        // Re-renders the empty-state text and every row's status line.
        void RefreshTranslations();

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnSelectionUpdated();

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) override {}

        void HandleInput(const u64 keys_down, const u64 keys_held);

        PU_SMART_CTOR(QueueList)
    };

    class QueueLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;
        std::shared_ptr<QueueList> list;
        pu::ui::elm::TextBlock::Ref header_text;
        pu::ui::elm::TextBlock::Ref hint_text;
        std::string last_hint;

        void UpdateHint();

    public:
        QueueLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);

        void OnSelectionUpdated();
        void RefreshTranslations();
        void ForceRefresh();
        void HandleInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos);

        PU_SMART_CTOR(QueueLayout)
    };

}
