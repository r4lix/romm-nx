#include "SnesRom.hpp"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace romm::nso {

    namespace {

        // Offsets inside the 64-byte SNES cartridge header, relative to its
        // base (0x7FC0 for LoROM, 0xFFC0 for HiROM).
        constexpr size_t kHdrTitle = 0x00;      // 21 bytes
        constexpr size_t kHdrMapMode = 0x15;
        constexpr size_t kHdrRomType = 0x16;
        constexpr size_t kHdrRomSize = 0x17;
        constexpr size_t kHdrSramSize = 0x18;
        constexpr size_t kHdrCountry = 0x19;
        constexpr size_t kHdrVersion = 0x1B;
        constexpr size_t kHdrChecksumComp = 0x1C;
        constexpr size_t kHdrChecksum = 0x1E;
        constexpr size_t kHdrEmuResetVec = 0x3C; // NMI/reset vector table, emulation mode reset
        constexpr size_t kHdrSize = 0x40;

        constexpr size_t kLoRomHeader = 0x7FC0;
        constexpr size_t kHiRomHeader = 0xFFC0;
        constexpr size_t kExLoRomHeader = 0x407FC0;
        constexpr size_t kExHiRomHeader = 0x40FFC0;

        constexpr size_t kMinRomSize = 128 * 1024;
        constexpr size_t kMaxRomSize = 8 * 1024 * 1024;

        uint16_t Read16(const std::vector<uint8_t>& d, size_t off) {
            return static_cast<uint16_t>(d[off] | (static_cast<uint16_t>(d[off + 1]) << 8));
        }

        // Confidence that a valid cartridge header lives at `base`. The usual
        // heuristic stack: checksum/complement pair, a reset vector pointing
        // into the ROM half of the address space, a printable title, a map mode
        // consistent with the slot being probed, and a declared size close to
        // the real one.
        int ScoreHeader(const std::vector<uint8_t>& d, size_t base, bool hi_slot) {
            if (base + kHdrSize > d.size()) return -1;

            int score = 0;

            const uint16_t comp = Read16(d, base + kHdrChecksumComp);
            const uint16_t sum = Read16(d, base + kHdrChecksum);
            if (static_cast<uint16_t>(sum + comp) == 0xFFFF && (sum != 0 || comp != 0)) score += 8;

            const uint16_t reset = Read16(d, base + kHdrEmuResetVec);
            if (reset >= 0x8000) score += 4;

            int printable = 0;
            for (size_t i = 0; i < 21; ++i) {
                const uint8_t c = d[base + kHdrTitle + i];
                if (c >= 0x20 && c < 0x7F) ++printable;
            }
            if (printable >= 19) score += 3;
            else if (printable >= 15) score += 1;

            const uint8_t map_mode = d[base + kHdrMapMode];
            const bool mode_is_hi = (map_mode & 0x01) != 0;
            if ((map_mode & 0xE0) == 0x20 && mode_is_hi == hi_slot) score += 2;

            const uint8_t size_exp = d[base + kHdrRomSize];
            if (size_exp >= 7 && size_exp <= 13) {
                const size_t declared = static_cast<size_t>(1024) << size_exp; // 1 << exp KiB
                if (declared >= d.size() / 2 && declared <= d.size() * 2) score += 2;
            }

            return score;
        }

        std::string TrimTitle(const std::vector<uint8_t>& d, size_t base) {
            std::string title;
            for (size_t i = 0; i < 21; ++i) {
                const uint8_t c = d[base + kHdrTitle + i];
                title.push_back((c >= 0x20 && c < 0x7F) ? static_cast<char>(c) : ' ');
            }
            while (!title.empty() && title.back() == ' ') title.pop_back();
            size_t first = 0;
            while (first < title.size() && title[first] == ' ') ++first;
            return title.substr(first);
        }

        struct Sha256Ctx {
            uint32_t state[8];
            uint64_t bit_len;
            uint8_t buffer[64];
            size_t buffer_len;
        };

        constexpr uint32_t kSha256K[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
        };

        inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

        void Sha256Transform(Sha256Ctx& ctx, const uint8_t block[64]) {
            uint32_t w[64];
            for (int i = 0; i < 16; ++i) {
                w[i] = (static_cast<uint32_t>(block[i * 4]) << 24) |
                       (static_cast<uint32_t>(block[i * 4 + 1]) << 16) |
                       (static_cast<uint32_t>(block[i * 4 + 2]) << 8) |
                       static_cast<uint32_t>(block[i * 4 + 3]);
            }
            for (int i = 16; i < 64; ++i) {
                const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
                const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
                w[i] = w[i - 16] + s0 + w[i - 7] + s1;
            }

            uint32_t a = ctx.state[0], b = ctx.state[1], c = ctx.state[2], d = ctx.state[3];
            uint32_t e = ctx.state[4], f = ctx.state[5], g = ctx.state[6], h = ctx.state[7];

            for (int i = 0; i < 64; ++i) {
                const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
                const uint32_t ch = (e & f) ^ ((~e) & g);
                const uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
                const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
                const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
                const uint32_t t2 = s0 + maj;
                h = g; g = f; f = e; e = d + t1;
                d = c; c = b; b = a; a = t1 + t2;
            }

            ctx.state[0] += a; ctx.state[1] += b; ctx.state[2] += c; ctx.state[3] += d;
            ctx.state[4] += e; ctx.state[5] += f; ctx.state[6] += g; ctx.state[7] += h;
        }

        void Sha256Init(Sha256Ctx& ctx) {
            ctx.state[0] = 0x6a09e667; ctx.state[1] = 0xbb67ae85;
            ctx.state[2] = 0x3c6ef372; ctx.state[3] = 0xa54ff53a;
            ctx.state[4] = 0x510e527f; ctx.state[5] = 0x9b05688c;
            ctx.state[6] = 0x1f83d9ab; ctx.state[7] = 0x5be0cd19;
            ctx.bit_len = 0;
            ctx.buffer_len = 0;
        }

        void Sha256Update(Sha256Ctx& ctx, const uint8_t* data, size_t len) {
            ctx.bit_len += static_cast<uint64_t>(len) * 8;
            while (len > 0) {
                const size_t take = (64 - ctx.buffer_len < len) ? (64 - ctx.buffer_len) : len;
                std::memcpy(ctx.buffer + ctx.buffer_len, data, take);
                ctx.buffer_len += take;
                data += take;
                len -= take;
                if (ctx.buffer_len == 64) {
                    Sha256Transform(ctx, ctx.buffer);
                    ctx.buffer_len = 0;
                }
            }
        }

        void Sha256Final(Sha256Ctx& ctx, uint8_t out[32]) {
            const uint64_t bits = ctx.bit_len;
            uint8_t pad = 0x80;
            Sha256Update(ctx, &pad, 1);
            ctx.bit_len = bits; // padding must not count towards the length
            pad = 0x00;
            while (ctx.buffer_len != 56) {
                Sha256Update(ctx, &pad, 1);
                ctx.bit_len = bits;
            }
            uint8_t len_bytes[8];
            for (int i = 0; i < 8; ++i) len_bytes[7 - i] = static_cast<uint8_t>((bits >> (i * 8)) & 0xFF);
            Sha256Update(ctx, len_bytes, 8);
            for (int i = 0; i < 8; ++i) {
                out[i * 4] = static_cast<uint8_t>(ctx.state[i] >> 24);
                out[i * 4 + 1] = static_cast<uint8_t>(ctx.state[i] >> 16);
                out[i * 4 + 2] = static_cast<uint8_t>(ctx.state[i] >> 8);
                out[i * 4 + 3] = static_cast<uint8_t>(ctx.state[i]);
            }
        }

    } // namespace

    std::string MappingName(SnesMapping mapping) {
        switch (mapping) {
            case SnesMapping::LoROM: return "LoROM";
            case SnesMapping::HiROM: return "HiROM";
            case SnesMapping::ExLoROM: return "ExLoROM";
            case SnesMapping::ExHiROM: return "ExHiROM";
            default: return "Unknown";
        }
    }

    std::string RomTypeName(uint8_t rom_type) {
        switch (rom_type) {
            case 0x00: return "ROM only";
            case 0x01: return "ROM + RAM";
            case 0x02: return "ROM + RAM + battery";
            case 0x03: case 0x04: case 0x05: return "DSP coprocessor";
            case 0x13: case 0x14: case 0x15: case 0x1A: return "Super FX (GSU)";
            case 0x23: case 0x24: case 0x25: case 0x34: case 0x35: return "SA-1";
            case 0x43: case 0x45: return "S-DD1";
            case 0x55: return "S-RTC";
            case 0xE3: return "Super Game Boy / satellite";
            case 0xF3: return "CX4";
            case 0xF5: return "SPC7110 / ST018";
            case 0xF6: return "ST010 / ST011";
            case 0xF9: return "SPC7110 + RTC";
            default: return "unknown coprocessor (0x" + std::to_string(rom_type) + ")";
        }
    }

    Sha256Stream::Sha256Stream() {
        Sha256Ctx ctx;
        Sha256Init(ctx);
        std::memcpy(state, ctx.state, sizeof(state));
        bit_len = ctx.bit_len;
        buffer_len = ctx.buffer_len;
    }

    void Sha256Stream::Update(const uint8_t* data, size_t length) {
        // The context here IS this object's fields; copying in and out keeps
        // the transform code in one place rather than duplicating it.
        Sha256Ctx ctx;
        std::memcpy(ctx.state, state, sizeof(state));
        ctx.bit_len = bit_len;
        std::memcpy(ctx.buffer, buffer, sizeof(buffer));
        ctx.buffer_len = buffer_len;
        Sha256Update(ctx, data, length);
        std::memcpy(state, ctx.state, sizeof(state));
        bit_len = ctx.bit_len;
        std::memcpy(buffer, ctx.buffer, sizeof(buffer));
        buffer_len = ctx.buffer_len;
    }

    std::string Sha256Stream::FinishHex() {
        Sha256Ctx ctx;
        std::memcpy(ctx.state, state, sizeof(state));
        ctx.bit_len = bit_len;
        std::memcpy(ctx.buffer, buffer, sizeof(buffer));
        ctx.buffer_len = buffer_len;
        uint8_t digest[32];
        Sha256Final(ctx, digest);
        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (int i = 0; i < 32; ++i) {
            out.push_back(hex[digest[i] >> 4]);
            out.push_back(hex[digest[i] & 0x0F]);
        }
        return out;
    }

    uint32_t Crc32(const uint8_t* data, size_t length) {
        static uint32_t table[256];
        static bool built = false;
        if (!built) {
            for (uint32_t i = 0; i < 256; ++i) {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
                table[i] = c;
            }
            built = true;
        }
        uint32_t crc = 0xFFFFFFFFu;
        for (size_t i = 0; i < length; ++i) {
            crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
        }
        return crc ^ 0xFFFFFFFFu;
    }

    std::string Sha256Hex(const uint8_t* data, size_t length) {
        Sha256Ctx ctx;
        Sha256Init(ctx);
        Sha256Update(ctx, data, length);
        uint8_t digest[32];
        Sha256Final(ctx, digest);

        static const char* hex = "0123456789abcdef";
        std::string out;
        out.reserve(64);
        for (int i = 0; i < 32; ++i) {
            out.push_back(hex[(digest[i] >> 4) & 0xF]);
            out.push_back(hex[digest[i] & 0xF]);
        }
        return out;
    }

    SnesRomInfo AnalyzeSnesRom(const std::vector<uint8_t>& raw) {
        SnesRomInfo info;
        info.original_size = raw.size();

        if (raw.size() < kMinRomSize) {
            info.error = "file is only " + std::to_string(raw.size()) +
                         " bytes; too small to be a SNES ROM";
            return info;
        }

        // A 512-byte copier (SMC/SWC) header is the classic .smc prefix. It is
        // identified by the file size being 512 bytes past a 1 KiB boundary.
        size_t offset = 0;
        if ((raw.size() % 1024) == 512) {
            offset = 512;
            info.had_copier_header = true;
        }

        std::vector<uint8_t> rom(raw.begin() + static_cast<long>(offset), raw.end());
        info.rom_size = rom.size();

        if (rom.size() < kMinRomSize) {
            info.error = "normalized ROM size " + std::to_string(rom.size()) +
                         " is too small to be a SNES ROM";
            return info;
        }
        if (rom.size() > kMaxRomSize) {
            info.unsupported = true;
            info.error = "normalized ROM size " + std::to_string(rom.size()) +
                         " exceeds the 8 MiB the SNES can address";
            return info;
        }
        // Odd sizes are a warning, not a rejection: the .sfrom body is the ROM
        // verbatim, so an unusual dump either boots in Canoe or it does not —
        // refusing to write the file cannot make it work.
        if ((rom.size() % 32768) != 0) {
            info.warnings.push_back("ROM size is not a multiple of 32 KiB (unusual dump)");
        }

        const int lo_score = ScoreHeader(rom, kLoRomHeader, false);
        const int hi_score = ScoreHeader(rom, kHiRomHeader, true);
        const int exlo_score = ScoreHeader(rom, kExLoRomHeader, false);
        const int exhi_score = ScoreHeader(rom, kExHiRomHeader, true);

        int best = lo_score;
        info.mapping = SnesMapping::LoROM;
        info.header_offset = kLoRomHeader;
        if (hi_score > best) { best = hi_score; info.mapping = SnesMapping::HiROM; info.header_offset = kHiRomHeader; }
        if (exlo_score > best) { best = exlo_score; info.mapping = SnesMapping::ExLoROM; info.header_offset = kExLoRomHeader; }
        if (exhi_score > best) { best = exhi_score; info.mapping = SnesMapping::ExHiROM; info.header_offset = kExHiRomHeader; }
        info.mapping_score = best;

        if (best < 8) {
            info.mapping = SnesMapping::Unknown;
            info.error = "no valid SNES cartridge header found at 0x7FC0 or 0xFFC0 "
                         "(best confidence " + std::to_string(best) + ")";
            return info;
        }

        const size_t base = info.header_offset;
        info.internal_title = TrimTitle(rom, base);
        info.map_mode = rom[base + kHdrMapMode];
        info.rom_type = rom[base + kHdrRomType];
        info.rom_size_exp = rom[base + kHdrRomSize];
        info.sram_size_exp = rom[base + kHdrSramSize];
        info.country = rom[base + kHdrCountry];
        info.version = rom[base + kHdrVersion];
        info.checksum_complement = Read16(rom, base + kHdrChecksumComp);
        info.checksum = Read16(rom, base + kHdrChecksum);
        info.checksum_ok = (static_cast<uint16_t>(info.checksum + info.checksum_complement) == 0xFFFF);

        info.crc32 = Crc32(rom.data(), rom.size());
        info.sha256 = Sha256Hex(rom.data(), rom.size());

        // --- Advisory checks, NOT a gate ----------------------------------
        //
        // Earlier builds refused anything outside map mode 0x20/0x21/0x30/0x31
        // with cartridge type <= 0x02. That was wrong on both counts and it
        // rejected working games:
        //
        //   Contra III      map mode 0x53 — a nonstandard byte in that dump,
        //                   yet the checksum, reset vector and internal title
        //                   at 0x7FC0 all check out.
        //   Doom            Super FX. CaVE ships it, and NSO's own Star Fox is
        //                   Super FX, so Canoe emulates the chip.
        //   Super Mario Kart DSP-1. Also a stock NSO title.
        //
        // The deciding fact: the mapping this function reports does not affect
        // a single byte of the generated .sfrom. The body is the ROM verbatim
        // and Canoe parses the cartridge header itself — the detection exists
        // to confirm "this really is a SNES ROM" and to fill in the log, not to
        // decide what the emulator can run. So anything with a credible header
        // is converted, and everything else is recorded as a warning.
        if (info.mapping == SnesMapping::ExLoROM || info.mapping == SnesMapping::ExHiROM) {
            info.warnings.push_back(MappingName(info.mapping) +
                                    " mapping is unusual; Canoe may not run it");
        }

        const bool standard_mode = (info.map_mode == 0x20 || info.map_mode == 0x21 ||
                                    info.map_mode == 0x30 || info.map_mode == 0x31);
        if (!standard_mode) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%02X", info.map_mode);
            info.warnings.push_back("nonstandard map mode byte " + std::string(buf));
        }

        if (info.rom_type > 0x02) {
            info.has_enhancement_chip = true;
            info.warnings.push_back("cartridge uses " + RomTypeName(info.rom_type) +
                                    "; Canoe emulates several of these, but romm-nx cannot "
                                    "supply a per-game preset it has no reference for");
        }

        if (!info.checksum_ok) {
            info.warnings.push_back("internal checksum does not match its complement "
                                    "(hacked, translated or bad dump)");
        }

        info.valid = true;
        return info;
    }

}
