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
        PS2Portrait
    };

    struct CoverProfile {
        CoverProfileType type = CoverProfileType::DefaultPortrait;
        std::string name = "DefaultPortrait";
        int columns = 6;
        int visibleRows = 3;
        int coverW = 180;
        int coverH = 270;
        int gapX = 84;
        int gapY = 20;
        int offsetX = 20;
        int offsetY = 15;
        FitMode fitMode = FitMode::Contain;
    };

    // Helper to scale 720p coordinates to 1080p virtual Plutonium coordinates
    inline constexpr int Scale720p(int value) {
        return (value * 3) / 2; // multiply by 1.5
    }

    CoverProfile GetCoverProfile(const romm::model::Platform& platform);

} // namespace romm::ui
