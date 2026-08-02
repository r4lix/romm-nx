#include "SfromWriter.hpp"

#include "NsoLog.hpp"

#include <cstdio>
#include <cstring>

namespace romm::nso {

    namespace {

        // The default Canoe payload, verbatim from the reference titles:
        //   0x0247, 0x0000, 0x1000, 0x0774, 0x0270
        // The third token is the slot CaVE varies per game when it applies one
        // of its emulator presets/patches; 0x1000 is its "no preset" value and
        // is what 24 of the 28 reference titles use.
        constexpr uint16_t kDefaultTokens[] = {0x0247, 0x0000, 0x1000, 0x0774, 0x0270};
        constexpr uint32_t kNoPresetToken = 0x1000;
        constexpr char kMagic[4] = {'C', 'a', 'n', '1'};

        // Per-game Canoe presets, lifted byte-for-byte out of CaVE's own
        // converted ROMs and keyed by the SHA-256 of the headerless ROM body.
        // These are the only six titles in that reference set whose payload
        // differs from the default; each varies the third token (the preset
        // slot) and two append further tokens.
        //
        // This is a lookup of measured values, not a guess: a ROM that hashes
        // to one of these is the exact dump CaVE converted, so the output is
        // byte-identical to what CaVE would have produced. Anything else gets
        // the default payload.
        struct CanoePreset {
            const char* sha256;      // of the normalized ROM body
            const char* description;
            uint8_t payload[24];
            size_t payload_length;
        };

        const CanoePreset kPresets[] = {
            {"1c12660c99571692d2fba4ba871a1086b115486697e789f85fb939c55eeec7c7",
             "Doom (Europe) - Super FX preset",
             {0x47, 0x02, 0x00, 0x00, 0x00, 0x10, 0x65, 0x0C, 0x74, 0x07, 0x70, 0x02}, 12},

            {"06d1c2b06b716052c5596aaa0c2e5632a027fee1a9a28439e509f813c30829a9",
             "Chrono Trigger (USA)",
             {0x47, 0x02, 0x00, 0x00, 0x0B, 0x11, 0x74, 0x07, 0x70, 0x02, 0x6A, 0x20,
              0x50, 0x04, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00}, 20},

            {"10eccc5d2fab81346dd759f6be478dcb682eef981e8d3d662da176e1f9a996bc",
             "Final Fantasy III (USA) (Rev 1)",
             {0x47, 0x02, 0x00, 0x00, 0xDC, 0x10, 0x76, 0x5A, 0x74, 0x07, 0x70, 0x02}, 12},

            {"69c19b76188476c6e77b65c14a89e75334b589fecafa61a087b09d1e6769880b",
             "Final Fantasy II (USA, Europe) (Rev 1)",
             {0x47, 0x02, 0x00, 0x00, 0x95, 0x10, 0x74, 0x07, 0x70, 0x02}, 10},

            {"f71817f55febd32fd1dce617a326a77b6b062dd0d4058ecd289f64af1b7a1d05",
             "Final Fantasy: Mystic Quest (USA) (Rev 1)",
             {0x47, 0x02, 0x00, 0x00, 0x03, 0x11, 0x74, 0x07, 0x70, 0x02}, 10},

            {"a3f44c2db8877e11eaeb58ad7e6d1bb8f2e845fd06e8e036f0d8f3baf6df1981",
             "Castlevania: Vampire's Kiss (Europe)",
             {0x47, 0x02, 0x00, 0x00, 0x32, 0x11, 0x74, 0x07, 0x70, 0x02}, 10}
        };

        const CanoePreset* FindPreset(const std::string& sha256) {
            for (const auto& preset : kPresets) {
                if (sha256 == preset.sha256) return &preset;
            }
            return nullptr;
        }

