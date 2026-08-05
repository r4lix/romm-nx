#include "N64Rom.hpp"
#include "SnesRom.hpp" // Sha256Hex, Sha256Stream

#include <zlib.h>

#include <cstdio>
#include <cstring>

namespace romm::nso {

    namespace {

        constexpr size_t kHeaderSize = 0x40;
        constexpr size_t kEntryPoint = 0x08;
        constexpr size_t kCrc1 = 0x10;
        constexpr size_t kCrc2 = 0x14;
        constexpr size_t kInternalName = 0x20;
        constexpr size_t kInternalNameSize = 20;
        constexpr size_t kCartridgeId = 0x3B;
        constexpr size_t kCountryCode = 0x3E;
        constexpr size_t kVersion = 0x3F;

        // Smallest retail cartridge is 4 MiB; the largest is 64 MiB.
        constexpr size_t kMinRomSize = 1024 * 1024;
        constexpr size_t kMaxRomSize = 64u * 1024 * 1024;

        uint32_t Read32BE(const uint8_t* d, size_t off) {
            return ((uint32_t)d[off] << 24) | ((uint32_t)d[off + 1] << 16) |
                   ((uint32_t)d[off + 2] << 8) | (uint32_t)d[off + 3];
        }

        // The first word is 0x80371240 on a cartridge; reading it in the three
        // possible orders is what identifies the dump's layout.
        //
        // A free function, and deliberately so: this used to live inside
        // AnalyzeN64Rom while NormalizeN64Rom asked AnalyzeN64Rom for the
        // answer — and AnalyzeN64Rom called NormalizeN64Rom to read the header.
        // That pair recursed until the pipeline worker's 1 MB stack ran out,
        // which is what crashed the app at step 5 rather than reporting a bad
        // ROM. Neither function may call the other again.
        N64Format DetectFormat(const uint8_t* d, size_t size) {
            if (size < 4) return N64Format::Unknown;
            if (d[0] == 0x80 && d[1] == 0x37 && d[2] == 0x12 && d[3] == 0x40) return N64Format::BigEndian;
            if (d[0] == 0x37 && d[1] == 0x80 && d[2] == 0x40 && d[3] == 0x12) return N64Format::ByteSwapped;
            if (d[0] == 0x40 && d[1] == 0x12 && d[2] == 0x37 && d[3] == 0x80) return N64Format::LittleEndian;
            return N64Format::Unknown;
        }

        // Big-endian copy of just the header. Analysing a ROM must not allocate
        // a second copy of it: these run to 64 MiB and the worker has a modest
        // heap to play with.
        void NormalizeHeader(const uint8_t* src, size_t size, N64Format format, uint8_t out[kHeaderSize]) {
            const size_t n = size < kHeaderSize ? size : kHeaderSize;
            for (size_t i = 0; i < n; ++i) out[i] = src[i];
            for (size_t i = n; i < kHeaderSize; ++i) out[i] = 0;

            if (format == N64Format::ByteSwapped) {
                for (size_t i = 0; i + 1 < kHeaderSize; i += 2) {
                    const uint8_t t = out[i];
                    out[i] = out[i + 1];
                    out[i + 1] = t;
                }
            } else if (format == N64Format::LittleEndian) {
                for (size_t i = 0; i + 3 < kHeaderSize; i += 4) {
                    const uint8_t a = out[i], b = out[i + 1];
                    out[i] = out[i + 3];
                    out[i + 1] = out[i + 2];
                    out[i + 2] = b;
                    out[i + 3] = a;
                }
            }
        }

    } // namespace

    const char* N64FormatName(N64Format format) {
        switch (format) {
            case N64Format::BigEndian:    return "z64 (big-endian)";
            case N64Format::ByteSwapped:  return "v64 (byte-swapped)";
            case N64Format::LittleEndian: return "n64 (little-endian)";
            default:                      return "unknown";
        }
    }

    const char* N64CountryName(char code) {
        switch (code) {
            case 'E': return "USA";
            case 'P': return "Europe";
            case 'J': return "Japan";
            case 'D': return "Germany";
            case 'F': return "France";
            case 'I': return "Italy";
            case 'S': return "Spain";
            case 'U': return "Australia";
            case 'X': case 'Y': return "Europe (alt)";
            case 'A': return "Asia";
            case 'C': return "China";
            case 'K': return "Korea";
            case 'B': return "Brazil";
            case 'N': return "Canada";
            case 'H': return "Netherlands";
            default:  return "unknown region";
        }
    }

