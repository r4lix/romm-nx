#include "SidebarList.hpp"
#include "../i18n/I18n.hpp"

namespace romm::ui {

    SidebarList::SidebarList(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav), item_height(60), padding_top(30), padding_left(25) {
        InitTextures();
    }

    SidebarList::~SidebarList() {
        ClearTextures();
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
        auto push_row = [&](const std::string& text) {
            selected_texs.push_back(pu::ui::render::RenderText(font_name, text, selected_clr, w - 50));
            unselected_texs.push_back(pu::ui::render::RenderText(font_name, text, unselected_clr, w - 50));
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
                                            : "status.all_platforms_hidden_short"));
            } else {
                for (const auto& plat : plats) {
                    // RomM data: rendered exactly as the server reported it.
                    push_row(plat.name);
                }
            }
            return;
        }

        const char* status_key = "status.initializing"; // Idle or default
        if (state == romm::model::ApiState::WaitingNetwork) status_key = "status.waiting_network_short";
        else if (state == romm::model::ApiState::Loading) status_key = "status.loading_platforms";
        else if (state == romm::model::ApiState::FailedConnect) status_key = "status.failed_connect";
        else if (state == romm::model::ApiState::Unauthorized) status_key = "status.unauthorized";
        push_row(romm::i18n::tr(status_key));
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

        bool sidebar_focused = (nav->GetLibraryFocus() == romm::navigation::LibraryFocus::Sidebar);

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

}
