#include "NsoN64MetaPack.hpp"

#include <zlib.h>

#include <cctype>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <utility>
#include <dirent.h>
#include <sys/stat.h>

namespace romm::nso {

    namespace {

        // Old *binary* cpio, the PDP-11 layout: 13 little-endian u16, and the
        // two 32-bit fields (mtime, filesize) are stored most-significant word
        // FIRST while each word is itself little-endian. Getting that backwards
        // produces an archive that inflates cleanly and then describes members
        // of absurd length, which is the sort of thing only a byte-for-byte
        // rebuild of a known-good pack catches.
        constexpr uint16_t kCpioMagic = 0070707;
        constexpr uint16_t kCpioDev = 0x8080; // every real member; TRAILER!!! uses 0

        void PutU16(std::string& out, uint16_t value) {
            out.push_back((char)(uint8_t)(value & 0xFF));
            out.push_back((char)(uint8_t)((value >> 8) & 0xFF));
        }

        void AppendMember(std::string& out, const std::string& name,
                          const std::string& data, uint32_t mtime, uint16_t dev) {
            const size_t before = out.size();

            const uint32_t name_size = (uint32_t)name.size() + 1; // the NUL counts
            const uint32_t data_size = (uint32_t)data.size();

            PutU16(out, kCpioMagic);
            PutU16(out, dev);
            PutU16(out, 0); // ino
            PutU16(out, 0); // mode
            PutU16(out, 0); // uid
            PutU16(out, 0); // gid
            PutU16(out, 0); // nlink
            PutU16(out, 0); // rdev
            PutU16(out, (uint16_t)((mtime >> 16) & 0xFFFF));
            PutU16(out, (uint16_t)(mtime & 0xFFFF));
            PutU16(out, (uint16_t)name_size);
            PutU16(out, (uint16_t)((data_size >> 16) & 0xFFFF));
            PutU16(out, (uint16_t)(data_size & 0xFFFF));

            out += name;
            out.push_back('\0');
            if ((out.size() - before) & 1u) out.push_back('\0'); // name padded to even

            out += data;
            if ((out.size() - before) & 1u) out.push_back('\0'); // data padded to even
        }

        // The three member payloads, reproducing the layout of a known-good
        // community pack down to the CRLF line endings and the absence of a
        // trailing newline. A JSON or Lua parser would not care; matching the
        // one file confirmed to boot on hardware costs nothing and removes a
        // variable from any future "why does this pack not work".
        constexpr const char* kNl = "\r\n";

