#include "LibraryLayout.hpp"
#include <switch.h>
#include "SidebarList.hpp"
#include "GameGrid.hpp"
#include "AlphabetBar.hpp"
#include "../navigation/NavigationManager.hpp"
#include "GlobalProgressBar.hpp"
#include <sys/statvfs.h>
#include <sstream>
#include <iomanip>

namespace romm::ui {



    LibraryLayout::LibraryLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {
        
        // Background color: Web Dark Slate (#101216)
        this->SetBackgroundColor(pu::ui::Color(16, 18, 22, 255));

        // Create Title text block (Orbitron Black, Very light text #EDE5FB)
        title_text = pu::ui::elm::TextBlock::New(60, 30, "GAMES");
        title_text->SetFont("Orbitron@45");
        title_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        this->Add(title_text);

        // Load new icons from RomFS
        auto tex_wifi_on = pu::ui::render::LoadImageFromFile("romfs:/wifi_on.png");
        if (tex_wifi_on) handle_wifi_on = pu::sdl2::TextureHandle::New(tex_wifi_on);

        auto tex_wifi_off = pu::ui::render::LoadImageFromFile("romfs:/wifi_off.png");
        if (tex_wifi_off) handle_wifi_off = pu::sdl2::TextureHandle::New(tex_wifi_off);

        auto tex_battery_charging = pu::ui::render::LoadImageFromFile("romfs:/battery_charging.png");
        if (tex_battery_charging) handle_battery_charging = pu::sdl2::TextureHandle::New(tex_battery_charging);

        auto tex_battery_full = pu::ui::render::LoadImageFromFile("romfs:/battery_full.png");
        if (tex_battery_full) handle_battery_full = pu::sdl2::TextureHandle::New(tex_battery_full);

        auto tex_battery_low = pu::ui::render::LoadImageFromFile("romfs:/battery_low.png");
        if (tex_battery_low) handle_battery_low = pu::sdl2::TextureHandle::New(tex_battery_low);

        auto tex_sd_card = pu::ui::render::LoadImageFromFile("romfs:/sd_card.png");
        if (tex_sd_card) handle_sd_card = pu::sdl2::TextureHandle::New(tex_sd_card);

        // Initialize psm service for battery stats
        psmInitialize();

        // 1. WiFi Icon & Text
        wifi_icon = pu::ui::elm::Image::New(930, 34, handle_wifi_off ? handle_wifi_off : handle_sd_card);
        wifi_icon->SetWidth(32);
        wifi_icon->SetHeight(32);
        this->Add(wifi_icon);

        info_text = pu::ui::elm::TextBlock::New(970, 35, "Disconnected");
        info_text->SetFont("Ubuntu@30");
        info_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        this->Add(info_text);

        // 2. Battery Icon & Text
        battery_icon = pu::ui::elm::Image::New(1200, 34, handle_battery_full ? handle_battery_full : handle_sd_card);
        battery_icon->SetWidth(32);
        battery_icon->SetHeight(32);
        this->Add(battery_icon);

        battery_text = pu::ui::elm::TextBlock::New(1240, 35, "100%");
        battery_text->SetFont("Ubuntu@30");
        battery_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        this->Add(battery_text);

        // 3. Storage Icon & Text
        storage_icon = pu::ui::elm::Image::New(1440, 34, handle_sd_card ? handle_sd_card : handle_wifi_off);
        storage_icon->SetWidth(32);
        storage_icon->SetHeight(32);
        this->Add(storage_icon);

        // Retrieve dynamic storage values via statvfs for initial layout
        double free_gb = 24.6;
        double total_gb = 64.0;
        struct statvfs vfs;
        if (statvfs("sdmc:/", &vfs) == 0) {
            double free_bytes = (double)vfs.f_bfree * vfs.f_frsize;
            double total_bytes = (double)vfs.f_blocks * vfs.f_frsize;
            free_gb = free_bytes / (1024.0 * 1024.0 * 1024.0);
            total_gb = total_bytes / (1024.0 * 1024.0 * 1024.0);
        }

        std::stringstream ss;
        ss << std::fixed << std::setprecision(1) << free_gb << " GB / " << total_gb << " GB Free";

        storage_text = pu::ui::elm::TextBlock::New(1480, 35, ss.str());
        storage_text->SetFont("Ubuntu@30");
        storage_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        this->Add(storage_text);

        // Register render callback to update network, battery, and storage dynamically
        this->AddRenderCallback([this]() {
            bool is_wifi = false;
            int wifi_pct = 0;
            bool is_ethernet = false;
            
            NifmInternetConnectionType type = (NifmInternetConnectionType)0;
            u32 wifi = 0;
            NifmInternetConnectionStatus status = (NifmInternetConnectionStatus)0;
            if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &wifi, &status))) {
                if (status == NifmInternetConnectionStatus_Connected) {
                    if (type == NifmInternetConnectionType_WiFi) {
                        is_wifi = true;
                        if (wifi >= 3) wifi_pct = 100;
                        else if (wifi == 2) wifi_pct = 66;
                        else if (wifi == 1) wifi_pct = 33;
                    } else if (type == NifmInternetConnectionType_Ethernet) {
                        is_ethernet = true;
                    }
                }
            }