    std::string N64Crc1Hex(uint32_t crc1) {
        char buf[16];
        std::snprintf(buf, sizeof(buf), "%08X", (unsigned)crc1);
        return buf;
    }

    bool NormalizeN64Rom(const std::vector<uint8_t>& data, std::vector<uint8_t>& out,
                         std::string& error) {
        error.clear();
        if (data.size() < kHeaderSize) {
            error = "too small to hold an N64 header";
            return false;
        }

        const N64Format format = DetectFormat(data.data(), data.size());
        out.clear();
        out.reserve(data.size());

        switch (format) {
            case N64Format::BigEndian:
                out = data;
                return true;

            case N64Format::ByteSwapped: {
                // 16-bit swap: AB CD -> BA DC.
                if (data.size() % 2 != 0) {
                    error = "byte-swapped image has an odd length";
                    return false;
                }
                out.resize(data.size());
                for (size_t i = 0; i + 1 < data.size(); i += 2) {
                    out[i]     = data[i + 1];
                    out[i + 1] = data[i];
                }
                return true;
            }

            case N64Format::LittleEndian: {
                // 32-bit word swap: AB CD EF GH -> HG FE DC BA.
                if (data.size() % 4 != 0) {
                    error = "little-endian image is not a whole number of words";
                    return false;
                }
                out.resize(data.size());
                for (size_t i = 0; i + 3 < data.size(); i += 4) {
                    out[i]     = data[i + 3];
                    out[i + 1] = data[i + 2];
                    out[i + 2] = data[i + 1];
                    out[i + 3] = data[i];
                }
                return true;
            }

            default:
                error = "unrecognised N64 byte order";
                return false;
        }
    }

