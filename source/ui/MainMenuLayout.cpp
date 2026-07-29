#include "MainMenuLayout.hpp"
#include "../navigation/NavigationManager.hpp"
#include "GlobalProgressBar.hpp"
#include "UpdateAvailableModal.hpp"
#include "../model/UpdateManager.hpp"
#include "../i18n/I18n.hpp"
#include "../Version.hpp"
#include <cctype>

namespace romm::ui {

    // --- MenuGrid Implementation ---

    // Card order is load-bearing (NavigationManager indexes menu entries by
    // position), so the keys live in one ordered table rather than being spread
    // across the constructor and the update-badge check below.
    const char* const MenuGrid::kItemKeys[] = {
        "menu.games",
        "menu.installed",
        "menu.queue",
        "menu.saves",
        "menu.file_browser",
        "menu.settings"
    };

    MenuGrid::MenuGrid(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav),
          card_w(360), card_h(320), col_spacing(50), row_spacing(40), offset_x(165), offset_y(50) {
        InitTextures();
    }

    MenuGrid::~MenuGrid() {
        ClearTextures();
    }

    void MenuGrid::InitTextures() {
        ClearTextures();

        // Use Orbitron for dashboard card text (bold & stylistic)
        std::string font_name = "Orbitron@37";
        pu::ui::Color selected_clr(237, 229, 251, 255); // Very light text (#EDE5FB)
        pu::ui::Color unselected_clr(190, 180, 225, 255); // Light lavender (#BEB4E1)

        for (size_t i = 0; i < GetItemCount(); ++i) {
            // Row-1 cards are 360px wide; 340 keeps a 10px margin either side
            // while giving longer translations ("File d'attente") the room they
            // need. RenderText's width argument truncates rather than wraps, so
            // a too-small limit shows an ellipsis instead of the label.
            s32 limit_w = (i < 4) ? 340 : 720;
            const std::string label = romm::i18n::tr(kItemKeys[i]);
            pu::sdl2::Texture sel_tex = pu::ui::render::RenderText(font_name, label, selected_clr, limit_w);
            pu::sdl2::Texture unsel_tex = pu::ui::render::RenderText(font_name, label, unselected_clr, limit_w);

            selected_texs.push_back(sel_tex);
            unselected_texs.push_back(unsel_tex);
        }
    }

    void MenuGrid::RefreshTranslations() {
        InitTextures();
    }

    void MenuGrid::ClearTextures() {
        for (auto& tex : selected_texs) {
            pu::ui::render::DeleteTexture(tex);
        }
        selected_texs.clear();

        for (auto& tex : unselected_texs) {
            pu::ui::render::DeleteTexture(tex);
        }
        unselected_texs.clear();
    }

    void MenuGrid::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        size_t selected_menu_idx = nav->GetSelectedMenuIdx();

        // Row 1: 4 cards, size 360 x 320
        s32 row1_y = y_coord + 30;
        s32 row1_h = 320;
        s32 row1_w = 360;
        s32 row1_spacing = 50;
        s32 row1_start_x = x_coord + 165;

        // Row 2: 2 cards, size 770 x 160
        s32 row2_y = row1_y + row1_h + 40;
        s32 row2_h = 160;
        s32 row2_w = 770;
        s32 row2_spacing = 50;
        s32 row2_start_x = x_coord + 165;

