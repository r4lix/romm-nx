#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace romm::nso {

    // Container the image arrived in. Only iNES/NES 2.0 can be injected — the
    // others are recognised solely so the error names them instead of saying
    // "not a NES ROM" about a file that plainly is one.
    enum class NesContainer {
        Unknown,
        INes,     // "NES\x1a", the universal case
        Nes20,    // iNES with the 2.0 extensions; a superset, injected the same way
        Unif,     // "UNIF"
        Fds       // "FDS\x1a" or a raw 65500-byte disk image
    };

    std::string NesContainerName(NesContainer container);

    struct NesRomInfo {
        bool valid = false;
        // Parses as a NES image, but not one this pipeline can hand to the app
        // (a disk image, a UNIF container). Distinct from a parse failure so the
        // UI can say "unsupported" rather than "invalid".
        bool unsupported = false;
        std::string error;

        // Non-fatal observations, shown on screen and written to the log. The
        // policy here matches the SNES side deliberately: convert anything with
        // a credible header and record the oddities, because the app reads the
        // header itself and guessing at what its emulator refuses is how the
        // first SNES build rejected three games that work.
        std::vector<std::string> warnings;

        NesContainer container = NesContainer::Unknown;
        size_t file_size = 0;
        size_t header_size = 0;   // 16 for iNES, 0 if ever headerless
        size_t trainer_size = 0;  // 512 when flags6 bit 2 is set
        size_t prg_size = 0;
        size_t chr_size = 0;
        bool chr_ram = false;     // chr_size == 0: the cartridge had CHR RAM

        int mapper = 0;
        int submapper = 0;        // NES 2.0 only, 0 otherwise
        bool battery = false;
        bool four_screen = false;
        bool vertical_mirroring = false;
        bool vs_unisystem = false;
        bool playchoice10 = false;

        // Some dumps carry a 128-byte "DiskDude!"-style signature in the unused
        // tail of the header, which pollutes the mapper's high nibble. Recorded
        // because it changes how the mapper number is read, not just as trivia.
        bool dirty_header = false;

        uint32_t crc32 = 0;   // of the ROM body, header excluded — the No-Intro identity
        std::string sha256;   // of the file exactly as it will be written
    };

    // Analyses a .nes image. Never modifies the data: the file is injected
    // verbatim, so anything the caller needs to know about its shape has to come
    // back through the struct.
    NesRomInfo AnalyzeNesRom(const std::vector<uint8_t>& data);

    // Human-readable name for the common mappers, for the log line and the
    // warning shown when a ROM uses something unusual. Returns "" for a mapper
    // with no well-known name, which the caller renders as the bare number.
    std::string NesMapperName(int mapper);

}
