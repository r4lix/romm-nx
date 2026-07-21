#include "CoverProfile.hpp"
#include "../model/ConfigManager.hpp"
#include <iostream>

namespace romm::ui {

    CoverProfile GetCoverProfile(const romm::model::Platform& platform) {
        // "Big" view mode trims each profile's row count (not just columns —
        // see the comment in GameGrid::AdjustProfileForHeight for why rows are
        // what actually grow the tiles on this canvas) so tiles render larger.
        // "Detail" mode isn't implemented yet, so it renders identically to Default.
        //
        // Only .columns/.visibleRows/.fitMode are set below. The pixel geometry
        // (coverW/coverH/gapX/gapY/offsetX/offsetY) is intentionally left at the
        // struct defaults: GameGrid::AdjustProfileForHeight unconditionally
        // recomputes and overwrites all of it before first render, so this is
        // the single place column/row counts are decided and the single place
        // (AdjustProfileForHeight) tile geometry is decided — no second set of
        // numbers to keep in sync.
        const bool big = (romm::model::ConfigManager::Instance().GetGridViewMode(platform.slug) == romm::model::GridViewMode::Big);

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
            // Shares the DS/GB-family layout metrics — same physical cartridge
            // box proportions — but keeps its own CoverProfileType so
            // platform-specific logic elsewhere (e.g. the RomM cover URL
            // rewrite quirk in CoverCache) isn't forced to treat it as NDS.
            CoverProfile profile;
            profile.type = CoverProfileType::Nintendo3DS;
            profile.name = "Nintendo3DS";
            applyDsMetrics(profile);
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

} // namespace romm::ui
