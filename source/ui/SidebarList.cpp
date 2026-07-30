#include "SidebarList.hpp"
#include "../i18n/I18n.hpp"
#include "../model/PlatformCatalog.hpp"

#include <algorithm>

namespace romm::ui {

    namespace {
        // Which slot the selected platform is pinned to in banner mode.
        //
        // 0 puts it flush at the top; 1 keeps the previous platform visible
        // above it. Whatever the value, the highlight never moves — the list
        // slides underneath it — so this is the single knob for the whole
        // pinning behaviour.
        constexpr s32 kSelectedSlot = 1;
    }

    SidebarList::SidebarList(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav), item_height(60), padding_top(30), padding_left(25) {
        cached_selector_style = romm::model::ConfigManager::Instance().GetPlatformSelectorStyle();
        InitTextures();
    }

    SidebarList::~SidebarList() {
        ClearTextures();
        banner_cache.Clear();
    }

    void SidebarList::Refresh() {
        auto nav = nav_mgr.lock();
        if (!nav) return;
        auto model = nav->GetModel();
        if (!model) return;

        auto state = model->GetPlatformState();
        size_t count = model->GetPlatforms().size();
        uint64_t generation = model->GetPlatformsGeneration();
        uint64_t i18n_generation = romm::i18n::Generation();

        if (state != cached_state || count != cached_platform_count ||
            generation != cached_platforms_generation ||
            i18n_generation != cached_i18n_generation) {
            cached_state = state;
            cached_platform_count = count;
            cached_platforms_generation = generation;
            cached_i18n_generation = i18n_generation;
            InitTextures();
        }
    }

    void SidebarList::RefreshTranslations() {
        // Only the status cards are localized — platform rows are RomM names.
        // Refresh() re-checks the i18n generation, so this just forces the pass.
        cached_i18n_generation = 0;
        Refresh();
    }

    void SidebarList::InitTextures() {
        ClearTextures();
        auto nav = nav_mgr.lock();
        if (!nav) return;

        auto model = nav->GetModel();
        if (!model) return;

        std::string font_name = "Ubuntu@30";
        pu::ui::Color selected_clr(237, 229, 251, 255); // Very light text (#EDE5FB)
        pu::ui::Color unselected_clr(190, 180, 225, 255); // Light lavender (#BEB4E1)

        // Every non-Success state draws a single status card; only the message
        // differs, so pick the key first and render once.
        //
        // The text rows are built in banner mode too: they are what a platform
        // with no banner art falls back to, so they must exist per row either
        // way. `id` is empty for the status cards, which have no platform.
        auto push_row = [&](const std::string& text, const std::string& id) {
            selected_texs.push_back(pu::ui::render::RenderText(font_name, text, selected_clr, w - 50));
            unselected_texs.push_back(pu::ui::render::RenderText(font_name, text, unselected_clr, w - 50));
            platform_ids.push_back(id);
        };

        auto state = model->GetPlatformState();
        if (state == romm::model::ApiState::Success) {
            const auto& plats = model->GetPlatforms();
            if (plats.empty()) {
                // The server may well have returned platforms — they're just
                // all hidden. Say which of the two it is, so the fix ("go turn
                // one back on") is obvious instead of looking like a fetch
                // failure.
                push_row(romm::i18n::tr(model->GetAllPlatforms().empty()
                                            ? "status.no_platforms"
                                            : "status.all_platforms_hidden_short"),
                         "");
            } else {
                for (const auto& plat : plats) {
                    // RomM data: rendered exactly as the server reported it.
                    // The banner key, by contrast, is the canonical id — the
                    // displayed name is localized and instance-specific and
                    // must never address an asset.
                    push_row(plat.name, romm::model::ResolvePlatformIdentity(plat.slug, plat.name));
                }
                rows_are_platforms = true;
            }
            return;
        }

        const char* status_key = "status.initializing"; // Idle or default
        if (state == romm::model::ApiState::WaitingNetwork) status_key = "status.waiting_network_short";
        else if (state == romm::model::ApiState::Loading) status_key = "status.loading_platforms";
        else if (state == romm::model::ApiState::FailedConnect) status_key = "status.failed_connect";
        else if (state == romm::model::ApiState::Unauthorized) status_key = "status.unauthorized";
        push_row(romm::i18n::tr(status_key), "");
    }

    void SidebarList::ClearTextures() {
        for (auto& tex : selected_texs) {
            pu::ui::render::DeleteTexture(tex);
        }
        selected_texs.clear();

        for (auto& tex : unselected_texs) {
            pu::ui::render::DeleteTexture(tex);
        }
        unselected_texs.clear();

        platform_ids.clear();
        rows_are_platforms = false;
    }

