#include "NsoGbDb.hpp"

#include "NsoJson.hpp"
#include "NsoLog.hpp"

#include <cctype>
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

namespace romm::nso {

    namespace {

        // File-local POSIX wrappers, as in NsoNesDb — see the note there for
        // why these are not shared.
        bool PathExists(const std::string& path) {
            struct stat st {};
            return stat(path.c_str(), &st) == 0;
        }

        bool IsDirectory(const std::string& path) {
            struct stat st {};
            return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }

        void ListSubdirectories(const std::string& path, std::vector<std::string>& out) {
            DIR* dir = opendir(path.c_str());
            if (!dir) return;
            while (dirent* entry = readdir(dir)) {
                const std::string name = entry->d_name;
                if (name == "." || name == "..") continue;
                if (IsDirectory(path + "/" + name)) out.push_back(name);
            }
            closedir(dir);
        }

        bool LooksLikeTitleId(const std::string& name) {
            if (name.size() != 16) return false;
            for (char c : name) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            }
            return true;
        }

        bool EqualsIgnoreCase(const std::string& a, const char* b) {
            size_t i = 0;
            for (; i < a.size() && b[i]; ++i) {
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
            }
            return i == a.size() && b[i] == '\0';
        }

        bool IsKnownGbTitleId(const std::string& id) {
            return EqualsIgnoreCase(id, kKnownGbTitleId) || EqualsIgnoreCase(id, kKnownGbTitleIdJp);
        }

        // "D-3DB7_e" or "C-7224_e": one letter, a dash, four hex digits. The
        // SNES app's codes are "S-####_e", which have the same shape but a
        // different letter — hence the explicit prefix test rather than a
        // generic "letter-dash-digits" one.
        bool IsGbCode(const std::string& code) {
            if (code.size() < 6) return false;
            if ((code[0] != 'C' && code[0] != 'D') || code[1] != '-') return false;
            for (size_t i = 2; i < 6; ++i) {
                if (!std::isxdigit(static_cast<unsigned char>(code[i]))) return false;
            }
            return true;
        }

        bool LooksLikeGb(const std::vector<std::string>& codes, const std::string& title_id) {
            if (codes.empty()) return IsKnownGbTitleId(title_id);
            size_t gb = 0;
            const size_t sample = codes.size() < 8 ? codes.size() : 8;
            for (size_t i = 0; i < sample; ++i) {
                if (IsGbCode(codes[i])) ++gb;
            }
            if (gb * 2 > sample) return true;
            return IsKnownGbTitleId(title_id);
        }

