#include "GameGrid.hpp"
#include "CoverCache.hpp"
#include "DetailLayout.hpp"
#include "PlaceholderCover.hpp"
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
        ClearListTextures();
        ClearPanelTextures();
        if (panel_keys_tex) {
            pu::ui::render::DeleteTexture(panel_keys_tex);
            panel_keys_tex = nullptr;
        }
    }

    int GameGrid::GetSelectedRomId() const {
        auto nav = nav_mgr.lock();
        if (!nav) return 0;
        const size_t idx = nav->GetSelectedGameIdx();
        if (idx >= filtered_games.size()) return 0;
        return filtered_games[idx].id;
    }

    void GameGrid::ClearListTextures() {
        for (auto& [idx, tex] : list_row_texs) {
            if (tex) pu::ui::render::DeleteTexture(tex);
        }
        list_row_texs.clear();
        list_cache_generation = 0;
        if (list_header_tex) {
            pu::ui::render::DeleteTexture(list_header_tex);
            list_header_tex = nullptr;
            list_header_str.clear();
        }
    }

    void GameGrid::ClearPanelTextures() {
        if (panel_title_tex) { pu::ui::render::DeleteTexture(panel_title_tex); panel_title_tex = nullptr; }
        if (panel_meta_tex)  { pu::ui::render::DeleteTexture(panel_meta_tex);  panel_meta_tex  = nullptr; }
        if (panel_desc_tex)  { pu::ui::render::DeleteTexture(panel_desc_tex);  panel_desc_tex  = nullptr; }
        if (panel_action_tex) {
            pu::ui::render::DeleteTexture(panel_action_tex);
            panel_action_tex = nullptr;
            panel_action_str.clear();
        }
        panel_cached_game_idx = 999999;
        panel_cached_generation = 0;
        panel_cached_detail_state = romm::model::DetailLoadState::NotLoaded;
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

    // Detail view geometry. The list is deliberately narrower than half the
    // canvas: titles are the only thing in it, while the panel has to hold a
    // cover at a readable size.
    static constexpr s32 DETAIL_ROW_H       = 52;
    static constexpr s32 DETAIL_LIST_W      = 660;
    static constexpr s32 DETAIL_LIST_PAD_X  = 16;
    // Permanent gutter for the multi-select marker. Reserved on every row, not
    // just marked ones, so marking a game doesn't shift its title sideways.
    static constexpr s32 DETAIL_MARK_W      = 36;
    static constexpr s32 DETAIL_PANEL_GAP   = 30;
    static constexpr s32 DETAIL_HEADER_H    = 44;

    void GameGrid::AdjustProfileForHeight(CoverProfile& profile) {
        if (profile.isDetailList) {
            // Rows are a fixed text height here, so how many fit is simply a
            // division — none of the aspect-ratio solving below applies.
            profile.columns = 1;
            profile.coverH = DETAIL_ROW_H;
            profile.coverW = DETAIL_LIST_W;
            profile.gapX = 0;
            profile.gapY = 0;
            profile.offsetX = 0;
            profile.offsetY = 0;
            const s32 usable = this->h - DETAIL_HEADER_H - 20;
            profile.visibleRows = std::max(1, (int)(usable / DETAIL_ROW_H));
            return;
        }

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
        auto nav_for_sel = nav_mgr.lock();
        const size_t sel_count = nav_for_sel ? nav_for_sel->GetBulkSelectionCount() : 0;
        if (game_idx == info_cached_game_idx && total == info_cached_total &&
            sel_count == info_cached_sel_count) {
            return;
        }
        info_cached_sel_count = sel_count;

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
        if (sel_count > 0) {
            sub_text += "  |  " + std::to_string(sel_count) + " selected (ZR to download)";
        }

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
            if (plat_state == romm::model::ApiState::WaitingNetwork) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Waiting for network connection...", text_color);
            } else if (plat_state == romm::model::ApiState::Loading) {
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
            if (roms_state == romm::model::ApiState::WaitingNetwork) {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "Waiting for network connection...", text_color);
            } else if (roms_state == romm::model::ApiState::Idle) {
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
        auto current_view_mode = romm::model::ConfigManager::Instance().GetGridViewMode(current_platform.slug);
        bool view_mode_changed = (current_view_mode != cached_view_mode);

        const std::string& search_query = nav->GetSearchQuery();

        if (plat_idx != cached_selected_platform_idx || letter_idx != cached_selected_letter_idx ||
            view_mode_changed || search_query != cached_search_query) {
            ClearStatusTex();

            if (plat_idx != cached_selected_platform_idx || view_mode_changed) {
                current_profile = GetCoverProfile(current_platform);
                cached_view_mode = current_view_mode;
            }

            filtered_games.clear();
            for (size_t idx : romm::model::FilterGames(current_platform.games, letter_idx, search_query)) {
                filtered_games.push_back(current_platform.games[idx]);
            }

            scroll_row_offset = 0;
            current_visible_start_idx = 999999;
            cached_selected_platform_idx = plat_idx;
            cached_selected_letter_idx = letter_idx;
            cached_search_query = search_query;
            // Invalidates the row/panel text caches, which are keyed by index
            // into a list that just changed underneath them.
            filter_generation++;
        }

        if (filtered_games.empty()) {
            ClearStatusTex();
            ClearInfoTextures();
            // Distinguish "this platform is empty" from "your search excluded
            // everything" — otherwise an over-narrow query looks like a failed
            // load, and the way out isn't obvious.
            if (!search_query.empty()) {
                status_tex = pu::ui::render::RenderText(
                    "Ubuntu@37", "No games match \"" + nav->GetSearchQueryDisplay() + "\"   —   press X to edit", text_color);
            } else {
                status_tex = pu::ui::render::RenderText("Ubuntu@37", "No games found", text_color);
            }
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
        //
        // Unconditional. This used to be gated on an allowlist of six profile
        // types, which silently excluded PSPPortrait, Nintendo3DS and
        // DefaultPortrait — the title texture was never built for them, so no
        // game name appeared while browsing, even though the render path below
        // draws the strip for every platform. A per-platform allowlist here was
        // only ever going to drift as profiles were added.
        RebuildInfoStrip(filtered_games[selected_game_idx], current_platform.name, selected_game_idx, filtered_games.size());

        // 4. Update visible range tracking (mostly for logging/debug now, as covers load on render)
        size_t visible_start_idx = scroll_row_offset * current_profile.columns;
        size_t visible_count = 0;
        if (visible_start_idx < filtered_games.size()) {
            visible_count = std::min(static_cast<size_t>(current_profile.visibleRows * current_profile.columns), filtered_games.size() - visible_start_idx);
        }
        current_visible_start_idx = visible_start_idx;
        current_visible_count = visible_count;
    }

    // Single source of truth for which cover a tile uses, shared by the render
    // loop and the prefetcher so the two can't disagree about what to download.
    //
    // Only HD spends big.png on grid tiles. A tile draws at roughly 180-380px
    // while big.png is 640x640, so in the grid it costs ~6x the bytes and ~6x
    // the texture memory for detail that is scaled away before it reaches the
    // screen — big earns that cost on the detail card and at fullscreen, not
    // here. Balanced and Low therefore browse on small.png and leave big to
    // the screens that actually render it large.
    //
    // Where big is used, it is the primary request rather than an upgrade
    // applied after small lands: fetching both costs a second transfer per game
    // for an image discarded the moment the big one arrives. small is only
    // worth a download of its own when big genuinely isn't there
    // (FailedPermanent — a 404/410 from the server). An already-cached small is
    // still drawn as a stand-in while big is in flight, since that costs
    // nothing but a disk read.
    static pu::sdl2::Texture ResolveCoverTexture(const romm::model::Game& game,
                                                 const std::string& platform_slug,
                                                 CoverProfileType profile_type,
                                                 romm::model::CoversQuality quality) {
        auto& cache = CoverCache::Instance();
        const bool prefer_big = (quality == romm::model::CoversQuality::HD) && !game.cover_path_large.empty();

        if (!prefer_big) {
            if (game.cover_path.empty()) return nullptr;
            return cache.GetOrRequest(game.id, platform_slug, game.cover_path, profile_type, false, true).texture;
        }

        auto big = cache.GetOrRequest(game.id, platform_slug, game.cover_path_large, profile_type, true, true);
        if (big.texture) {
            return big.texture;
        }

        if (game.cover_path.empty()) return nullptr;
        const bool big_unavailable = (big.state == CoverState::FailedPermanent);
        return cache.GetOrRequest(game.id, platform_slug, game.cover_path, profile_type, false, big_unavailable).texture;
    }

    // Greedy word wrap into '\n'-separated lines.
    //
    // Necessary because pu::ui::render::RenderText's max_width argument does
    // NOT wrap — it pops characters off the end and appends "..." until the
    // single line fits. Passing it a paragraph yields one truncated line, which
    // is what the panel description was doing. Font::RenderText does honour
    // '\n' (it calls RenderMultilineText), so wrapping ourselves and passing no
    // max_width gives a real paragraph.
    static std::string WrapText(const std::string& font, const std::string& text, s32 max_w) {
        if (text.empty() || max_w <= 0) return text;

        std::string out;
        std::string line;

        auto flush_line = [&]() {
            out += line;
            out += '\n';
            line.clear();
        };

        size_t pos = 0;
        while (pos <= text.size()) {
            const size_t brk = text.find_first_of(" \n", pos);
            const size_t word_end = (brk == std::string::npos) ? text.size() : brk;
            std::string word = text.substr(pos, word_end - pos);

            if (!word.empty()) {
                // A single token wider than the column (a long filename or URL)
                // can't be wrapped at spaces — break it mid-word rather than
                // letting it run outside the panel.
                while (pu::ui::render::GetTextWidth(font, word) > max_w && word.size() > 1) {
                    size_t fit = word.size();
                    while (fit > 1 && pu::ui::render::GetTextWidth(font, word.substr(0, fit)) > max_w) {
                        --fit;
                    }
                    if (!line.empty()) flush_line();
                    line = word.substr(0, fit);
                    flush_line();
                    word = word.substr(fit);
                }

                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && pu::ui::render::GetTextWidth(font, candidate) > max_w) {
                    flush_line();
                    line = word;
                } else {
                    line = candidate;
                }
            }

            if (brk == std::string::npos) break;
            if (text[brk] == '\n') flush_line();
            pos = brk + 1;
        }

        if (!line.empty()) out += line;
        return out;
    }

    static std::string FormatSize(int64_t bytes) {
        if (bytes <= 0) return "Unknown";
        char buf[64];
        double mb = (double)bytes / (1024.0 * 1024.0);
        if (mb >= 1024.0)     std::sprintf(buf, "%.2f GB", mb / 1024.0);
        else if (mb >= 1.0)   std::sprintf(buf, "%.2f MB", mb);
        else                  std::sprintf(buf, "%.2f KB", (double)bytes / 1024.0);
        return buf;
    }

    void GameGrid::RebuildListTextures(size_t visible_start, size_t visible_count) {
        // The list itself changed identity (platform / letter / search / view
        // mode), so every cached index now refers to a different game.
        if (list_cache_generation != filter_generation) {
            ClearListTextures();
            list_cache_generation = filter_generation;
        }

        // Keep a margin either side of the viewport so reversing direction
        // doesn't immediately re-rasterise what was just dropped.
        constexpr size_t kMargin = 8;
        const size_t keep_lo = visible_start > kMargin ? visible_start - kMargin : 0;
        const size_t keep_hi = visible_start + visible_count + kMargin;

        for (auto it = list_row_texs.begin(); it != list_row_texs.end();) {
            if (it->first < keep_lo || it->first >= keep_hi) {
                if (it->second) pu::ui::render::DeleteTexture(it->second);
                it = list_row_texs.erase(it);
            } else {
                ++it;
            }
        }

        const pu::ui::Color row_color(237, 229, 251, 255);
        for (size_t i = 0; i < visible_count; ++i) {
            const size_t idx = visible_start + i;
            if (idx >= filtered_games.size()) break;
            if (list_row_texs.count(idx)) continue;
            // Wrap width clips overlong titles to the list column instead of
            // letting them run under the panel.
            list_row_texs[idx] =
                pu::ui::render::RenderText("Ubuntu@30", filtered_games[idx].title, row_color,
                                           DETAIL_LIST_W - (DETAIL_LIST_PAD_X * 2) - DETAIL_MARK_W - 40);
        }
    }

    void GameGrid::RebuildPanelTextures(const romm::model::Game& game, size_t game_idx, size_t total, s32 panel_w) {
        auto nav = nav_mgr.lock();
        auto model = nav ? nav->GetModel() : nullptr;
        const auto* detail = model ? model->GetCachedDetail(game.id) : nullptr;
        const auto detail_state = model ? model->GetDetailState(game.id)
                                        : romm::model::DetailLoadState::NotLoaded;

        // The detail fetch is debounced and lands asynchronously, so the panel's
        // contents change without the selection changing. Key the cache on the
        // load state too, or the description would never appear until you moved
        // the cursor away and back.
        if (panel_cached_game_idx == game_idx &&
            panel_cached_generation == filter_generation &&
            panel_cached_detail_state == detail_state &&
            panel_title_tex) {
            return;
        }
        ClearPanelTextures();

        panel_title_tex = pu::ui::render::RenderText(
            "Orbitron@30", WrapText("Orbitron@30", game.title, panel_w),
            pu::ui::Color(237, 229, 251, 255));

        std::string meta;
        if (detail && !detail->developer.empty()) meta += detail->developer + "   |   ";
        meta += FormatSize(detail && detail->file_size_bytes > 0 ? detail->file_size_bytes
                                                                 : game.fs_size_bytes);
        meta += "   |   " + std::to_string(game_idx + 1) + " of " + std::to_string(total);
        panel_meta_tex = pu::ui::render::RenderText(
            "Ubuntu@24", WrapText("Ubuntu@24", meta, panel_w),
            pu::ui::Color(190, 180, 225, 255));

        std::string desc;
        switch (detail_state) {
            case romm::model::DetailLoadState::Loaded:
                desc = (detail && !detail->description.empty()) ? detail->description
                                                                : "No description available.";
                break;
            case romm::model::DetailLoadState::Loading:
                desc = "Loading details...";
                break;
            case romm::model::DetailLoadState::Failed:
                desc = "Couldn't load details.";
                break;
            default:
                desc = "";
                break;
        }
        if (!desc.empty()) {
            panel_desc_tex = pu::ui::render::RenderText(
                "Ubuntu@24", WrapText("Ubuntu@24", desc, panel_w),
                pu::ui::Color(210, 202, 235, 255));
        }

        panel_cached_game_idx = game_idx;
        panel_cached_generation = filter_generation;
        panel_cached_detail_state = detail_state;
    }

    void GameGrid::RenderDetailView(pu::ui::render::Renderer::Ref& drawer, s32 x_coord, s32 y_coord,
                                    const std::string& platform_slug, size_t selected_game_idx, bool grid_focused) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        const s32 list_x = x_coord;
        const s32 panel_x = x_coord + DETAIL_LIST_W + DETAIL_PANEL_GAP;
        const s32 panel_w = this->w - DETAIL_LIST_W - DETAIL_PANEL_GAP - 20;

        // ---- Header: count, and the active search if there is one ----------
        {
            std::string header = std::to_string(filtered_games.size()) + " games";
            const std::string& q = nav->GetSearchQueryDisplay();
            if (!q.empty()) header += "   |   Search: \"" + q + "\"";
            const size_t sel_count = nav->GetBulkSelectionCount();
            if (sel_count > 0) header += "   |   " + std::to_string(sel_count) + " selected (ZR)";

            if (header != list_header_str || !list_header_tex) {
                if (list_header_tex) pu::ui::render::DeleteTexture(list_header_tex);
                list_header_str = header;
                list_header_tex = pu::ui::render::RenderText("Ubuntu@24", header,
                                                             pu::ui::Color(190, 180, 225, 255));
            }
            if (list_header_tex) {
                drawer->RenderTexture(list_header_tex, list_x + DETAIL_LIST_PAD_X, y_coord + 8);
            }
        }

        const s32 rows_y = y_coord + DETAIL_HEADER_H;
        const size_t visible_start = (size_t)scroll_row_offset;
        const size_t visible_count = std::min((size_t)current_profile.visibleRows,
                                              filtered_games.size() - std::min(visible_start, filtered_games.size()));

        RebuildListTextures(visible_start, visible_count);

        // ---- Left: title list ----------------------------------------------
        for (size_t i = 0; i < visible_count; ++i) {
            const size_t idx = visible_start + i;
            if (idx >= filtered_games.size()) break;
            const s32 row_y = rows_y + (s32)i * DETAIL_ROW_H;
            const bool is_selected = (idx == selected_game_idx);

            if (is_selected) {
                const pu::ui::Color sel = grid_focused ? pu::ui::Color(120, 85, 220, 255)
                                                       : pu::ui::Color(120, 85, 220, 110);
                drawer->RenderRoundedRectangleFill(sel, list_x, row_y - 4, DETAIL_LIST_W, DETAIL_ROW_H - 4, 8);
            }

            if (romm::model::ConfigManager::Instance().ShowInstalledBadge() &&
                romm::model::DownloadManager::Instance().GetCachedInstallState(platform_slug, filtered_games[idx].fs_name)) {
                drawer->RenderCircleFill(pu::ui::Color(46, 204, 113, 255),
                                         list_x + DETAIL_LIST_W - 22, row_y + (DETAIL_ROW_H - 4) / 2 - 2, 7);
            }

            // Multi-select marker: an always-visible box in the gutter that
            // fills in when marked. A thin edge stripe was too easy to miss
            // against the selection highlight.
            {
                const s32 box = 24;
                const s32 box_x = list_x + DETAIL_LIST_PAD_X;
                const s32 box_y = row_y + (DETAIL_ROW_H - 4 - box) / 2;
                const bool marked = nav->IsBulkSelected(filtered_games[idx].id);

                drawer->RenderRoundedRectangleFill(pu::ui::Color(70, 76, 92, 255), box_x, box_y, box, box, 5);
                drawer->RenderRoundedRectangleFill(pu::ui::Color(24, 27, 34, 255), box_x + 2, box_y + 2, box - 4, box - 4, 4);
                if (marked) {
                    drawer->RenderRoundedRectangleFill(pu::ui::Color(230, 199, 167, 255),
                                                       box_x + 5, box_y + 5, box - 10, box - 10, 3);
                }
            }

            auto row_it = list_row_texs.find(idx);
            if (row_it != list_row_texs.end() && row_it->second) {
                drawer->RenderTexture(row_it->second, list_x + DETAIL_LIST_PAD_X + DETAIL_MARK_W, row_y + 2);
            }
        }

        if (filtered_games.empty()) return;

        // ---- Right: live panel for the selected row -------------------------
        const size_t sel = std::min(selected_game_idx, filtered_games.size() - 1);
        const auto& game = filtered_games[sel];

        drawer->RenderRoundedRectangleFill(pu::ui::Color(30, 34, 43, 255), panel_x, y_coord, panel_w, this->h - 10, 16);

        // Only the selected cover is ever requested here. That is the whole
        // performance argument for this view mode: a grid page pulls ~30 covers
        // plus prefetch rows, this pulls one.
        const s32 cover_box_w = panel_w - 80;
        const s32 cover_box_h = 420;
        const s32 cover_box_x = panel_x + 40;
        const s32 cover_box_y = y_coord + 30;

        pu::sdl2::Texture cover = ResolveCoverTexture(
            game, platform_slug, current_profile.type,
            romm::model::ConfigManager::Instance().GetCoversQuality());

        // Cover focus ring — the cover is a target in its own right now (A
        // opens it fullscreen), so it needs to show when it holds the cursor.
        if (nav->GetLibraryFocus() == romm::navigation::LibraryFocus::Panel && nav->IsPanelOnCover()) {
            drawer->RenderRoundedRectangleFill(pu::ui::Color(230, 199, 167, 255),
                                               cover_box_x - 4, cover_box_y - 4,
                                               cover_box_w + 8, cover_box_h + 8, 12);
        }
        drawer->RenderRoundedRectangleFill(pu::ui::Color(16, 18, 22, 255), cover_box_x, cover_box_y, cover_box_w, cover_box_h, 10);
        if (cover) {
            const s32 tw = pu::ui::render::GetTextureWidth(cover);
            const s32 th = pu::ui::render::GetTextureHeight(cover);
            const float scale = std::min((float)cover_box_w / tw, (float)cover_box_h / th);
            const s32 dw = (s32)(tw * scale);
            const s32 dh = (s32)(th * scale);
            pu::ui::render::TextureRenderOptions opts;
            opts.width = dw;
            opts.height = dh;
            drawer->RenderTexture(cover, cover_box_x + (cover_box_w - dw) / 2, cover_box_y + (cover_box_h - dh) / 2, opts);
        } else {
            DrawPlaceholderCover(drawer, GetPlaceholderCover(platform_slug), cover_box_x, cover_box_y, cover_box_w, cover_box_h);
        }

        // ---- Cover lookahead -------------------------------------------------
        //
        // Requesting only the selected cover meant every cursor move started
        // from nothing, so slow browsing showed a placeholder for a beat. Warm
        // a couple of neighbours instead — deliberately a handful, not a full
        // grid's worth: this view mode exists to keep the request count low,
        // and the covers are 640x640 PNGs.
        //
        // Ordering matters. The selected cover is requested above, before any
        // of these, so it always claims a download slot first and a lookahead
        // can never delay what's actually on screen.
        {
            if (detail_last_sel_idx != 999999 && sel != detail_last_sel_idx) {
                detail_sel_dir = (sel > detail_last_sel_idx) ? 1 : -1;
            }
            detail_last_sel_idx = sel;

            const int total = (int)filtered_games.size();
            const int cur = (int)sel;
            // Two in the direction of travel, one behind: enough to cover a
            // reversal without doubling the request count.
            const int offsets[] = { detail_sel_dir, detail_sel_dir * 2, -detail_sel_dir };
            for (int off : offsets) {
                const int idx = cur + off;
                if (idx < 0 || idx >= total || idx == cur) continue;
                ResolveCoverTexture(filtered_games[idx], platform_slug, current_profile.type,
                                    romm::model::ConfigManager::Instance().GetCoversQuality());
            }
        }

        // Until the user commits to the panel with A, it shows the cover and
        // nothing else: no detail fetch, no description, no per-frame action
        // state. Browsing the list is then just one cover request per selection.
        const bool panel_focused = (nav->GetLibraryFocus() == romm::navigation::LibraryFocus::Panel);
        if (!panel_focused) {
            // Title under the cover rather than a button legend — the legend is
            // already on the layout's bottom hint bar, and the name is what's
            // actually useful while browsing. Still no detail fetch here: this
            // comes from the ROM list the grid already holds.
            RebuildPanelTextures(game, sel, filtered_games.size(), panel_w - 80);
            if (panel_title_tex) {
                const s32 tw = pu::ui::render::GetTextureWidth(panel_title_tex);
                drawer->RenderTexture(panel_title_tex, panel_x + (panel_w - tw) / 2,
                                      cover_box_y + cover_box_h + 24);
            }
            return;
        }

        RebuildPanelTextures(game, sel, filtered_games.size(), panel_w - 80);

        s32 text_y = cover_box_y + cover_box_h + 20;
        if (panel_title_tex) {
            drawer->RenderTexture(panel_title_tex, panel_x + 40, text_y);
            text_y += pu::ui::render::GetTextureHeight(panel_title_tex) + 10;
        }
        if (panel_meta_tex) {
            drawer->RenderTexture(panel_meta_tex, panel_x + 40, text_y);
            text_y += pu::ui::render::GetTextureHeight(panel_meta_tex) + 12;
        }

        // ---- Action button, pinned to the bottom of the panel ---------------
        const s32 btn_h = 56;
        const s32 btn_y = y_coord + this->h - 10 - btn_h - 20;
        const s32 btn_x = panel_x + 40;
        const s32 btn_w = panel_w - 80;

        auto model = nav->GetModel();
        const auto* detail = model ? model->GetCachedDetail(game.id) : nullptr;
        std::string action_label;
        if (!detail) {
            action_label = "Loading...";
        } else {
            switch (ComputeDownloadActionState(game.id, platform_slug, detail)) {
                case DownloadActionState::Uninstall:   action_label = "Uninstall"; break;
                case DownloadActionState::Queued:      action_label = "Remove from queue"; break;
                case DownloadActionState::Downloading: action_label = "Downloading..."; break;
                case DownloadActionState::Failed:      action_label = "Retry"; break;
                case DownloadActionState::AddToQueue:  action_label = "Add to queue"; break;
                default:                               action_label = "Download"; break;
            }
        }

        if (action_label != panel_action_str || !panel_action_tex) {
            if (panel_action_tex) pu::ui::render::DeleteTexture(panel_action_tex);
            panel_action_str = action_label;
            panel_action_tex = pu::ui::render::RenderText("Orbitron@24", action_label,
                                                          pu::ui::Color(237, 229, 251, 255));
        }

        const bool btn_focused = panel_focused && !nav->IsPanelOnCover();
        const pu::ui::Color btn_bg = btn_focused ? pu::ui::Color(120, 85, 220, 255)
                                                 : pu::ui::Color(45, 50, 62, 255);
        drawer->RenderRoundedRectangleFill(btn_bg, btn_x, btn_y, btn_w, btn_h, 10);
        if (panel_action_tex) {
            const s32 lw = pu::ui::render::GetTextureWidth(panel_action_tex);
            const s32 lh = pu::ui::render::GetTextureHeight(panel_action_tex);
            drawer->RenderTexture(panel_action_tex, btn_x + (btn_w - lw) / 2, btn_y + (btn_h - lh) / 2);
        }

        if (panel_keys_tex == nullptr) {
            panel_keys_tex = pu::ui::render::RenderText("Ubuntu@20",
                                                        "A  Open cover / Action      Up/Down  Move      B  Back",
                                                        pu::ui::Color(140, 130, 175, 255));
        }
        if (panel_keys_tex) {
            const s32 kw = pu::ui::render::GetTextureWidth(panel_keys_tex);
            drawer->RenderTexture(panel_keys_tex, panel_x + (panel_w - kw) / 2, btn_y + btn_h + 6);
        }

        // Description fills whatever vertical space is left between the metadata
        // and the button, clipped rather than allowed to run over either.
        if (panel_desc_tex) {
            const s32 avail_h = btn_y - 16 - text_y;
            if (avail_h > 20) {
                const s32 desc_h = pu::ui::render::GetTextureHeight(panel_desc_tex);
                // Clamp the scroll to what actually overflows, so Down can't
                // push the text off the top of its own window.
                const s32 max_scroll = (desc_h > avail_h) ? (desc_h - avail_h) : 0;
                nav->SetPanelDescScrollMax(max_scroll);
                const s32 scroll = std::min(nav->GetPanelDescScroll(), max_scroll);

                SDL_Renderer* r = pu::ui::render::GetMainRenderer();
                SDL_Rect old_clip;
                const SDL_bool had_clip = SDL_RenderIsClipEnabled(r);
                if (had_clip) SDL_RenderGetClipRect(r, &old_clip);
                SDL_Rect clip = { (int)(panel_x + 40), (int)text_y, (int)(panel_w - 80), (int)avail_h };
                SDL_RenderSetClipRect(r, &clip);

                drawer->RenderTexture(panel_desc_tex, panel_x + 40, text_y - scroll);

                if (had_clip) SDL_RenderSetClipRect(r, &old_clip);
                else          SDL_RenderSetClipRect(r, nullptr);

                // Overflow indicator — otherwise there's no cue that Up/Down do
                // anything here.
                if (max_scroll > 0) {
                    const s32 track_x = panel_x + panel_w - 46;
                    drawer->RenderRoundedRectangleFill(pu::ui::Color(45, 50, 62, 160), track_x, text_y, 5, avail_h, 2);
                    const s32 thumb_h = std::max(24, (avail_h * avail_h) / desc_h);
                    const s32 thumb_y = text_y + ((avail_h - thumb_h) * scroll) / max_scroll;
                    drawer->RenderRoundedRectangleFill(pu::ui::Color(120, 85, 220, 255), track_x, thumb_y, 5, thumb_h, 2);
                }
            }
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

        if (current_profile.isDetailList) {
            RenderDetailView(drawer, x_coord, y_coord, platform_slug, selected_game_idx, grid_focused);
            return;
        }

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

            pu::sdl2::Texture tex = ResolveCoverTexture(
                game, platform_slug, current_profile.type,
                romm::model::ConfigManager::Instance().GetCoversQuality());

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
                DrawPlaceholderCover(drawer, GetPlaceholderCover(platform_slug), actual_x, actual_y, actual_w, actual_h);
            }

            if (romm::model::ConfigManager::Instance().ShowInstalledBadge()) {
                const auto& platforms = nav->GetModel()->GetPlatforms();
                size_t plat_idx = nav->GetLoadedPlatformIdx();
                if (plat_idx < platforms.size()) {
                    const auto& current_platform = platforms.at(plat_idx);
                    // Cached variant: IsGameInstalled() is a raw stat() syscall,
                    // and this runs per tile per frame — ~1800 SD-card stats a
                    // second on a full grid, whether or not anything is
                    // downloading. The cache is cleared by
                    // InvalidateInstallCache() on download completion/cancel and
                    // refreshed on uninstall, so badges stay correct.
                    if (romm::model::DownloadManager::Instance().GetCachedInstallState(current_platform.slug, game.fs_name)) {
                        s32 dot_radius = 12;
                        s32 dot_x = actual_x + actual_w - dot_radius - 8;
                        s32 dot_y = actual_y + dot_radius + 8;
                        drawer->RenderCircleFill(pu::ui::Color(46, 204, 113, 255), dot_x, dot_y, dot_radius);
                    }
                }
            }

            // Bulk-selection marker, mirrored to the opposite corner from the
            // install badge so both can show at once.
            if (nav->IsBulkSelected(game.id)) {
                const s32 mark_r = 12;
                const s32 mark_x = actual_x + mark_r + 8;
                const s32 mark_y = actual_y + mark_r + 8;
                drawer->RenderCircleFill(pu::ui::Color(20, 22, 28, 220), mark_x, mark_y, mark_r + 3);
                drawer->RenderCircleFill(pu::ui::Color(230, 199, 167, 255), mark_x, mark_y, mark_r);
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
                        ResolveCoverTexture(filtered_games[game_idx], platform_slug, current_profile.type, quality);
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
                        ResolveCoverTexture(filtered_games[game_idx], platform_slug, current_profile.type, quality);
                    }
                }
            }
        }
    }

} // namespace romm::ui
