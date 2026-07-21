#pragma once

#include <pu/Plutonium>
#include <string>
#include <chrono>

namespace romm::ui {

    // Reusable top-bar status cluster: Wi-Fi/Ethernet, battery, and SD card
    // free space. Was previously LibraryLayout-only, built from child
    // Image/TextBlock elements that got their nifm/psm/statvfs queries and
    // SetText/SetImage calls re-run every single frame regardless of whether
    // anything changed. This version polls on a throttle and only touches a
    // texture when its displayed value actually differs from last frame.
    class StatusBar : public pu::ui::elm::Element {
    private:
        s32 x, y, w, h;

        pu::sdl2::Texture wifi_on_tex = nullptr;
        pu::sdl2::Texture wifi_off_tex = nullptr;
        pu::sdl2::Texture battery_charging_tex = nullptr;
        pu::sdl2::Texture battery_full_tex = nullptr;
        pu::sdl2::Texture battery_low_tex = nullptr;
        pu::sdl2::Texture sd_card_tex = nullptr;

        bool psm_ok = false;
        std::chrono::steady_clock::time_point last_poll;

        struct CachedText {
            std::string str;
            pu::sdl2::Texture tex = nullptr;
        };
        CachedText net_text, battery_text, storage_text;

        bool net_connected = false;
        bool net_is_wifi_or_eth = false; // true = show "on" icon, false = "off"
        bool battery_charging_state = false;
        bool battery_low_state = false;

        void Poll();
        void UpdateCached(CachedText& c, const std::string& s);

    public:
        StatusBar(s32 x, s32 y, s32 w, s32 h);
        ~StatusBar() override;

        s32 GetX() override { return x; }
        s32 GetY() override { return y; }
        s32 GetWidth() override { return w; }
        s32 GetHeight() override { return h; }

        void OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x, const s32 y) override;
        void OnInput(const u64, const u64, const u64, const pu::ui::TouchPoint) override {}

        PU_SMART_CTOR(StatusBar)
    };

}
