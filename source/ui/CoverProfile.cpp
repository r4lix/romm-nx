#include "CoverProfile.hpp"
#include "../model/ConfigManager.hpp"
#include <algorithm>
#include <iostream>

namespace romm::ui {

    static CoverProfile GetBaseCoverProfile(const romm::model::Platform& platform, bool big) {
        // "Big" view mode trims each profile's row count (not just columns —
        // see the comment in GameGrid::AdjustProfileForHeight for why rows are
        // what actually grow the tiles on this canvas) so tiles render larger.
        //
        // Only .columns/.visibleRows/.fitMode are set below. The pixel geometry
        // (coverW/coverH/gapX/gapY/offsetX/offsetY) is intentionally left at the
        // struct defaults: GameGrid::AdjustProfileForHeight unconditionally
        // recomputes and overwrites all of it before first render, so this is
        // the single place column/row counts are decided and the single place
        // (AdjustProfileForHeight) tile geometry is decided — no second set of
        // numbers to keep in sync.
        if (platform.slug == "ps1" || platform.slug == "psx" ||
            platform.slug == "playstation" || platform.slug == "sony-playstation") {
            // PS1Square: 6 columns x 3 rows (4 columns x 2 rows in Big mode).
            return {
                .type = CoverProfileType::PS1Square,
                .name = "PS1Square",
                .columns = big ? 4 : 6,
                .visibleRows = big ? 2 : 3,
                .fitMode = FitMode::Contain
            };
        }

        if (platform.slug == "ps2" || platform.slug == "playstation-2" ||
            platform.slug == "playstation2" || platform.slug == "sony-playstation-2") {
            // PlayStation 2 profile: 7 columns x 3 rows (5 columns x 2 rows in Big mode)
            return {
                .type = CoverProfileType::PS2Portrait,
                .name = "PS2Portrait",
                .columns = big ? 5 : 7,
                .visibleRows = big ? 2 : 3,
                .fitMode = FitMode::Contain
            };
        }

        if (platform.slug == "psp" || platform.slug == "sony-psp" || platform.slug == "playstation-portable") {
            // PSPPortrait: 5 columns x 2 rows (4 columns x 1 row in Big mode)
            return {
                .type    = CoverProfileType::PSPPortrait,
                .name    = "PSPPortrait",
                .columns = big ? 4 : 5,
                .visibleRows = big ? 1 : 2,
                .fitMode = FitMode::Contain
            };
        }

        // Helper to apply the shared Nintendo DS layout column/row counts to handheld profiles
        auto applyDsMetrics = [big](CoverProfile& profile) {
            profile.columns = big ? 4 : 5;
            profile.visibleRows = big ? 2 : 3;
            profile.fitMode = FitMode::Contain;
        };

        if (platform.slug == "nds" || platform.slug == "nintendo-ds" ||
            platform.slug == "nintendo_ds" || platform.slug == "Nintendo DS") {
            CoverProfile profile;
            profile.type = CoverProfileType::NintendoDS;
            profile.name = "NintendoDS";
            applyDsMetrics(profile);
            return profile;
        }

        if (platform.slug == "3ds" || platform.slug == "nintendo-3ds" ||
            platform.slug == "n3ds" || platform.slug == "nintendo_3ds") {
            // 3DS carries the same landscape box proportions as NDS, but not the
            // same column count. It used to borrow applyDsMetrics() (5x3 / 4x2),
            // which was sized for a tile that AdjustProfileForHeight never
            // actually gave it: 3DS had no aspect branch there, so it fell
            // through to DefaultPortrait's 2:3 tile and every cover ended up
            // width-limited inside a tile 40% taller than the art. With the
            // tile now solved at the real 276:250 ratio, a 3DS tile is much
            // wider than a DS one at equal height, so the grid fits a sixth
            // column across the same canvas instead of stranding ~600px of
            // horizontal margin.
            CoverProfile profile;
            profile.type = CoverProfileType::Nintendo3DS;
            profile.name = "Nintendo3DS";
            profile.columns = big ? 4 : 6;
            profile.visibleRows = big ? 2 : 3;
            profile.fitMode = FitMode::Contain;
            return profile;
        }

        if (platform.slug == "n64" || platform.slug == "nintendo-64" ||
            platform.slug == "nintendo64" || platform.slug == "nintendo_64") {
            // Measured the same way as SNES, off a real grid: the art rendered
            // 108x78 at 720p inside the portrait default's tile, i.e. 1.385:1.
            // PAL N64 boxes are the same landscape shape as PAL SNES ones, so
            // this takes the same 7:5 tile and the same grid — anything else
            // would be two answers to one question.
            CoverProfile profile;
            profile.type = CoverProfileType::Nintendo64;
            profile.name = "Nintendo64";
            profile.columns = big ? 3 : 4;
            profile.visibleRows = big ? 2 : 3;
            profile.fitMode = FitMode::Contain;
            return profile;
        }

        if (platform.slug == "snes" || platform.slug == "super-nes" ||
            platform.slug == "super-nintendo" || platform.slug == "super-nintendo-entertainment-system" ||
            platform.slug == "sfc" || platform.slug == "sfam" || platform.slug == "super-famicom") {
            // SNES boxes are landscape (~7:5), and until this profile existed
            // they were drawn in DefaultPortrait's 2:3 tile — width-bound inside
            // a tile more than twice as tall as the art, so a cover occupied
            // under half its tile and the box text was unreadable. Same failure
            // 3DS had before it got its own aspect; see the note there.
            //
            // Both modes are far coarser than any other profile, and
            // deliberately so: legibility of the box text is the entire point
            // here, so the grids are sized for reading rather than for fitting
            // a page. Each step was tried on hardware: Default went 6x4 -> 4x2
            // -> 4x3, Big went 4x3 -> 4x2 -> 3x2.
            //
            // Default (4x3) fills the canvas at 343x245 a tile, height-bound,
            // so it shows 12 games with no wasted vertical space. Big (3x2)
            // gives 491x351 — nearly a quarter of the canvas per cover — which
            // is what makes the small print on a box readable.
            CoverProfile profile;
            profile.type = CoverProfileType::SuperNintendo;
            profile.name = "SuperNintendo";
            profile.columns = big ? 3 : 4;
            profile.visibleRows = big ? 2 : 3;
            profile.fitMode = FitMode::Contain;
            return profile;
        }

        if (platform.slug == "gb" || platform.slug == "game-boy" ||
            platform.slug == "gameboy" || platform.slug == "nintendo-game-boy") {
            CoverProfile profile;
            profile.type = CoverProfileType::GameBoy;
            profile.name = "GameBoy";
            applyDsMetrics(profile);
            return profile;
        }

        if (platform.slug == "gbc" || platform.slug == "game-boy-color" ||
            platform.slug == "gameboy-color" || platform.slug == "nintendo-game-boy-color") {
            CoverProfile profile;
            profile.type = CoverProfileType::GameBoyColor;
            profile.name = "GameBoyColor";
            applyDsMetrics(profile);
            return profile;
        }

        if (platform.slug == "gba" || platform.slug == "game-boy-advance" ||
            platform.slug == "gameboy-advance" || platform.slug == "nintendo-game-boy-advance") {
            CoverProfile profile;
            profile.type = CoverProfileType::GameBoyAdvance;
            profile.name = "GameBoyAdvance";
            applyDsMetrics(profile);
            return profile;
        }

        return {
            .type = CoverProfileType::DefaultPortrait,
            .name = "DefaultPortrait",
            .columns = big ? 5 : 6,
            .visibleRows = big ? 2 : 3,
            .fitMode = FitMode::Contain
        };
    }

