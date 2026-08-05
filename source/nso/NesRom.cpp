#include "NesRom.hpp"
#include "SnesRom.hpp" // Crc32 / Sha256Hex — platform-neutral, defined there first

#include <cstdio>
#include <cstring>

namespace romm::nso {

    namespace {

        // The 16-byte iNES header. Byte 4 is PRG in 16 KiB units, byte 5 CHR in
        // 8 KiB units; everything else is flags.
        constexpr size_t kHeaderSize = 16;
        constexpr size_t kTrainerSize = 512;
        constexpr size_t kPrgUnit = 16 * 1024;
        constexpr size_t kChrUnit = 8 * 1024;

        constexpr size_t kHdrPrgUnits = 4;
        constexpr size_t kHdrChrUnits = 5;
        constexpr size_t kHdrFlags6 = 6;
        constexpr size_t kHdrFlags7 = 7;
        constexpr size_t kHdrByte8 = 8;
        constexpr size_t kHdrByte9 = 9;

        // A raw FDS disk side is 65500 bytes; the headered form starts "FDS\x1a".
        constexpr size_t kFdsSideSize = 65500;

        bool HasMagic(const std::vector<uint8_t>& d, const char* magic) {
            const size_t n = std::strlen(magic);
            if (d.size() < n) return false;
            return std::memcmp(d.data(), magic, n) == 0;
        }

        // Bytes 12-15 of a clean iNES 1.0 header are zero. Old dumping tools
        // wrote their name across the header's tail, and byte 7's high nibble is
        // the mapper's high nibble — so an unclean header means that nibble is
        // text, not a mapper. The usual fix, and the one every emulator applies:
        // ignore the high nibble.
        //
        // NES 2.0 must be excluded from this, which is why the caller passes the
        // flag: 2.0 gives bytes 12-15 real meanings (CPU/PPU timing, console
        // type, misc ROM count, expansion device), so a perfectly clean 2.0
        // header routinely has them set. The first NES ROM CaVE injected on this
        // console is exactly that case — byte 12 = 0x01, PAL timing.
        bool HeaderIsDirty(const std::vector<uint8_t>& d, bool nes20) {
            if (nes20) return false;
            for (size_t i = 12; i < kHeaderSize; ++i) {
                if (d[i] != 0) return true;
            }
            return false;
        }

    } // namespace

    std::string NesContainerName(NesContainer container) {
        switch (container) {
            case NesContainer::INes:  return "iNES";
            case NesContainer::Nes20: return "NES 2.0";
            case NesContainer::Unif:  return "UNIF";
            case NesContainer::Fds:   return "FDS disk image";
            default:                  return "unknown";
        }
    }

    std::string NesMapperName(int mapper) {
        switch (mapper) {
            case 0:   return "NROM";
            case 1:   return "MMC1";
            case 2:   return "UxROM";
            case 3:   return "CNROM";
            case 4:   return "MMC3";
            case 5:   return "MMC5";
            case 7:   return "AxROM";
            case 9:   return "MMC2";
            case 10:  return "MMC4";
            case 11:  return "Color Dreams";
            case 16:  return "Bandai FCG";
            case 18:  return "Jaleco SS88006";
            case 19:  return "Namco 163";
            case 21:
            case 23:
            case 25:  return "VRC4";
            case 22:  return "VRC2";
            case 24:
            case 26:  return "VRC6";
            case 33:  return "Taito TC0190";
            case 34:  return "BNROM/NINA-001";
            case 66:  return "GxROM";
            case 69:  return "Sunsoft FME-7";
            case 71:  return "Camerica";
            case 73:  return "VRC3";
            case 75:  return "VRC1";
            case 85:  return "VRC7";
            case 118: return "TxSROM";
            case 119: return "TQROM";
            default:  return "";
        }
    }

