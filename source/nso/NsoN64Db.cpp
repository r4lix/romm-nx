#include "NsoN64Db.hpp"

#include "NsoJson.hpp"
#include "NsoLog.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

namespace romm::nso {

    namespace {

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

        bool IsKnownN64TitleId(const std::string& id) {
            for (const char* known : kKnownN64TitleIds) {
                if (EqualsIgnoreCase(id, known)) return true;
            }
            return false;
        }

        // "N-9416_e", "N-2153_pf": 'N', a dash, four digits, then a language
        // suffix that is one or two letters (_e, _p, _j, _pd, _pf, _ps, _pi).
        bool IsN64Code(const std::string& code) {
            if (code.size() < 6) return false;
            if (code[0] != 'N' || code[1] != '-') return false;
            for (size_t i = 2; i < 6; ++i) {
                if (!std::isdigit(static_cast<unsigned char>(code[i]))) return false;
            }
            return true;
        }

        bool LooksLikeN64(const std::vector<std::string>& codes, const std::string& title_id) {
            if (codes.empty()) return IsKnownN64TitleId(title_id);
            size_t n64 = 0;
            const size_t sample = codes.size() < 8 ? codes.size() : 8;
            for (size_t i = 0; i < sample; ++i) {
                if (IsN64Code(codes[i])) ++n64;
            }
            if (n64 * 2 > sample) return true;
            return IsKnownN64TitleId(title_id);
        }

        const char* const kAtmosphereRoots[] = {
            "sdmc:/atmosphere/contents",
            "sdmc:/atmosphere/titles"
        };

    } // namespace

    NsoSnesInstall DetectNsoN64() {
        NsoSnesInstall best;
        auto& log = NsoLog::Instance();

        log.Line("--- Nintendo 64 Online detection ---");

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
                if (!LooksLikeN64(db.codes, id)) {
                    log.Line("    " + id + " : database has " + std::to_string(db.codes.size()) +
                             " entries, not a Nintendo 64 title list");
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

                // An exact id match wins; the four apps are otherwise
                // indistinguishable by their databases.
                if (IsKnownN64TitleId(id)) return candidate;
                if (!best.found) best = candidate;
            }
        }

