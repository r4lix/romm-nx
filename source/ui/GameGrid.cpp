#include "GameGrid.hpp"
#include "CoverCache.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/DownloadManager.hpp"
#include <cstdio>
#include <iostream>
#include <chrono>
#include <algorithm>

namespace romm::ui {

    GameGrid::GameGrid(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav) {
        OnSelectionUpdated();
    }

    GameGrid::~GameGrid() {
        ClearStatusTex();
        ClearInfoTextures();
    }

    void GameGrid::ClearStatusTex() {
        if (status_tex) {
            pu::ui::render::DeleteTexture(status_tex);
            status_tex = nullptr;
        }
    }

    void GameGrid::ClearInfoTextures() {
        if (info_title_tex) {
            pu::ui::render::DeleteTexture(info_title_tex);
            info_title_tex = nullptr;
        }
        if (info_sub_tex) {
            pu::ui::render::DeleteTexture(info_sub_tex);
            info_sub_tex = nullptr;
        }
        info_cached_game_idx = 999999;
    }

    void GameGrid::AdjustProfileForHeight(CoverProfile& profile) {
        s32 available_h;
        bool is_handheld = (profile.type == CoverProfileType::NintendoDS ||
                            profile.type == CoverProfileType::GameBoy ||
                            profile.type == CoverProfileType::GameBoyColor ||
                            profile.type == CoverProfileType::GameBoyAdvance ||
                            profile.type == CoverProfileType::PSPPortrait);
        if (is_handheld) {
            available_h = this->h - 85;
        } else {
            available_h = this->h - 95;
        }

        // profile.columns/visibleRows come from GetCoverProfile(), which already
        // accounts for the current grid view mode (Default vs. Big).
        int cols = profile.columns;
        int rows = profile.visibleRows;

        // Which axis constrains tile size — width (split across columns) or
        // height (split across rows) — depends on the profile's aspect ratio
        // and the column/row count. This canvas is short and wide, so for any
        // profile with 2+ rows, height is almost always the binding constraint:
        // reducing columns alone barely changes tile size, it just widens the
        // gaps between same-sized tiles. So compute both candidate sizes (using
        // FIXED gap/offset constants, not ones back-derived from a guessed tile
        // size) and take whichever is smaller, then center the leftover space
        // on the other axis. This is what makes "Big" mode (fewer rows) actually
        // grow the tiles instead of just spreading them out.
        auto solve = [&](s32 offsetX, s32 gapX, s32 offsetY, s32 gapY, int aspect_w, int aspect_h) {
            s32 h_bound = (available_h - 2 * offsetY - (rows - 1) * gapY) / rows;
            s32 w_bound_as_h = ((this->w - 2 * offsetX - (cols - 1) * gapX) * aspect_h) / (cols * aspect_w);
            profile.coverH = std::min(h_bound, w_bound_as_h);
            profile.coverW = (profile.coverH * aspect_w) / aspect_h;
            profile.gapX = gapX;
            profile.gapY = gapY;
            profile.offsetX = (this->w - cols * profile.coverW - (cols - 1) * gapX) / 2;
            profile.offsetY = (available_h - rows * profile.coverH - (rows - 1) * gapY) / 2;
        };

        if (profile.type == CoverProfileType::PS2Portrait) {
            solve(36, 40, 15, 20, 161, 230); // Aspect ratio 161:230
        }
        else if (profile.type == CoverProfileType::PSPPortrait) {
            solve(36, 40, 15, 20, 165, 252); // Aspect ratio 165:252
        }
        else if (profile.type == CoverProfileType::PS1Square) {
            solve(32, 24, 32, 24, 1, 1); // Square
        }
        else if (profile.type == CoverProfileType::NintendoDS ||
                 profile.type == CoverProfileType::GameBoy ||
                 profile.type == CoverProfileType::GameBoyColor ||
                 profile.type == CoverProfileType::GameBoyAdvance) {
            solve(20, 30, 12, 15, 276, 250); // Aspect ratio 276:250
        }
        else {
            // DefaultPortrait fallback
            solve(20, 30, 12, 15, 120, 180); // Aspect ratio 120:180
        }
    }