            if (is_wifi) {
                if (wifi_icon && handle_wifi_on) wifi_icon->SetImage(handle_wifi_on);
                if (info_text) info_text->SetText("Wi-Fi (" + std::to_string(wifi_pct) + "%)");
            } else if (is_ethernet) {
                if (wifi_icon && handle_wifi_on) wifi_icon->SetImage(handle_wifi_on);
                if (info_text) info_text->SetText("Ethernet");
            } else {
                if (wifi_icon && handle_wifi_off) wifi_icon->SetImage(handle_wifi_off);
                if (info_text) info_text->SetText("Disconnected");
            }

            // Update battery status
            u32 charge = 100;
            PsmChargerType charger = PsmChargerType_Unconnected;
            bool charging = false;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge)) && R_SUCCEEDED(psmGetChargerType(&charger))) {
                charging = (charger != PsmChargerType_Unconnected);
                if (battery_text) {
                    battery_text->SetText(std::to_string(charge) + "%" + (charging ? " (Charging)" : ""));
                }
                if (battery_icon) {
                    if (charging) {
                        if (handle_battery_charging) battery_icon->SetImage(handle_battery_charging);
                    } else if (charge <= 20) {
                        if (handle_battery_low) battery_icon->SetImage(handle_battery_low);
                    } else {
                        if (handle_battery_full) battery_icon->SetImage(handle_battery_full);
                    }
                }
            }

            // Update storage free space every 300 frames (approx 5 seconds)
            static int storage_tick = 0;
            if (storage_tick++ % 300 == 0) {
                double free_gb_val = 24.6;
                double total_gb_val = 64.0;
                struct statvfs vfs_val;
                if (statvfs("sdmc:/", &vfs_val) == 0) {
                    double free_bytes = (double)vfs_val.f_bfree * vfs_val.f_frsize;
                    double total_bytes = (double)vfs_val.f_blocks * vfs_val.f_frsize;
                    free_gb_val = free_bytes / (1024.0 * 1024.0 * 1024.0);
                    total_gb_val = total_bytes / (1024.0 * 1024.0 * 1024.0);
                }

                std::stringstream ss_val;
                ss_val << std::fixed << std::setprecision(1) << free_gb_val << " GB / " << total_gb_val << " GB Free";
                if (storage_text) {
                    storage_text->SetText(ss_val.str());
                }
            }
        });

        // Create hint text block (Ubuntu, Light lavender #BEB4E1)
        hint_text = pu::ui::elm::TextBlock::New(60, 1080 - 65, "A Select / Focus   |   B Back / Main Menu   |   + Exit");
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
    }

    LibraryLayout::~LibraryLayout() {
        psmExit();
    }

    void LibraryLayout::OnSelectionUpdated() {
        auto nav = nav_mgr.lock();
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