        if (!best.found) {
            best = NsoSnesInstall();
            best.title_id = kKnownN64TitleIds[0];
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

    NsoTitleMeta N64DefaultTitleMeta() {
        NsoTitleMeta meta;
        meta.save_count = 1;
        meta.volume = 80;
        return meta;
    }

    const std::vector<NsoGuideKey>& N64GuideKeys() {
        // Exactly the 26 CaVE writes, with its own placeholder labels. The
        // controller is the reason this list is so much longer than the others:
        // four C buttons, four stick directions and Z on top of the usual set.
        static const std::vector<NsoGuideKey> keys = {
            {"META_TITLE_KEY_GUIDE_a_",             "A"},
            {"META_TITLE_KEY_GUIDE_b_",             "B"},
            {"META_TITLE_KEY_GUIDE_cunit_",         "C"},
            {"META_TITLE_KEY_GUIDE_cunit_down_",    "C Down"},
            {"META_TITLE_KEY_GUIDE_cunit_left_",    "C Left"},
            {"META_TITLE_KEY_GUIDE_cunit_right_",   "C Right"},
            {"META_TITLE_KEY_GUIDE_cunit_up_",      "C Up"},
            {"META_TITLE_KEY_GUIDE_dpad_",          "D-Pad"},
            {"META_TITLE_KEY_GUIDE_dpad_down_",     "D-Pad Down"},
            {"META_TITLE_KEY_GUIDE_dpad_left_",     "D-Pad Left"},
            {"META_TITLE_KEY_GUIDE_dpad_right_",    "D-Pad Right"},
            {"META_TITLE_KEY_GUIDE_dpad_up_",       "D-Pad Up"},
            {"META_TITLE_KEY_GUIDE_l_",             "L"},
            {"META_TITLE_KEY_GUIDE_mouse_l_",       "Mouse L"},
            {"META_TITLE_KEY_GUIDE_mouse_r_",       "Mouse R"},
            {"META_TITLE_KEY_GUIDE_notation_",      "Notation"},
            {"META_TITLE_KEY_GUIDE_r_",             "R"},
            {"META_TITLE_KEY_GUIDE_start_",         "Start"},
            {"META_TITLE_KEY_GUIDE_stick_",         "Stick"},
            {"META_TITLE_KEY_GUIDE_stick_down_",    "Stick Down"},
            {"META_TITLE_KEY_GUIDE_stick_left_",    "Stick Left"},
            {"META_TITLE_KEY_GUIDE_stick_right_",   "Stick Right"},
            {"META_TITLE_KEY_GUIDE_stick_up_",      "Stick Up"},
            {"META_TITLE_KEY_GUIDE_supplementary_", "Supplementary"},
            {"META_TITLE_KEY_GUIDE_y_",             "Y"},
            {"META_TITLE_KEY_GUIDE_z_r_",           "Z"}
        };
        return keys;
    }

    std::string BuildN64TitleEntryJson(const NsoTitleMeta& meta, const NsoN64Extras& extras) {
        const std::string dir = "/titles/" + meta.code + "/";
        std::string out;
        out.reserve(800);
        // Key order as CaVE writes it for an injected title: alphabetical with
        // GPU384MHz folded in at "g" rather than sorted ahead of the lowercase
        // keys. The stock entries put it first instead — this reproduces what
        // the tool writes, which is what these files are compared against.
        out += "{";
        out += "\"code\":" + json::QuoteString(meta.code);
        out += ",\"cold_reset\":";
        out += extras.cold_reset ? "true" : "false";
        out += ",\"control_opt_layout_pattern\":" + std::to_string(extras.control_opt_layout_pattern);
        out += ",\"controller_position\":" + json::QuoteString(extras.controller_position);
        out += ",\"copyright\":" + json::QuoteString(meta.copyright);
        out += ",\"cover\":" + json::QuoteString(dir + meta.code + ".png");
        out += ",\"details_screen\":" + json::QuoteString(dir + meta.code + "-details.png");
        out += ",\"GPU384MHz\":";
        out += extras.gpu_384mhz ? "true" : "false";
        out += ",\"lcla6_release_date\":" + json::QuoteString(meta.lcla6_release_date);
        out += ",\"players_count\":" + std::to_string(meta.players_count);
        out += ",\"publisher\":" + json::QuoteString(meta.publisher);
        out += ",\"release_date\":" + json::QuoteString(meta.release_date);
        // No rewind_interval: the N64 app has no rewind, and no stock entry
        // carries the field.
        out += ",\"rom\":" + json::QuoteString(dir + meta.code + ".bin");
        out += ",\"save_count\":" + std::to_string(meta.save_count);
        out += ",\"simultaneous\":";
        out += meta.simultaneous ? "true" : "false";
        out += ",\"sort_publisher\":" + json::QuoteString(meta.sort_publisher);
        out += ",\"sort_title\":" + json::QuoteString(meta.sort_title);
        out += ",\"sram_file_size\":" + std::to_string(extras.sram_file_size);
        out += ",\"title\":" + json::QuoteString(meta.title);
        out += ",\"title_ko\":" + json::QuoteString(meta.title);
        out += ",\"title_zhHans\":\"\xE3\x83\xBC\"";
        out += ",\"title_zhHant\":\"\xE3\x83\xBC\"";
        out += ",\"volume\":" + std::to_string(meta.volume);
        out += "}";
        return out;
    }

    bool VerifySerializedN64Db(const std::string& text,
                               const std::vector<std::string>& before,
                               const std::string& expected_new,
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

        // ".bin" and not ".bnz": the entry names the uncompressed path even
        // though the compressed file is what sits next to it on the card.
        const std::string entry = text.substr(added->value_start, added->value_end - added->value_start);
        const std::string dir = "/titles/" + expected_new + "/";
        const char* required[3] = {".bin", ".png", "-details.png"};
        for (const char* suffix : required) {
            const std::string needle = "\"" + dir + expected_new + suffix + "\"";
            if (entry.find(needle) == std::string::npos) {
                error = "new entry does not reference " + dir + expected_new + suffix;
                return false;
            }
        }

        return true;
    }

    std::string AllocateN64GameCode(const TitlesDb& db, const std::string& sha256,
                                    const std::string& preferred_code) {
        if (!preferred_code.empty()) return preferred_code;

        // Four digits, seeded from the ROM hash so a given dump always lands on
        // the same code. Stock codes are four digits too and occupy the low
        // ranges; starting at 5000 keeps romm-nx clear of them without ruling
        // out a collision the probe below still handles.
        unsigned long seed = std::strtoul(sha256.substr(0, 8).c_str(), nullptr, 16);
        const int span = 5000; // 5000..9999
        int candidate = 5000 + (int)(seed % (unsigned long)span);

        for (int attempt = 0; attempt < span; ++attempt) {
            const std::string code = "N-" + std::to_string(candidate) + "_e";
            if (!HasCode(db, code)) return code;
            candidate = 5000 + ((candidate - 5000 + 1) % span);
        }
        return std::string();
    }

}
