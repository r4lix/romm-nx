#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace romm::nso {

    struct GbaRomInfo {
        bool valid = false;
        bool unsupported = false;
        std::string error;
        std::vector<std::string> warnings;

        size_t file_size = 0;
        std::string internal_title;  // 12 bytes at 0xA0
        // 4 ASCII characters at 0xAC ("AG5E"). This is what the title code is
        // built from: CaVE names an injected GBA title "A-<game code>_e", so the
        // same cartridge always lands on the same entry.
        std::string game_code;
        std::string maker_code;      // 2 ASCII at 0xB0
        uint8_t software_version = 0;

        uint8_t header_checksum = 0;
        bool header_checksum_ok = false;

        std::string sha256; // of the file as it will be written
    };

    // Parses a .gba image. Injected verbatim, so nothing here modifies it.
    GbaRomInfo AnalyzeGbaRom(const std::vector<uint8_t>& data);

}
