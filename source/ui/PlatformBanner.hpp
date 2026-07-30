#pragma once

#include <pu/Plutonium>
#include <string>
#include <unordered_map>

namespace romm::ui {

    // Geometry for the column of platform banners drawn by the library sidebar
    // when Settings > Theme > Platform Selector is set to Banners.
    //
    // Every field is derived from the sidebar's live width/height by
    // ComputeBannerLayout() — nothing here is a stored constant, so changing
    // the sidebar's size (or dropping the alphabet bar, or a future resolution
    // change) re-derives the slot count instead of silently cropping.
    struct BannerLayout {
        // False when banners can't be drawn legibly at this size at all; the
        // caller must fall back to the text list rather than draw something
        // unreadable.
        bool viable = false;

        s32 slot_count = 0; // how many banners fit, top to bottom
        s32 banner_w = 0;   // identical for every slot
        s32 banner_h = 0;   // exactly banner_w * 9 / 16
        s32 spacing = 0;    // vertical gap between two banners
        s32 origin_x = 0;   // left edge of every banner, relative to the sidebar
        s32 origin_y = 0;   // top edge of slot 0, relative to the sidebar

        s32 SlotY(const s32 slot) const { return origin_y + slot * (banner_h + spacing); }
    };

    // Picks the layout that shows the most banners the sidebar can hold while
    // each stays readable. Accounts for the sidebar's width, its height minus
    // the top/bottom padding, the inter-banner spacing, the 16:9 ratio and a
    // minimum readable banner height. Cheap, but not free — call it when the
    // sidebar's dimensions change, not per frame.
    BannerLayout ComputeBannerLayout(s32 sidebar_w, s32 sidebar_h);

    // Lazily loads romfs:/assets/platform_banners/<canonical-id>.png and holds
    // the texture for as long as the cache lives.
    //
    // The key is the canonical platform id from
    // romm::model::ResolvePlatformIdentity() — never a display name, which is
    // localized and server-supplied, and never the raw RomM slug, which varies
    // per instance.
    //
    // Misses are cached as nullptr, so a platform with no bundled art probes
    // RomFS once and then falls back to its text row for free on every
    // subsequent frame. Render thread only (SDL textures).
    class PlatformBannerCache {
    public:
        PlatformBannerCache() = default;
        ~PlatformBannerCache();

        PlatformBannerCache(const PlatformBannerCache&) = delete;
        PlatformBannerCache& operator=(const PlatformBannerCache&) = delete;

        // nullptr when the platform has no banner, or the file failed to
        // decode. Both are ordinary states, not errors.
        pu::sdl2::Texture Get(const std::string& canonical_id);

        // Releases every loaded texture. Called when the sidebar goes away and
        // when the user switches back to the text selector — a mode the banners
        // aren't needed in shouldn't keep ~20 textures resident.
        void Clear();

    private:
        std::unordered_map<std::string, pu::sdl2::Texture> textures;
    };

}