        std::string HexWord(uint32_t value) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "0x%08X", (unsigned)value);
            return buf;
        }

        // Only ever fed the option strings below, none of which can contain a
        // quote or a backslash — but a user-visible comment reaches this, so it
        // still has to be escaped rather than trusted.
        std::string Quoted(const std::string& value) {
            std::string out = "\"";
            for (char c : value) {
                if (c == '"' || c == '\\') out.push_back('\\');
                if ((unsigned char)c < 0x20) { out += ' '; continue; }
                out.push_back(c);
            }
            out.push_back('"');
            return out;
        }

        std::string BuildMeta(const N64MetaPackOptions& options) {
            std::string out;
            out += "{"; out += kNl;
            out += "  \"Hardware\": \"NUS\","; out += kNl;
            out += "  \"TitleCode\": \"00_XXXXXX.000\","; out += kNl;
            out += "  \"DataVersion\": \"1.0.0\","; out += kNl;
            out += "  \"OptionInfo\": {"; out += kNl;
            // Ahead of SndSampleRate, as the European reference pack has it.
            if (options.pal) {
                out += "    \"PAL\": 1,"; out += kNl;
            }
            out += "    \"SndSampleRate\": " + std::to_string(options.snd_sample_rate); out += kNl;
            out += "  }"; out += kNl;
            out += "}";
            return out;
        }

        std::string BuildCfg(const N64MetaPackOptions& options) {
            std::string out;
            out += "{\t"; out += kNl;
            out += "\t\"RomOption\": {"; out += kNl;
            out += "\t\t\"TickPerInst\": " + std::to_string(options.tick_per_inst) + ","; out += kNl;
            out += "\t\t\"BackupType\": " + Quoted(options.backup_type);
            if (!options.pak_type.empty()) {
                out += ","; out += kNl;
                out += "\t\t\"SIDevice_PakType\": " + Quoted(options.pak_type);
            }
            out += kNl;
            out += "\t},"; out += kNl;
            out += "\t\"RSP\": {},"; out += kNl;
            if (options.idle.empty()) {
                // Nothing the scan could justify. See the header: a guessed
                // JmpAddr does not fail safely.
                out += "\t\"Idle\": [],"; out += kNl;
            } else {
                out += "\t\"Idle\": ["; out += kNl;
                for (size_t i = 0; i < options.idle.size(); ++i) {
                    out += "\t\t{"; out += kNl;
                    out += "\t\t\t\"JmpAddr\": " + Quoted(HexWord(options.idle[i].jmp_addr)) + ","; out += kNl;
                    out += "\t\t\t\"JmpInst\": " + Quoted(HexWord(options.idle[i].jmp_inst)) + ","; out += kNl;
                    out += "\t\t\t\"IsIdle\": \"0x1\""; out += kNl;
                    out += "\t\t}";
                    if (i + 1 < options.idle.size()) out += ",";
                    out += kNl;
                }
                out += "\t],"; out += kNl;
            }
            out += "\t\"SpecialInst\": [],"; out += kNl;
            out += "\t\"RendererSetting\": {"; out += kNl;
            out += "\t}"; out += kNl;
            out += "}";
            return out;
        }

        std::string BuildLua() {
            // A pack may rewrite ROM bytes here — the reference one patches
            // brightness and then fixes the CIC checksum the patch invalidated.
            // Generated packs change nothing, so the checksum stays valid and
            // the function is empty.
            std::string out;
            out += "function RomPatch()\t"; out += kNl;
            out += "\treturn"; out += kNl;
            out += "end";
            return out;
        }

        bool Inflate(const std::string& in, std::string& out, std::string& error) {
            out.clear();
            z_stream zs {};
            if (inflateInit(&zs) != Z_OK) {
                error = "zlib refused to start";
                return false;
            }
            zs.next_in = (Bytef*)in.data();
            zs.avail_in = (uInt)in.size();

            char buf[16 * 1024];
            int rc = Z_OK;
            do {
                zs.next_out = (Bytef*)buf;
                zs.avail_out = (uInt)sizeof(buf);
                rc = inflate(&zs, Z_NO_FLUSH);
                if (rc != Z_OK && rc != Z_STREAM_END) {
                    inflateEnd(&zs);
                    error = "not a zlib stream, or it is truncated";
                    return false;
                }
                out.append(buf, sizeof(buf) - zs.avail_out);
                // A MetaPack is under a kilobyte; anything this large is not one
                // and would otherwise be an unbounded allocation from a file the
                // user dropped on the card.
                if (out.size() > 4u * 1024 * 1024) {
                    inflateEnd(&zs);
                    error = "decompresses to more than 4 MiB; not a MetaPack";
                    return false;
                }
            } while (rc != Z_STREAM_END);

            inflateEnd(&zs);
            return true;
        }

        bool EndsWithNoCase(const std::string& text, const char* suffix) {
            const size_t n = std::strlen(suffix);
            if (text.size() < n) return false;
            for (size_t i = 0; i < n; ++i) {
                const char a = (char)std::tolower((unsigned char)text[text.size() - n + i]);
                const char b = (char)std::tolower((unsigned char)suffix[i]);
                if (a != b) return false;
            }
            return true;
        }

        bool EqualsNoCase(const std::string& a, const std::string& b) {
            if (a.size() != b.size()) return false;
            for (size_t i = 0; i < a.size(); ++i) {
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
            }
            return true;
        }

        std::string StripExtension(const std::string& name) {
            const size_t dot = name.find_last_of('.');
            const size_t slash = name.find_last_of("/\\");
            if (dot == std::string::npos) return name;
            if (slash != std::string::npos && dot < slash) return name;
            return name.substr(0, dot);
        }

    } // namespace

    std::string BuildN64MetaPack(const N64MetaPackOptions& options, uint32_t mtime) {
        std::string archive;
        AppendMember(archive, "metapack\\00_XXXXXX.000.meta", BuildMeta(options), mtime, kCpioDev);
        AppendMember(archive, "metapack\\02_XXXXXX.000.cfg",  BuildCfg(options),  mtime, kCpioDev);
        AppendMember(archive, "metapack\\06_XXXXXX.000.lua",  BuildLua(),         mtime, kCpioDev);
        AppendMember(archive, "TRAILER!!!", std::string(), 0, 0);

        uLongf bound = compressBound((uLong)archive.size());
        std::string packed;
        packed.resize(bound);
        // Level 9, as the reference pack uses. The archive is under a kilobyte,
        // so unlike the 64 MiB ROM there is nothing to save by going lower.
        const int rc = compress2((Bytef*)&packed[0], &bound,
                                 (const Bytef*)archive.data(), (uLong)archive.size(), 9);
        if (rc != Z_OK) return std::string();
        packed.resize(bound);
        return packed;
    }

    bool WriteN64MetaPack(const std::string& path, const N64MetaPackOptions& options,
                          std::string& error) {
        error.clear();
        const std::string bytes = BuildN64MetaPack(options, (uint32_t)std::time(nullptr));
        if (bytes.empty()) {
            error = "could not compress the MetaPack";
            return false;
        }

        std::vector<std::string> members;
        std::string verify_error;
        if (!VerifyN64MetaPack(bytes, members, verify_error)) {
            error = "generated MetaPack failed its own check: " + verify_error;
            return false;
        }

        const std::string tmp = path + ".tmp";
        std::remove(tmp.c_str());
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) {
            error = "cannot create " + tmp;
            return false;
        }
        const bool wrote = std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
        const bool closed = std::fclose(f) == 0;
        if (!wrote || !closed) {
            std::remove(tmp.c_str());
            error = "write failed (card full?)";
            return false;
        }
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            error = "cannot move the MetaPack into place";
            return false;
        }
        return true;
    }

    bool VerifyN64MetaPack(const std::string& bytes, std::vector<std::string>& out_members,
                           std::string& error) {
        error.clear();
        out_members.clear();

        std::string raw;
        if (!Inflate(bytes, raw, error)) return false;

        size_t pos = 0;
        bool saw_trailer = false;
        while (pos + 26 <= raw.size()) {
            auto u16 = [&](size_t index) -> uint32_t {
                const size_t at = pos + index * 2;
                return (uint32_t)(uint8_t)raw[at] | ((uint32_t)(uint8_t)raw[at + 1] << 8);
            };
            if (u16(0) != kCpioMagic) {
                error = "bad cpio magic; this is not a MetaPack";
                return false;
            }
            const uint32_t mtime_words = (u16(8) << 16) | u16(9);
            (void)mtime_words;
            const uint32_t name_size = u16(10);
            const uint32_t data_size = (u16(11) << 16) | u16(12);
            if (name_size == 0 || pos + 26 + name_size > raw.size()) {
                error = "cpio member name runs past the end of the archive";
                return false;
            }

            const std::string name(raw.data() + pos + 26, name_size - 1);
            size_t data_at = pos + 26 + name_size;
            data_at += data_at & 1u;
            if (data_at + data_size > raw.size()) {
                error = "cpio member \"" + name + "\" runs past the end of the archive";
                return false;
            }
            pos = data_at + data_size;
            pos += pos & 1u;

            if (name == "TRAILER!!!") { saw_trailer = true; break; }
            out_members.push_back(name);
        }

        if (!saw_trailer) {
            error = "archive has no TRAILER!!! record";
            return false;
        }

        // The app wants all three; a pack missing one is either not a MetaPack
        // or was repacked by something that dropped a member.
        const char* required[3] = {".meta", ".cfg", ".lua"};
        for (const char* suffix : required) {
            bool found = false;
            for (const std::string& name : out_members) {
                if (EndsWithNoCase(name, suffix)) { found = true; break; }
            }
            if (!found) {
                error = std::string("no ") + suffix + " member";
                return false;
            }
        }
        return true;
    }

    std::string FindUserN64MetaPack(const std::string& rom_filename,
                                    const std::string& title,
                                    uint32_t crc1,
                                    std::string& out_matched_by) {
        out_matched_by.clear();

        struct stat st {};
        if (stat(kN64MetaPackDir, &st) != 0 || !S_ISDIR(st.st_mode)) return std::string();

        std::vector<std::string> packs;
        if (DIR* dir = opendir(kN64MetaPackDir)) {
            while (dirent* entry = readdir(dir)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                if (EndsWithNoCase(name, kN64MetaPackSuffix)) packs.push_back(name);
            }
            closedir(dir);
        }
        if (packs.empty()) return std::string();

        char crc_buf[16];
        std::snprintf(crc_buf, sizeof(crc_buf), "%08X", (unsigned)crc1);

        // Most specific first. CRC1 comes off the cartridge header, so it keeps
        // matching however the file was renamed — and unlike the filename it
        // cannot match the wrong region's dump.
        const std::pair<std::string, const char*> rules[3] = {
            {std::string(crc_buf),              "CRC1"},
            {StripExtension(rom_filename),      "ROM filename"},
            {title,                             "title"}
        };

        for (const auto& rule : rules) {
            if (rule.first.empty()) continue;
            for (const std::string& pack : packs) {
                if (EqualsNoCase(StripExtension(pack), rule.first)) {
                    out_matched_by = rule.second;
                    return std::string(kN64MetaPackDir) + "/" + pack;
                }
            }
        }
        return std::string();
    }

}