    void GameGrid::RebuildInfoStrip(const romm::model::Game& game, const std::string& platform_name, size_t game_idx, size_t total) {
        if (game_idx == info_cached_game_idx && total == info_cached_total) return;

        ClearInfoTextures();

        pu::ui::Color title_color(237, 229, 251, 255); // #EDE5FB
        pu::ui::Color sub_color(190, 180, 225, 255);   // #BEB4E1

        // Title texture
        info_title_tex = pu::ui::render::RenderText("Orbitron@30", game.title, title_color);

        // Sub info: Size, Developer, Index
        std::string size_str = "Size: Unknown";
        if (game.fs_size_bytes > 0) {
            double size_mb = (double)game.fs_size_bytes / (1024.0 * 1024.0);
            if (size_mb >= 1024.0) {
                char buf[64];
                std::sprintf(buf, "Size: %.2f GB", size_mb / 1024.0);
                size_str = buf;
            } else if (size_mb >= 1.0) {
                char buf[64];
                std::sprintf(buf, "Size: %.2f MB", size_mb);
                size_str = buf;
            } else {
                double size_kb = (double)game.fs_size_bytes / 1024.0;
                if (size_kb >= 1.0) {
                    char buf[64];
                    std::sprintf(buf, "Size: %.2f KB", size_kb);
                    size_str = buf;
                } else {
                    char buf[64];
                    std::sprintf(buf, "Size: %lld B", game.fs_size_bytes);
                    size_str = buf;
                }
            }
        }

        std::string sub_text = size_str;
        sub_text += "  |  " + std::to_string(game_idx + 1) + " / " + std::to_string(total);

        info_sub_tex = pu::ui::render::RenderText("Ubuntu@24", sub_text, sub_color, w - 40);

        info_cached_game_idx = game_idx;
        info_cached_total = total;
    }