    CoverProfile GetCoverProfile(const romm::model::Platform& platform) {
        const auto mode = romm::model::ConfigManager::Instance().GetGridViewMode(platform.slug);

        CoverProfile profile = GetBaseCoverProfile(platform, mode == romm::model::GridViewMode::Big);

        // Detail mode keeps the platform's cover type — the panel still draws a
        // real cover and needs the right aspect handling — but collapses the
        // grid to a single column. That one change is what makes the existing
        // row/column navigation in NavigationManager work unmodified: Up/Down
        // step by one entry and Left falls back to the sidebar, which is exactly
        // list behaviour. visibleRows is recomputed from the row height in
        // GameGrid::AdjustProfileForHeight.
        if (mode == romm::model::GridViewMode::Detail) {
            profile.isDetailList = true;
            profile.columns = 1;
            profile.visibleRows = 1;
        }
        return profile;
    }

    void GetFallbackCoverAspect(CoverProfileType type, int& aspect_w, int& aspect_h) {
        // Mirrors the ratios GameGrid::AdjustProfileForHeight solves its grid
        // tiles at, so a cover's placeholder frame and its grid tile agree on
        // the platform's shape.
        switch (type) {
            case CoverProfileType::PS2Portrait:    aspect_w = 161; aspect_h = 230; break;
            case CoverProfileType::PSPPortrait:    aspect_w = 165; aspect_h = 252; break;
            case CoverProfileType::PS1Square:      aspect_w = 1;   aspect_h = 1;   break;
            case CoverProfileType::NintendoDS:
            case CoverProfileType::Nintendo3DS:
            case CoverProfileType::GameBoy:
            case CoverProfileType::GameBoyColor:
            case CoverProfileType::GameBoyAdvance: aspect_w = 276; aspect_h = 250; break;
            case CoverProfileType::SuperNintendo:
            case CoverProfileType::Nintendo64:      aspect_w = 7;   aspect_h = 5;   break;
            case CoverProfileType::DefaultPortrait:
            default:                               aspect_w = 120; aspect_h = 180; break;
        }
    }

    CoverFit FitCoverInSlot(int slot_x, int slot_y, int slot_w, int slot_h,
                            int tex_w, int tex_h, int pad) {
        CoverFit fit;

        // The mat is carved out of the slot rather than added around it, so a
        // frame can never spill past the area the layout reserved for it.
        const int avail_w = std::max(1, slot_w - 2 * pad);
        const int avail_h = std::max(1, slot_h - 2 * pad);

        // Degenerate input (no texture and a caller that passed nothing usable)
        // falls back to filling the slot rather than dividing by zero.
        if (tex_w <= 0 || tex_h <= 0) {
            tex_w = avail_w;
            tex_h = avail_h;
        }

        // Proportional contain. Both axes use the same scale, which is what
        // rules out stretching; taking the min rules out cropping.
        const double scale = std::min((double)avail_w / (double)tex_w,
                                      (double)avail_h / (double)tex_h);
        fit.img_w = std::max(1, (int)(tex_w * scale));
        fit.img_h = std::max(1, (int)(tex_h * scale));

        fit.frame_w = fit.img_w + 2 * pad;
        fit.frame_h = fit.img_h + 2 * pad;
        // Centred on both axes, so the art stays put as the frame grows and
        // shrinks around it.
        fit.frame_x = slot_x + (slot_w - fit.frame_w) / 2;
        fit.frame_y = slot_y + (slot_h - fit.frame_h) / 2;
        fit.img_x = fit.frame_x + pad;
        fit.img_y = fit.frame_y + pad;
        return fit;
    }

} // namespace romm::ui
