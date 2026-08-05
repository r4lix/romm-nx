#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace romm::nso {

    // Byte order of an N64 dump. Unlike every other platform handled so far,
    // the same cartridge ships in three interchangeable-looking layouts, and a
    // file in the wrong one is not rejected by anything — it simply boots to
    // garbage. Detection is by the 4-byte magic, which is the same header word
    // read three ways.
    enum class N64Format {
        Unknown,
        BigEndian,    // .z64  80 37 12 40  — native cartridge order
        ByteSwapped,  // .v64  37 80 40 12  — Doctor V64 / 16-bit swapped
        LittleEndian  // .n64  40 12 37 80  — 32-bit word swapped
    };

    const char* N64FormatName(N64Format format);

    struct N64RomInfo {
        bool valid = false;
        bool unsupported = false;
        std::string error;
        std::vector<std::string> warnings;

        N64Format source_format = N64Format::Unknown;
        // True when the source was not already big-endian, i.e. the bytes
        // written differ from the bytes downloaded.
        bool converted = false;

        size_t file_size = 0;
        std::string internal_name;   // 20 bytes at 0x20, space padded
        std::string cartridge_id;    // 2 chars at 0x3B ("SM" for Super Mario 64)
        char country_code = 0;       // 0x3E: 'E' USA, 'P' Europe, 'J' Japan...
        uint8_t version = 0;         // 0x3F
        uint32_t crc1 = 0;           // 0x10 — the usual dump identity
        uint32_t crc2 = 0;           // 0x14
        // 0x08. The address the boot segment is loaded at, which is what makes
        // ROM 0x1000 onwards addressable: ROM offset 0x1000 IS entry_point, 1:1.
        // ScanN64BootIdle needs it, and nothing else does.
        uint32_t entry_point = 0;

        // Of the NORMALIZED bytes, not the download: two dumps of one cartridge
        // in different byte orders must land on the same entry.
        std::string sha256;
    };

    // Analyses a .z64/.v64/.n64 image. Does not modify `data`.
    N64RomInfo AnalyzeN64Rom(const std::vector<uint8_t>& data);

    // Rewrites `data` into big-endian (.z64) order. A source already in that
    // order is copied verbatim. Returns false only for a size that cannot be
    // whole words, which AnalyzeN64Rom rejects first.
    bool NormalizeN64Rom(const std::vector<uint8_t>& data, std::vector<uint8_t>& out,
                         std::string& error);

    // "CRC1 in uppercase hex", for the log.
    std::string N64Crc1Hex(uint32_t crc1);

    // Region name for the country byte at 0x3E ("USA", "Europe", "Japan"...).
    const char* N64CountryName(char code);

    // Whether this dump is the NTSC-U build. Worth knowing before an injection,
    // not because the app cares — it runs any region — but because the
    // community MetaPacks an N64 title needs to boot are published almost
    // entirely for US ROMs. Measured on the compatibility list: ~135 US titles
    // have one against ~9 European.
    inline bool IsN64UsRegion(char country_code) { return country_code == 'E'; }

    // Whether this dump is a 50 Hz build. A MetaPack for one carries
    // OptionInfo.PAL = 1, and a PAL ROM run without it gets NTSC video timing —
    // which is one of the ways an injected title reaches the emulator and then
    // sits on a black screen. Confirmed in the community pack for the European
    // Ocarina of Time: it is the only field distinguishing it from a US pack.
    //
    // Brazil ('B') is PAL-M at 60 Hz and Canada ('N') is NTSC, so neither
    // belongs here despite not being 'E'.
    inline bool IsN64PalRegion(char country_code) {
        switch (country_code) {
            case 'P': // Europe
            case 'D': // Germany
            case 'F': // France
            case 'I': // Italy
            case 'S': // Spain
            case 'U': // Australia
            case 'H': // Netherlands
            case 'X': case 'Y': // Europe, alternate codes
                return true;
            default:
                return false;
        }
    }

    // One entry for a MetaPack's "Idle" array: where the emulator may stop
    // interpreting and jump to the next scheduled event.
    struct N64IdleLoop {
        uint32_t addr = 0;
        uint32_t inst = 0;
    };

    // Finds idle loops in the boot segment, which is the only part of a ROM
    // whose runtime address is knowable statically (ROM 0x1000 maps 1:1 onto
    // the header's entry address; overlays loaded later do not).
    //
    // Accepts ONLY an unconditional self-branch with a nop in its delay slot —
    // `beq $zero,$zero,-1` + `nop`, i.e. 1000FFFF 00000000. That restriction is
    // what makes emitting the result safe rather than a guess: such a loop has
    // no exit other than an interrupt, so telling the emulator to fast-forward
    // out of it cannot skip work the game needed to do. A conditional branch
    // can leave on its own and must never be guessed at.
    //
    // Validated against the community pack for Ocarina of Time (Master Quest,
    // Europe), which names 0x80000810: the scan returns it, first of two.
    // Doom 64 and Resident Evil 2 yield nothing, and correctly so — their packs'
    // idle addresses are a conditional branch and overlay code respectively.
    // No candidates means no Idle array, not a fabricated one.
    std::vector<N64IdleLoop> ScanN64BootIdle(const std::string& source_path,
                                             N64Format format, uint32_t entry_point);

    // Reads only the header of `source_path` — enough to identify the byte
    // order and read the cartridge fields — without loading the ROM.
    N64RomInfo InspectN64RomFile(const std::string& source_path);

    // The N64 app stores ROMs as a raw zlib stream named "<CODE>.bnz" while the
    // database entry points at "<CODE>.bin". Verified against CaVE's own
    // output: 78 DA, a 64 MiB big-endian ROM inside.
    //
    // Streams source -> big-endian -> zlib -> destination in bounded memory,
    // filling `out_sha256` with the hash of the NORMALIZED bytes on the way
    // past. A 64 MiB cartridge held twice over is not something to do on a
    // console that may be running in applet mode.
    bool ConvertN64RomToBnz(const std::string& source_path, const std::string& out_path,
                            N64Format source_format, std::string& out_sha256,
                            std::string& error);

}
