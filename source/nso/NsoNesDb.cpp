#include "NsoNesDb.hpp"

#include "NsoJson.hpp"
#include "NsoLog.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

namespace romm::nso {

    namespace {

        // Small POSIX wrappers, file-local for the same reason
        // NsoSnesInstaller.cpp keeps its own: promoting them to the namespace
        // would collide with those copies at every unqualified call site in the
        // files that include both headers.
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

        bool IsKnownNesTitleId(const std::string& id) {
            return EqualsIgnoreCase(id, kKnownNesTitleId) ||
                   EqualsIgnoreCase(id, kKnownFamicomTitleId);
        }

        // Is this candidate the NES app? Every app in the family uses the same
        // lclassics.titlesdb file name, so the codes are what tell them apart —
        // "CLV-P-*" here against SNES's "S-####_e". An empty database carries no
        // evidence either way, which is a real case (CaVE can create one), so it
        // falls back to the title id rather than rejecting a fresh install.
        bool LooksLikeNes(const std::vector<std::string>& codes, const std::string& title_id) {
            if (codes.empty()) return IsKnownNesTitleId(title_id);
            size_t nes = 0;
            const size_t sample = codes.size() < 8 ? codes.size() : 8;
            for (size_t i = 0; i < sample; ++i) {
                if (codes[i].rfind("CLV-P-", 0) == 0) ++nes;
            }
            if (nes * 2 > sample) return true;
            return IsKnownNesTitleId(title_id);
        }

        const char* const kAtmosphereRoots[] = {
            "sdmc:/atmosphere/contents",
            "sdmc:/atmosphere/titles" // pre-1.0 Atmosphère layout
        };

        // Injected codes live in CLV-P-NZ__E, well clear of Nintendo's NA__E
        // block. See the header for why that separation is not cosmetic.
        constexpr const char* kInjectedPrefix = "CLV-P-NZ";
        constexpr int kLetterSpan = 26 * 26;

        std::string CodeFromSlot(int slot) {
            const char a = static_cast<char>('A' + (slot / 26) % 26);
            const char b = static_cast<char>('A' + slot % 26);
            std::string code = kInjectedPrefix;
            code += a;
            code += b;
            code += 'E';
            return code;
        }

    } // namespace

    NsoSnesInstall DetectNsoNes() {
        NsoSnesInstall best;
        auto& log = NsoLog::Instance();

        log.Line("--- NES Online detection ---");

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
                if (!LooksLikeNes(db.codes, id)) {
                    log.Line("    " + id + " : database has " + std::to_string(db.codes.size()) +
                             " entries, not a NES title list");
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
                // Reported for the log only. NES has no signature check and CaVE
                // ships no unlock for it, so absence here is not a warning the
                // way it is on SNES.
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

                if (IsKnownNesTitleId(id)) return candidate;
                if (!best.found) best = candidate;
            }
        }

