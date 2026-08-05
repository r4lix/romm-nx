#include "GbaRom.hpp"
#include "SnesRom.hpp" // Sha256Hex

#include <cctype>
#include <cstdio>

namespace romm::nso {

    namespace {

        constexpr size_t kTitleOffset = 0xA0;
        constexpr size_t kTitleSize = 12;
        constexpr size_t kGameCode = 0xAC;
        constexpr size_t kMakerCode = 0xB0;
        constexpr size_t kFixedByte = 0xB2; // always 0x96
        constexpr size_t kSoftwareVersion = 0xBC;
        constexpr size_t kHeaderChecksum = 0xBD;
        constexpr size_t kHeaderEnd = 0xC0;

        constexpr size_t kMaxCartridgeSize = 32u * 1024 * 1024;

        bool IsCodeChar(uint8_t c) {
            return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        }

    } // namespace

    GbaRomInfo AnalyzeGbaRom(const std::vector<uint8_t>& data) {
        GbaRomInfo info;
        info.file_size = data.size();

        if (data.size() < kHeaderEnd) {
            info.error = "File is too small to be a Game Boy Advance ROM";
            return info;
        }

        // 0xB2 is 0x96 on every commercial cartridge and is what identifies the
        // image. The header checksum is deliberately NOT part of this test: ROM
        // hacks and translations routinely leave it stale while running fine.
        if (data[kFixedByte] != 0x96) {
            info.error = "Missing the 0x96 marker in the cartridge header; "
                         "this does not look like a Game Boy Advance ROM";
            return info;
        }

        for (size_t i = 0; i < kTitleSize; ++i) {
            const uint8_t c = data[kTitleOffset + i];
            if (c == 0) break;
            if (c >= 0x20 && c < 0x7F) info.internal_title.push_back((char)c);
        }
        while (!info.internal_title.empty() && info.internal_title.back() == ' ') {
            info.internal_title.pop_back();
        }

        bool code_ok = true;
        for (size_t i = 0; i < 4; ++i) {
            const uint8_t c = data[kGameCode + i];
            if (!IsCodeChar(c)) { code_ok = false; break; }
            info.game_code.push_back((char)c);
        }
        if (!code_ok) {
            // Homebrew and some prototypes leave the field blank. The code still
            // has to be four characters, so fall back to the content hash rather
            // than writing an entry with an empty or unprintable code.
            info.game_code.clear();
            info.warnings.push_back("No game code in the header; one was derived from the ROM hash");
        }

        for (size_t i = 0; i < 2; ++i) {
            const uint8_t c = data[kMakerCode + i];
            if (c >= 0x20 && c < 0x7F) info.maker_code.push_back((char)c);
        }
        info.software_version = data[kSoftwareVersion];

        // chk = -(sum of 0xA0..0xBC) - 0x19, in 8 bits.
        uint8_t sum = 0;
        for (size_t i = kTitleOffset; i <= 0xBC; ++i) {
            sum = (uint8_t)(sum - data[i]);
        }
        sum = (uint8_t)(sum - 0x19);
        info.header_checksum = data[kHeaderChecksum];
        info.header_checksum_ok = (sum == info.header_checksum);
        if (!info.header_checksum_ok) {
            info.warnings.push_back("Header checksum mismatch; the ROM has been modified");
        }

        if (data.size() > kMaxCartridgeSize) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Larger than a 32 MiB cartridge (%zu bytes)", data.size());
            info.warnings.push_back(buf);
        }
        if ((data.size() & (data.size() - 1)) != 0) {
            info.warnings.push_back("Size is not a power of two; the dump may be trimmed");
        }

        info.sha256 = Sha256Hex(data.data(), data.size());
        if (info.game_code.empty()) {
            // Four characters from the hash, in the same A-Z/0-9 alphabet the
            // real codes use.
            for (size_t i = 0; i < 4 && i < info.sha256.size(); ++i) {
                const char c = (char)std::toupper((unsigned char)info.sha256[i]);
                info.game_code.push_back(c);
            }
            while (info.game_code.size() < 4) info.game_code.push_back('0');
        }

        info.valid = true;
        return info;
    }

}
