#include "AlphabetBar.hpp"

namespace romm::ui {

    AlphabetBar::AlphabetBar(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav) {
        
        letters.push_back("ALL");
        for (char c = 'A'; c <= 'Z'; ++c) {
            std::string s(1, c);
            letters.push_back(s);
        }

        InitTextures();
    }

    AlphabetBar::~AlphabetBar() {
        ClearTextures();
    }

    void AlphabetBar::InitTextures() {
        ClearTextures();

        std::string font_name = "Ubuntu@30";
        pu::ui::Color selected_clr(237, 229, 251, 255); // Very light text (#EDE5FB)
        pu::ui::Color unselected_clr(190, 180, 225, 255); // Light lavender (#BEB4E1)

        for (const auto& letter : letters) {
            pu::sdl2::Texture sel_tex = pu::ui::render::RenderText(font_name, letter, selected_clr);
            pu::sdl2::Texture unsel_tex = pu::ui::render::RenderText(font_name, letter, unselected_clr);
            selected_texs.push_back(sel_tex);
            unselected_texs.push_back(unsel_tex);
        }
    }

    void AlphabetBar::ClearTextures() {
        for (auto& tex : selected_texs) {
            pu::ui::render::DeleteTexture(tex);
        }
        selected_texs.clear();

        for (auto& tex : unselected_texs) {
            pu::ui::render::DeleteTexture(tex);
        }
        unselected_texs.clear();
    }

    void AlphabetBar::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        auto nav = nav_mgr.lock();
        if (!nav || !nav->ShowAlphabetFilter()) return;

        // Draw background panel (#1E222B)
        pu::ui::Color panel_color(30, 34, 43, 255);
        drawer->RenderRoundedRectangleFill(panel_color, x_coord, y_coord, w, h, 8);

        size_t selected_letter_idx = nav->GetSelectedLetterIdx();
        bool is_focused = (nav->GetLibraryFocus() == romm::navigation::LibraryFocus::Alphabet);

        float item_width_fp = (float)w / letters.size();

        for (size_t i = 0; i < letters.size(); ++i) {
            s32 item_x = x_coord + (s32)(i * item_width_fp);
            s32 item_w_val = (s32)item_width_fp;

            bool is_selected = (i == selected_letter_idx);

            if (is_selected) {
                // Violet highlight capsule
                pu::ui::Color capsule_color = is_focused ? pu::ui::Color(85, 63, 152, 200) : pu::ui::Color(85, 63, 152, 80); // #553F98
                
                if (is_focused) {
                    pu::ui::Color outline_color(230, 199, 167, 255); // #E6C7A7
                    // Draw outer border and inner capsule
                    drawer->RenderRoundedRectangleFill(outline_color, item_x + 2, y_coord + 4, item_w_val - 4, h - 8, 6);
                    drawer->RenderRoundedRectangleFill(capsule_color, item_x + 4, y_coord + 6, item_w_val - 8, h - 12, 4);
                } else {
                    drawer->RenderRoundedRectangleFill(capsule_color, item_x + 2, y_coord + 4, item_w_val - 4, h - 8, 6);
                }
            }

            pu::sdl2::Texture text_tex = is_selected ? selected_texs.at(i) : unselected_texs.at(i);
            s32 text_w = pu::ui::render::GetTextureWidth(text_tex);
            s32 text_h = pu::ui::render::GetTextureHeight(text_tex);

            s32 tx = item_x + (item_w_val - text_w) / 2;
            s32 ty = y_coord + (h - text_h) / 2;

            drawer->RenderTexture(text_tex, tx, ty);
        }
    }

}
