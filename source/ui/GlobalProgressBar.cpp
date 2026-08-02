#include "GlobalProgressBar.hpp"
#include "PlaceholderCover.hpp"
#include "../model/DownloadManager.hpp"
#include "../model/DataModel.hpp"
#include "CoverCache.hpp"
#include "../i18n/I18n.hpp"
#include <iomanip>
#include <sstream>
#include <algorithm>

namespace romm::ui {

    static std::string FormatSpeed(size_t bps) {
        std::stringstream ss;
        double speed_mbps = (double)bps / (1024.0 * 1024.0);
        if (speed_mbps >= 1.0) {
            ss << std::fixed << std::setprecision(1) << speed_mbps << " MB/s";
        } else {
            ss << std::fixed << std::setprecision(1) << ((double)bps / 1024.0) << " KB/s";
        }
        return ss.str();
    }

    static std::string FormatEta(long long remaining_bytes, size_t bps) {
        if (bps == 0 || remaining_bytes <= 0) return "";
        long long secs = remaining_bytes / (long long)bps;
        if (secs < 60) {
            return romm::i18n::format("progress.eta.seconds", {{"seconds", std::to_string(secs)}});
        }
        if (secs < 3600) {
            return romm::i18n::format("progress.eta.minutes", {
                {"minutes", std::to_string(secs / 60)},
                {"seconds", std::to_string(secs % 60)}
            });
        }
        return romm::i18n::format("progress.eta.hours", {
            {"hours", std::to_string(secs / 3600)},
            {"minutes", std::to_string((secs % 3600) / 60)}
        });
    }

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
                text_tex = pu::ui::render::RenderText("Ubuntu@20", text, text_color, w - 95);
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
                speed_tex = pu::ui::render::RenderText("Ubuntu@18", speed, text_color, w - 95);
            }
            current_speed = speed;
        }
    }

    void GlobalProgressBar::PollDownloadState() {
        auto& dl_mgr = romm::model::DownloadManager::Instance();
        auto active_snap = dl_mgr.GetActiveDownloadSnapshot();
        auto queue_snap = dl_mgr.GetQueueSnapshot();

        cached_active = (active_snap.state == romm::model::DownloadState::Preparing ||
                         active_snap.state == romm::model::DownloadState::DownloadingGame ||
                         active_snap.state == romm::model::DownloadState::DownloadingCover ||
                         active_snap.state == romm::model::DownloadState::SyncingCover ||
                         active_snap.state == romm::model::DownloadState::Injecting);

        cached_pending = 0;
        for (const auto& t : queue_snap) {
            if (t.state == romm::model::DownloadState::Queued) cached_pending++;
        }

        cached_pct = 0.0f;
        cached_title = "";
        cached_sub = "";

        if (cached_active) {
            cached_cover_rom_id = active_snap.rom_id;
            cached_cover_slug = active_snap.platform_slug;
            cached_cover_rel = active_snap.cover_path_rel;

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
            // Game name is RomM data (or the on-disk filename): only truncated
            // to fit the pill, never translated.
            if (game_name.empty()) {
                game_name = romm::i18n::format("progress.rom_id", {{"id", std::to_string(active_snap.rom_id)}});
            }
            if (game_name.length() > 28) {
                game_name = game_name.substr(0, 25) + "...";
            }
            cached_title = game_name;

            std::string sub;
            if (active_snap.state == romm::model::DownloadState::Preparing) {
                sub = romm::i18n::tr("progress.preparing");
            } else if (active_snap.state == romm::model::DownloadState::DownloadingGame) {
                long long downloaded = active_snap.downloaded_bytes.load();
                if (active_snap.total_bytes > 0) {
                    cached_pct = (float)downloaded / active_snap.total_bytes;
                    if (cached_pct > 1.0f) cached_pct = 1.0f;
                }
                size_t bps = active_snap.download_speed_bps.load();
                int pct_int = (int)(cached_pct * 100);

                const std::string eta = FormatEta(active_snap.total_bytes - downloaded, bps);
                sub = romm::i18n::format(eta.empty() ? "progress.downloading" : "progress.downloading_eta", {
                    {"percent", std::to_string(pct_int)},
                    {"speed", bps > 0 ? FormatSpeed(bps) : "0.0 KB/s"},
                    {"eta", eta}
                });
            } else {
                // DownloadingCover / SyncingCover: game payload is done
                cached_pct = 1.0f;
                sub = romm::i18n::tr("progress.cover_sync");
            }

            // The queued-count tail is its own template taking the status as a
            // placeholder, so the separator and word order stay translatable
            // rather than being hardcoded here.
            cached_sub = (cached_pending > 0)
                ? romm::i18n::format("progress.with_queued", {
                      {"status", sub},
                      {"queued", std::to_string(cached_pending)}
                  })
                : sub;
        } else if (cached_pending > 0) {
            cached_title = romm::i18n::tr("progress.queue");
            cached_sub = romm::i18n::format(
                cached_pending > 1 ? "progress.queued_items_plural" : "progress.queued_items",
                {{"count", std::to_string(cached_pending)}});
        }
    }

    void GlobalProgressBar::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        if (auto nav = nav_mgr.lock()) {
            if (nav->GetUninstallModalState().active) return;
        }

        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_poll).count() >= 250 ||
            last_poll == std::chrono::steady_clock::time_point{}) {
            last_poll = now;
            PollDownloadState();
        }

        if (!cached_active && cached_pending == 0) {
            return;
        }

        UpdateTextures(cached_title, cached_sub);

        // Draw background pill (taller when a download is active, to fit the
        // thumbnail + progress track)
        s32 render_h = cached_active ? 70 : h;

        pu::ui::Color bg_color(30, 34, 43, 230); // Web Charcoal Grey
        pu::ui::Color border_color(45, 50, 62, 255);
        drawer->RenderRoundedRectangleFill(border_color, x_coord, y_coord, w, render_h, 12);
        drawer->RenderRoundedRectangleFill(bg_color, x_coord + 2, y_coord + 2, w - 4, render_h - 4, 10);

        s32 content_x = x_coord + 15;

        if (cached_active) {
            // Cover thumbnail. Looked up from CoverCache every frame — the
            // cache owns the texture and may evict/delete it at any time, so
            // holding the raw pointer across frames would eventually render a
            // freed texture.
            pu::sdl2::Texture cover_tex = nullptr;
            if (!cached_cover_rel.empty()) {
                cover_tex = CoverCache::Instance().GetOrRequest(cached_cover_rom_id, cached_cover_slug, cached_cover_rel).texture;
            }

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
                auto plat_ph = GetPlaceholderCover(cached_cover_slug);
                if (plat_ph) {
                    DrawPlaceholderCover(drawer, plat_ph, content_x, y_coord + 10, 50, 50);
                } else {
                    drawer->RenderRoundedRectangleFill(pu::ui::Color(60, 60, 60, 255), content_x + 6, y_coord + 10, 38, 50, 4);
                }
            }
            content_x += 65;

            // Title line
            if (text_tex) {
                drawer->RenderTexture(text_tex, content_x, y_coord + 7);
            }

            // Progress track + fill
            s32 track_x = content_x;
            s32 track_w = w - (content_x - x_coord) - 15;
            s32 track_y = y_coord + 36;
            drawer->RenderRoundedRectangleFill(pu::ui::Color(45, 50, 62, 255), track_x, track_y, track_w, 8, 4);
            s32 fill_w = (s32)(track_w * cached_pct);
            if (fill_w > 0) {
                if (fill_w > track_w) fill_w = track_w;
                drawer->RenderRoundedRectangleFill(pu::ui::Color(140, 100, 240, 255), track_x, track_y, fill_w, 8, 4);
            }

            // Sub line: percent • speed • ETA • queued
            if (speed_tex) {
                drawer->RenderTexture(speed_tex, content_x, y_coord + 47);
            }
        } else {
            // Queue-only compact pill
            if (text_tex) {
                drawer->RenderTexture(text_tex, content_x + 10, y_coord + 12);
            }
            if (speed_tex) {
                drawer->RenderTexture(speed_tex, content_x + 10, y_coord + 35);
            }
        }
    }

}
