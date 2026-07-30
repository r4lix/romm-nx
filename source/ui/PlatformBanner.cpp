#include "PlatformBanner.hpp"

#include <iostream>

namespace romm::ui {

    namespace {

        // Width of the slate separator drawn down the sidebar's right edge
        // (SidebarList::OnRender). Banners have to stay clear of it, so it is
        // subtracted before anything else.
        constexpr s32 kDividerWidth = 3;

        // Breathing room around the column.
        constexpr s32 kSidePadding = 20;
        constexpr s32 kPaddingTop = 24;
        constexpr s32 kPaddingBottom = 24;
        constexpr s32 kSpacing = 12;

        // The readability floor, and the only judgement call in here.
        //
        // The banners are wordmarks ("PlayStation Portable" is the longest),
        // and below roughly this height their lettering stops being legible at
        // 1080p from couch distance. It also keeps the column full-width:
        // squeezing in one more slot means narrowing every banner to match the
        // shorter height at 16:9, which trades one extra platform for a thin
        // strip of art floating in a wide sidebar. Raise it for bigger, fewer
        // banners; lower it for more, smaller ones.
        constexpr s32 kMinBannerHeight = 160;

        // Nothing sane produces this many slots; it just stops a degenerate
        // size from spinning the search loop.
        constexpr s32 kMaxSlots = 64;

    }

    BannerLayout ComputeBannerLayout(const s32 sidebar_w, const s32 sidebar_h) {
        BannerLayout layout;

        const s32 content_w = sidebar_w - kDividerWidth - (2 * kSidePadding);
        const s32 avail_h = sidebar_h - kPaddingTop - kPaddingBottom;
        if (content_w <= 0 || avail_h <= 0) return layout;

        // Tallest banner the sidebar's width allows at 16:9. A banner is never
        // wider than this, so this is also the height ceiling.
        const s32 width_limited_h = (content_w * 9) / 16;
        if (width_limited_h < kMinBannerHeight) {
            // Too narrow for a legible banner at any slot count.
            return layout;
        }

        // Try increasing slot counts and keep the last one that still clears
        // the readability floor. More slots means a shorter banner, so the
        // first failure is also the last — but the loop is written as a search
        // rather than a formula because the width cap makes the height
        // piecewise, not linear.
        for (s32 slots = 1; slots <= kMaxSlots; ++slots) {
            const s32 total_spacing = (slots - 1) * kSpacing;
            if (avail_h - total_spacing < kMinBannerHeight) break;

            s32 banner_h = (avail_h - total_spacing) / slots;
            if (banner_h > width_limited_h) banner_h = width_limited_h;
            if (banner_h < kMinBannerHeight) break;

            // Derive the width back from the height so the drawn rectangle is
            // exactly 16:9 rather than 16:9 with a rounding error.
            s32 banner_w = (banner_h * 16) / 9;
            if (banner_w > content_w) {
                banner_w = content_w;
                banner_h = (banner_w * 9) / 16;
                if (banner_h < kMinBannerHeight) break;
            }

            layout.viable = true;
            layout.slot_count = slots;
            layout.banner_w = banner_w;
            layout.banner_h = banner_h;
            layout.spacing = kSpacing;
        }

        if (!layout.viable) return layout;

        // Centre the column horizontally in the usable width, and the whole
        // block vertically in the usable height — the last slot rarely lands
        // exactly on the bottom padding, and splitting the remainder looks
        // deliberate where top-aligning looks truncated.
        const s32 total_h = (layout.slot_count * layout.banner_h) +
                            ((layout.slot_count - 1) * layout.spacing);
        layout.origin_x = (sidebar_w - kDividerWidth - layout.banner_w) / 2;
        layout.origin_y = kPaddingTop + ((avail_h - total_h) / 2);

        return layout;
    }

    PlatformBannerCache::~PlatformBannerCache() {
        Clear();
    }

    pu::sdl2::Texture PlatformBannerCache::Get(const std::string& canonical_id) {
        if (canonical_id.empty()) return nullptr;

        auto it = textures.find(canonical_id);
        if (it != textures.end()) {
            return it->second; // may legitimately be nullptr — a cached miss
        }

        pu::sdl2::Texture tex =
            pu::ui::render::LoadImageFromFile("romfs:/assets/platform_banners/" + canonical_id + ".png");
        if (!tex) {
            std::cout << "[BANNER] No banner for platform '" << canonical_id
                      << "', falling back to its text row" << std::endl;
        }
        textures[canonical_id] = tex;
        return tex;
    }

    void PlatformBannerCache::Clear() {
        for (auto& entry : textures) {
            if (entry.second) {
                pu::ui::render::DeleteTexture(entry.second);
            }
        }
        textures.clear();
    }

}