        void PushU16(std::vector<uint8_t>& out, uint16_t v) {
            out.push_back(static_cast<uint8_t>(v & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        }

        void PushU32(std::vector<uint8_t>& out, uint32_t v) {
            out.push_back(static_cast<uint8_t>(v & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        }

        std::string ToHex(const std::vector<uint8_t>& bytes) {
            static const char* hex = "0123456789ABCDEF";
            std::string out;
            out.reserve(bytes.size() * 3);
            for (size_t i = 0; i < bytes.size(); ++i) {
                if (i) out.push_back(' ');
                out.push_back(hex[(bytes[i] >> 4) & 0xF]);
                out.push_back(hex[bytes[i] & 0xF]);
            }
            return out;
        }

        bool ReadWholeFile(const std::string& path, std::vector<uint8_t>& out, std::string& error) {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) {
                error = "cannot open " + path;
                return false;
            }
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size <= 0) {
                std::fclose(f);
                error = "file is empty: " + path;
                return false;
            }
            out.resize(static_cast<size_t>(size));
            const size_t read = std::fread(out.data(), 1, out.size(), f);
            std::fclose(f);
            if (read != out.size()) {
                error = "short read on " + path;
                return false;
            }
            return true;
        }

    } // namespace

    SfromConversionResult ConvertToSfrom(const std::string& inputRom,
                                         const std::string& outputSfrom) {
        SfromConversionResult result;
        result.outputPath = outputSfrom;

        auto& log = NsoLog::Instance();

        std::vector<uint8_t> raw;
        std::string io_error;
        if (!ReadWholeFile(inputRom, raw, io_error)) {
            result.error = io_error;
            return result;
        }

        const SnesRomInfo info = AnalyzeSnesRom(raw);
        result.rom = info;
        result.mapping = MappingName(info.mapping);

        log.KV("rom.file", inputRom);
        log.KV("rom.size_before_header_removal", std::to_string(info.original_size));
        log.KV("rom.copier_header", info.had_copier_header ? "yes (512 bytes stripped)" : "no");
        log.KV("rom.size_after_header_removal", std::to_string(info.rom_size));
        log.KV("rom.mapping", result.mapping + " (confidence " + std::to_string(info.mapping_score) + ")");
        log.KV("rom.internal_title", info.internal_title);
        {
            char buf[96];
            std::snprintf(buf, sizeof(buf),
                          "map_mode=0x%02X rom_type=0x%02X (%s) size_exp=%u sram_exp=%u country=0x%02X ver=%u",
                          info.map_mode, info.rom_type, RomTypeName(info.rom_type).c_str(),
                          (unsigned)info.rom_size_exp, (unsigned)info.sram_size_exp,
                          info.country, (unsigned)info.version);
            log.KV("rom.header", buf);
            std::snprintf(buf, sizeof(buf), "checksum=0x%04X complement=0x%04X %s",
                          info.checksum, info.checksum_complement,
                          info.checksum_ok ? "(consistent)" : "(INCONSISTENT)");
            log.KV("rom.checksum", buf);
            std::snprintf(buf, sizeof(buf), "0x%08X", info.crc32);
            log.KV("rom.crc32", buf);
        }
        log.KV("rom.sha256", info.sha256);

        if (!info.valid) {
            result.error = info.error;
            return result;
        }

        for (const std::string& warning : info.warnings) {
            log.KV("rom.warning", warning);
        }

        const size_t rom_offset = info.had_copier_header ? 512u : 0u;

        std::vector<uint8_t> payload;
        if (const CanoePreset* preset = FindPreset(info.sha256)) {
            payload.assign(preset->payload, preset->payload + preset->payload_length);
            result.presetId = static_cast<uint32_t>(payload[4] | (payload[5] << 8));
            log.KV("sfrom.preset", std::string("matched \"") + preset->description +
                                  "\" by ROM hash - using CaVE's exact payload");
        } else {
            for (uint16_t token : kDefaultTokens) PushU16(payload, token);
            result.presetId = kNoPresetToken;
            log.KV("sfrom.preset", "0x1000 (Canoe default, no per-game preset)");
            if (info.has_enhancement_chip) {
                log.KV("sfrom.preset_note",
                       "this cartridge has an enhancement chip and no reference preset is known "
                       "for this dump; converting with the default payload");
            }
        }

        std::vector<uint8_t> footer = payload;
        PushU32(footer, static_cast<uint32_t>(payload.size()));
        footer.insert(footer.end(), kMagic, kMagic + 4);
        result.romBytes = info.rom_size;
        result.footerBytes = footer.size();
        result.totalBytes = info.rom_size + footer.size();
        result.footerHex = ToHex(footer);

        log.KV("sfrom.payload_bytes", std::to_string(payload.size()));
        log.KV("sfrom.footer", result.footerHex);
        log.KV("sfrom.total_bytes", std::to_string(result.totalBytes));

        // Write to a .part first so a torn write can never look like a finished
        // conversion to the installer's existence check.
        const std::string part = outputSfrom + ".part";
        std::remove(part.c_str());
        FILE* out = std::fopen(part.c_str(), "wb");
        if (!out) {
            result.error = "cannot create " + part;
            return result;
        }

        bool ok = std::fwrite(raw.data() + rom_offset, 1, info.rom_size, out) == info.rom_size;
        if (ok) ok = std::fwrite(footer.data(), 1, footer.size(), out) == footer.size();
        if (std::fclose(out) != 0) ok = false;

        if (!ok) {
            std::remove(part.c_str());
            result.error = "write failed (SD card full or read-only?)";
            return result;
        }

        std::remove(outputSfrom.c_str()); // sdmc: rename() refuses an existing target
        if (std::rename(part.c_str(), outputSfrom.c_str()) != 0) {
            std::remove(part.c_str());
            result.error = "cannot finalize " + outputSfrom;
            return result;
        }

        std::string validate_error;
        if (!ValidateSfrom(outputSfrom, info, validate_error)) {
            std::remove(outputSfrom.c_str());
            result.error = "generated file failed validation: " + validate_error;
            return result;
        }

        result.success = true;
        return result;
    }

    bool ValidateSfrom(const std::string& path, const SnesRomInfo& expected, std::string& error) {
        std::vector<uint8_t> data;
        if (!ReadWholeFile(path, data, error)) return false;

        if (data.size() < 8 + 4) {
            error = "file is too small to contain a Canoe trailer";
            return false;
        }
        if (std::memcmp(data.data() + data.size() - 4, kMagic, 4) != 0) {
            error = "missing 'Can1' trailer magic";
            return false;
        }

        const size_t len_off = data.size() - 8;
        const uint32_t payload_len = static_cast<uint32_t>(data[len_off]) |
                                     (static_cast<uint32_t>(data[len_off + 1]) << 8) |
                                     (static_cast<uint32_t>(data[len_off + 2]) << 16) |
                                     (static_cast<uint32_t>(data[len_off + 3]) << 24);
        if (payload_len + 8u > data.size()) {
            error = "trailer payload length " + std::to_string(payload_len) + " overruns the file";
            return false;
        }

        const size_t rom_bytes = data.size() - 8 - payload_len;
        if (rom_bytes != expected.rom_size) {
            error = "ROM body is " + std::to_string(rom_bytes) + " bytes, expected " +
                    std::to_string(expected.rom_size);
            return false;
        }

        // The cartridge header must still be where the analysis said it was —
        // catches a truncated or misaligned copy that happens to be the right
        // total length.
        if (expected.header_offset + 0x20 > rom_bytes) {
            error = "cartridge header offset lies outside the written ROM body";
            return false;
        }
        if (data[expected.header_offset + 0x15] != expected.map_mode) {
            error = "map mode byte at the cartridge header does not match the analysed ROM";
            return false;
        }

        return true;
    }

    bool WriteSfromSig(const std::string& path, std::string& error) {
        static const uint8_t kZeros[256] = {0};
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) {
            error = "cannot create " + path;
            return false;
        }
        const bool ok = std::fwrite(kZeros, 1, sizeof(kZeros), f) == sizeof(kZeros);
        if (std::fclose(f) != 0 || !ok) {
            std::remove(path.c_str());
            error = "write failed for " + path;
            return false;
        }
        return true;
    }

}