    N64RomInfo InspectN64RomFile(const std::string& source_path) {
        N64RomInfo info;
        FILE* f = std::fopen(source_path.c_str(), "rb");
        if (!f) {
            info.error = "cannot open " + source_path;
            return info;
        }
        std::fseek(f, 0, SEEK_END);
        const long size = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        std::vector<uint8_t> head(kHeaderSize);
        const size_t read = std::fread(head.data(), 1, head.size(), f);
        std::fclose(f);
        if (size < 0 || read != head.size()) {
            info.error = "could not read the ROM header";
            return info;
        }

        // AnalyzeN64Rom does the header work and only needs these 64 bytes.
        // Its size checks then have to be re-applied against the real file
        // length, since what it was handed is exactly one header long.
        info = AnalyzeN64Rom(head);
        info.file_size = (size_t)size;
        if (info.source_format == N64Format::Unknown) return info;
        info.error.clear();
        info.warnings.clear();
        info.valid = true;
        if (info.file_size < kMinRomSize) {
            info.valid = false;
            info.error = "Smaller than any N64 cartridge";
            return info;
        }
        if (info.file_size > kMaxRomSize) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Larger than a 64 MiB cartridge (%zu bytes)", info.file_size);
            info.warnings.push_back(buf);
        }
        if (info.file_size % 4 != 0) {
            info.valid = false;
            info.error = "Length is not a whole number of 32-bit words; this dump cannot be byte-order converted";
            return info;
        }
        if (info.file_size % (1024 * 1024) != 0) {
            info.warnings.push_back("Size is not a whole number of megabytes; the dump may be trimmed");
        }
        if (info.converted) {
            info.warnings.push_back(std::string("Converting from ") +
                                    N64FormatName(info.source_format) + " to big-endian");
        }
        info.sha256.clear(); // filled by the conversion pass
        return info;
    }

    bool ConvertN64RomToBnz(const std::string& source_path, const std::string& out_path,
                            N64Format source_format, std::string& out_sha256,
                            std::string& error) {
        error.clear();
        out_sha256.clear();

        FILE* in = std::fopen(source_path.c_str(), "rb");
        if (!in) {
            error = "cannot open " + source_path;
            return false;
        }
        FILE* out = std::fopen(out_path.c_str(), "wb");
        if (!out) {
            std::fclose(in);
            error = "cannot create " + out_path;
            return false;
        }

        z_stream zs {};
        // Level 6 rather than the 9 CaVE used: the app only has to
        // decompress, any valid stream will do, and level 9 on a 64 MiB
        // cartridge costs minutes of console time for a percent of size.
        if (deflateInit(&zs, 6) != Z_OK) {
            std::fclose(in);
            std::fclose(out);
            error = "zlib refused to start";
            return false;
        }

        constexpr size_t kChunk = 256 * 1024; // a multiple of 4, so no swap straddles a chunk
        std::vector<uint8_t> in_buf(kChunk);
        std::vector<uint8_t> out_buf(kChunk);
        Sha256Stream hash;
        bool ok = true;

        for (;;) {
            const size_t got = std::fread(in_buf.data(), 1, in_buf.size(), in);
            if (got == 0) break;

            switch (source_format) {
                case N64Format::ByteSwapped:
                    for (size_t i = 0; i + 1 < got; i += 2) {
                        const uint8_t t = in_buf[i];
                        in_buf[i] = in_buf[i + 1];
                        in_buf[i + 1] = t;
                    }
                    break;
                case N64Format::LittleEndian:
                    for (size_t i = 0; i + 3 < got; i += 4) {
                        const uint8_t a = in_buf[i];
                        const uint8_t b = in_buf[i + 1];
                        in_buf[i] = in_buf[i + 3];
                        in_buf[i + 1] = in_buf[i + 2];
                        in_buf[i + 2] = b;
                        in_buf[i + 3] = a;
                    }
                    break;
                default:
                    break; // already big-endian
            }

            hash.Update(in_buf.data(), got);

            zs.next_in = in_buf.data();
            zs.avail_in = (uInt)got;
            while (zs.avail_in > 0) {
                zs.next_out = out_buf.data();
                zs.avail_out = (uInt)out_buf.size();
                if (deflate(&zs, Z_NO_FLUSH) != Z_OK) {
                    error = "zlib failed while compressing";
                    ok = false;
                    break;
                }
                const size_t produced = out_buf.size() - zs.avail_out;
                if (produced && std::fwrite(out_buf.data(), 1, produced, out) != produced) {
                    error = "write failed (card full?)";
                    ok = false;
                    break;
                }
            }
            if (!ok) break;
        }

        if (ok) {
            int rc = Z_OK;
            do {
                zs.next_out = out_buf.data();
                zs.avail_out = (uInt)out_buf.size();
                rc = deflate(&zs, Z_FINISH);
                const size_t produced = out_buf.size() - zs.avail_out;
                if (produced && std::fwrite(out_buf.data(), 1, produced, out) != produced) {
                    error = "write failed (card full?)";
                    ok = false;
                    break;
                }
            } while (rc == Z_OK);
            if (ok && rc != Z_STREAM_END) {
                error = "zlib did not finish the stream";
                ok = false;
            }
        }

        deflateEnd(&zs);
        std::fclose(in);
        const bool closed = (std::fclose(out) == 0);
        if (ok && !closed) {
            error = "failed to flush " + out_path;
            ok = false;
        }
        if (!ok) {
            std::remove(out_path.c_str());
            return false;
        }
        out_sha256 = hash.FinishHex();
        return true;
    }
    std::vector<N64IdleLoop> ScanN64BootIdle(const std::string& source_path,
                                             N64Format format, uint32_t entry_point) {
        std::vector<N64IdleLoop> found;
        if (entry_point < 0x80000000u || entry_point >= 0x80800000u) return found;

        FILE* f = std::fopen(source_path.c_str(), "rb");
        if (!f) return found;

        // The boot segment. Past this the 1:1 ROM->RAM mapping stops holding and
        // any hit would be a fabricated address, so the scan simply stops rather
        // than reporting something it cannot justify. Two megabytes covers every
        // candidate seen so far by a wide margin (the real one in Ocarina of
        // Time sits at ROM 0x1410).
        constexpr size_t kBootStart = 0x1000;
        constexpr size_t kBootLimit = 2u * 1024 * 1024;
        constexpr size_t kMaxEntries = 8;

        // 4-byte overlap between chunks: the loop and its delay slot are two
        // consecutive words and must not be split across reads.
        constexpr size_t kChunk = 256 * 1024;
        std::vector<uint8_t> buf(kChunk + 4);

        if (std::fseek(f, (long)kBootStart, SEEK_SET) != 0) {
            std::fclose(f);
            return found;
        }

        size_t base = kBootStart;
        size_t carry = 0;
        while (base < kBootLimit && found.size() < kMaxEntries) {
            const size_t want = kChunk - carry;
            const size_t got = std::fread(buf.data() + carry, 1, want, f);
            if (got == 0) break;
            const size_t have = carry + got;

            switch (format) {
                case N64Format::ByteSwapped:
                    for (size_t i = carry; i + 1 < have; i += 2) {
                        const uint8_t t = buf[i];
                        buf[i] = buf[i + 1];
                        buf[i + 1] = t;
                    }
                    break;
                case N64Format::LittleEndian:
                    for (size_t i = carry; i + 3 < have; i += 4) {
                        const uint8_t a = buf[i], b = buf[i + 1];
                        buf[i] = buf[i + 3];
                        buf[i + 1] = buf[i + 2];
                        buf[i + 2] = b;
                        buf[i + 3] = a;
                    }
                    break;
                default:
                    break;
            }

            for (size_t i = 0; i + 8 <= have && found.size() < kMaxEntries; i += 4) {
                const uint32_t inst = Read32BE(buf.data(), i);
                if (inst != 0x1000FFFFu) continue;
                if (Read32BE(buf.data(), i + 4) != 0) continue; // delay slot must be a nop

                const size_t rom_off = base + i;
                if (rom_off >= kBootLimit) break;
                const uint32_t addr = (uint32_t)(rom_off - kBootStart) + entry_point;
                if (addr < 0x80000000u || addr >= 0x80800000u) continue;
                found.push_back({addr, inst});
            }

            if (have < 8) break;
            // Keep the last word so a loop straddling the boundary is still seen.
            buf[0] = buf[have - 4];
            buf[1] = buf[have - 3];
            buf[2] = buf[have - 2];
            buf[3] = buf[have - 1];
            base += have - 4;
            carry = 4;
        }

        std::fclose(f);
        return found;
    }

    N64RomInfo AnalyzeN64Rom(const std::vector<uint8_t>& data) {
        N64RomInfo info;
        info.file_size = data.size();

        if (data.size() < kHeaderSize) {
            info.error = "File is too small to be an N64 ROM";
            return info;
        }

        info.source_format = DetectFormat(data.data(), data.size());
        if (info.source_format == N64Format::Unknown) {
            info.error = "Not an N64 ROM: the header does not start with 0x80371240 "
                         "in any of the three known byte orders";
            return info;
        }
        info.converted = (info.source_format != N64Format::BigEndian);

        // Only the header is normalized here — 64 bytes on the stack — so this
        // is safe to call on a 64 MiB image.
        uint8_t be[kHeaderSize];
        NormalizeHeader(data.data(), data.size(), info.source_format, be);

        info.crc1 = Read32BE(be, kCrc1);
        info.crc2 = Read32BE(be, kCrc2);
        info.entry_point = Read32BE(be, kEntryPoint);

        for (size_t i = 0; i < kInternalNameSize; ++i) {
            const uint8_t c = be[kInternalName + i];
            if (c == 0) break;
            if (c >= 0x20 && c < 0x7F) info.internal_name.push_back((char)c);
        }
        while (!info.internal_name.empty() && info.internal_name.back() == ' ') {
            info.internal_name.pop_back();
        }

        for (size_t i = 0; i < 2; ++i) {
            const uint8_t c = be[kCartridgeId + i];
            if (c >= 0x20 && c < 0x7F) info.cartridge_id.push_back((char)c);
        }
        info.country_code = (char)be[kCountryCode];
        info.version = be[kVersion];

        if (data.size() < kMinRomSize) {
            info.error = "Smaller than any N64 cartridge";
            return info;
        }
        if (data.size() > kMaxRomSize) {
            char buf[96];
            std::snprintf(buf, sizeof(buf), "Larger than a 64 MiB cartridge (%zu bytes)", data.size());
            info.warnings.push_back(buf);
        }
        if (data.size() % (1024 * 1024) != 0) {
            info.warnings.push_back("Size is not a whole number of megabytes; the dump may be trimmed");
        }
        if (info.converted) {
            info.warnings.push_back(std::string("Converted from ") +
                                    N64FormatName(info.source_format) + " to big-endian");
        }
        if (info.crc1 == 0 && info.crc2 == 0) {
            info.warnings.push_back("Header carries no CRCs; this is not a stock dump");
        }

        // Of the NORMALIZED bytes, so one cartridge dumped in two byte orders
        // reuses one entry rather than accumulating two. Only computed for an
        // already-big-endian image held whole: converting a copy here purely to
        // hash it is exactly the allocation this function avoids, and the
        // streaming converter fills this in for every other case.
        if (info.source_format == N64Format::BigEndian) {
            info.sha256 = Sha256Hex(data.data(), data.size());
        }
        info.valid = true;
        return info;
    }

}
