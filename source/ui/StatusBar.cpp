#include "StatusBar.hpp"
#include <switch.h>
#include <sys/statvfs.h>
#include <sstream>
#include <iomanip>
#include <iostream>

namespace romm::ui {

    StatusBar::StatusBar(s32 x, s32 y, s32 w, s32 h)
        : Element(), x(x), y(y), w(w), h(h) {

        wifi_on_tex = pu::ui::render::LoadImageFromFile("romfs:/wifi_on.png");
        wifi_off_tex = pu::ui::render::LoadImageFromFile("romfs:/wifi_off.png");
        battery_charging_tex = pu::ui::render::LoadImageFromFile("romfs:/battery_charging.png");
        battery_full_tex = pu::ui::render::LoadImageFromFile("romfs:/battery_full.png");
        battery_low_tex = pu::ui::render::LoadImageFromFile("romfs:/battery_low.png");
        sd_card_tex = pu::ui::render::LoadImageFromFile("romfs:/sd_card.png");

        Result rc = psmInitialize();
        psm_ok = R_SUCCEEDED(rc);
        if (!psm_ok) {
            std::cerr << "[STATUS_BAR] psmInitialize failed: " << rc << std::endl;
        }

        // Force an immediate poll on first render instead of waiting out the
        // throttle window with stale/placeholder text.
        last_poll = std::chrono::steady_clock::time_point();
        Poll();
    }

    StatusBar::~StatusBar() {
        if (wifi_on_tex) { pu::ui::render::DeleteTexture(wifi_on_tex); wifi_on_tex = nullptr; }
        if (wifi_off_tex) { pu::ui::render::DeleteTexture(wifi_off_tex); wifi_off_tex = nullptr; }
        if (battery_charging_tex) { pu::ui::render::DeleteTexture(battery_charging_tex); battery_charging_tex = nullptr; }
        if (battery_full_tex) { pu::ui::render::DeleteTexture(battery_full_tex); battery_full_tex = nullptr; }
        if (battery_low_tex) { pu::ui::render::DeleteTexture(battery_low_tex); battery_low_tex = nullptr; }
        if (sd_card_tex) { pu::ui::render::DeleteTexture(sd_card_tex); sd_card_tex = nullptr; }
        if (net_text.tex) pu::ui::render::DeleteTexture(net_text.tex);
        if (battery_text.tex) pu::ui::render::DeleteTexture(battery_text.tex);
        if (storage_text.tex) pu::ui::render::DeleteTexture(storage_text.tex);
        if (psm_ok) psmExit();
    }

    void StatusBar::UpdateCached(CachedText& c, const std::string& s) {
        if (c.str == s && c.tex) return;
        if (c.tex) {
            pu::ui::render::DeleteTexture(c.tex);
            c.tex = nullptr;
        }
        c.str = s;
        pu::ui::Color text_color(190, 180, 225, 255);
        c.tex = pu::ui::render::RenderText("Ubuntu@30", s, text_color);
    }

    void StatusBar::Poll() {
        // Network
        NifmInternetConnectionType type = (NifmInternetConnectionType)0;
        u32 wifi = 0;
        NifmInternetConnectionStatus status = (NifmInternetConnectionStatus)0;
        std::string net_str = "Disconnected";
        net_connected = false;
        net_is_wifi_or_eth = false;

        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &wifi, &status)) &&
            status == NifmInternetConnectionStatus_Connected) {
            net_connected = true;
            net_is_wifi_or_eth = true;
            if (type == NifmInternetConnectionType_WiFi) {
                int pct = 0;
                if (wifi >= 3) pct = 100;
                else if (wifi == 2) pct = 66;
                else if (wifi == 1) pct = 33;
                net_str = "Wi-Fi (" + std::to_string(pct) + "%)";
            } else if (type == NifmInternetConnectionType_Ethernet) {
                net_str = "Ethernet";
            } else {
                net_str = "Connected";
            }
        }
        UpdateCached(net_text, net_str);

        // Battery
        std::string batt_str = "100%";
        battery_charging_state = false;
        battery_low_state = false;
        if (psm_ok) {
            u32 charge = 100;
            PsmChargerType charger = PsmChargerType_Unconnected;
            if (R_SUCCEEDED(psmGetBatteryChargePercentage(&charge)) &&
                R_SUCCEEDED(psmGetChargerType(&charger))) {
                battery_charging_state = (charger != PsmChargerType_Unconnected);
                battery_low_state = (charge <= 20);
                batt_str = std::to_string(charge) + "%" + (battery_charging_state ? " (Charging)" : "");
            }
        }
        UpdateCached(battery_text, batt_str);

        // Storage
        double free_gb = 0.0, total_gb = 0.0;
        struct statvfs vfs;
        if (statvfs("sdmc:/", &vfs) == 0) {
            free_gb = (double)vfs.f_bfree * vfs.f_frsize / (1024.0 * 1024.0 * 1024.0);
            total_gb = (double)vfs.f_blocks * vfs.f_frsize / (1024.0 * 1024.0 * 1024.0);
        }
        std::ostringstream ss;
        ss << std::fixed << std::setprecision(1) << free_gb << " GB / " << total_gb << " GB Free";
        UpdateCached(storage_text, ss.str());
    }

    void StatusBar::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        // Wi-Fi/battery/storage don't change fast enough to justify polling
        // nifm/psm/statvfs every frame — every previous instance of this bar
        // did exactly that, at 60fps, unconditionally re-rendering text too.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_poll).count() >= 1000) {
            last_poll = now;
            Poll();
        }

        // 1. Wi-Fi / Ethernet
        pu::sdl2::Texture wifi_tex = net_is_wifi_or_eth ? wifi_on_tex : wifi_off_tex;
        if (!wifi_tex) wifi_tex = sd_card_tex;
        if (wifi_tex) {
            pu::ui::render::TextureRenderOptions opts;
            opts.width = 32;
            opts.height = 32;
            drawer->RenderTexture(wifi_tex, x_coord + 930, y_coord + 34, opts);
        }
        if (net_text.tex) {
            drawer->RenderTexture(net_text.tex, x_coord + 970, y_coord + 35);
        }

        // 2. Battery
        pu::sdl2::Texture batt_tex = battery_full_tex;
        if (battery_charging_state && battery_charging_tex) batt_tex = battery_charging_tex;
        else if (battery_low_state && battery_low_tex) batt_tex = battery_low_tex;
        if (!batt_tex) batt_tex = sd_card_tex;
        if (batt_tex) {
            pu::ui::render::TextureRenderOptions opts;
            opts.width = 32;
            opts.height = 32;
            drawer->RenderTexture(batt_tex, x_coord + 1200, y_coord + 34, opts);
        }
        if (battery_text.tex) {
            drawer->RenderTexture(battery_text.tex, x_coord + 1240, y_coord + 35);
        }

        // 3. Storage
        if (sd_card_tex) {
            pu::ui::render::TextureRenderOptions opts;
            opts.width = 32;
            opts.height = 32;
            drawer->RenderTexture(sd_card_tex, x_coord + 1440, y_coord + 34, opts);
        }
        if (storage_text.tex) {
            drawer->RenderTexture(storage_text.tex, x_coord + 1480, y_coord + 35);
        }
    }

}
