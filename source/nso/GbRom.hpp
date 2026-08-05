#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace romm::nso {

    // Which emulation mode the Game Boy Switch Online app must run the ROM in.
    // One app covers both systems and each database entry says which, so this is
    // read out of the cartridge header rather than taken from the RomM platform:
    // a Color game can sit under either "gb" or "gbc" in a library, but byte
    // 0x143 is never wrong.
    enum class GbMode {
        Dmg, // original Game Boy — code "D-", rom ".gb",  "platform":"DMG"
        Cgb  // Game Boy Color    — code "C-", rom ".gbc", "platform":"CGB"
    };

    struct GbRomInfo {
        bool valid = false;
        bool unsupported = false;
        std::string error;
        std::vector<std::string> warnings;

        GbMode mode = GbMode::Dmg;
        bool cgb_only = false;   // 0xC0: refuses to run on a DMG at all
        bool sgb_support = false;

        size_t file_size = 0;
        size_t declared_rom_size = 0;
        uint8_t cartridge_type = 0;
        uint8_t ram_size_code = 0;
        uint8_t destination = 0; // 0x00 Japan, 0x01 everywhere else

        std::string internal_title;

        uint8_t header_checksum = 0;
        bool header_checksum_ok = false;
        // Big-endian word at 0x14E. This is what the title code is built from:
        // CaVE names an injected Game Boy title "<D|C>-<checksum in hex>_e", so
        // the same dump always lands on the same code, in CaVE and here alike.
        uint16_t global_checksum = 0;
        bool global_checksum_ok = false;

        std::string sha256; // of the file as it will be written
    };

    // Parses a .gb/.gbc image. The file is injected verbatim — Game Boy ROMs
    // carry no external header — so nothing here modifies the data.
    GbRomInfo AnalyzeGbRom(const std::vector<uint8_t>& data);

    // Human-readable cartridge type ("MBC1+RAM+BATTERY"), or "" when unknown.
    std::string GbCartridgeTypeName(uint8_t type);

    // "3DB7" — uppercase hex of the global checksum, the body of the code.
    std::string GbChecksumHex(uint16_t checksum);

}
