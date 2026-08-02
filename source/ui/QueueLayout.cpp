#include "QueueLayout.hpp"
#include "../nso/NsoSnesInstaller.hpp"
#include "PlaceholderCover.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../model/DownloadManager.hpp"
#include "GlobalProgressBar.hpp"
#include "CoverCache.hpp"
#include "../i18n/I18n.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>

namespace romm::ui {

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    static bool IsActiveState(romm::model::DownloadState s) {
        return s == romm::model::DownloadState::Preparing ||
               s == romm::model::DownloadState::DownloadingGame ||
               s == romm::model::DownloadState::DownloadingCover ||
               s == romm::model::DownloadState::SyncingCover ||
               s == romm::model::DownloadState::Injecting;
    }

    static int StateSortRank(romm::model::DownloadState s) {
        if (IsActiveState(s)) return 0;
        switch (s) {
            case romm::model::DownloadState::Queued:    return 1;
            case romm::model::DownloadState::Failed:    return 2;
            case romm::model::DownloadState::Cancelled: return 3;
            case romm::model::DownloadState::Completed: return 4;
            default:                                    return 5;
        }
    }

    // Display order: active first, then queued, then failed/cancelled (they
    // want attention), completed last. Stable within each group.
    static std::vector<romm::model::DownloadTask> SortedQueueSnapshot() {
        auto snap = romm::model::DownloadManager::Instance().GetQueueSnapshot();
        std::stable_sort(snap.begin(), snap.end(), [](const auto& a, const auto& b) {
            return StateSortRank(a.state.load()) < StateSortRank(b.state.load());
        });
        return snap;
    }

    static std::string MakeStatusString(const romm::model::DownloadTask& t) {
        switch (t.state.load()) {
            case romm::model::DownloadState::Preparing:
                return romm::i18n::tr("queue.status.preparing");
            case romm::model::DownloadState::DownloadingGame: {
                float progress_pct = 0.0f;
                if (t.total_bytes > 0) {
                    progress_pct = (float)t.downloaded_bytes.load() / t.total_bytes;
                }
                int pct_int = (int)(progress_pct * 100);

                std::string speed_str = romm::i18n::tr("queue.status.speed_starting");
                size_t bps = t.download_speed_bps.load();
                if (bps > 0) {
                    // Transfer rate: a number plus an SI-style unit, identical
                    // in every language romm-nx ships.
                    std::stringstream ss;
                    double speed_mbps = (double)bps / (1024.0 * 1024.0);
                    if (speed_mbps >= 1.0) {
                        ss << std::fixed << std::setprecision(1) << speed_mbps << " MB/s";
                    } else {
                        ss << std::fixed << std::setprecision(1) << ((double)bps / 1024.0) << " KB/s";
                    }
                    speed_str = ss.str();
                }
                return romm::i18n::format("queue.status.downloading", {
                    {"percent", std::to_string(pct_int)},
                    {"speed", speed_str}
                });
            }
            case romm::model::DownloadState::DownloadingCover:
            case romm::model::DownloadState::SyncingCover:
                return romm::i18n::tr("queue.status.cover_sync");
            case romm::model::DownloadState::Injecting: {
                // Live step from the injection pipeline, so the queue shows
                // which of the 15 stages is running rather than a spinner.
                const auto steps = romm::nso::NsoSnesInstaller::Instance().GetSteps();
                for (size_t i = 0; i < steps.size(); ++i) {
                    if (steps[i].status != romm::nso::NsoStepStatus::Running) continue;
                    return romm::i18n::format("queue.status.injecting_step", {
                        {"step", steps[i].name},
                        {"index", std::to_string(i + 1)},
                        {"total", std::to_string(steps.size())}
                    });
                }
                return romm::i18n::tr("queue.status.injecting");
            }
            case romm::model::DownloadState::Queued:
                return romm::i18n::tr("queue.status.queued");
            case romm::model::DownloadState::Completed:
                return romm::i18n::tr("queue.status.completed");
            case romm::model::DownloadState::Failed:
                // error_message is already localized by DownloadManager, except
                // where it carries a curl/system string — see the note there.
                return t.error_message.empty()
                    ? romm::i18n::tr("queue.status.failed")
                    : romm::i18n::format("queue.status.failed_reason", {{"reason", t.error_message}});
            case romm::model::DownloadState::Cancelled:
                return romm::i18n::tr("queue.status.cancelled");
            default:
                return romm::i18n::tr("queue.status.unknown");
        }
    }

