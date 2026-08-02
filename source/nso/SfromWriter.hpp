#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "SnesRom.hpp"

// Native, in-process .sfrom writer for the Switch Online SNES emulator (Canoe).
//
// Format, established by inspecting the 28 CaVE-generated titles already
// installed on this console plus the matching converted ROMs in CaVE's own
// `roms/snes` folder (see docs/nso-snes-format.md):
//
//     [ raw SNES ROM, copier header stripped, no padding ]
//     [ Canoe configuration payload, N bytes            ]
//     [ uint32 little-endian N                          ]
//     [ 'C' 'a' 'n' '1'                                 ]
//
// The payload is a list of little-endian uint16 tokens. 24 of the 28 reference
// titles — LoROM and HiROM, 512 KiB through 4 MiB — carry byte-for-byte the
// same five-token payload, which is what this writer emits. The four that
// differ are games CaVE applied a per-game Canoe preset to (Chrono Trigger,
// Final Fantasy III, Doom, Castlevania) by varying the third token and, in two
// cases, appending extra tokens. romm-nx does not have a source for those
// per-game values, so it emits only the common default and says so in the log.
namespace romm::nso {

    struct SfromConversionResult {
        bool success = false;
        std::string outputPath;
        std::string mapping;
        uint32_t presetId = 0;
        std::string error;

        // Everything worth having in the log after a conversion.
        SnesRomInfo rom;
        size_t romBytes = 0;
        size_t footerBytes = 0;
        size_t totalBytes = 0;
        std::string footerHex;
    };

    // Reads `inputRom` (.sfc/.smc), normalizes it, and writes `outputSfrom`.
    // Writes nothing at all when the ROM cannot be handled safely — the caller
    // can rely on the output file being absent on failure.
    SfromConversionResult ConvertToSfrom(const std::string& inputRom,
                                         const std::string& outputSfrom);

    // Structural check of a generated file, run before anything is installed:
    // trailer magic, self-consistent payload length, and a ROM body whose size
    // and cartridge header still match what was converted.
    bool ValidateSfrom(const std::string& path, const SnesRomInfo& expected, std::string& error);

    // The 256 zero bytes CaVE writes next to every .sfrom. All 28 reference
    // files are byte-identical, so this is generated rather than shipped.
    bool WriteSfromSig(const std::string& path, std::string& error);

}