    const BannerLayout& SidebarList::EnsureBannerLayout() {
        if (layout_w != w || layout_h != h) {
            banner_layout = ComputeBannerLayout(w, h);
            layout_w = w;
            layout_h = h;
            std::cout << "[BANNER] Sidebar " << w << "x" << h << " fits "
                      << banner_layout.slot_count << " banner(s) at "
                      << banner_layout.banner_w << "x" << banner_layout.banner_h << std::endl;
        }
        return banner_layout;
    }

    void SidebarList::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        auto model = nav->GetModel();
        if (!model) return;

        // Draw sidebar panel background (#1E222B)
        pu::ui::Color bg_color(30, 34, 43, 255);
        drawer->RenderRectangleFill(bg_color, x_coord, y_coord, w, h);

        // Draw slate vertical separator line (#2D323E)
        pu::ui::Color divider_color(45, 50, 62, 255);
        drawer->RenderRectangleFill(divider_color, x_coord + w - 3, y_coord, 3, h);

        size_t selected_idx = nav->GetSelectedPlatformIdx();
        auto state = model->GetPlatformState();
        if (state != romm::model::ApiState::Success) {
            selected_idx = 0; // Lock highlight to the status card
        }

        const bool sidebar_focused = (nav->GetLibraryFocus() == romm::navigation::LibraryFocus::Sidebar);

        // Settings > Theme can flip this while the library layout stays alive,
        // so it is read here rather than cached at construction. Leaving banner
        // mode drops the textures instead of keeping them resident for a mode
        // that will never draw them.
        const auto style = romm::model::ConfigManager::Instance().GetPlatformSelectorStyle();
        if (style != cached_selector_style) {
            cached_selector_style = style;
            if (style != romm::model::PlatformSelectorStyle::Banners) {
                banner_cache.Clear();
            }
        }

        // Banner mode only applies to a real, loaded platform list: the status
        // cards ("Loading platforms...") are messages, not platforms, and are
        // always drawn as text. A sidebar too small to hold one legible banner
        // falls back the same way rather than drawing something unreadable.
        if (style == romm::model::PlatformSelectorStyle::Banners &&
            state == romm::model::ApiState::Success &&
            rows_are_platforms && EnsureBannerLayout().viable) {
            RenderBanners(drawer, x_coord, y_coord, selected_idx, sidebar_focused);
            return;
        }