    // -----------------------------------------------------------------------
    // QueueList
    // -----------------------------------------------------------------------

    QueueList::QueueList(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav) {
        pu::ui::Color text_color(237, 229, 251, 255);
        empty_tex = pu::ui::render::RenderText("Ubuntu@30", romm::i18n::tr("queue.empty"), text_color);
    }

    void QueueList::RefreshTranslations() {
        pu::ui::Color text_color(237, 229, 251, 255);
        if (empty_tex) pu::ui::render::DeleteTexture(empty_tex);
        empty_tex = pu::ui::render::RenderText("Ubuntu@30", romm::i18n::tr("queue.empty"), text_color);
        // Row titles are game names, but the status lines under them are ours —
        // a full rebuild is the simplest way to get both consistent.
        BuildList();
    }

    QueueList::~QueueList() {
        ClearTextures();
        if (empty_tex) pu::ui::render::DeleteTexture(empty_tex);
    }

    void QueueList::ClearTextures() {
        for (auto& item : items) {
            if (item.text_tex_selected) pu::ui::render::DeleteTexture(item.text_tex_selected);
            if (item.text_tex_unselected) pu::ui::render::DeleteTexture(item.text_tex_unselected);
            if (item.status_tex_selected) pu::ui::render::DeleteTexture(item.status_tex_selected);
            if (item.status_tex_unselected) pu::ui::render::DeleteTexture(item.status_tex_unselected);
        }
        items.clear();
    }

    void QueueList::RenderStatusTextures(QueueItem& item) {
        if (item.status_tex_selected)   { pu::ui::render::DeleteTexture(item.status_tex_selected);   item.status_tex_selected = nullptr; }
        if (item.status_tex_unselected) { pu::ui::render::DeleteTexture(item.status_tex_unselected); item.status_tex_unselected = nullptr; }

        pu::ui::Color detail_selected_clr(210, 200, 235, 255);
        pu::ui::Color detail_unselected_clr(140, 130, 170, 255);
        item.status_tex_selected = pu::ui::render::RenderText("Ubuntu@20", item.status_str, detail_selected_clr, w - 120);
        item.status_tex_unselected = pu::ui::render::RenderText("Ubuntu@20", item.status_str, detail_unselected_clr, w - 120);
    }

    void QueueList::BuildList() {
        ClearTextures();
        auto nav = nav_mgr.lock();
        if (!nav) return;
        auto model = nav->GetModel();
        if (!model) return;

        auto snap = SortedQueueSnapshot();

        pu::ui::Color selected_clr(237, 229, 251, 255);
        pu::ui::Color unselected_clr(190, 180, 225, 255);

        for (const auto& t : snap) {
            QueueItem item;
            item.task = t;

            item.game_title = t.title;
            if (item.game_title.empty()) {
                const auto* detail = model->GetCachedDetail(t.rom_id);
                if (detail) item.game_title = detail->file_name;
                else item.game_title = t.original_filename;

                size_t last_dot = item.game_title.find_last_of('.');
                if (last_dot != std::string::npos) {
                    item.game_title = item.game_title.substr(0, last_dot);
                }
            }
            // Last resort when neither RomM nor the filesystem gave us a name.
            if (item.game_title.empty()) {
                item.game_title = romm::i18n::format("queue.rom_id", {{"id", std::to_string(t.rom_id)}});
            }

            item.status_str = MakeStatusString(t);
            item.text_tex_selected = pu::ui::render::RenderText("Ubuntu@30", item.game_title, selected_clr, w - 120);
            item.text_tex_unselected = pu::ui::render::RenderText("Ubuntu@30", item.game_title, unselected_clr, w - 120);
            RenderStatusTextures(item);

            items.push_back(item);
        }

        if (selected_idx >= items.size() && !items.empty()) {
            selected_idx = items.size() - 1;
        } else if (items.empty()) {
            selected_idx = 0;
        }

        list_built = true;
        last_refresh = std::chrono::steady_clock::now();
    }

