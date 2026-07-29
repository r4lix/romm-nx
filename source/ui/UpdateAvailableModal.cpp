#include "UpdateAvailableModal.hpp"
#include "../model/UpdateManager.hpp"
#include "../i18n/I18n.hpp"

namespace romm::ui {

    void UpdateAvailableModal::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x_coord, const s32 y_coord) {
        auto nav = nav_mgr.lock();
        if (!nav || !nav->IsUpdateModalActive()) return;

        auto manifest = romm::model::UpdateManager::Instance().GetRemoteManifest();

        // Dim background
        drawer->RenderRectangleFill(pu::ui::Color(0, 0, 0, 180), 0, 0, 1920, 1080);

        s32 modal_w = 700;
        s32 modal_h = 260;
        s32 modal_x = (1920 - modal_w) / 2;
        s32 modal_y = (1080 - modal_h) / 2;

        pu::ui::Color border_color(230, 199, 167, 255); // Cream accent
        pu::ui::Color bg_color(16, 18, 22, 255);         // Web Dark Slate

        drawer->RenderRoundedRectangleFill(border_color, modal_x, modal_y, modal_w, modal_h, 16);
        drawer->RenderRoundedRectangleFill(bg_color, modal_x + 4, modal_y + 4, modal_w - 8, modal_h - 8, 12);

        pu::ui::Color text_color(237, 229, 251, 255);
        pu::ui::Color sub_color(190, 180, 225, 255);

        auto title_tex = pu::ui::render::RenderText("Orbitron@30", romm::i18n::tr("modal.update.title"), text_color);
        if (title_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(title_tex);
            drawer->RenderTexture(title_tex, modal_x + (modal_w - tw) / 2, modal_y + 35);
            pu::ui::render::DeleteTexture(title_tex);
        }

        // The version string comes from the update manifest, so it is a value,
        // not a translatable word.
        std::string sub_text = manifest.version.empty()
            ? romm::i18n::tr("modal.update.body_generic")
            : romm::i18n::format("modal.update.body", {{"version", manifest.version}});
        auto sub_tex = pu::ui::render::RenderText("Ubuntu@20", sub_text, sub_color);
        if (sub_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(sub_tex);
            drawer->RenderTexture(sub_tex, modal_x + (modal_w - tw) / 2, modal_y + 100);
            pu::ui::render::DeleteTexture(sub_tex);
        }

        auto btn_tex = pu::ui::render::RenderText("Orbitron@24", romm::i18n::tr("hint.modal.update"), text_color);
        if (btn_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(btn_tex);
            drawer->RenderTexture(btn_tex, modal_x + (modal_w - tw) / 2, modal_y + modal_h - 60);
            pu::ui::render::DeleteTexture(btn_tex);
        }
    }

}
