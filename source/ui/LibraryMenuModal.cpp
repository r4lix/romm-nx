#include "LibraryMenuModal.hpp"
#include "../model/ConfigManager.hpp"

namespace romm::ui {

    namespace {
        constexpr s32 PANEL_MARGIN_TOP = 40;
        constexpr s32 PANEL_MARGIN_BOTTOM = 40;
        constexpr s32 PANEL_W = 560;
        constexpr s32 ROW_H = 90;
    }

    void LibraryMenuModal::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x_coord, const s32 y_coord) {
        auto nav = nav_mgr.lock();
        if (!nav || !nav->IsLibraryMenuActive()) return;

        // Dim background
        drawer->RenderRectangleFill(pu::ui::Color(0, 0, 0, 180), 0, 0, 1920, 1080);

        s32 panel_x = 1920 - PANEL_W - 60;
        s32 panel_y = PANEL_MARGIN_TOP;
        s32 panel_h = 1080 - PANEL_MARGIN_TOP - PANEL_MARGIN_BOTTOM;

        pu::ui::Color border_color(45, 50, 62, 255); // Slate Border Grey
        pu::ui::Color bg_color(16, 18, 22, 255);     // Web Dark Slate
        pu::ui::Color text_color(237, 229, 251, 255);
        pu::ui::Color muted_color(190, 180, 225, 150);
        pu::ui::Color cream(230, 199, 167, 255);

        drawer->RenderRoundedRectangleFill(border_color, panel_x, panel_y, PANEL_W, panel_h, 16);
        drawer->RenderRoundedRectangleFill(bg_color, panel_x + 4, panel_y + 4, PANEL_W - 8, panel_h - 8, 12);

        // Title
        pu::sdl2::Texture tex_title = pu::ui::render::RenderText("Orbitron@30", "MENU", cream);
        if (tex_title) {
            drawer->RenderTexture(tex_title, panel_x + 40, panel_y + 30);
            pu::ui::render::DeleteTexture(tex_title);
        }

        auto& config = romm::model::ConfigManager::Instance();
        std::string platform_slug;
        if (auto model = nav->GetModel()) {
            const auto& platforms = model->GetPlatforms();
            size_t plat_idx = nav->GetLoadedPlatformIdx();
            if (plat_idx < platforms.size()) {
                platform_slug = platforms[plat_idx].slug;
            }
        }
        std::string view_mode_label = "View Mode: " + config.GetGridViewModeString(platform_slug);

        struct Row { std::string main; std::string note; };
        Row rows[LibraryMenuModal::GetRowCount()] = {
            { "Search", "Coming Soon" },
            { "Sort", "Coming Soon" },
            { view_mode_label, "" }
        };

        size_t selected = nav->GetLibraryMenuSelectedIdx();
        s32 list_y = panel_y + 100;

        for (size_t i = 0; i < LibraryMenuModal::GetRowCount(); ++i) {
            s32 row_y = list_y + (s32)i * ROW_H;
            bool is_selected = (i == selected);

            if (is_selected) {
                pu::ui::Color sel_bg(85, 63, 152, 255); // Violet accent
                drawer->RenderRoundedRectangleFill(sel_bg, panel_x + 24, row_y, PANEL_W - 48, ROW_H - 12, 8);
            }

            pu::sdl2::Texture tex_main = pu::ui::render::RenderText("Orbitron@24", rows[i].main, text_color);
            if (tex_main) {
                s32 th = pu::ui::render::GetTextureHeight(tex_main);
                drawer->RenderTexture(tex_main, panel_x + 50, row_y + (ROW_H - 12 - th) / 2);
                pu::ui::render::DeleteTexture(tex_main);
            }

            if (!rows[i].note.empty()) {
                pu::sdl2::Texture tex_note = pu::ui::render::RenderText("Ubuntu@20", rows[i].note, muted_color);
                if (tex_note) {
                    s32 tw = pu::ui::render::GetTextureWidth(tex_note);
                    s32 th = pu::ui::render::GetTextureHeight(tex_note);
                    drawer->RenderTexture(tex_note, panel_x + PANEL_W - 50 - tw, row_y + (ROW_H - 12 - th) / 2);
                    pu::ui::render::DeleteTexture(tex_note);
                }
            }
        }

        std::string hint = (selected == 2) ? "A Cycle Mode   |   B Close" : "A Select   |   B Close";
        pu::sdl2::Texture tex_hints = pu::ui::render::RenderText("Ubuntu@20", hint, muted_color);
        if (tex_hints) {
            drawer->RenderTexture(tex_hints, panel_x + 40, panel_y + panel_h - 50);
            pu::ui::render::DeleteTexture(tex_hints);
        }
    }

}