    void QueueList::RefreshList() {
        if (!list_built) {
            BuildList();
            return;
        }

        // The previous implementation fully rebuilt every row's textures each
        // idle frame (~20+ TTF renders at 60fps). Throttle, then re-render
        // only what changed.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_refresh).count() < 250) {
            return;
        }
        last_refresh = now;

        auto snap = SortedQueueSnapshot();

        bool membership_changed = (snap.size() != items.size());
        if (!membership_changed) {
            for (size_t i = 0; i < snap.size(); ++i) {
                if (snap[i].rom_id != items[i].task.rom_id) {
                    membership_changed = true;
                    break;
                }
            }
        }
        if (membership_changed) {
            BuildList();
            return;
        }

        for (size_t i = 0; i < snap.size(); ++i) {
            auto& item = items[i];
            item.task = snap[i]; // keep current state for input handling
            std::string s = MakeStatusString(snap[i]);
            if (s != item.status_str) {
                item.status_str = s;
                RenderStatusTextures(item);
            }
        }
    }

    void QueueList::OnSelectionUpdated() {
        RefreshList();
    }

    std::string QueueList::GetContextHint() const {
        if (items.empty() || selected_idx >= items.size()) {
            return romm::i18n::tr("hint.queue.back");
        }
        auto st = items[selected_idx].task.state.load();
        if (IsActiveState(st)) {
            return romm::i18n::tr("hint.queue.cancel");
        }
        switch (st) {
            case romm::model::DownloadState::Failed:
            case romm::model::DownloadState::Cancelled:
                return romm::i18n::tr("hint.queue.retry");
            case romm::model::DownloadState::Queued:
            case romm::model::DownloadState::Completed:
            default:
                return romm::i18n::tr("hint.queue.remove");
        }
    }

    void QueueList::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        RefreshList();

        if (items.empty()) {
            s32 tw = pu::ui::render::GetTextureWidth(empty_tex);
            s32 th = pu::ui::render::GetTextureHeight(empty_tex);
            drawer->RenderTexture(empty_tex, x_coord + (w - tw) / 2, y_coord + (h - th) / 2);
            return;
        }

        s32 item_h = 100;
        s32 max_visible = h / item_h;

        if (selected_idx < (size_t)scroll_offset) {
            scroll_offset = selected_idx;
        } else if (selected_idx >= (size_t)scroll_offset + max_visible) {
            scroll_offset = (int)(selected_idx - max_visible + 1);
        }

        for (size_t i = 0; i < (size_t)max_visible; ++i) {
            size_t idx = scroll_offset + i;
            if (idx >= items.size()) break;

            s32 iy = y_coord + (s32)i * item_h;
            bool is_selected = (idx == selected_idx);

            // Draw row background pill
            if (is_selected) {
                pu::ui::Color bg(85, 63, 152, 255); // Violet
                drawer->RenderRoundedRectangleFill(bg, x_coord, iy, w, item_h - 10, 10);
            } else {
                pu::ui::Color bg(30, 34, 43, 200); // Dark Slate
                pu::ui::Color border(45, 50, 62, 255);
                drawer->RenderRoundedRectangleFill(border, x_coord, iy, w, item_h - 10, 10);
                drawer->RenderRoundedRectangleFill(bg, x_coord + 2, iy + 2, w - 4, item_h - 14, 8);
            }

            auto& item = items[idx];

            // Render cover art from singleton Cache
            s32 cover_w = 56;
            s32 cover_h = 74;
            s32 cover_x = x_coord + 15;
            s32 cover_y = iy + 8;

            pu::sdl2::Texture cover_tex = nullptr;
            if (!item.task.cover_path_rel.empty()) {
                cover_tex = CoverCache::Instance().GetOrRequest(item.task.rom_id, item.task.platform_slug, item.task.cover_path_rel).texture;
            }

            if (cover_tex) {
                pu::ui::render::TextureRenderOptions opts;
                opts.width = cover_w;
                opts.height = cover_h;
                drawer->RenderTexture(cover_tex, cover_x, cover_y, opts);
            } else {
                auto plat_ph = GetPlaceholderCover(item.task.platform_slug);
                if (plat_ph) {
                    DrawPlaceholderCover(drawer, plat_ph, cover_x, cover_y, cover_w, cover_h);
                } else {
                    // Placeholder grey rectangle
                    drawer->RenderRoundedRectangleFill(pu::ui::Color(60, 60, 60, 255), cover_x, cover_y, cover_w, cover_h, 6);
                }
            }

            // Render text
            auto title_tex = is_selected ? item.text_tex_selected : item.text_tex_unselected;
            if (title_tex) {
                drawer->RenderTexture(title_tex, x_coord + 85, iy + 12);
            }

            auto status_tex = is_selected ? item.status_tex_selected : item.status_tex_unselected;
            if (status_tex) {
                drawer->RenderTexture(status_tex, x_coord + 85, iy + 52);
            }
        }
    }

    void QueueList::HandleInput(const u64 keys_down, const u64 keys_held) {
        if (items.empty()) return;

        if (keys_down & HidNpadButton_Up || keys_down & HidNpadButton_StickLUp) {
            if (selected_idx > 0) selected_idx--;
        }
        else if (keys_down & HidNpadButton_Down || keys_down & HidNpadButton_StickLDown) {
            if (selected_idx < items.size() - 1) selected_idx++;
        }
        else if (keys_down & HidNpadButton_A) {
            auto& dl_mgr = romm::model::DownloadManager::Instance();
            const int rom_id = items[selected_idx].task.rom_id;

            // Act on the task's CURRENT state, not the one captured at render
            // time — acting on a stale row could otherwise cancel whichever
            // download happens to be active now.
            auto fresh = dl_mgr.GetTaskSnapshot(rom_id);
            if (fresh.rom_id != 0) {
                auto st = fresh.state.load();
                if (IsActiveState(st)) {
                    dl_mgr.CancelDownload();
                } else if (st == romm::model::DownloadState::Failed ||
                           st == romm::model::DownloadState::Cancelled) {
                    dl_mgr.RetryFailed(rom_id);
                } else {
                    // Queued or Completed: remove just this entry
                    dl_mgr.RemoveFromQueue(rom_id);
                }
            }
            BuildList();
        }
        else if (keys_down & HidNpadButton_X) {
            auto& dl_mgr = romm::model::DownloadManager::Instance();
            dl_mgr.ClearCompleted();
            BuildList();
        }
    }


    // --- QueueLayout Implementation ---

    QueueLayout::QueueLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {

        this->SetBackgroundColor(pu::ui::Color(16, 18, 22, 255));

        header_text = pu::ui::elm::TextBlock::New(0, 90, romm::i18n::tr("queue.title"));
        header_text->SetFont("Orbitron@45");
        header_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        header_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(header_text);

        last_hint = romm::i18n::tr("hint.queue.default");
        hint_text = pu::ui::elm::TextBlock::New(0, 1080 - 65, last_hint);
        hint_text->SetFont("Ubuntu@30");
        hint_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        hint_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(hint_text);

        list = QueueList::New(460, 220, 1000, 600, nav);
        this->Add(list);

        auto global_progress = GlobalProgressBar::New(40, 20, 460, 56, nav);
        this->Add(global_progress);
    }

    void QueueLayout::UpdateHint() {
        if (!list || !hint_text) return;
        std::string hint = list->GetContextHint();
        if (hint != last_hint) {
            last_hint = hint;
            hint_text->SetText(hint);
        }
    }

    void QueueLayout::OnSelectionUpdated() {
        if (list) list->RefreshList();
        UpdateHint();
    }

    void QueueLayout::ForceRefresh() {
        if (list) list->BuildList();
        UpdateHint();
    }

    void QueueLayout::RefreshTranslations() {
        if (header_text) header_text->SetText(romm::i18n::tr("queue.title"));
        if (list) list->RefreshTranslations();
        // Clearing last_hint forces UpdateHint() past its "unchanged" check,
        // which would otherwise keep the old-language footer.
        last_hint.clear();
        UpdateHint();
    }

    void QueueLayout::HandleInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) {
        if (list) list->HandleInput(keys_down, keys_held);
        UpdateHint();
    }

}
