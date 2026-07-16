#include "CoverProfile.hpp"
#include <iostream>

namespace romm::ui {

    CoverProfile GetCoverProfile(const romm::model::Platform& platform) {
        if (platform.slug == "ps1" || platform.slug == "psx" || 
            platform.slug == "playstation" || platform.slug == "sony-playstation") {
            // PS1Square profile defined in 720p, scaled to 1080p
            return {
                .type = CoverProfileType::PS1Square,
                .name = "PS1Square",
                .columns = 6,
                .visibleRows = 3,
                .coverW = Scale720p(160),
                .coverH = Scale720p(160),
                .gapX = Scale720p(10),
                .gapY = Scale720p(10),
                .offsetX = Scale720p(8), // 12px in 1080p
                .offsetY = Scale720p(4), // 6px in 1080p
                .fitMode = FitMode::Contain
            };
        }

        if (platform.slug == "ps2" || platform.slug == "playstation-2" || 
            platform.slug == "playstation2" || platform.slug == "sony-playstation-2") {
            // PlayStation 2 profile: 5 columns x 2 rows
            return {
                .type = CoverProfileType::PS2Portrait,
                .name = "PS2Portrait",
                .columns = 5,
                .visibleRows = 2,
                .coverW = Scale720p(161), // 241px @ 1080p
                .coverH = Scale720p(230), // 345px @ 1080p
                .gapX = Scale720p(44),    // 66px @ 1080p
                .gapY = Scale720p(20),    // 30px @ 1080p
                .offsetX = Scale720p(24), // 36px @ 1080p
                .offsetY = Scale720p(20), // 30px @ 1080p
                .fitMode = FitMode::Contain
            };
        }

        if (platform.slug == "psp" || platform.slug == "sony-psp" || platform.slug == "playstation-portable") {
            // PSPPortrait: 5 columns x 2 visible rows — reduced spacing between covers
            // Grid canvas: 1540 x 800 px (1080p Plutonium coordinates).
            // coverH: (800 - 2*offsetY - 1*gapY) / 2 = (800-24-20)/2 = 378px -> Scale720p(252)
            // coverW at typical PSP box-art ~0.656 ratio: 378*0.656 ≈ 248px -> Scale720p(165)
            // gapX: (1540 - 2*51 - 5*248) / 4 ≈ 51px -> Scale720p(34)
            // Total: 2*51 + 4*51 + 5*248 = 102 + 204 + 1240 = 1546px (fills canvas and centers)
            return {
                .type    = CoverProfileType::PSPPortrait,
                .name    = "PSPPortrait",
                .columns = 5,
                .visibleRows = 2,
                .coverW  = Scale720p(165),  // 247px @ 1080p
                .coverH  = Scale720p(252),  // 378px @ 1080p
                .gapX    = Scale720p(34),   //  51px — reduced horizontal spacing
                .gapY    = Scale720p(13),   //  19px
                .offsetX = Scale720p(34),   //  51px
                .offsetY = Scale720p(8),    //  12px
                .fitMode = FitMode::Contain
            };
        }

        // Helper to apply the shared Nintendo DS layout parameters to handheld profiles
        auto applyDsMetrics = [](CoverProfile& profile) {
            profile.columns = 5;
            profile.visibleRows = 3;
            profile.coverW  = 276;             // 184px @ 720p
            profile.coverH  = 250;             // 166.67px @ 720p
            profile.gapX    = Scale720p(20);   // 20px @ 720p, 30px @ 1080p
            profile.gapY    = Scale720p(10);   // 10px @ 720p, 15px @ 1080p
            profile.offsetX = 20;              // 13.33px @ 720p, 20px @ 1080p
            profile.offsetY = Scale720p(10);   // 10px @ 720p, 15px @ 1080p
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

        // DefaultPortrait fallback defined in 720p, scaled to 1080p
        return {
            .type = CoverProfileType::DefaultPortrait,
            .name = "DefaultPortrait",
            .columns = 6,
            .visibleRows = 3,
            .coverW = Scale720p(120), // 180 in 1080p
            .coverH = Scale720p(180), // 270 in 1080p
            .gapX = Scale720p(56),    // 84 in 1080p
            .gapY = Scale720p(14),    // 21 in 1080p
            .offsetX = Scale720p(14),
            .offsetY = Scale720p(10),
            .fitMode = FitMode::Contain // Use Contain avoiding Stretch
        };
    }

} // namespace romm::ui
