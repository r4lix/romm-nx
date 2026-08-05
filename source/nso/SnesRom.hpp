#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace romm::nso {

    enum class SnesMapping {
        Unknown,
        LoROM,
        HiROM,
        ExLoROM, // detected only so it can be rejected with a precise message
        ExHiROM
    };

    std::string MappingName(SnesMapping mapping);

    struct SnesRomInfo {
        bool valid = false;
        // Set when the image parses as a SNES ROM but is outside what the SNES
        // address space (and therefore Canoe) can map at all. Distinct from a
        // parse failure: the UI says "unsupported", not "invalid".
        bool unsupported = false;
        std::string error;

        // Non-fatal observations worth showing the user and recording in the
        // log: an enhancement chip, an unusual map mode byte, an odd size.
        // A ROM with warnings is still converted — the .sfrom body is the ROM
        // verbatim either way, and Canoe reads the cartridge header itself.
        std::vector<std::string> warnings;
        bool has_enhancement_chip = false;

        size_t original_size = 0;
        size_t rom_size = 0; // after copier-header removal
        bool had_copier_header = false;

        SnesMapping mapping = SnesMapping::Unknown;
        size_t header_offset = 0; // 0x7FC0 (LoROM) / 0xFFC0 (HiROM)
        int mapping_score = 0;

        std::string internal_title; // 21 bytes from the cartridge header, trimmed
        uint8_t map_mode = 0;
        uint8_t rom_type = 0;
        uint8_t rom_size_exp = 0;
        uint8_t sram_size_exp = 0;
        uint8_t country = 0;
        uint8_t version = 0;
        uint16_t checksum = 0;
        uint16_t checksum_complement = 0;
        bool checksum_ok = false;

        uint32_t crc32 = 0;
        std::string sha256; // lowercase hex of the normalized (headerless) ROM
    };

    // Analyses a raw .sfc/.smc image. `data` is modified in place only to the
    // extent of nothing — the caller gets the normalized offset via
    // `had_copier_header` and can drop the first 512 bytes itself.
    SnesRomInfo AnalyzeSnesRom(const std::vector<uint8_t>& data);

    // Human-readable name for a cartridge type byte, for the log and the error
    // message shown when a chip is not supported.
    std::string RomTypeName(uint8_t rom_type);

    uint32_t Crc32(const uint8_t* data, size_t length);
    std::string Sha256Hex(const uint8_t* data, size_t length);

    // Incremental form of the same hash, for input too large to hold in memory.
    // The N64 path streams a 64 MiB cartridge through byte-order conversion and
    // zlib in one pass; buffering it twice to hash it would double the peak.
    struct Sha256Stream {
        uint32_t state[8];
        uint64_t bit_len;
        uint8_t buffer[64];
        size_t buffer_len;

        Sha256Stream();
        void Update(const uint8_t* data, size_t length);
        std::string FinishHex();
    };

}
