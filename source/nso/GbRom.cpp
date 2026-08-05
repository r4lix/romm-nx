#include "GbRom.hpp"
#include "SnesRom.hpp" // Sha256Hex

#include <cstdio>
#include <cstring>

namespace romm::nso {

    namespace {

        constexpr size_t kLogoOffset = 0x104;
        constexpr size_t kLogoSize = 48;
        constexpr size_t kTitleOffset = 0x134;
        constexpr size_t kCgbFlag = 0x143;
        constexpr size_t kSgbFlag = 0x146;
        constexpr size_t kCartType = 0x147;
        constexpr size_t kRomSize = 0x148;
        constexpr size_t kRamSize = 0x149;
        constexpr size_t kDestination = 0x14A;
        constexpr size_t kHeaderChecksum = 0x14D;
        constexpr size_t kGlobalChecksum = 0x14E;
        constexpr size_t kHeaderEnd = 0x150;

        // The boot ROM refuses to run a cartridge whose logo does not match, so
        // this doubles as "is this actually a Game Boy ROM".
        const uint8_t kNintendoLogo[kLogoSize] = {
            0xCE, 0xED, 0x66, 0x66, 0xCC, 0x0D, 0x00, 0x0B, 0x03, 0x73, 0x00, 0x83,
            0x00, 0x0C, 0x00, 0x0D, 0x00, 0x08, 0x11, 0x1F, 0x88, 0x89, 0x00, 0x0E,
            0xDC, 0xCC, 0x6E, 0xE6, 0xDD, 0xDD, 0xD9, 0x99, 0xBB, 0xBB, 0x67, 0x63,
            0x6E, 0x0E, 0xEC, 0xCC, 0xDD, 0xDC, 0x99, 0x9F, 0xBB, 0xB9, 0x33, 0x3E
        };

    } // namespace

    std::string GbCartridgeTypeName(uint8_t type) {
        switch (type) {
            case 0x00: return "ROM ONLY";
            case 0x01: return "MBC1";
            case 0x02: return "MBC1+RAM";
            case 0x03: return "MBC1+RAM+BATTERY";
            case 0x05: return "MBC2";
            case 0x06: return "MBC2+BATTERY";
            case 0x0F: return "MBC3+TIMER+BATTERY";
            case 0x10: return "MBC3+TIMER+RAM+BATTERY";
            case 0x11: return "MBC3";
            case 0x12: return "MBC3+RAM";
            case 0x13: return "MBC3+RAM+BATTERY";
            case 0x19: return "MBC5";
            case 0x1A: return "MBC5+RAM";
            case 0x1B: return "MBC5+RAM+BATTERY";
            case 0x1C: return "MBC5+RUMBLE";
            case 0x1D: return "MBC5+RUMBLE+RAM";
            case 0x1E: return "MBC5+RUMBLE+RAM+BATTERY";
            case 0x20: return "MBC6";
            case 0x22: return "MBC7+SENSOR+RUMBLE+RAM+BATTERY";
            case 0xFC: return "POCKET CAMERA";
            case 0xFD: return "BANDAI TAMA5";
            case 0xFE: return "HuC3";
            case 0xFF: return "HuC1+RAM+BATTERY";
            default:   return "";
        }
    }

    std::string GbChecksumHex(uint16_t checksum) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "%04X", (unsigned)checksum);
        return buf;
    }

    GbRomInfo AnalyzeGbRom(const std::vector<uint8_t>& data) {
        GbRomInfo info;
        info.file_size = data.size();

        if (data.size() < kHeaderEnd) {
            info.error = "File is too small to be a Game Boy ROM";
            return info;
        }

        if (std::memcmp(data.data() + kLogoOffset, kNintendoLogo, kLogoSize) != 0) {
            // Not a rejection on its own — a few homebrew and prototype dumps
            // carry a patched logo — but it is the strongest single signal that
            // this is not a Game Boy ROM at all, so it is reported as an error
            // rather than a warning. A ROM that fails this would also refuse to
            // boot on hardware.
            info.error = "Nintendo logo missing from the cartridge header; "
                         "this does not look like a Game Boy ROM";
            return info;
        }

        // 0x143 belongs to the title on pre-Color cartridges, which is why the
        // title is only read up to 0x142 when the byte is a Color flag.
        const uint8_t cgb = data[kCgbFlag];
        const bool colour = (cgb == 0x80 || cgb == 0xC0);
        info.mode = colour ? GbMode::Cgb : GbMode::Dmg;
        info.cgb_only = (cgb == 0xC0);

        const size_t title_end = colour ? kCgbFlag : (kCgbFlag + 1);
        for (size_t i = kTitleOffset; i < title_end; ++i) {
            const uint8_t c = data[i];
            if (c == 0) break;
            if (c >= 0x20 && c < 0x7F) info.internal_title.push_back((char)c);
        }
        while (!info.internal_title.empty() && info.internal_title.back() == ' ') {
            info.internal_title.pop_back();
        }

        info.sgb_support = (data[kSgbFlag] == 0x03);
        info.cartridge_type = data[kCartType];
        info.ram_size_code = data[kRamSize];
        info.destination = data[kDestination];

        const uint8_t size_code = data[kRomSize];
        if (size_code <= 0x08) {
            info.declared_rom_size = (size_t)(32 * 1024) << size_code;
        }

        // Header checksum: x = x - byte - 1 across 0x134..0x14C. A cartridge
        // failing this does not boot on hardware, so it is worth saying.
        uint8_t sum = 0;
        for (size_t i = kTitleOffset; i <= 0x14C; ++i) {
            sum = (uint8_t)(sum - data[i] - 1);
        }
        info.header_checksum = data[kHeaderChecksum];
        info.header_checksum_ok = (sum == info.header_checksum);

        // Global checksum: every byte except the two holding it. Real cartridges
        // do not verify it, but it is what the title code is built from, so a
        // mismatch is worth recording — it means the dump was modified.
        uint32_t global = 0;
        for (size_t i = 0; i < data.size(); ++i) {
            if (i == kGlobalChecksum || i == kGlobalChecksum + 1) continue;
            global += data[i];
        }
        info.global_checksum = (uint16_t)((data[kGlobalChecksum] << 8) | data[kGlobalChecksum + 1]);
        info.global_checksum_ok = ((uint16_t)(global & 0xFFFF) == info.global_checksum);

        if (!info.header_checksum_ok) {
            info.warnings.push_back("Header checksum mismatch; this dump would not boot on hardware");
        }
        if (!info.global_checksum_ok) {
            info.warnings.push_back("Global checksum mismatch; the dump has been modified "
                                    "(the title code is taken from it regardless)");
        }
        if (info.declared_rom_size != 0 && info.declared_rom_size != data.size()) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "Header declares %zu bytes, file is %zu",
                          info.declared_rom_size, data.size());
            info.warnings.push_back(buf);
        }
        if (info.cgb_only) {
            info.warnings.push_back("Color-only cartridge (0xC0)");
        }
        if (info.sgb_support) {
            info.warnings.push_back("Super Game Boy features present");
        }
        if (GbCartridgeTypeName(info.cartridge_type).empty()) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Unknown cartridge type 0x%02X",
                          (unsigned)info.cartridge_type);
            info.warnings.push_back(buf);
        }

        info.sha256 = Sha256Hex(data.data(), data.size());
        info.valid = true;
        return info;
    }

}
