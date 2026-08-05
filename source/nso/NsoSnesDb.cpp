#include "NsoSnesDb.hpp"

#include "NsoJson.hpp"
#include "NsoLog.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <map>
#include <sys/stat.h>

namespace romm::nso {

    namespace {

        // The SNES entries in Nintendo's Switch Online family. Kept as a
        // preference rather than a requirement so a different region's build
        // still gets picked up by the structural scan below.
        constexpr const char* kKnownSnesTitleId = "01008D300C50C000";
        const char* const kKnownSnesTitleIds[] = {
            "01008D300C50C000", // SNES (Americas / Europe)
            "0100E8600C504000"  // Super Famicom (Japan)
        };

        const char* kAtmosphereRoots[] = {
            "sdmc:/atmosphere/contents",
            "sdmc:/atmosphere/titles" // pre-1.0 Atmosphère layout
        };

        bool PathExists(const std::string& path) {
            struct stat st {};
            return stat(path.c_str(), &st) == 0;
        }

        bool IsDirectory(const std::string& path) {
            struct stat st {};
            return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }

        bool ReadFileText(const std::string& path, std::string& out) {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) return false;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size < 0) {
                std::fclose(f);
                return false;
            }
            out.resize((size_t)size);
            const size_t read = size > 0 ? std::fread(&out[0], 1, out.size(), f) : 0;
            std::fclose(f);
            return read == out.size();
        }

        bool LooksLikeTitleId(const std::string& name) {
            if (name.size() != 16) return false;
            for (char c : name) {
                if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
            }
            return true;
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

        bool EqualsIgnoreCase(const std::string& a, const char* b) {
            size_t i = 0;
            for (; a[i] && b[i]; ++i) {
                if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
            }
            return i == a.size() && b[i] == '\0';
        }

        bool IsKnownSnesTitleId(const std::string& id) {
            for (const char* known : kKnownSnesTitleIds) {
                if (EqualsIgnoreCase(id, known)) return true;
            }
            return false;
        }

        // Is this candidate the SNES app? Normally decided by the title codes,
        // which are "S-####_e" here and use their own prefixes in the NES / N64 /
        // GB / Mega Drive apps (all of which use this same file name).
        //
        // A database with no codes at all is the case that matters: CaVE can
        // create an empty database, and rejecting it outright — which an earlier
        // build did — made romm-nx report "No SNES Online LayeredFS found" on a
        // perfectly good, freshly created install. With nothing to inspect, fall
        // back to the title id.
        bool LooksLikeSnes(const std::vector<std::string>& codes, const std::string& title_id) {
            if (codes.empty()) return IsKnownSnesTitleId(title_id);
            size_t snes = 0;
            const size_t sample = codes.size() < 8 ? codes.size() : 8;
            for (size_t i = 0; i < sample; ++i) {
                if (codes[i].size() > 2 && codes[i][0] == 'S' && codes[i][1] == '-') ++snes;
            }
            if (snes * 2 > sample) return true;
            // Codes that don't look like ours, but the id says SNES: trust the id.
            return IsKnownSnesTitleId(title_id);
        }

        // Collation weight of one byte: punctuation and everything else first,
        // then digits, then letters. This is the ordering .NET's default
        // (culture-aware) string comparer produces, which is what wrote the
        // string tables romm-nx has to insert into.
        int CollateClass(unsigned char c) {
            if (c >= '0' && c <= '9') return 1;
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return 2;
            return 0;
        }

        unsigned char CollateFold(unsigned char c) {
            if (c >= 'A' && c <= 'Z') return (unsigned char)(c - 'A' + 'a');
            return c;
        }

    } // namespace

    int CollateCompare(const std::string& a, const std::string& b) {
        const size_t shared = (a.size() < b.size()) ? a.size() : b.size();
        for (size_t i = 0; i < shared; ++i) {
            const unsigned char ca = (unsigned char)a[i];
            const unsigned char cb = (unsigned char)b[i];

            const int class_a = CollateClass(ca);
            const int class_b = CollateClass(cb);
            if (class_a != class_b) return (class_a < class_b) ? -1 : 1;

            const unsigned char folded_a = CollateFold(ca);
            const unsigned char folded_b = CollateFold(cb);
            if (folded_a != folded_b) return (folded_a < folded_b) ? -1 : 1;
        }
        if (a.size() != b.size()) return (a.size() < b.size()) ? -1 : 1;
        // Total order: fall back to bytes so keys differing only in case never
        // compare equal.
        return a.compare(b) < 0 ? -1 : (a == b ? 0 : 1);
    }

    std::string CodeToStringKey(const std::string& code) {
        std::string out = code;
        std::replace(out.begin(), out.end(), '-', '_');
        return out;
    }

    std::string MakeSortKey(const std::string& value) {
        std::string out;
        out.reserve(value.size());
        bool pending_space = false;
        for (unsigned char c : value) {
            if (c >= 0x80) continue;           // drop ™, ®, accents: the reference keys are ASCII
            if (c < 0x20) { pending_space = true; continue; }
            if (c == ' ') { pending_space = true; continue; }
            if (pending_space && !out.empty()) out.push_back(' ');
            pending_space = false;
            out.push_back((char)std::tolower(c));
        }
        return out;
    }

    NsoSnesInstall DetectNsoSnes() {
        NsoSnesInstall best;
        auto& log = NsoLog::Instance();

        // Detection failing is the single most confusing way this feature can
        // go wrong — the screen just says "not found" with no clue why. Record
        // every root scanned and every candidate rejected, so the log answers
        // the question instead of the user guessing.
        log.Line("--- SNES Online detection ---");

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
                if (!LooksLikeSnes(db.codes, id)) {
                    log.Line("    " + id + " : database has " + std::to_string(db.codes.size()) +
                             " entries, not a SNES title list");
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
                std::sort(langs.begin(), langs.end());
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

                // An exact title-id match wins outright; otherwise keep the
                // first structurally valid candidate found.
                if (id == kKnownSnesTitleId) return candidate;
                if (!best.found) best = candidate;
            }
        }

        if (!best.found) {
            // Nothing with a database. That is not the same as "nowhere to
            // inject": a console with the app and the Full Unlock but no
            // LayeredFS yet is a valid target, it just needs an empty database
            // written into it first. Offer the known SNES title id so the user
            // is not made to hand-create directories.
            best = NsoSnesInstall();
            best.title_id = kKnownSnesTitleId;
            best.content_root = std::string(kAtmosphereRoots[0]) + "/" + best.title_id;
            best.romfs_root = best.content_root + "/romfs";
            best.titles_dir = best.romfs_root + "/titles";
            best.database_path = best.titles_dir + "/lclassics.titlesdb";
            best.has_exefs_mod = PathExists(best.content_root + "/exefs/subsdk9");
            best.database_exists = false;
            best.found = true; // a target, pending a database
            best.error = "no database yet - one can be created at " + best.database_path;
            log.Line("  no database found; offering " + best.title_id + " as a target to create");
        }
        return best;
    }

    bool LoadTitlesDb(const std::string& path, TitlesDb& out) {
        out = TitlesDb();
        if (!ReadFileText(path, out.text)) {
            out.error = "cannot read " + path;
            return false;
        }
        if (out.text.empty()) {
            out.error = "database is empty: " + path;
            return false;
        }

        std::string validation_error;
        if (!json::Validate(out.text, validation_error)) {
            out.error = "database is not valid JSON (" + validation_error + ")";
            return false;
        }

        const size_t root = json::SkipWhitespace(out.text, 0);
        if (root == std::string::npos || out.text[root] != '{') {
            out.error = "database root is not a JSON object";
            return false;
        }
        out.root_object_start = root;

        json::Member titles;
        if (!json::FindRootMember(out.text, "titles", titles)) {
            // A freshly created, still-empty CaVE database is a perfectly good
            // target — it just has nothing to enumerate yet. Treat a missing
            // "titles" member as "zero titles so far", not as a broken file.
            out.has_titles_object = false;
            out.loaded = true;
            return true;
        }
        if (out.text[titles.value_start] != '{') {
            out.error = "\"titles\" is not an object";
            return false;
        }
        out.has_titles_object = true;
        out.titles_object_start = titles.value_start;

        std::vector<json::Member> entries;
        if (!json::ScanObject(out.text, out.titles_object_start, entries)) {
            out.error = "\"titles\" object could not be enumerated";
            return false;
        }
        out.codes.reserve(entries.size());
        for (const auto& entry : entries) out.codes.push_back(entry.key);

        out.loaded = true;
        return true;
    }

    bool HasCode(const TitlesDb& db, const std::string& code) {
        for (const auto& existing : db.codes) {
            if (existing == code) return true;
        }
        return false;
    }

    bool GetEntryField(const TitlesDb& db, const std::string& code,
                       const std::string& field, std::string& out) {
        out.clear();
        if (!db.loaded) return false;

        std::vector<json::Member> entries;
        if (!json::ScanObject(db.text, db.titles_object_start, entries)) return false;
        const json::Member* entry = json::FindMember(entries, code);
        if (!entry || db.text[entry->value_start] != '{') return false;

        std::vector<json::Member> fields;
        if (!json::ScanObject(db.text, entry->value_start, fields)) return false;
        const json::Member* member = json::FindMember(fields, field);
        if (!member || db.text[member->value_start] != '"') return false;

        return json::DecodeString(db.text, member->value_start, out);
    }

    std::string BuildTitleEntryJson(const NsoTitleMeta& meta) {
        const std::string dir = "/titles/" + meta.code + "/";
        std::string out;
        out.reserve(640);
        out += "{";
        out += "\"code\":" + json::QuoteString(meta.code);
        out += ",\"copyright\":" + json::QuoteString(meta.copyright);
        out += ",\"cover\":" + json::QuoteString(dir + meta.code + ".png");
        out += ",\"details_screen\":" + json::QuoteString(dir + meta.code + "-details.png");
        out += ",\"lcla6_release_date\":" + json::QuoteString(meta.lcla6_release_date);
        out += ",\"players_count\":" + std::to_string(meta.players_count);
        out += ",\"publisher\":" + json::QuoteString(meta.publisher);
        out += ",\"release_date\":" + json::QuoteString(meta.release_date);
        // Rendered literally: the reference entries all carry 1.5, and printf
        // rounding of a double could emit 1.5000000000000000 on some libcs.
        out += ",\"rewind_interval\":1.5";
        out += ",\"rom\":" + json::QuoteString(dir + meta.code + ".sfrom");
        out += ",\"save_count\":" + std::to_string(meta.save_count);
        out += ",\"simultaneous\":";
        out += meta.simultaneous ? "true" : "false";
        out += ",\"sort_publisher\":" + json::QuoteString(meta.sort_publisher);
        out += ",\"sort_title\":" + json::QuoteString(meta.sort_title);
        out += ",\"title\":" + json::QuoteString(meta.title);
        out += ",\"title_ko\":" + json::QuoteString(meta.title);
        // U+30FC, exactly what every reference entry stores for the Chinese
        // localizations of a title that has none.
        out += ",\"title_zhHans\":\"\xE3\x83\xBC\"";
        out += ",\"title_zhHant\":\"\xE3\x83\xBC\"";
        out += ",\"volume\":" + std::to_string(meta.volume);
        out += "}";
        return out;
    }

    bool UpsertTitleEntry(const TitlesDb& db, const std::string& code,
                          const std::string& entry_json,
                          std::string& out_text, std::string& error) {
        error.clear();
        if (!db.loaded) {
            error = "database was not loaded";
            return false;
        }

        if (!db.has_titles_object) {
            // Database exists but has no "titles" member yet (a brand-new CaVE
            // database). Create the whole object around this first entry.
            return json::InsertMember(db.text, db.root_object_start, "titles",
                                      "{" + json::QuoteString(code) + ":" + entry_json + "}",
                                      out_text, error);
        }

        std::vector<json::Member> entries;
        if (!json::ScanObject(db.text, db.titles_object_start, entries)) {
            error = "\"titles\" object could not be enumerated";
            return false;
        }

        if (const json::Member* existing = json::FindMember(entries, code)) {
            // Reinstall of the same ROM: replace just this entry's value span.
            out_text = json::ReplaceValue(db.text, *existing, entry_json);
            return true;
        }

        return json::InsertMember(db.text, db.titles_object_start, code, entry_json, out_text, error);
    }

    bool VerifySerializedDb(const std::string& text,
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

        // The entry must point at the three files the pipeline generates.
        const std::string entry = text.substr(added->value_start, added->value_end - added->value_start);
        const std::string dir = "/titles/" + expected_new + "/";
        const char* required[3] = {".sfrom", ".png", "-details.png"};
        for (const char* suffix : required) {
            const std::string needle = "\"" + dir + expected_new + suffix + "\"";
            if (entry.find(needle) == std::string::npos) {
                error = "new entry does not reference " + dir + expected_new + suffix;
                return false;
            }
        }

        return true;
    }

    std::string AllocateGameCode(const TitlesDb& db, const std::string& sha256,
                                 const std::string& preferred_code) {
        if (!preferred_code.empty()) {
            // Reinstalling the same ROM keeps its slot, which is what makes the
            // operation idempotent instead of leaking a new entry every run.
            return preferred_code;
        }

        // Deterministic starting point from the ROM hash so the same dump lands
        // on the same code on a fresh install too.
        unsigned long seed = std::strtoul(sha256.substr(0, 8).c_str(), nullptr, 16);
        const int span = 7000; // 3000..9999, the range CaVE's own codes live in
        int candidate = 3000 + (int)(seed % (unsigned long)span);

        for (int attempt = 0; attempt < span; ++attempt) {
            const std::string code = "S-" + std::to_string(candidate) + "_e";
            if (!HasCode(db, code)) return code;
            candidate = 3000 + ((candidate - 3000 + 1) % span);
        }
        return std::string(); // database is full of S-codes; caller reports it
    }

    const std::vector<NsoGuideKey>& SnesGuideKeys() {
        // Exactly the key set CaVE writes for an injected SNES title: one label
        // per input the key-guide screen can show.
        static const std::vector<NsoGuideKey> keys = {
            {"META_TITLE_KEY_GUIDE_a_",             "A"},
            {"META_TITLE_KEY_GUIDE_b_",             "B"},
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
            {"META_TITLE_KEY_GUIDE_select_",        "Select"},
            {"META_TITLE_KEY_GUIDE_start_",         "Start"},
            {"META_TITLE_KEY_GUIDE_supplementary_", "Supplementary"},
            {"META_TITLE_KEY_GUIDE_x_",             "X"},
            {"META_TITLE_KEY_GUIDE_y_",             "Y"}
        };
        return keys;
    }

    bool PatchStringsFile(const std::string& text, const std::string& code,
                          const std::string& description,
                          const std::vector<NsoGuideKey>& guide,
                          std::string& out_text, bool& changed, std::string& error) {
        error.clear();
        changed = false;
        out_text = text;

        std::string validation_error;
        if (!json::Validate(text, validation_error)) {
            error = "strings file is not valid JSON (" + validation_error + ")";
            return false;
        }

        json::Member strings;
        if (!json::FindRootMember(text, "strings", strings)) {
            error = "strings file has no top-level \"strings\" object";
            return false;
        }
        if (text[strings.value_start] != '{') {
            error = "\"strings\" is not an object";
            return false;
        }

        const std::string key_code = CodeToStringKey(code);

        // The info-screen description plus the caller's key-guide set.
        std::vector<std::pair<std::string, std::string>> wanted;
        wanted.reserve(1 + guide.size());
        wanted.emplace_back("META_TITLE_COMMENT_" + key_code, description);
        for (const auto& kv : guide) {
            wanted.emplace_back(std::string(kv.suffix) + key_code, kv.label);
        }

        // KEY ORDER MATTERS. A stock strings.lng is sorted, and the first build
        // of this feature appended its keys at the end instead — the emulator
        // then rendered the raw key name ("META_TITLE_COMMENT_S_9896_e") on the
        // game's info screen where the description belongs.
        //
        // The order is NOT strcmp: the table is collated the way .NET's default
        // comparer does it (CaVE is a C# app), i.e. punctuation sorts before
        // digits, digits before letters, and letters compare case-folded. That
        // is why "..._WITH_PASSWORD" precedes "..._WITHOUT_PASSWORD" and
        // "..._dpad_right_S_2152_e" precedes "..._dpad_S_2002_e", both of which
        // a byte comparison gets backwards. CollateCompare below reproduces the
        // order of all 2438 keys in a stock file, in both objects, with zero
        // violations — an ordinal comparator scores 5+ violations immediately
        // and would have inserted keys in the wrong place on every run.
        std::vector<json::Member> members;
        if (!json::ScanObject(text, strings.value_start, members)) {
            error = "\"strings\" object could not be enumerated";
            return false;
        }

        // True when this member still sits between its neighbours. Anything
        // romm-nx appended in an earlier build fails this and gets moved.
        auto correctly_placed = [&members](size_t index) {
            if (index > 0 && CollateCompare(members[index - 1].key, members[index].key) >= 0) return false;
            if (index + 1 < members.size() && CollateCompare(members[index].key, members[index + 1].key) >= 0) return false;
            return true;
        };

        struct Edit {
            size_t offset;
            size_t erase;
            std::string insert;
        };
        std::vector<Edit> edits;
        std::vector<std::pair<std::string, std::string>> to_insert; // key -> value json

        for (const auto& kv : wanted) {
            const std::string value_json = json::QuoteString(kv.second);

            size_t index = members.size();
            for (size_t i = 0; i < members.size(); ++i) {
                if (members[i].key == kv.first) { index = i; break; }
            }

            if (index == members.size()) {
                to_insert.emplace_back(kv.first, value_json);
                continue;
            }

            const json::Member& member = members[index];
            if (correctly_placed(index)) {
                const std::string current = text.substr(member.value_start,
                                                        member.value_end - member.value_start);
                if (current == value_json) continue;
                edits.push_back({member.value_start, member.value_end - member.value_start, value_json});
                continue;
            }

            // Misplaced (an earlier romm-nx run appended it): drop it here and
            // let the insertion pass put it back where it belongs. Removing a
            // member takes one adjacent comma with it, or the object is left
            // with a dangling separator.
            if (index > 0) {
                const size_t from = members[index - 1].value_end; // comma before us
                edits.push_back({from, member.value_end - from, std::string()});
            } else if (members.size() > 1) {
                const size_t to = members[index + 1].key_start;   // comma after us
                edits.push_back({member.key_start, to - member.key_start, std::string()});
            } else {
                edits.push_back({member.key_start, member.value_end - member.key_start, std::string()});
            }
            to_insert.emplace_back(kv.first, value_json);
        }

        if (edits.empty() && to_insert.empty()) return true;

        // Back to front, so every span recorded above is still valid when its
        // turn comes.
        std::sort(edits.begin(), edits.end(),
                  [](const Edit& a, const Edit& b) { return a.offset > b.offset; });
        for (const auto& edit : edits) {
            out_text.replace(edit.offset, edit.erase, edit.insert);
        }
        changed = !edits.empty();

        if (!to_insert.empty()) {
            json::Member strings_now;
            if (!json::FindRootMember(out_text, "strings", strings_now)) {
                error = "\"strings\" object vanished mid-edit";
                return false;
            }
            std::vector<json::Member> current;
            if (!json::ScanObject(out_text, strings_now.value_start, current)) {
                error = "\"strings\" object could not be re-enumerated";
                return false;
            }
            const size_t object_end = json::SkipValue(out_text, strings_now.value_start);
            if (object_end == std::string::npos) {
                error = "\"strings\" object is malformed after the value updates";
                return false;
            }

            // Several new keys routinely share one insertion point — in a fresh
            // CaVE database, which has almost no META_TITLE_* keys yet, all 18
            // of them do. So group by offset and emit each group as a single
            // pre-sorted run, rather than inserting one key at a time and
            // relying on a tie-break. (The tie-break this replaces compared the
            // serialized text byte-wise, which disagrees with the collation
            // whenever case matters: "dpad_S_..." vs "dpad_down_...".)
            const size_t brace_offset = object_end - 1;
            std::map<size_t, std::vector<std::pair<std::string, std::string>>> groups;
            for (const auto& kv : to_insert) {
                const json::Member* successor = nullptr;
                for (const auto& member : current) {
                    if (CollateCompare(member.key, kv.first) > 0) { successor = &member; break; }
                }
                groups[successor ? successor->key_start : brace_offset].push_back(kv);
            }

            // Highest offset first, so every offset still refers to the same
            // byte when its turn comes.
            for (auto group = groups.rbegin(); group != groups.rend(); ++group) {
                auto& items = group->second;
                std::sort(items.begin(), items.end(),
                          [](const std::pair<std::string, std::string>& a,
                             const std::pair<std::string, std::string>& b) {
                              return CollateCompare(a.first, b.first) < 0;
                          });

                std::string run;
                for (size_t i = 0; i < items.size(); ++i) {
                    if (i) run.push_back(',');
                    run += json::QuoteString(items[i].first);
                    run.push_back(':');
                    run += items[i].second;
                }
                if (group->first == brace_offset) {
                    // Appending after the last member (or filling an empty object).
                    if (!current.empty()) run.insert(run.begin(), ',');
                } else {
                    // Inserted before an existing member, so the run carries its
                    // own trailing separator.
                    run.push_back(',');
                }
                out_text.insert(group->first, run);
            }
            changed = true;
        }

        if (changed && !json::Validate(out_text, validation_error)) {
            error = "patched strings file failed re-validation (" + validation_error + ")";
            return false;
        }

        // Confirm the keys we touched landed between the right neighbours.
        // Deliberately scoped to our own keys: the rest of the table is
        // Nintendo's, and refusing to install because some unrelated stock key
        // disagrees with this comparator would be a self-inflicted outage.
        if (changed) {
            json::Member final_strings;
            std::vector<json::Member> final_members;
            if (!json::FindRootMember(out_text, "strings", final_strings) ||
                !json::ScanObject(out_text, final_strings.value_start, final_members)) {
                error = "patched strings file could not be re-enumerated";
                return false;
            }
            for (const auto& kv : wanted) {
                for (size_t i = 0; i < final_members.size(); ++i) {
                    if (final_members[i].key != kv.first) continue;
                    const bool before_ok = (i == 0) ||
                        CollateCompare(final_members[i - 1].key, final_members[i].key) < 0;
                    const bool after_ok = (i + 1 == final_members.size()) ||
                        CollateCompare(final_members[i].key, final_members[i + 1].key) < 0;
                    if (!before_ok || !after_ok) {
                        error = "inserted key \"" + kv.first + "\" landed out of collation order";
                        return false;
                    }
                    break;
                }
            }
        }

        return true;
    }


    std::vector<std::string> TitleStringKeys(const std::string& code,
                                             const std::vector<NsoGuideKey>& guide) {
        const std::string key_code = CodeToStringKey(code);
        std::vector<std::string> keys;
        keys.reserve(1 + guide.size());
        keys.push_back("META_TITLE_COMMENT_" + key_code);
        for (const auto& kv : guide) keys.push_back(std::string(kv.suffix) + key_code);
        return keys;
    }

    // Erases members from an object, back to front, taking one adjacent comma
    // with each so the object stays well formed.
    static bool EraseMembers(const std::string& text, size_t object_start,
                             const std::vector<std::string>& keys,
                             std::string& out_text, size_t& removed, std::string& error) {
        removed = 0;
        out_text = text;
        std::vector<json::Member> members;
        if (!json::ScanObject(text, object_start, members)) {
            error = "object could not be enumerated";
            return false;
        }

        std::vector<size_t> hits;
        for (size_t i = 0; i < members.size(); ++i) {
            for (const auto& key : keys) {
                if (members[i].key == key) { hits.push_back(i); break; }
            }
        }
        if (hits.empty()) return true;

        // Back to front so each recorded span is still valid when its turn comes.
        for (size_t n = hits.size(); n-- > 0; ) {
            const size_t i = hits[n];
            const json::Member& m = members[i];
            size_t from = m.key_start;
            size_t to = m.value_end;
            if (i > 0) {
                from = members[i - 1].value_end;      // the comma before us
            } else if (members.size() > 1) {
                to = members[i + 1].key_start;        // the comma after us
            }
            out_text.erase(from, to - from);
            ++removed;
        }
        return true;
    }

    bool RemoveTitleEntry(const TitlesDb& db, const std::string& code,
                          std::string& out_text, bool& found, std::string& error) {
        found = false;
        out_text = db.text;
        if (!db.loaded || !db.has_titles_object) return true;

        size_t removed = 0;
        if (!EraseMembers(db.text, db.titles_object_start, {code}, out_text, removed, error)) return false;
        found = (removed > 0);
        if (!found) return true;

        std::string validation_error;
        if (!json::Validate(out_text, validation_error)) {
            error = "database failed re-validation after removal (" + validation_error + ")";
            return false;
        }
        return true;
    }

    bool UnpatchStringsFile(const std::string& text, const std::string& code,
                            const std::vector<NsoGuideKey>& guide,
                            std::string& out_text, bool& changed, std::string& error) {
        changed = false;
        out_text = text;

        std::string validation_error;
        if (!json::Validate(text, validation_error)) {
            error = "strings file is not valid JSON (" + validation_error + ")";
            return false;
        }
        json::Member strings;
        if (!json::FindRootMember(text, "strings", strings) || text[strings.value_start] != '{') {
            error = "strings file has no top-level \"strings\" object";
            return false;
        }

        size_t removed = 0;
        if (!EraseMembers(text, strings.value_start, TitleStringKeys(code, guide), out_text, removed, error)) return false;
        changed = (removed > 0);
        if (!changed) return true;

        if (!json::Validate(out_text, validation_error)) {
            error = "strings file failed re-validation after removal (" + validation_error + ")";
            return false;
        }
        return true;
    }


    bool CreateEmptyDatabase(const NsoSnesInstall& target, std::string& error) {
        error.clear();
        if (target.database_path.empty()) {
            error = "no target path";
            return false;
        }
        const std::string& path = target.database_path;
        for (size_t i = 1; i < path.size(); ++i) {
            if (path[i] != '/') continue;
            const std::string sub = path.substr(0, i);
            if (!sub.empty() && sub.back() == ':') continue;
            mkdir(sub.c_str(), 0777);
        }

        const std::string body = "{\"titles\":{}}";
        const std::string tmp = path + ".tmp";
        std::remove(tmp.c_str());
        FILE* f = std::fopen(tmp.c_str(), "wb");
        if (!f) { error = "cannot create " + tmp; return false; }
        const bool wrote = std::fwrite(body.data(), 1, body.size(), f) == body.size();
        const bool closed = std::fclose(f) == 0;
        if (!wrote || !closed) {
            std::remove(tmp.c_str());
            error = "write failed for " + tmp;
            return false;
        }
        std::remove(path.c_str());
        if (std::rename(tmp.c_str(), path.c_str()) != 0) {
            std::remove(tmp.c_str());
            error = "cannot move " + tmp + " into place";
            return false;
        }
        NsoLog::Instance().Line("created empty database at " + path);
        return true;
    }

}
