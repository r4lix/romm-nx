#include "QueueLayout.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../model/DownloadManager.hpp"
#include "GlobalProgressBar.hpp"
#include "CoverCache.hpp"
#include <iostream>

namespace romm::ui {

    QueueList::QueueList(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav) {
        pu::ui::Color text_color(237, 229, 251, 255);
        empty_tex = pu::ui::render::RenderText("Ubuntu@30", "Queue is empty.", text_color);
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

    void QueueList::BuildList() {
        ClearTextures();
        auto nav = nav_mgr.lock();
        if (!nav) return;
        auto model = nav->GetModel();
        if (!model) return;
        
        auto& dl_mgr = romm::model::DownloadManager::Instance();
        auto queue_snap = dl_mgr.GetQueueSnapshot();
        auto active_snap = dl_mgr.GetActiveDownloadSnapshot();

        pu::ui::Color selected_clr(237, 229, 251, 255);
        pu::ui::Color unselected_clr(190, 180, 225, 255);
        pu::ui::Color detail_selected_clr(210, 200, 235, 255);
        pu::ui::Color detail_unselected_clr(140, 130, 170, 255);
        
        // Add active download to the top if any
        if (active_snap.rom_id != 0) {
            QueueItem item;
            item.task = active_snap;
            
            item.game_title = active_snap.title;
            if (item.game_title.empty()) {
                const auto* detail = model->GetCachedDetail(active_snap.rom_id);
                if (detail) item.game_title = detail->file_name;
                else item.game_title = active_snap.original_filename;
                
                size_t last_dot = item.game_title.find_last_of('.');
                if (last_dot != std::string::npos) {
                    item.game_title = item.game_title.substr(0, last_dot);
                }
            }
            if (item.game_title.empty()) item.game_title = "ROM ID " + std::to_string(active_snap.rom_id);
            
            // Format status for active download
            std::string state_str = "";
            if (active_snap.state == romm::model::DownloadState::DownloadingGame) {
                float progress_pct = 0.0f;
                if (active_snap.total_bytes > 0) {
                    progress_pct = (float)active_snap.downloaded_bytes / active_snap.total_bytes;
                }
                int pct_int = (int)(progress_pct * 100);
                
                std::string speed_str = "";
                if (active_snap.download_speed_bps > 0) {
                    double speed_mbps = (double)active_snap.download_speed_bps / (1024.0 * 1024.0);
                    std::stringstream ss;
                    if (speed_mbps >= 1.0) {
                        ss << std::fixed << std::setprecision(1) << speed_mbps << " MB/s";
                    } else {
                        double speed_kbps = (double)active_snap.download_speed_bps / 1024.0;
                        ss << std::fixed << std::setprecision(1) << speed_kbps << " KB/s";
                    }
                    speed_str = ss.str();
                } else {
                    speed_str = "Starting...";
                }
                state_str = "Downloading  •  " + std::to_string(pct_int) + "%  •  " + speed_str;
            } else if (active_snap.state == romm::model::DownloadState::Preparing) {
                state_str = "Preparing...";
            } else if (active_snap.state == romm::model::DownloadState::DownloadingCover || active_snap.state == romm::model::DownloadState::SyncingCover) {
                state_str = "Cover Sync...";
            } else {
                state_str = "Active";
            }
            
            item.text_tex_selected = pu::ui::render::RenderText("Ubuntu@30", item.game_title, selected_clr, w - 120);
            item.text_tex_unselected = pu::ui::render::RenderText("Ubuntu@30", item.game_title, unselected_clr, w - 120);
            item.status_tex_selected = pu::ui::render::RenderText("Ubuntu@20", state_str, detail_selected_clr, w - 120);
            item.status_tex_unselected = pu::ui::render::RenderText("Ubuntu@20", state_str, detail_unselected_clr, w - 120);
            
            items.push_back(item);
        }

        // Add queued items
        for (const auto& t : queue_snap) {
            if (!active_snap.final_path.empty() && t.final_path == active_snap.final_path) continue; // Prevent duplication by path
            
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
            if (item.game_title.empty()) item.game_title = "ROM ID " + std::to_string(t.rom_id);
            
            std::string state_str = "";
            switch (t.state) {
                case romm::model::DownloadState::Queued: state_str = "Queued"; break;
                case romm::model::DownloadState::Preparing: state_str = "Preparing"; break;
                case romm::model::DownloadState::DownloadingGame: state_str = "Downloading"; break;
                case romm::model::DownloadState::DownloadingCover: state_str = "Cover Sync"; break;
                case romm::model::DownloadState::SyncingCover: state_str = "Cover Sync"; break;
                case romm::model::DownloadState::Completed: state_str = "Completed"; break;
                case romm::model::DownloadState::Failed: {
                    state_str = "Failed";
                    if (!t.error_message.empty()) state_str += " (" + t.error_message + ")";
                    break;
                }
                case romm::model::DownloadState::Cancelled: state_str = "Cancelled"; break;
                default: state_str = "Unknown"; break;
            }

            item.text_tex_selected = pu::ui::render::RenderText("Ubuntu@30", item.game_title, selected_clr, w - 120);
            item.text_tex_unselected = pu::ui::render::RenderText("Ubuntu@30", item.game_title, unselected_clr, w - 120);
            item.status_tex_selected = pu::ui::render::RenderText("Ubuntu@20", state_str, detail_selected_clr, w - 120);
            item.status_tex_unselected = pu::ui::render::RenderText("Ubuntu@20", state_str, detail_unselected_clr, w - 120);
            
            items.push_back(item);
        }
        
        if (selected_idx >= items.size() && !items.empty()) {
            selected_idx = items.size() - 1;
        } else if (items.empty()) {
            selected_idx = 0;
        }
        
        list_built = true;
    }

    void QueueList::OnSelectionUpdated() {
        BuildList();
    }

    void QueueList::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        if (!list_built) BuildList();

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
                // Placeholder grey rectangle
                drawer->RenderRoundedRectangleFill(pu::ui::Color(60, 60, 60, 255), cover_x, cover_y, cover_w, cover_h, 6);
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
            auto& item = items[selected_idx];
            
            if (item.task.state == romm::model::DownloadState::Failed || item.task.state == romm::model::DownloadState::Cancelled) {
                dl_mgr.RetryFailed(item.task.rom_id);
            } else if (item.task.state == romm::model::DownloadState::Queued) {
                dl_mgr.RemoveFromQueue(item.task.rom_id);
            } else if (item.task.state == romm::model::DownloadState::Completed) {
                dl_mgr.ClearCompleted();
            } else {
                // Active task, do we cancel it?
                dl_mgr.CancelDownload();
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

        header_text = pu::ui::elm::TextBlock::New(0, 90, "Download Queue");
        header_text->SetFont("Orbitron@45");
        header_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        header_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(header_text);

        hint_text = pu::ui::elm::TextBlock::New(0, 1080 - 65, "A Remove/Retry   |   X Clear Completed   |   B Back");
        hint_text->SetFont("Ubuntu@30");
        hint_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        hint_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(hint_text);

        list = QueueList::New(460, 220, 1000, 600, nav);
        this->Add(list);

        auto global_progress = GlobalProgressBar::New(40, 20, 460, 56, nav);
        this->Add(global_progress);
    }

    void QueueLayout::OnSelectionUpdated() {
        if (list) list->OnSelectionUpdated();
    }

    void QueueLayout::ForceRefresh() {
        if (list) list->BuildList();
    }

    void QueueLayout::HandleInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) {
        if (list) list->HandleInput(keys_down, keys_held);
    }

}
