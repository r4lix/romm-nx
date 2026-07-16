#include "GlobalProgressBar.hpp"
#include "../model/DownloadManager.hpp"
#include "../model/DataModel.hpp"
#include "CoverCache.hpp"
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace romm::ui {

    GlobalProgressBar::GlobalProgressBar(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav) {
    }

    GlobalProgressBar::~GlobalProgressBar() {
        if (text_tex) {
            pu::ui::render::DeleteTexture(text_tex);
            text_tex = nullptr;
        }
        if (speed_tex) {
            pu::ui::render::DeleteTexture(speed_tex);
            speed_tex = nullptr;
        }
    }

    void GlobalProgressBar::UpdateTextures(const std::string& text, const std::string& speed) {
        if (current_text != text) {
            if (text_tex) {
                pu::ui::render::DeleteTexture(text_tex);
                text_tex = nullptr;
            }
            if (!text.empty()) {
                pu::ui::Color text_color(237, 229, 251, 255); // #EDE5FB
                text_tex = pu::ui::render::RenderText("Ubuntu@20", text, text_color);
            }
            current_text = text;
        }
        if (current_speed != speed) {
            if (speed_tex) {
                pu::ui::render::DeleteTexture(speed_tex);
                speed_tex = nullptr;
            }
            if (!speed.empty()) {
                pu::ui::Color text_color(180, 180, 180, 255); // Gray for speed
                speed_tex = pu::ui::render::RenderText("Ubuntu@18", speed, text_color);
            }
            current_speed = speed;
        }
    }

    void GlobalProgressBar::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        if (auto nav = nav_mgr.lock()) {
            if (nav->GetUninstallModalState().active) return;
        }

        auto& dl_mgr = romm::model::DownloadManager::Instance();
        auto active_snap = dl_mgr.GetActiveDownloadSnapshot();
        auto queue_snap = dl_mgr.GetQueueSnapshot();
        
        bool is_active = (active_snap.state == romm::model::DownloadState::Preparing || 
                          active_snap.state == romm::model::DownloadState::DownloadingGame || 
                          active_snap.state == romm::model::DownloadState::DownloadingCover || 
                          active_snap.state == romm::model::DownloadState::SyncingCover);

        int pending = 0;
        for (const auto& t : queue_snap) {
            if (t.state == romm::model::DownloadState::Queued) pending++;
        }

        if (!is_active && pending == 0) {
            return;
        }

        std::string status_text = "";
        std::string speed_text = "";
        float progress_pct = 0.0f;

        if (is_active) {
            if (current_rom_id != active_snap.rom_id) {
                current_rom_id = active_snap.rom_id;
                cover_tex = nullptr; // Let CoverCache fetch it
            }

            if (cover_tex == nullptr && !active_snap.cover_path_rel.empty()) {
                cover_tex = CoverCache::Instance().GetOrRequest(active_snap.rom_id, active_snap.platform_slug, active_snap.cover_path_rel).texture;
            }

            // Format Game Name (extension stripped if using filename)
            std::string game_name = active_snap.title;
            if (game_name.empty()) {
                game_name = active_snap.original_filename;
                if (game_name.empty()) game_name = active_snap.filename;
                
                size_t last_dot = game_name.find_last_of('.');
                if (last_dot != std::string::npos) {
                    game_name = game_name.substr(0, last_dot);
                }
            }
            if (game_name.empty()) game_name = "ROM ID " + std::to_string(active_snap.rom_id);
            
            // Truncate game name if too long
            if (game_name.length() > 25) {
                game_name = game_name.substr(0, 22) + "...";
            }
            
            status_text = game_name;

            if (active_snap.state == romm::model::DownloadState::Preparing) {
                speed_text = "Preparing...";
            } else if (active_snap.state == romm::model::DownloadState::DownloadingGame) {
                if (active_snap.total_bytes > 0) {
                    progress_pct = (float)active_snap.downloaded_bytes / active_snap.total_bytes;
                }
                
                // Format speed
                std::string speed_val = "";
                if (active_snap.download_speed_bps > 0) {
                    double speed_mbps = (double)active_snap.download_speed_bps / (1024.0 * 1024.0);
                    std::stringstream ss;
                    if (speed_mbps >= 1.0) {
                        ss << std::fixed << std::setprecision(1) << speed_mbps << " MB/s";
                    } else {
                        double speed_kbps = (double)active_snap.download_speed_bps / 1024.0;
                        ss << std::fixed << std::setprecision(1) << speed_kbps << " KB/s";
                    }
                    speed_val = ss.str();
                } else {
                    speed_val = "0.0 KB/s";
                }
                int pct_int = (int)(progress_pct * 100);
                speed_text = std::to_string(pct_int) + "% | " + speed_val;

            } else if (active_snap.state == romm::model::DownloadState::DownloadingCover) {
                speed_text = "Downloading Cover...";
            } else if (active_snap.state == romm::model::DownloadState::SyncingCover) {
                speed_text = "Syncing Cover...";
            }

            std::cout << "[DL_WIDGET] Active title=" << game_name 
                      << " progress=" << (int)(progress_pct * 100) 
                      << " speed=" << active_snap.download_speed_bps 
                      << " queued=" << pending << std::endl;

        } else if (pending > 0) {
            status_text = "Queue";
            speed_text = std::to_string(pending) + " items queued";
        }

        UpdateTextures(status_text, speed_text);

        // Draw background pill (expanded for rich layout)
        s32 render_h = h;
        if (is_active) render_h = 70; // Taller to fit thumbnail
        
        pu::ui::Color bg_color(30, 34, 43, 230); // Web Charcoal Grey
        pu::ui::Color border_color(45, 50, 62, 255);
        drawer->RenderRoundedRectangleFill(border_color, x_coord, y_coord, w, render_h, 12);
        drawer->RenderRoundedRectangleFill(bg_color, x_coord + 2, y_coord + 2, w - 4, render_h - 4, 10);

        s32 content_x = x_coord + 15;
        
        // Draw Thumbnail or placeholder if active
        if (is_active) {
            s32 placeholder_w = 38;
            s32 placeholder_h = 50;
            if (cover_tex != nullptr) {
                s32 c_w = pu::ui::render::GetTextureWidth(cover_tex);
                s32 c_h = pu::ui::render::GetTextureHeight(cover_tex);
                float scale = std::min(50.0f / c_w, 50.0f / c_h);
                s32 draw_w = (s32)(c_w * scale);
                s32 draw_h = (s32)(c_h * scale);
                pu::ui::render::TextureRenderOptions opts;
                opts.width = draw_w;
                opts.height = draw_h;
                drawer->RenderTexture(cover_tex, content_x + (50 - draw_w) / 2, y_coord + 10 + (50 - draw_h) / 2, opts);
            } else {
                // Placeholder grey rectangle
                drawer->RenderRoundedRectangleFill(pu::ui::Color(60, 60, 60, 255), content_x + (50 - placeholder_w) / 2, y_coord + 10, placeholder_w, placeholder_h, 4);
            }
            content_x += 65; // offset for text
        }

        // Draw progress fill in background of text area
        if (progress_pct > 0.0f) {
            pu::ui::Color fill_color(85, 63, 152, 100); // Violet accent, transparent
            s32 fill_w = (s32)((w - content_x + x_coord - 10) * progress_pct);
            if (fill_w > 0) {
                drawer->RenderRoundedRectangleFill(fill_color, content_x, y_coord + 5, fill_w, render_h - 10, 6);
            }
        }

        // Draw text
        if (text_tex) {
            drawer->RenderTexture(text_tex, content_x + 10, y_coord + 12);
        }
        if (speed_tex) {
            drawer->RenderTexture(speed_tex, content_x + 10, y_coord + 35);
        }
    }

}