    NesRomInfo AnalyzeNesRom(const std::vector<uint8_t>& data) {
        NesRomInfo info;
        info.file_size = data.size();

        if (data.size() < kHeaderSize + kPrgUnit) {
            info.error = "File is too small to be a NES ROM";
            return info;
        }

        // The iNES magic is checked FIRST. The other containers are identified
        // partly by size, and a headered ROM that happens to divide evenly by a
        // disk side would otherwise be reported as an FDS image.
        if (!HasMagic(data, "NES\x1a")) {
            // Containers that parse but cannot be injected. Named explicitly:
            // "not a NES ROM" about a perfectly good FDS dump sends the user
            // hunting for a corrupt download that isn't there.
            if (HasMagic(data, "UNIF")) {
                info.container = NesContainer::Unif;
                info.unsupported = true;
                info.error = "UNIF container — convert it to iNES (.nes) first";
                return info;
            }
            if (HasMagic(data, "FDS\x1a") || (data.size() % kFdsSideSize) == 0) {
                info.container = NesContainer::Fds;
                info.unsupported = true;
                info.error = "Famicom Disk System image — the NES app only takes cartridge ROMs";
                return info;
            }
            // Headerless dumps exist, but the header is the only thing that says
            // where PRG ends and CHR begins. Guessing that split wrong produces a
            // file the app accepts and then renders as garbage, so it is refused
            // rather than inferred.
            info.error = "No iNES header — the .nes file must keep its 16-byte header";
            return info;
        }

        info.header_size = kHeaderSize;

        const uint8_t flags6 = data[kHdrFlags6];
        const uint8_t flags7 = data[kHdrFlags7];

        // NES 2.0 announces itself in bits 2-3 of byte 7. Its PRG/CHR sizes take
        // four extra high bits from byte 9, which is how a 2 MiB cartridge fits
        // in a field that otherwise caps at 4 MiB/2 MiB.
        const bool nes20 = ((flags7 & 0x0C) == 0x08);
        info.container = nes20 ? NesContainer::Nes20 : NesContainer::INes;
        info.dirty_header = HeaderIsDirty(data, nes20);

        size_t prg_units = data[kHdrPrgUnits];
        size_t chr_units = data[kHdrChrUnits];
        // NES 2.0 overloads the size fields: a high nibble of 0xF switches that
        // field from a unit count to 2^E * (2*MM+1) bytes. Rare, but a ROM using
        // it would otherwise compute an absurd size and get rejected as
        // truncated — so the exponent form is decoded, and when it appears the
        // size cross-check below is skipped rather than trusted.
        bool exponent_size = false;
        if (nes20) {
            const size_t prg_hi = static_cast<size_t>(data[kHdrByte9] & 0x0F);
            const size_t chr_hi = static_cast<size_t>(data[kHdrByte9] & 0xF0) >> 4;
            if (prg_hi == 0x0F || chr_hi == 0x0F) {
                exponent_size = true;
                info.warnings.push_back("NES 2.0 exponent-form ROM size; size check skipped");
            } else {
                prg_units |= prg_hi << 8;
                chr_units |= chr_hi << 8;
            }
        }
        info.prg_size = prg_units * kPrgUnit;
        info.chr_size = chr_units * kChrUnit;
        info.chr_ram = (chr_units == 0);

        info.trainer_size = (flags6 & 0x04) ? kTrainerSize : 0;
        info.battery = (flags6 & 0x02) != 0;
        info.vertical_mirroring = (flags6 & 0x01) != 0;
        info.four_screen = (flags6 & 0x08) != 0;
        info.vs_unisystem = (flags7 & 0x01) != 0;
        info.playchoice10 = (flags7 & 0x02) != 0;

        info.mapper = (flags6 >> 4) & 0x0F;
        if (!info.dirty_header) {
            info.mapper |= (flags7 & 0xF0);
        }
        if (nes20) {
            info.mapper |= static_cast<int>(data[kHdrByte8] & 0x0F) << 8;
            info.submapper = (data[kHdrByte8] >> 4) & 0x0F;
        }

        if (info.prg_size == 0 && !exponent_size) {
            info.error = "Header declares no PRG ROM";
            return info;
        }

        const size_t expected = kHeaderSize + info.trainer_size + info.prg_size + info.chr_size;
        if (exponent_size) {
            // Sizes are not comparable; take the file as it stands.
            info.prg_size = data.size() - kHeaderSize - info.trainer_size;
            info.chr_size = 0;
        } else if (data.size() < expected) {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "Truncated: header declares %zu bytes, file has %zu",
                          expected, data.size());
            info.error = buf;
            return info;
        } else if (data.size() > expected) {
            // Trailing data is normal enough (a ROM padded by a copier, or one
            // carrying an appended title block) that it is a note, not a
            // rejection — the app reads sizes from the header, as we just did.
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                          "%zu trailing bytes after the declared ROM data",
                          data.size() - expected);
            info.warnings.push_back(buf);
        }

        if (info.dirty_header) {
            info.warnings.push_back("Header has a dumper signature in its unused bytes; "
                                    "the mapper's high nibble was ignored");
        }
        if (info.trainer_size != 0) {
            info.warnings.push_back("ROM carries a 512-byte trainer");
        }
        if (info.vs_unisystem) {
            info.warnings.push_back("VS. UniSystem arcade ROM");
        }
        if (info.playchoice10) {
            info.warnings.push_back("PlayChoice-10 ROM");
        }

        // The app's own catalogue is almost entirely NROM/MMC1/UxROM/CNROM/MMC3
        // plus a handful of VRC and Sunsoft titles. Anything outside the common
        // set is flagged so a game that boots to a black screen has an
        // explanation waiting in the log — but it is still converted, for the
        // same reason the SNES side stopped gating on cartridge type.
        switch (info.mapper) {
            case 0: case 1: case 2: case 3: case 4: case 7: case 9: case 10:
                break;
            default: {
                const std::string name = NesMapperName(info.mapper);
                char buf[160];
                if (name.empty()) {
                    std::snprintf(buf, sizeof(buf), "Uncommon mapper %d", info.mapper);
                } else {
                    std::snprintf(buf, sizeof(buf), "Uncommon mapper %d (%s)",
                                  info.mapper, name.c_str());
                }
                info.warnings.push_back(buf);
                break;
            }
        }

        // Two hashes, two jobs. CRC32 of the body without the header is the
        // No-Intro identity, so it can be matched against a dump database; the
        // SHA-256 covers the file byte for byte as it will be written, which is
        // what the injected-games index needs to recognise a reinstall.
        const size_t body_off = kHeaderSize + info.trainer_size;
        info.crc32 = Crc32(data.data() + body_off, data.size() - body_off);
        info.sha256 = Sha256Hex(data.data(), data.size());

        info.valid = true;
        return info;
    }

}