        const char* const kAtmosphereRoots[] = {
            "sdmc:/atmosphere/contents",
            "sdmc:/atmosphere/titles"
        };

    } // namespace

    const char* GbRomSuffix(GbMode mode) {
        return mode == GbMode::Cgb ? ".gbc" : ".gb";
    }

    NsoSnesInstall DetectNsoGb() {
        NsoSnesInstall best;
        auto& log = NsoLog::Instance();

        log.Line("--- Game Boy Online detection ---");

        for (const char* root : kAtmosphereRoots) {
            if (!IsDirectory(root)) {
                log.Line("  root " + std::string(root) + " : does not exist");
                continue;
            }

            std::vector<std::string> ids;
            ListSubdirectories(root, ids);
            log.Line("  root " + std::string(root) + " : " + std::to_string(ids.size()) + " subdirectories");

            for (const std::string& id : ids) {
                if (!LooksLikeTitleId(id)) continue;

                const std::string content_root = std::string(root) + "/" + id;
                const std::string romfs_root = content_root + "/romfs";
                const std::string titles_dir = romfs_root + "/titles";
                const std::string db_path = titles_dir + "/lclassics.titlesdb";
                if (!PathExists(db_path)) {
                    log.Line("    " + id + " : no romfs/titles/lclassics.titlesdb");
                    continue;
                }

                TitlesDb db;
                if (!LoadTitlesDb(db_path, db)) {
                    log.Line("    " + id + " : database unreadable - " + db.error);
                    continue;
                }
                if (!LooksLikeGb(db.codes, id)) {
                    log.Line("    " + id + " : database has " + std::to_string(db.codes.size()) +
                             " entries, not a Game Boy title list");
                    continue;
                }
                log.Line("    " + id + " : ACCEPTED, " + std::to_string(db.codes.size()) + " entries" +
                         (db.has_titles_object ? "" : " (no \"titles\" object yet - new database)"));

                NsoSnesInstall candidate;
                candidate.found = true;
                candidate.title_id = id;
                candidate.content_root = content_root;
                candidate.romfs_root = romfs_root;
                candidate.titles_dir = titles_dir;
                candidate.database_path = db_path;
                candidate.database_exists = true;
                candidate.entry_count = db.codes.size();
                candidate.has_exefs_mod = PathExists(content_root + "/exefs/subsdk9");
                if (PathExists(romfs_root + "/DBINFO")) candidate.dbinfo_path = romfs_root + "/DBINFO";

                const std::string strings_root = romfs_root + "/bootapp/resources/strings";
                std::vector<std::string> langs;
                ListSubdirectories(strings_root, langs);
                for (const std::string& lang : langs) {
                    const std::string file = strings_root + "/" + lang + "/strings.lng";
                    if (PathExists(file)) {
                        candidate.strings_files.push_back(file);
                        candidate.strings_languages.push_back(lang);
                    }
                }

                std::vector<std::string> asset_dirs;
                ListSubdirectories(titles_dir, asset_dirs);
                candidate.injected_asset_dirs = asset_dirs.size();

                if (IsKnownGbTitleId(id)) return candidate;
                if (!best.found) best = candidate;
            }
        }

        if (!best.found) {
            best = NsoSnesInstall();
            best.title_id = kKnownGbTitleId;
            best.content_root = std::string(kAtmosphereRoots[0]) + "/" + best.title_id;
            best.romfs_root = best.content_root + "/romfs";
            best.titles_dir = best.romfs_root + "/titles";
            best.database_path = best.titles_dir + "/lclassics.titlesdb";
            best.database_exists = false;
            best.found = true;
            best.error = "no database yet - one can be created at " + best.database_path;
            log.Line("  no database found; offering " + best.title_id + " as a target to create");
        }
        return best;
    }

    NsoTitleMeta GbDefaultTitleMeta() {
        NsoTitleMeta meta;
        meta.save_count = 1;
        meta.volume = 100;
        return meta;
    }

    const std::vector<NsoGuideKey>& GbGuideKeys() {
        static const std::vector<NsoGuideKey> keys;
        return keys;
    }

    std::string BuildGbTitleEntryJson(const NsoTitleMeta& meta, GbMode mode,
                                      const NsoGbExtras& extras) {
        const std::string dir = "/titles/" + meta.code + "/";
        const bool dmg = (mode == GbMode::Dmg);
        std::string out;
        out.reserve(720);
        // ASCII order, which puts the capitalised palette keys first — exactly
        // the order CaVE writes.
        out += "{";
        out += "\"CGB-Default\":" + json::QuoteString(extras.cgb_default);
        out += ",\"CGB-Nostalgic\":" + json::QuoteString(extras.cgb_nostalgic);
        if (dmg) {
            // Only DMG entries carry these, in CaVE's output and in the stock
            // catalogue alike — a Color game has no monochrome palette to set.
            out += ",\"DMG_MGB-Default\":" + json::QuoteString(extras.dmg_default);
            out += ",\"DMG_MGB-Nostalgic\":" + json::QuoteString(extras.dmg_nostalgic);
        }
        out += ",\"code\":" + json::QuoteString(meta.code);
        out += ",\"copyright\":" + json::QuoteString(meta.copyright);
        out += ",\"cover\":" + json::QuoteString(dir + meta.code + ".png");
        out += ",\"details_screen\":" + json::QuoteString(dir + meta.code + "-details.png");
        out += ",\"lcla6_release_date\":" + json::QuoteString(meta.lcla6_release_date);
        out += ",\"platform\":\"";
        out += dmg ? "DMG" : "CGB";
        out += "\"";
        out += ",\"players_count\":" + std::to_string(meta.players_count);
        out += ",\"publisher\":" + json::QuoteString(meta.publisher);
        out += ",\"release_date\":" + json::QuoteString(meta.release_date);
        out += ",\"rewind_interval\":1.5";
        out += ",\"rom\":" + json::QuoteString(dir + meta.code + GbRomSuffix(mode));
        out += ",\"save_count\":" + std::to_string(meta.save_count);
        out += ",\"sort_publisher\":" + json::QuoteString(meta.sort_publisher);
        out += ",\"sort_title\":" + json::QuoteString(meta.sort_title);
        out += ",\"title\":" + json::QuoteString(meta.title);
        out += ",\"title_ko\":" + json::QuoteString(meta.title);
        out += ",\"volume\":" + std::to_string(meta.volume);
        out += "}";
        return out;
    }

    bool VerifySerializedGbDb(const std::string& text,
                              const std::vector<std::string>& before,
                              const std::string& expected_new,
                              GbMode mode,
                              std::string& error) {
        error.clear();

        std::string validation_error;
        if (!json::Validate(text, validation_error)) {
            error = "serialized database is not valid JSON (" + validation_error + ")";
            return false;
        }

        json::Member titles;
        if (!json::FindRootMember(text, "titles", titles)) {
            error = "serialized database lost its \"titles\" object";
            return false;
        }

        std::vector<json::Member> entries;
        if (!json::ScanObject(text, titles.value_start, entries)) {
            error = "serialized \"titles\" object could not be enumerated";
            return false;
        }

        for (const std::string& code : before) {
            if (!json::FindMember(entries, code)) {
                error = "existing entry " + code + " disappeared from the serialized database";
                return false;
            }
        }

        const json::Member* added = json::FindMember(entries, expected_new);
        if (!added) {
            error = "new entry " + expected_new + " is missing from the serialized database";
            return false;
        }

        const std::string entry = text.substr(added->value_start, added->value_end - added->value_start);
        const std::string dir = "/titles/" + expected_new + "/";
        const std::string required[3] = {GbRomSuffix(mode), ".png", "-details.png"};
        for (const std::string& suffix : required) {
            const std::string needle = "\"" + dir + expected_new + suffix + "\"";
            if (entry.find(needle) == std::string::npos) {
                error = "new entry does not reference " + dir + expected_new + suffix;
                return false;
            }
        }

        return true;
    }

    std::string AllocateGbGameCode(const TitlesDb& db, const GbRomInfo& rom,
                                   const std::string& preferred_code) {
        if (!preferred_code.empty()) return preferred_code;

        const char prefix = (rom.mode == GbMode::Cgb) ? 'C' : 'D';
        const std::string base = GbChecksumHex(rom.global_checksum);

        // The natural code first: prefix + the ROM's own global checksum, which
        // is what CaVE would have produced for this dump.
        const std::string wanted = std::string(1, prefix) + "-" + base + "_e";
        if (!HasCode(db, wanted)) return wanted;

        // Taken. Rather than overwrite whatever is sitting there — it could be a
        // stock title, or CaVE's copy of this very game — walk the 16 codes that
        // share the top three nibbles, then give up and let the caller report it.
        for (int nibble = 0; nibble < 16; ++nibble) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%c-%c%c%c%X_e", prefix,
                          base[0], base[1], base[2], nibble);
            const std::string candidate = buf;
            if (candidate == wanted) continue;
            if (!HasCode(db, candidate)) return candidate;
        }
        return std::string();
    }

}