        for (size_t i = 0; i < GetItemCount(); ++i) {
            s32 card_x = 0;
            s32 card_y = 0;
            s32 card_w_val = 0;
            s32 card_h_val = 0;

            if (i < 4) { // Row 1: 0, 1, 2, 3
                card_x = row1_start_x + i * (row1_w + row1_spacing);
                card_y = row1_y;
                card_w_val = row1_w;
                card_h_val = row1_h;
            } else { // Row 2: 4, 5
                card_x = row2_start_x + (i - 4) * (row2_w + row2_spacing);
                card_y = row2_y;
                card_w_val = row2_w;
                card_h_val = row2_h;
            }

            bool is_selected = (i == selected_menu_idx);

            pu::ui::Color border_color;
            pu::ui::Color fill_color(30, 34, 43, 255); // Web Charcoal Grey (#1E222B)
            s32 border_width = 2;

            if (is_selected) {
                // Cream accent: #E6C7A7
                border_color = pu::ui::Color(230, 199, 167, 255);
                border_width = 4;
            } else {
                border_color = pu::ui::Color(45, 50, 62, 255); // Slate Border Grey (#2D323E)
                border_width = 2;
            }

            // Draw card border and background
            if (border_width > 0) {
                drawer->RenderRoundedRectangleFill(border_color, card_x, card_y, card_w_val, card_h_val, 16);
                drawer->RenderRoundedRectangleFill(fill_color, card_x + border_width, card_y + border_width, card_w_val - (border_width * 2), card_h_val - (border_width * 2), 12);
            } else {
                drawer->RenderRoundedRectangleFill(fill_color, card_x, card_y, card_w_val, card_h_val, 16);
            }

            // Draw centered card text
            pu::sdl2::Texture text_tex = is_selected ? selected_texs.at(i) : unselected_texs.at(i);
            s32 text_w = pu::ui::render::GetTextureWidth(text_tex);
            s32 text_h = pu::ui::render::GetTextureHeight(text_tex);

            s32 tx = card_x + (card_w_val - text_w) / 2;
            s32 ty = card_y + (card_h_val - text_h) / 2;

            drawer->RenderTexture(text_tex, tx, ty);

            // Keyed off the card's identity, not its (now translated) label.
            if (std::string(kItemKeys[i]) == "menu.settings" &&
                romm::model::UpdateManager::Instance().GetState() == romm::model::UpdateState::UpdateAvailable) {
                s32 dot_radius = 10;
                drawer->RenderCircleFill(pu::ui::Color(231, 76, 60, 255), card_x + card_w_val - dot_radius - 14, card_y + dot_radius + 14, dot_radius);
            }
        }
    }


    // --- MainMenuLayout Implementation ---

    MainMenuLayout::MainMenuLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {

        // Main background: Web Dark Slate (#101216)
        this->SetBackgroundColor(pu::ui::Color(16, 18, 22, 255));

        // Top Status Bar (Left: Version, Right: Language/status info)
        auto status_left = pu::ui::elm::TextBlock::New(60, 45, "v" + romm::ROMM_NX_VERSION);
        status_left->SetFont("Ubuntu@30");
        status_left->SetColor(pu::ui::Color(190, 180, 225, 255)); // Light lavender (#BEB4E1)
        this->Add(status_left);

        // Active UI language, upper-cased ("EN" / "FR"). A language code, not a
        // translated word — it deliberately reads the same in every language.
        language_text = pu::ui::elm::TextBlock::New(1920 - 160, 45, "EN");
        language_text->SetFont("Ubuntu@30");
        language_text->SetColor(pu::ui::Color(190, 180, 225, 255)); // Light lavender (#BEB4E1)
        this->Add(language_text);

        // Header text block (Centered, Orbitron Black)
        header_text = pu::ui::elm::TextBlock::New(0, 90, romm::i18n::tr("menu.title"));
        header_text->SetFont("Orbitron@45");
        header_text->SetColor(pu::ui::Color(237, 229, 251, 255)); // Very light text (#EDE5FB)
        header_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(header_text);

        // Cards Grid (starts at y=210)
        grid = MenuGrid::New(0, 210, 1920, 700, nav);
        this->Add(grid);

        // Footer Hint (Centered, Ubuntu)
        hint_text = pu::ui::elm::TextBlock::New(0, 1080 - 65, romm::i18n::tr("hint.main_menu"));
        hint_text->SetFont("Ubuntu@30");
        hint_text->SetColor(pu::ui::Color(190, 180, 225, 255)); // Light lavender (#BEB4E1)
        hint_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(hint_text);

        // Global Progress Bar (moved lower to y=110 to avoid clashing with top status version text)
        auto global_progress = romm::ui::GlobalProgressBar::New(60, 110, 460, 56, nav);
        this->Add(global_progress);

        // Update-available popup — added last so it renders on top of everything else
        update_modal = UpdateAvailableModal::New(nav);
        this->Add(update_modal);

        RefreshTranslations();
    }

    void MainMenuLayout::OnSelectionUpdated() {
        // Selection highlights update dynamically on redraw
    }

    void MainMenuLayout::RefreshTranslations() {
        if (header_text) header_text->SetText(romm::i18n::tr("menu.title"));
        if (hint_text) hint_text->SetText(romm::i18n::tr("hint.main_menu"));
        if (language_text) {
            std::string code = romm::i18n::GetLanguageCode();
            for (auto& c : code) c = (char)toupper((unsigned char)c);
            language_text->SetText(code);
        }
        if (grid) grid->RefreshTranslations();
    }

}
