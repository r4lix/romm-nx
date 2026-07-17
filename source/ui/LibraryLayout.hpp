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

    class LibraryLayout : public pu::ui::Layout {
    private:
        std::weak_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<SidebarList> sidebar;
        std::shared_ptr<GameGrid> grid;
        std::shared_ptr<AlphabetBar> alphabet_bar;
        std::shared_ptr<LibraryMenuModal> library_menu_modal;
        
        pu::ui::elm::TextBlock::Ref title_text;
        pu::ui::elm::TextBlock::Ref info_text;
        pu::ui::elm::Image::Ref storage_icon;
        pu::ui::elm::TextBlock::Ref storage_text;
        pu::ui::elm::TextBlock::Ref hint_text;

        pu::sdl2::TextureHandle::Ref handle_wifi_on;
        pu::sdl2::TextureHandle::Ref handle_wifi_off;
        pu::sdl2::TextureHandle::Ref handle_battery_charging;
        pu::sdl2::TextureHandle::Ref handle_battery_full;
        pu::sdl2::TextureHandle::Ref handle_battery_low;
        pu::sdl2::TextureHandle::Ref handle_sd_card;

        pu::ui::elm::Image::Ref wifi_icon;
        pu::ui::elm::Image::Ref battery_icon;
        pu::ui::elm::TextBlock::Ref battery_text;

    public:
        LibraryLayout(std::shared_ptr<romm::navigation::NavigationManager> nav);
        ~LibraryLayout() override;
        
        void OnSelectionUpdated();

        PU_SMART_CTOR(LibraryLayout)
    };

}
