#pragma once

#include <string>
#include "../model/DataModel.hpp"

namespace romm::ui {

    enum class FitMode {
        Contain,
        Cover,
        SmartCover
    };

    enum class CoverProfileType {
        DefaultPortrait,
        PS1Square,
        PSPPortrait,
        NintendoDS,
        GameBoy,
        GameBoyColor,
        GameBoyAdvance,
        PS2Portrait,
        Nintendo3DS
    };

    struct CoverProfile {
        CoverProfileType type = CoverProfileType::DefaultPortrait;
        std::string name = "DefaultPortrait";
        int columns = 6;
        int visibleRows = 3;
        // Pixel geometry below is unset by GetCoverProfile() — it's always
        // recomputed by GameGrid::AdjustProfileForHeight before first render.
        int coverW = 180;
        int coverH = 270;
        int gapX = 84;
        int gapY = 20;
        int offsetX = 20;
        int offsetY = 15;
        FitMode fitMode = FitMode::Contain;
        // Detail view mode: render as a single-column title list with a live
        // detail panel, rather than a cover grid. Carried on the profile so
        // GameGrid doesn't have to re-read the setting and risk consulting the
        // global default where GetCoverProfile used the per-platform override.
        bool isDetailList = false;
    };

    // Helper to scale 720p coordinates to 1080p virtual Plutonium coordinates
    inline constexpr int Scale720p(int value) {
        return (value * 3) / 2; // multiply by 1.5
    }

    CoverProfile GetCoverProfile(const romm::model::Platform& platform);

    // -----------------------------------------------------------------------
    // Detail-view cover geometry
    // -----------------------------------------------------------------------

    // The nominal art shape for a profile. This is a *stand-in only*, used
    // while a cover has no decoded texture yet — as soon as one exists its own
    // dimensions are used instead, so this can never override a real image or
    // misshape an unusual cover. Its job is purely to stop the frame visibly
    // resizing at the moment the image lands.
    void GetFallbackCoverAspect(CoverProfileType type, int& aspect_w, int& aspect_h);

    // Result of fitting a cover into a layout slot.
    //
    // The distinction between the two rects is the whole point:
    //
    //  * the SLOT is fixed for a given platform and view. Everything around the
    //    cover (title, metadata, buttons) is positioned against it, so nothing
    //    moves when an image finishes loading or the selection changes.
    //  * the FRAME is what actually gets painted — the image at its true aspect
    //    plus a uniform mat — and it hugs the art instead of filling the slot.
    //    Slot-sized backgrounds are exactly what produced the large black
    //    rectangles around covers whose shape didn't match the slot.
    struct CoverFit {
        int frame_x = 0, frame_y = 0, frame_w = 0, frame_h = 0;
        int img_x   = 0, img_y   = 0, img_w   = 0, img_h   = 0;
    };

    // Proportional `contain` fit: never crops, never stretches, never scales
    // past the slot. tex_w/tex_h are the real decoded texture dimensions where
    // available, or a GetFallbackCoverAspect() pair where not.
    //
    // Callers must compute this per frame from the currently-resolved texture
    // rather than storing it — a cached CoverFit is how the previous game's
    // dimensions would survive a fast scroll.
    CoverFit FitCoverInSlot(int slot_x, int slot_y, int slot_w, int slot_h,
                            int tex_w, int tex_h, int pad);

} // namespace romm::ui
