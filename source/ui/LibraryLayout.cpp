#include "LibraryLayout.hpp"
#include "SidebarList.hpp"
#include "GameGrid.hpp"
#include "AlphabetBar.hpp"
#include "../navigation/NavigationManager.hpp"
#include "GlobalProgressBar.hpp"
#include "LibraryMenuModal.hpp"
#include "StatusBar.hpp"
#include "UninstallConfirmModal.hpp"
#include "../i18n/I18n.hpp"

namespace romm::ui {

    LibraryLayout::LibraryLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {

        // Background color: Web Dark Slate (#101216)
        this->SetBackgroundColor(pu::ui::Color(16, 18, 22, 255));

        // Create Title text block (Orbitron Black, Very light text #EDE5FB).
        // "ROMM" is the product name — not translated in any language.
        title_text = pu::ui::elm::TextBlock::New(60, 30, "ROMM");
        title_text->SetFont("Orbitron@45");
        title_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        this->Add(title_text);

        // Wi-Fi / battery / storage cluster (own polling throttle, see StatusBar)
        status_bar = StatusBar::New(0, 0, 1920, 1080);
        this->Add(status_bar);

        // Create hint text block (Ubuntu, Light lavender #BEB4E1)
        hint_text = pu::ui::elm::TextBlock::New(60, 1080 - 65, romm::i18n::tr("hint.library"));
        hint_text->SetFont("Ubuntu@30");
        hint_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        this->Add(hint_text);

        // Global Progress Bar (Top left, free space)
        auto global_progress = romm::ui::GlobalProgressBar::New(380 + 16, 14, 460, 56, nav);
        this->Add(global_progress);

        // Create sidebar, alphabet bar, and grid components
        // Sidebar takes width=380, height=880 (from y=100 to y=980)
        sidebar = SidebarList::New(0, 100, 380, 880, nav);
        this->Add(sidebar);

        // Alphabet bar takes the top of the grid area
        alphabet_bar = AlphabetBar::New(380 + 30, 100, 1920 - 380 - 60, 60, nav);
        this->Add(alphabet_bar);

        // Grid takes the remaining space below the alphabet bar
        grid = GameGrid::New(380, 180, 1920 - 380, 800, nav);
        this->Add(grid);

        // Uninstall confirmation. Needed here because Detail view mode's panel
        // can trigger an uninstall without ever leaving the Library screen —
        // NavigationManager gates all input on uninstall_modal.active, so a
        // layout that can raise the modal but not draw it would soft-lock.
        this->Add(romm::ui::UninstallConfirmModal::New(nav));

        // Y-Menu overlay — added last so it renders on top of everything else
        library_menu_modal = LibraryMenuModal::New(nav);
        this->Add(library_menu_modal);
    }

    LibraryLayout::~LibraryLayout() {}

    void LibraryLayout::RefreshTranslations() {
        // Drop the memo rather than re-setting the hint here: which of the two
        // hint strings applies depends on model state this function doesn't
        // look at. Language only ever changes from Settings, so this layout is
        // off-screen; OnSelectionUpdated re-picks the right string in the new
        // language when navigation brings the library back up.
        last_hint_key = nullptr;
        // The sidebar's status cards and the grid's status/info strips are
        // pre-rendered textures, so they need an explicit rebuild; everything
        // else on this screen is drawn from tr() each frame.
        if (sidebar) sidebar->RefreshTranslations();
        if (grid) grid->RefreshTranslations();
    }

    void LibraryLayout::OnSelectionUpdated() {
        auto nav = nav_mgr.lock();
        // With no platforms on screen every key in the normal hint is inert, and
        // the one key that does something (A = retry the fetch) isn't in it.
        // Swap the whole line rather than appending, so the hint never advertises
        // Mark/Download against an empty grid.
        if (hint_text && nav) {
            auto model = nav->GetModel();
            const bool empty = (!model || model->GetPlatforms().empty());
            const char* key = empty ? "hint.library.empty" : "hint.library";
            if (key != last_hint_key) {
                last_hint_key = key;
                hint_text->SetText(romm::i18n::tr(key));
            }
        }
        if (nav && grid) {
            if (sidebar) {
                sidebar->Refresh();
            }
            if (nav->ShowAlphabetFilter()) {
                grid->SetY(180);
                grid->SetHeight(800);
            } else {
                grid->SetY(100);
                grid->SetHeight(880);
            }
            grid->OnSelectionUpdated();
        }
    }

}