    void GameGrid::OnSelectionUpdated() {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        auto model = nav->GetModel();
        if (!model) return;

        pu::ui::Color text_color(237, 229, 251, 255); // #EDE5FB

        auto plat_state = model->GetPlatformState();
        if (plat_state != romm::model::ApiState::Success) {
            ClearStatusTex();
            ClearInfoTextures();
            if (plat_state == romm::model::ApiState::Loading) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Loading platforms...", text_color);
            } else if (plat_state == romm::model::ApiState::FailedConnect) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Failed to connect", text_color);
            } else if (plat_state == romm::model::ApiState::Unauthorized) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Unauthorized / invalid API key", text_color);
            } else {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Failed to connect", text_color);
            }
            return;
        }

        auto roms_state = model->GetRomsState();
        if (roms_state != romm::model::ApiState::Success) {
            ClearStatusTex();
            ClearInfoTextures();
            if (roms_state == romm::model::ApiState::Idle) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Select a platform and press A to load ROMs", text_color);
            } else if (roms_state == romm::model::ApiState::Loading) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Loading ROMs...", text_color);
            } else if (roms_state == romm::model::ApiState::FailedConnect) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Failed to connect", text_color);
            } else if (roms_state == romm::model::ApiState::Unauthorized) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Unauthorized / invalid API key", text_color);
            } else if (roms_state == romm::model::ApiState::NoData) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "No games found", text_color);
            }
            return;
        }

        const auto& platforms = model->GetPlatforms();
        if (platforms.empty()) return;

        size_t plat_idx = nav->GetLoadedPlatformIdx();
        if (plat_idx >= platforms.size()) return;

        const auto& current_platform = platforms.at(plat_idx);
        size_t letter_idx = nav->GetSelectedLetterIdx();

        // 1. Rebuild filtered games only if platform, letter, or the grid view
        // mode (Default/Big, from the Y-Menu) changed — GetCoverProfile() picks
        // column counts based on the live view mode, so a mode change needs the
        // same refresh path as a platform change even though the platform itself
        // didn't move.
        auto current_view_mode = romm::model::ConfigManager::Instance().GetGridViewMode();
        bool view_mode_changed = (current_view_mode != cached_view_mode);

        if (plat_idx != cached_selected_platform_idx || letter_idx != cached_selected_letter_idx || view_mode_changed) {
            ClearStatusTex();

            if (plat_idx != cached_selected_platform_idx || view_mode_changed) {
                current_profile = GetCoverProfile(current_platform);
                cached_view_mode = current_view_mode;
            }

            filtered_games.clear();
            char target_letter = ' ';
            if (letter_idx > 0) {
                target_letter = 'A' + (letter_idx - 1);
            }

            for (const auto& game : current_platform.games) {
                if (target_letter == ' ') {
                    filtered_games.push_back(game);
                } else {
                    if (!game.title.empty()) {
                        char first_char = std::toupper(game.title[0]);
                        if (first_char == target_letter) {
                            filtered_games.push_back(game);
                        }
                    }
                }
            }

            scroll_row_offset = 0;
            current_visible_start_idx = 999999;
            cached_selected_platform_idx = plat_idx;
            cached_selected_letter_idx = letter_idx;
        }

        if (filtered_games.empty()) {
            ClearStatusTex();
            ClearInfoTextures();
            status_tex = pu::ui::render::RenderText("Ubuntu@37", "No games found", text_color);
            return;
        }

        // We have successfully resolved games, make sure status_tex is null
        ClearStatusTex();

        AdjustProfileForHeight(current_profile);

        // 2. Adjust scroll offset to keep selected game in viewport
        size_t selected_game_idx = nav->GetSelectedGameIdx();
        if (selected_game_idx >= filtered_games.size()) {
            selected_game_idx = 0;
        }

        if (selected_game_idx != cached_info_game_idx) {
            cached_info_game_idx = selected_game_idx;
            info_scroll_x = 0.0f;
            info_wait_timer = 2.0f;
            info_scroll_state = InfoScrollState::WaitStart;
            last_info_scroll_update_time = std::chrono::steady_clock::time_point();
        }

        int selected_row = selected_game_idx / current_profile.columns;

        if (selected_row < scroll_row_offset) {
            scroll_row_offset = selected_row;
        } else if (selected_row >= scroll_row_offset + current_profile.visibleRows) {
            scroll_row_offset = selected_row - current_profile.visibleRows + 1;
        }

        // Ensure scroll offset is within bounds
        int total_rows = (filtered_games.size() + current_profile.columns - 1) / current_profile.columns;
        if (scroll_row_offset > total_rows - current_profile.visibleRows) {
            scroll_row_offset = total_rows - current_profile.visibleRows;
        }
        if (scroll_row_offset < 0) {
            scroll_row_offset = 0;
        }

        // 3. Update info strip
        if (current_profile.type == CoverProfileType::PS1Square || current_profile.type == CoverProfileType::NintendoDS ||
            current_profile.type == CoverProfileType::GameBoy || current_profile.type == CoverProfileType::GameBoyColor ||
            current_profile.type == CoverProfileType::GameBoyAdvance || current_profile.type == CoverProfileType::PS2Portrait) {
            RebuildInfoStrip(filtered_games[selected_game_idx], current_platform.name, selected_game_idx, filtered_games.size());
        }

        // 4. Update visible range tracking (mostly for logging/debug now, as covers load on render)
        size_t visible_start_idx = scroll_row_offset * current_profile.columns;
        size_t visible_count = 0;
        if (visible_start_idx < filtered_games.size()) {
            visible_count = std::min(static_cast<size_t>(current_profile.visibleRows * current_profile.columns), filtered_games.size() - visible_start_idx);
        }
        current_visible_start_idx = visible_start_idx;
        current_visible_count = visible_count;
    }

    static void PrefetchCover(const romm::model::Game& game, size_t game_idx, romm::model::CoversQuality quality, int cols, size_t selected_game_idx, CoverProfileType profile_type, const std::string& platform_slug) {
        // 1. Always request small cover first to ensure it's cached/loaded
        pu::sdl2::Texture small_tex = nullptr;
        if (!game.cover_path.empty()) {
            small_tex = CoverCache::Instance().GetOrRequest(game.id, platform_slug, game.cover_path, profile_type, false, true).texture;
        }

        // 2. If small cover is loaded (Ready in cache) and we want big cover:
        if (small_tex && !game.cover_path_large.empty() && quality != romm::model::CoversQuality::SD) {
            bool allow_download = false;
            if (quality == romm::model::CoversQuality::HD) {
                if (CoverCache::Instance().GetPendingBigDownloadsCount() < 2) {
                    allow_download = true;
                }
            } else if (quality == romm::model::CoversQuality::Balanced) {
                bool is_nearby = (game_idx == selected_game_idx) ||
                                 (game_idx == selected_game_idx - 1) ||
                                 (game_idx == selected_game_idx + 1) ||
                                 (game_idx == selected_game_idx - cols) ||
                                 (game_idx == selected_game_idx + cols);
                if (is_nearby && CoverCache::Instance().GetPendingBigDownloadsCount() < 1) {
                    allow_download = true;
                }
            }

            // Request big cover (load from disk, or download if allow_download is true)
            CoverCache::Instance().GetOrRequest(game.id, platform_slug, game.cover_path_large, profile_type, true, allow_download);
        }
    }

    void GameGrid::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        // Run background texture loader
        CoverCache::Instance().PollCompleted();

        auto nav = nav_mgr.lock();
        if (!nav) return;

        auto model = nav->GetModel();
        if (!model) return;

        size_t plat_idx = nav->GetLoadedPlatformIdx();
        const auto& platforms = model->GetPlatforms();
        std::string platform_slug = "default";
        if (plat_idx < platforms.size()) {
            platform_slug = platforms[plat_idx].slug;
        }

        // Render status texture (if loading / failed / no data)
        if (status_tex) {
            s32 msg_w = pu::ui::render::GetTextureWidth(status_tex);
            s32 msg_h = pu::ui::render::GetTextureHeight(status_tex);
            drawer->RenderTexture(status_tex, x_coord + (w - msg_w) / 2, y_coord + (h - msg_h) / 2);
            return;
        }

        if (filtered_games.empty()) return;

        size_t selected_game_idx = nav->GetSelectedGameIdx();
        bool grid_focused = (nav->GetLibraryFocus() == romm::navigation::LibraryFocus::Grid);

        size_t visible_start_idx = current_visible_start_idx;
        size_t visible_count = current_visible_count;

        // 1. Draw Cover Grid with clipping viewport bounds
        SDL_Renderer* renderer = pu::ui::render::GetMainRenderer();
        SDL_Rect old_clip;
        SDL_bool has_old_clip = SDL_RenderIsClipEnabled(renderer);
        if (has_old_clip) {
            SDL_RenderGetClipRect(renderer, &old_clip);
        }

        SDL_Rect grid_clip = {
            (int)x_coord,
            (int)y_coord,
            (int)this->w,
            (int)this->h
        };
        SDL_RenderSetClipRect(renderer, &grid_clip);

        for (size_t i = 0; i < visible_count; ++i) {
            size_t game_idx = visible_start_idx + i;
            if (game_idx >= filtered_games.size()) break;

            const auto& game = filtered_games[game_idx];

            int row = game_idx / current_profile.columns;
            int col = game_idx % current_profile.columns;

            // Compute rendering row relative to scroll offset
            int screen_row = row - scroll_row_offset;

            s32 tile_x = x_coord + current_profile.offsetX + col * (current_profile.coverW + current_profile.gapX);
            s32 tile_y = y_coord + current_profile.offsetY + screen_row * (current_profile.coverH + current_profile.gapY);

            bool is_selected = (game_idx == selected_game_idx);

            // Prefer the HD cover (path_cover_large) for grid display.
            pu::sdl2::Texture tex = nullptr;
            bool is_handheld = (current_profile.type == CoverProfileType::PSPPortrait || current_profile.type == CoverProfileType::NintendoDS ||
                                current_profile.type == CoverProfileType::GameBoy || current_profile.type == CoverProfileType::GameBoyColor ||
                                current_profile.type == CoverProfileType::GameBoyAdvance);
            if (is_handheld) {
                auto& config = romm::model::ConfigManager::Instance();
                auto quality = config.GetCoversQuality();

                bool allow_download = false;
                std::string large_path = game.cover_path_large;

                if (!large_path.empty() && quality != romm::model::CoversQuality::SD) {
                    if (quality == romm::model::CoversQuality::HD) {
                        // HD mode: allow download if concurrent downloads < 2
                        if (CoverCache::Instance().GetPendingBigDownloadsCount() < 2) {
                            allow_download = true;
                        }
                    } else if (quality == romm::model::CoversQuality::Balanced) {
                        // Balanced mode: only download if selected or nearby AND concurrent downloads < 1
                        int cols = current_profile.columns;
                        bool is_nearby = (game_idx == selected_game_idx) ||
                                         (game_idx == selected_game_idx - 1) ||
                                         (game_idx == selected_game_idx + 1) ||
                                         (game_idx == selected_game_idx - cols) ||
                                         (game_idx == selected_game_idx + cols);
                        if (is_nearby && CoverCache::Instance().GetPendingBigDownloadsCount() < 1) {
                            allow_download = true;
                        }
                    }
                }

                if (!large_path.empty() && quality != romm::model::CoversQuality::SD) {
                    tex = CoverCache::Instance().GetOrRequest(game.id, platform_slug, large_path, current_profile.type, true, allow_download).texture;
                }

                if (!tex && !game.cover_path.empty()) {
                    tex = CoverCache::Instance().GetOrRequest(game.id, platform_slug, game.cover_path, current_profile.type, false, true).texture;
                }
            } else {
                if (!game.cover_path_large.empty()) {
                    tex = CoverCache::Instance().GetOrRequest(game.id, platform_slug, game.cover_path_large, current_profile.type, true, true).texture;
                }
                if (!tex && !game.cover_path.empty()) {
                    tex = CoverCache::Instance().GetOrRequest(game.id, platform_slug, game.cover_path, current_profile.type, false, true).texture;
                }
            }

            s32 actual_w = current_profile.coverW;
            s32 actual_h = current_profile.coverH;
            s32 actual_x = tile_x;
            s32 actual_y = tile_y;

            // Calculate actual rendering size based on fitMode and loaded texture
            // Avoid touching PS1Square layout behavior
            if (tex && current_profile.type != CoverProfileType::PS1Square) {
                if (current_profile.fitMode == FitMode::Contain) {
                    s32 tw = pu::ui::render::GetTextureWidth(tex);
                    s32 th = pu::ui::render::GetTextureHeight(tex);
                    float scale = std::min((float)current_profile.coverW / tw, (float)current_profile.coverH / th);
                    actual_w = (s32)(tw * scale);
                    actual_h = (s32)(th * scale);
                    actual_x = tile_x + (current_profile.coverW - actual_w) / 2;
                    actual_y = tile_y + (current_profile.coverH - actual_h) / 2;
                }
            }

            pu::ui::Color placeholder_color(30, 34, 43, 255); // Web Charcoal Grey (#1E222B)
            pu::ui::Color border_color;
            s32 border_width = 2;

            if (is_selected) {
                if (grid_focused) {
                    border_color = pu::ui::Color(120, 85, 220, 255); // Brighter, more vibrant violet
                    border_width = 8; // Thicker border (was 4)
                } else {
                    border_color = pu::ui::Color(120, 85, 220, 120); // More visible dimmed violet
                    border_width = 5; // (was 3)
                }
            } else {
                border_color = pu::ui::Color(45, 50, 62, 255); // Slate Border Grey (#2D323E)
                border_width = 2;
            }

            // Draw outer border (slightly larger than cover to act as frame) - fits actual cover bounds
            drawer->RenderRoundedRectangleFill(border_color, 
                actual_x - border_width, actual_y - border_width, 
                actual_w + (border_width * 2), actual_h + (border_width * 2), 6);
            
            if (tex) {
                if (current_profile.fitMode == FitMode::Contain) {
                    // Draw placeholder fitting actual bounds
                    drawer->RenderRoundedRectangleFill(placeholder_color, actual_x, actual_y, actual_w, actual_h, 4);
                    
                    pu::ui::render::TextureRenderOptions opts;
                    opts.width = actual_w;
                    opts.height = actual_h;
                    drawer->RenderTexture(tex, actual_x, actual_y, opts);
                } else if (current_profile.fitMode == FitMode::SmartCover) {
                    // SmartCover: Scale to fill slot, crop overflow, center crop
                    // First draw placeholder to hide any sub-pixel edges
                    drawer->RenderRoundedRectangleFill(placeholder_color, tile_x, tile_y, current_profile.coverW, current_profile.coverH, 4);
                    s32 tw = pu::ui::render::GetTextureWidth(tex);
                    s32 th = pu::ui::render::GetTextureHeight(tex);
                    
                    float target_aspect = (float)current_profile.coverW / current_profile.coverH;
                    float tex_aspect = (float)tw / th;
                    
                    s32 crop_w = tw;
                    s32 crop_h = th;
                    s32 src_x = 0;
                    s32 src_y = 0;
                    
                    // Determine if the aspect ratio is extremely unusual (fallback to contain)
                    if (tex_aspect > target_aspect * 1.5f || tex_aspect < target_aspect * 0.66f) {
                        float scale = std::min((float)current_profile.coverW / tw, (float)current_profile.coverH / th);
                        s32 draw_w = (s32)(tw * scale);
                        s32 draw_h = (s32)(th * scale);
                        s32 draw_x = tile_x + (current_profile.coverW - draw_w) / 2;
                        s32 draw_y = tile_y + (current_profile.coverH - draw_h) / 2;
                        pu::ui::render::TextureRenderOptions opts;
                        opts.width = draw_w;
                        opts.height = draw_h;
                        drawer->RenderTexture(tex, draw_x, draw_y, opts);
                    } else {
                        // Calculate cropped source rect
                        if (tex_aspect > target_aspect) {
                            crop_w = (s32)(th * target_aspect);
                            src_x = (tw - crop_w) / 2;
                        } else if (tex_aspect < target_aspect) {
                            crop_h = (s32)(tw / target_aspect);
                            src_y = (th - crop_h) / 2;
                        }
                        
                        SDL_Rect src_rect = { src_x, src_y, crop_w, crop_h };
                        SDL_Rect dst_rect = { tile_x, tile_y, current_profile.coverW, current_profile.coverH };
                        
                        SDL_RenderCopyEx(pu::ui::render::GetMainRenderer(), tex, &src_rect, &dst_rect, 0, nullptr, SDL_FLIP_NONE);
                    }
                } else {
                    // FitMode::Cover or Stretch
                    pu::ui::render::TextureRenderOptions opts;
                    opts.width = current_profile.coverW;
                    opts.height = current_profile.coverH;
                    drawer->RenderTexture(tex, tile_x, tile_y, opts);
                }
            } else {
                drawer->RenderRoundedRectangleFill(placeholder_color, actual_x, actual_y, actual_w, actual_h, 4);
            }

            if (romm::model::ConfigManager::Instance().ShowInstalledBadge()) {
                const auto& platforms = nav->GetModel()->GetPlatforms();
                size_t plat_idx = nav->GetLoadedPlatformIdx();
                if (plat_idx < platforms.size()) {
                    const auto& current_platform = platforms.at(plat_idx);
                    if (romm::model::DownloadManager::Instance().IsGameInstalled(current_platform.slug, game.fs_name)) {
                        s32 dot_radius = 12;
                        s32 dot_x = actual_x + actual_w - dot_radius - 8;
                        s32 dot_y = actual_y + dot_radius + 8;
                        drawer->RenderCircleFill(pu::ui::Color(46, 204, 113, 255), dot_x, dot_y, dot_radius);
                    }
                }
            }
        }

        if (has_old_clip) {
            SDL_RenderSetClipRect(renderer, &old_clip);
        } else {
            SDL_RenderSetClipRect(renderer, nullptr);
        }
        
        // 2. Draw Bottom Info Strip (Enabled for all platforms)
        if (true) {
            s32 info_y;
            bool is_handheld = (current_profile.type == CoverProfileType::NintendoDS ||
                                current_profile.type == CoverProfileType::GameBoy ||
                                current_profile.type == CoverProfileType::GameBoyColor ||
                                current_profile.type == CoverProfileType::GameBoyAdvance ||
                                current_profile.type == CoverProfileType::PSPPortrait);
            if (is_handheld) {
                info_y = y_coord + this->h - 85;
            } else {
                info_y = y_coord + this->h - 95;
            }
            
            // Marquee delta-time update for bottom info strip
            auto marquee_now = std::chrono::steady_clock::now();
            float marquee_dt = 0.0f;
            if (last_info_scroll_update_time.time_since_epoch().count() > 0) {
                marquee_dt = std::chrono::duration<float>(marquee_now - last_info_scroll_update_time).count();
            }
            last_info_scroll_update_time = marquee_now;
            if (marquee_dt > 0.1f) marquee_dt = 0.1f;

            s32 max_info_w = w - 40; // 1500px
            s32 info_title_w = 0;
            s32 info_title_h = 0;
            if (info_title_tex) {
                info_title_w = pu::ui::render::GetTextureWidth(info_title_tex);
                info_title_h = pu::ui::render::GetTextureHeight(info_title_tex);
            }

            if (info_title_w > max_info_w) {
                float max_scroll = (float)(info_title_w - max_info_w);
                float scroll_speed = 80.0f; // px per second

                if (info_scroll_state == InfoScrollState::WaitStart) {
                    info_wait_timer -= marquee_dt;
                    if (info_wait_timer <= 0.0f) {
                        info_scroll_state = InfoScrollState::Scrolling;
                    }
                } else if (info_scroll_state == InfoScrollState::Scrolling) {
                    info_scroll_x += scroll_speed * marquee_dt;
                    if (info_scroll_x >= max_scroll) {
                        info_scroll_x = max_scroll;
                        info_scroll_state = InfoScrollState::WaitEnd;
                        info_wait_timer = 2.0f;
                    }
                } else if (info_scroll_state == InfoScrollState::WaitEnd) {
                    info_wait_timer -= marquee_dt;
                    if (info_wait_timer <= 0.0f) {
                        info_scroll_x = 0.0f;
                        info_scroll_state = InfoScrollState::WaitStart;
                        info_wait_timer = 2.0f;
                    }
                }
            } else {
                info_scroll_x = 0.0f;
            }

            if (info_title_tex) {
                SDL_Renderer* renderer = pu::ui::render::GetMainRenderer();
                SDL_Rect old_clip;
                SDL_bool has_clip = SDL_RenderIsClipEnabled(renderer);
                if (has_clip) {
                    SDL_RenderGetClipRect(renderer, &old_clip);
                }

                s32 info_area_x = x_coord + (this->w - max_info_w) / 2;

                SDL_Rect clip_rect = {
                    (int)info_area_x,
                    (int)info_y,
                    (int)max_info_w,
                    (int)info_title_h
                };
                SDL_RenderSetClipRect(renderer, &clip_rect);

                pu::ui::render::TextureRenderOptions opts;
                opts.width = info_title_w;
                opts.height = info_title_h;

                s32 draw_x = 0;
                if (info_title_w <= max_info_w) {
                    draw_x = x_coord + (this->w - info_title_w) / 2;
                } else {
                    draw_x = info_area_x - (s32)info_scroll_x;
                }

                drawer->RenderTexture(info_title_tex, draw_x, info_y, opts);

                if (has_clip) {
                    SDL_RenderSetClipRect(renderer, &old_clip);
                } else {
                    SDL_RenderSetClipRect(renderer, nullptr);
                }
            }
            if (info_sub_tex) {
                s32 sub_w = pu::ui::render::GetTextureWidth(info_sub_tex);
                s32 draw_sub_x = x_coord + (this->w - sub_w) / 2;
                s32 sub_y = is_handheld ? (info_y + 32) : (info_y + 45);
                drawer->RenderTexture(info_sub_tex, draw_sub_x, sub_y);
            }
        }

        // 3. Direction-Aware Cover Prefetching (All Platforms)
        {
            auto& config = romm::model::ConfigManager::Instance();
            auto quality = config.GetCoversQuality();

            int scroll_dir = 0;
            if (scroll_row_offset > last_scroll_row_offset) {
                last_scroll_direction = 1;
            } else if (scroll_row_offset < last_scroll_row_offset) {
                last_scroll_direction = -1;
            }
            scroll_dir = last_scroll_direction;
            last_scroll_row_offset = scroll_row_offset;

            int prefetch_before = 2;
            int prefetch_after = 2;

            if (scroll_dir > 0) {
                prefetch_before = 1;
                prefetch_after = 3;
            } else if (scroll_dir < 0) {
                prefetch_before = 3;
                prefetch_after = 1;
            }

            int cols = current_profile.columns;
            int total_games = filtered_games.size();

            // Prefetch rows before visible range
            for (int r = 1; r <= prefetch_before; ++r) {
                int target_row = scroll_row_offset - r;
                if (target_row < 0) break;

                for (int col = 0; col < cols; ++col) {
                    int game_idx = target_row * cols + col;
                    if (game_idx >= 0 && game_idx < total_games) {
                        PrefetchCover(filtered_games[game_idx], game_idx, quality, cols, selected_game_idx, current_profile.type, platform_slug);
                    }
                }
            }

            // Prefetch rows after visible range
            for (int r = 0; r < prefetch_after; ++r) {
                int target_row = scroll_row_offset + current_profile.visibleRows + r;
                int max_row = (total_games + cols - 1) / cols;
                if (target_row >= max_row) break;

                for (int col = 0; col < cols; ++col) {
                    int game_idx = target_row * cols + col;
                    if (game_idx >= 0 && game_idx < total_games) {
                        PrefetchCover(filtered_games[game_idx], game_idx, quality, cols, selected_game_idx, current_profile.type, platform_slug);
                    }
                }
            }
        }
    }

} // namespace romm::ui
