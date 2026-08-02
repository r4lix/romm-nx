#pragma once

#include <pu/Plutonium>
#include <memory>

namespace romm::navigation {
    class NavigationManager;
}

namespace romm::ui {
    class SidebarList;
    class GameGrid;
    class AlphabetBar;
    class LibraryMenuModal;
    class StatusBar;

    class LibraryLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<SidebarList> sidebar;
        std::shared_ptr<GameGrid> grid;
        std::shared_ptr<AlphabetBar> alphabet_bar;
        std::shared_ptr<LibraryMenuModal> library_menu_modal;
        std::shared_ptr<StatusBar> status_bar;

        pu::ui::elm::TextBlock::Ref title_text;
        pu::ui::elm::TextBlock::Ref hint_text;
        // Which hint string is currently on screen. Compared by pointer (both
        // values are string literals) purely to keep OnSelectionUpdated from
        // re-rasterising the same text on every selection change.
        const char* last_hint_key = nullptr;

    public:
        LibraryLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~LibraryLayout() override;

        void OnSelectionUpdated();
        void RefreshTranslations();

        std::shared_ptr<GameGrid> GetGameGrid() const { return grid; }

        PU_SMART_CTOR(LibraryLayout)
    };

}
