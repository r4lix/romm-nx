#include "UninstallConfirmModal.hpp"
#include "../i18n/I18n.hpp"

namespace romm::ui {

    void UninstallConfirmModal::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x, const s32 y) {
        if (auto nav = nav_mgr.lock()) {
            const auto& payload = nav->GetUninstallModalState();
            if (!payload.active) return;

            // Dim background
            drawer->RenderRectangleFill(pu::ui::Color(0, 0, 0, 180), 0, 0, 1920, 1080);

            // Modal container (Centered)
            s32 modal_w = 560;
            s32 modal_h = 240;
            s32 modal_x = (1920 - modal_w) / 2;
            s32 modal_y = (1080 - modal_h) / 2;

            pu::ui::Color border_color(45, 50, 62, 255); // Slate Border Grey
            pu::ui::Color bg_color(16, 18, 22, 255);     // Web Dark Slate

            drawer->RenderRoundedRectangleFill(border_color, modal_x, modal_y, modal_w, modal_h, 16);
            drawer->RenderRoundedRectangleFill(bg_color, modal_x + 4, modal_y + 4, modal_w - 8, modal_h - 8, 12);

            pu::ui::Color text_color(237, 229, 251, 255);
            
            // Title
            pu::sdl2::Texture tex_title = pu::ui::render::RenderText("Orbitron@30", romm::i18n::tr("modal.uninstall.title"), text_color);
            if (tex_title) {
                s32 tw = pu::ui::render::GetTextureWidth(tex_title);
                drawer->RenderTexture(tex_title, modal_x + (modal_w - tw) / 2, modal_y + 30);
                pu::ui::render::DeleteTexture(tex_title);
            }

            // Subtitle: the game's own title (or its filename) — RomM data,
            // shown exactly as stored, never translated.
            pu::sdl2::Texture tex_file = pu::ui::render::RenderText("Ubuntu-Regular@22", payload.title.empty() ? payload.filename : payload.title, pu::ui::Color(180, 180, 180, 255));
            if (tex_file) {
                s32 tw = pu::ui::render::GetTextureWidth(tex_file);
                drawer->RenderTexture(tex_file, modal_x + (modal_w - tw) / 2, modal_y + 85);
                pu::ui::render::DeleteTexture(tex_file);
            }

            // Buttons Line
            pu::sdl2::Texture tex_buttons = pu::ui::render::RenderText("Orbitron@24", romm::i18n::tr("hint.modal.uninstall"), text_color);
            if (tex_buttons) {
                s32 tw = pu::ui::render::GetTextureWidth(tex_buttons);
                drawer->RenderTexture(tex_buttons, modal_x + (modal_w - tw) / 2, modal_y + 160);
                pu::ui::render::DeleteTexture(tex_buttons);
            }
        }
    }

}