        if (!best.found) {
            // Same reasoning as the SNES detector: a console with the app but no
            // LayeredFS yet is a target, not a failure. Offer the known NES id so
            // the user is not made to hand-create directories.
            best = NsoSnesInstall();
            best.title_id = kKnownNesTitleId;
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

    std::string BuildNesTitleEntryJson(const NsoTitleMeta& meta, const NsoNesExtras& extras) {
        const std::string dir = "/titles/" + meta.code + "/";
        std::string out;
        out.reserve(720);
        // Alphabetical, minified, literal UTF-8 — the exact shape of the
        // reference entry in docs/nso-nes-format.md.
        out += "{";
        out += "\"armet_threshold\":" + std::to_string(extras.armet_threshold);
        out += ",\"armet_version\":" + json::QuoteString(extras.armet_version);
        out += ",\"code\":" + json::QuoteString(meta.code);
        out += ",\"copyright\":" + json::QuoteString(meta.copyright);
        out += ",\"cover\":" + json::QuoteString(dir + meta.code + ".png");
        // Details screen is "<CODE>00.png" here, not SNES's "<CODE>-details.png".
        out += ",\"details_screen\":" + json::QuoteString(dir + meta.code + "00.png");
        out += ",\"fadein\":[" + std::to_string(extras.fadein_a) + "," +
               std::to_string(extras.fadein_b) + "]";
        out += ",\"lcla6_release_date\":" + json::QuoteString(meta.lcla6_release_date);
        out += ",\"overscan\":[" + std::to_string(extras.overscan_top) + "," +
               std::to_string(extras.overscan_bottom) + "," +
               std::to_string(extras.overscan_left) + "," +
               std::to_string(extras.overscan_right) + "]";
        out += ",\"players_count\":" + std::to_string(meta.players_count);
        out += ",\"publisher\":" + json::QuoteString(meta.publisher);
        out += ",\"release_date\":" + json::QuoteString(meta.release_date);
        // Literal, for the same reason as the SNES builder: printf rounding of a
        // double could emit 1.5000000000000000 on some libcs.
        out += ",\"rewind_interval\":1.5";
        out += ",\"rom\":" + json::QuoteString(dir + meta.code + ".nes");
        out += ",\"save_count\":" + std::to_string(meta.save_count);
        out += ",\"simultaneous\":";
        out += meta.simultaneous ? "true" : "false";
        out += ",\"sort_publisher\":" + json::QuoteString(meta.sort_publisher);
        out += ",\"sort_title\":" + json::QuoteString(meta.sort_title);
        out += ",\"title\":" + json::QuoteString(meta.title);
        out += ",\"title_ko\":" + json::QuoteString(meta.title);
        // U+30FC, what every reference entry stores for a title with no Chinese
        // localization.
        out += ",\"title_zhHans\":\"\xE3\x83\xBC\"";
        out += ",\"title_zhHant\":\"\xE3\x83\xBC\"";
        out += ",\"volume\":" + std::to_string(meta.volume);
        out += "}";
        return out;
    }

    NsoTitleMeta NesDefaultTitleMeta() {
        NsoTitleMeta meta;
        // Both differ from the struct's SNES-shaped defaults, and both are what
        // CaVE wrote for the ROM it injected outside the stock catalogue.
        meta.volume = 80;
        meta.save_count = 0;
        return meta;
    }

    const std::vector<NsoGuideKey>& NesGuideKeys() {
        // Empty on purpose — see the header. CaVE wrote only the description key
        // for its injected NES title.
        static const std::vector<NsoGuideKey> keys;
        return keys;
    }

    bool VerifySerializedNesDb(const std::string& text,
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

        const std::string entry = text.substr(added->value_start, added->value_end - added->value_start);
        const std::string dir = "/titles/" + expected_new + "/";
        const char* required[3] = {".nes", ".png", "00.png"};
        for (const char* suffix : required) {
            const std::string needle = "\"" + dir + expected_new + suffix + "\"";
            if (entry.find(needle) == std::string::npos) {
                error = "new entry does not reference " + dir + expected_new + suffix;
                return false;
            }
        }

        return true;
    }

    std::string AllocateNesGameCode(const TitlesDb& db, const std::string& sha256,
                                    const std::string& preferred_code) {
        if (!preferred_code.empty()) {
            // Reinstalling the same ROM keeps its slot, which is what makes the
            // operation idempotent instead of leaking an entry every run.
            return preferred_code;
        }

        // Deterministic starting point from the ROM hash, so the same dump lands
        // on the same code on a fresh install too.
        const unsigned long seed = std::strtoul(sha256.substr(0, 8).c_str(), nullptr, 16);
        int slot = static_cast<int>(seed % (unsigned long)kLetterSpan);

        for (int attempt = 0; attempt < kLetterSpan; ++attempt) {
            const std::string code = CodeFromSlot(slot);
            if (!HasCode(db, code)) return code;
            slot = (slot + 1) % kLetterSpan;
        }
        return std::string(); // all 676 slots taken; caller reports it
    }

    bool IsInjectedNesCode(const std::string& code) {
        return code.rfind(kInjectedPrefix, 0) == 0;
    }

}
