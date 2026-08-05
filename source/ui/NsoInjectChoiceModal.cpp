#include "NsoInjectChoiceModal.hpp"
#include "../i18n/I18n.hpp"

namespace romm::ui {

    namespace {
        void DrawCentered(pu::ui::render::Renderer::Ref& drawer, const char* font, const std::string& text,
                          s32 modal_x, s32 modal_w, s32 y, pu::ui::Color color) {
            if (text.empty()) return;
            auto tex = pu::ui::render::RenderText(font, text, color);
            if (!tex) return;
            const s32 tw = pu::ui::render::GetTextureWidth(tex);
            drawer->RenderTexture(tex, modal_x + (modal_w - tw) / 2, y);
            pu::ui::render::DeleteTexture(tex);
        }
    }

    void NsoInjectChoiceModal::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32, const s32) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        const auto& payload = nav->GetNsoInjectModalState();
        if (!payload.active) return;

        drawer->RenderRectangleFill(pu::ui::Color(0, 0, 0, 190), 0, 0, 1920, 1080);

        const s32 modal_w = 860;
        // Taller than the four-row version it replaced: the toggle is set apart
        // from the actions, and the N64 caution below wraps to two lines.
        const s32 modal_h = 560;
        const s32 modal_x = (1920 - modal_w) / 2;
        const s32 modal_y = (1080 - modal_h) / 2;

        const pu::ui::Color border_color(45, 50, 62, 255);  // Slate Border Grey
        const pu::ui::Color bg_color(16, 18, 22, 255);      // Web Dark Slate
        const pu::ui::Color text_color(237, 229, 251, 255);
        const pu::ui::Color dim_color(150, 150, 162, 255);
        const pu::ui::Color accent_color(190, 180, 225, 255);
        const pu::ui::Color warn_color(240, 190, 90, 255);
        const pu::ui::Color selection_color(85, 63, 152, 255);

        drawer->RenderRoundedRectangleFill(border_color, modal_x, modal_y, modal_w, modal_h, 16);
        drawer->RenderRoundedRectangleFill(bg_color, modal_x + 4, modal_y + 4, modal_w - 8, modal_h - 8, 12);

        DrawCentered(drawer, "Orbitron@30", romm::i18n::tr("modal.nso_inject.title"),
                     modal_x, modal_w, modal_y + 30, text_color);
        // The game's own title — RomM data, shown exactly as stored.
        DrawCentered(drawer, "Ubuntu@22", payload.title, modal_x, modal_w, modal_y + 78, dim_color);

        // Rows, in NsoInjectChoiceRow order: three actions, then the toggle.
        using Row = romm::navigation::NsoInjectChoiceRow;
        const std::string rows[(size_t)Row::Count] = {
            romm::i18n::tr("modal.nso_inject.inject_only"),
            romm::i18n::tr("modal.nso_inject.rom_only"),
            romm::i18n::tr("modal.nso_inject.both"),
            romm::i18n::format("modal.nso_inject.remember_toggle", {{"platform", payload.platform_name}})
        };

        const s32 row_h = 54;
        const s32 row_spacing = 8;
        const s32 row_x = modal_x + 40;
        const s32 row_w = modal_w - 80;
        s32 row_y = modal_y + 126;

        for (size_t i = 0; i < (size_t)Row::Count; ++i) {
            const bool is_selected = (i == payload.selected_row);
            const bool is_toggle = ((Row)i == Row::Remember);

            // The toggle sits apart from the three actions: it changes what the
            // actions mean rather than being one of them.
            if (is_toggle) row_y += 14;

            if (is_selected) {
                drawer->RenderRoundedRectangleFill(pu::ui::Color(230, 199, 167, 255), row_x, row_y, row_w, row_h, 8);
                drawer->RenderRoundedRectangleFill(selection_color, row_x + 3, row_y + 3, row_w - 6, row_h - 6, 6);
            } else {
                drawer->RenderRoundedRectangleFill(border_color, row_x, row_y, row_w, row_h, 8);
                drawer->RenderRoundedRectangleFill(bg_color, row_x + 2, row_y + 2, row_w - 4, row_h - 4, 6);
            }

            s32 label_x = row_x + 24;
            if (is_toggle) {
                // A real checkbox rather than the label changing between
                // "Remember" and "Forget": the row has to read as a state, not
                // as another action.
                const s32 box = 24;
                const s32 box_x = row_x + 20;
                const s32 box_y = row_y + (row_h - box) / 2;
                drawer->RenderRoundedRectangleFill(accent_color, box_x, box_y, box, box, 5);
                drawer->RenderRoundedRectangleFill(is_selected ? selection_color : bg_color,
                                                   box_x + 2, box_y + 2, box - 4, box - 4, 4);
                if (payload.remember) {
                    drawer->RenderRoundedRectangleFill(accent_color, box_x + 6, box_y + 6, box - 12, box - 12, 3);
                }
                label_x = box_x + box + 16;
            }

            auto tex = pu::ui::render::RenderText("Ubuntu@22", rows[i], is_selected ? text_color : accent_color);
            if (tex) {
                const s32 th = pu::ui::render::GetTextureHeight(tex);
                drawer->RenderTexture(tex, label_x, row_y + (row_h - th) / 2);
                pu::ui::render::DeleteTexture(tex);
            }
            row_y += row_h + row_spacing;
        }

        row_y += 12;

        // Stated, never a refusal — both cautions are things the user can act
        // on afterwards: the Full Unlock can live inside a custom NSP romm-nx
        // cannot see, and a MetaPack can be added to the title later.
        using Caution = romm::navigation::NsoInjectModalPayload::Caution;
        if (payload.caution != Caution::None) {
            const char* caution_key = (payload.caution == Caution::NeedsMetaPack)
                                          ? "modal.nso_inject.warning_metapack"
                                          : "modal.nso_inject.warning";
            auto warn = pu::ui::render::RenderText("Ubuntu@18", romm::i18n::tr(caution_key),
                                                   warn_color, row_w);
            if (warn) {
                drawer->RenderTexture(warn, row_x, row_y);
                pu::ui::render::DeleteTexture(warn);
            }
        }

        DrawCentered(drawer, "Orbitron@22", romm::i18n::tr("hint.modal.nso_inject"),
                     modal_x, modal_w, modal_y + modal_h - 56, text_color);
    }

}