        RenderTextRows(drawer, x_coord, y_coord, selected_idx, sidebar_focused);
    }

    void SidebarList::RenderTextRows(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord,
                                     const size_t selected_idx, const bool sidebar_focused) {
        // Adjust scroll offset
        if ((int)selected_idx < sidebar_scroll_offset) {
            sidebar_scroll_offset = (int)selected_idx;
        } else if ((int)selected_idx >= sidebar_scroll_offset + visible_platform_count) {
            sidebar_scroll_offset = (int)selected_idx - visible_platform_count + 1;
        }

        int total_platforms = (int)selected_texs.size();
        if (sidebar_scroll_offset > total_platforms - visible_platform_count) {
            sidebar_scroll_offset = total_platforms - visible_platform_count;
        }
        if (sidebar_scroll_offset < 0) {
            sidebar_scroll_offset = 0;
        }

        int visible_count = std::min(visible_platform_count, total_platforms - sidebar_scroll_offset);
        if (visible_count < 0) visible_count = 0;

        // Log scroll metrics with throttling (only if offset or count changes)
        static int last_logged_offset = -1;
        static int last_logged_count = -1;
        if (sidebar_scroll_offset != last_logged_offset || visible_count != last_logged_count) {
            last_logged_offset = sidebar_scroll_offset;
            last_logged_count = visible_count;
            std::cout << "[PERF] Sidebar visible items rendered: " << visible_count << std::endl;
            std::cout << "[PERF] Sidebar scrollOffset: " << sidebar_scroll_offset << std::endl;
        }

        for (int i = 0; i < visible_count; ++i) {
            int actual_idx = sidebar_scroll_offset + i;
            if (actual_idx >= total_platforms) break;

            s32 item_y = y_coord + padding_top + (i * item_height);

            if ((size_t)actual_idx == selected_idx) {
                // Violet highlight capsule (#553F98)
                pu::ui::Color highlight_color = sidebar_focused ? pu::ui::Color(85, 63, 152, 200) : pu::ui::Color(85, 63, 152, 80);
                drawer->RenderRoundedRectangleFill(highlight_color, x_coord + 15, item_y + 4, w - 30, item_height - 8, 8);

                // Draw a small cream accent tab if focused (#E6C7A7)
                if (sidebar_focused) {
                    drawer->RenderRectangleFill(pu::ui::Color(230, 199, 167, 255), x_coord + 15, item_y + 10, 4, item_height - 20);
                }

                // Draw text texture
                if ((size_t)actual_idx < selected_texs.size()) {
                    drawer->RenderTexture(selected_texs.at(actual_idx), x_coord + padding_left + 10, item_y + 8);
                }
            } else {
                // Draw unselected text texture
                if ((size_t)actual_idx < unselected_texs.size()) {
                    drawer->RenderTexture(unselected_texs.at(actual_idx), x_coord + padding_left, item_y + 8);
                }
            }
        }
    }

    void SidebarList::RenderBanners(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord,
                                    const size_t selected_idx, const bool sidebar_focused) {
        const int total_platforms = (int)platform_ids.size();

        // The selection is pinned to kSelectedSlot and the list slides
        // underneath it, so there is no scroll offset to track — the window is
        // always anchored on the selected platform.
        //
        // Clamped at the start of the list: there is no platform above the
        // first one, and holding the pin there would open a gap at the top
        // rather than show anything. So the first platform sits in slot 0 and
        // the pin takes effect from the second onwards — the one place the
        // highlight moves, and only because the alternative is empty space.
        //
        // Not clamped at the end: trailing slots simply run out. Sliding the
        // window up to fill them would drag the highlight down the sidebar,
        // which is exactly what the pinning exists to prevent.
        int window_start = (int)selected_idx - kSelectedSlot;
        if (window_start < 0) window_start = 0;

        for (s32 slot = 0; slot < banner_layout.slot_count; ++slot) {
            const int idx = window_start + slot;
            if (idx >= total_platforms) break;

            const s32 bx = x_coord + banner_layout.origin_x;
            const s32 by = y_coord + banner_layout.SlotY(slot);
            const s32 bw = banner_layout.banner_w;
            const s32 bh = banner_layout.banner_h;
            const bool is_selected = (idx == (int)selected_idx);

            // Focus ring, matching the grid's cover framing: bright violet when
            // the sidebar has focus, dimmed while focus sits in the grid, plain
            // slate for the rest.
            pu::ui::Color frame_color(45, 50, 62, 255); // Slate Border Grey (#2D323E)
            s32 frame_width = 2;
            if (is_selected) {
                frame_color = sidebar_focused ? pu::ui::Color(120, 85, 220, 255)
                                              : pu::ui::Color(120, 85, 220, 120);
                frame_width = sidebar_focused ? 6 : 4;
            }
            drawer->RenderRoundedRectangleFill(frame_color, bx - frame_width, by - frame_width,
                                               bw + (frame_width * 2), bh + (frame_width * 2), 10);

            // Backing plate: banner art is letterboxed inside the slot when its
            // ratio isn't exactly 16:9, and this is what shows through.
            drawer->RenderRoundedRectangleFill(pu::ui::Color(30, 34, 43, 255), bx, by, bw, bh, 6);

            // Unselected banners are dimmed enough to read the selection at a
            // glance, but not so far that the art stops being identifiable.
            const s32 alpha = is_selected ? 255 : 150;

            pu::sdl2::Texture banner = banner_cache.Get(platform_ids.at((size_t)idx));
            if (banner) {
                const s32 tw = pu::ui::render::GetTextureWidth(banner);
                const s32 th = pu::ui::render::GetTextureHeight(banner);
                if (tw > 0 && th > 0) {
                    // Contain-fit rather than a straight stretch to the slot.
                    // The bundled art is already 16:9 so this is normally an
                    // exact fit; it's what keeps a hand-replaced asset of some
                    // other ratio letterboxed instead of squashed.
                    const float scale = std::min((float)bw / (float)tw, (float)bh / (float)th);
                    const s32 draw_w = (s32)((float)tw * scale);
                    const s32 draw_h = (s32)((float)th * scale);

                    pu::ui::render::TextureRenderOptions opts;
                    opts.width = draw_w;
                    opts.height = draw_h;
                    opts.alpha_mod = alpha;
                    drawer->RenderTexture(banner, bx + ((bw - draw_w) / 2), by + ((bh - draw_h) / 2), opts);
                    continue;
                }
            }

            // No banner for this platform (or it failed to decode): draw the
            // platform's own text row, centred in the slot it would have
            // occupied. Only this one slot degrades — every other platform
            // keeps its art.
            const auto& texs = is_selected ? selected_texs : unselected_texs;
            if ((size_t)idx < texs.size() && texs.at((size_t)idx)) {
                pu::sdl2::Texture label = texs.at((size_t)idx);
                const s32 tw = pu::ui::render::GetTextureWidth(label);
                const s32 th = pu::ui::render::GetTextureHeight(label);
                pu::ui::render::TextureRenderOptions opts;
                opts.alpha_mod = alpha;
                drawer->RenderTexture(label, bx + ((bw - tw) / 2), by + ((bh - th) / 2), opts);
            }
        }
    }

}
