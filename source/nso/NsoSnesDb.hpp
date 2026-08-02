#pragma once

#include <string>
#include <vector>

// Detection of, and safe edits to, the SNES Switch Online LayeredFS database.
//
// Layout as found on this console (CaVE 1.5.2, database version 5.1.0.0):
//
//   sdmc:/atmosphere/contents/01008D300C50C000/
//     exefs/main.npdm, exefs/subsdk9          <- the unlock/mod, never touched
//     romfs/DBINFO                            <- CaVE bookkeeping, never touched
//     romfs/titles/lclassics.titlesdb         <- the database (minified JSON)
//     romfs/titles/<CODE>/<CODE>.sfrom
//     romfs/titles/<CODE>/<CODE>.sfromsig
//     romfs/titles/<CODE>/<CODE>.png          <- 512x374 cover
//     romfs/titles/<CODE>/<CODE>-details.png  <- 400x300 details screen
//     romfs/bootapp/resources/strings/<lang>/strings.lng
//
// The database is `{"titles":{"<CODE>":{...}, ...}}` with entries in insertion
// order, and it mixes stock titles (whose assets live in the base game's romfs)
// with injected ones. Every edit here is a byte-span splice via NsoJson so that
// unmodelled fields survive untouched.
namespace romm::nso {

    struct NsoSnesInstall {
        bool found = false;
        std::string title_id;
        std::string content_root;   // sdmc:/atmosphere/contents/<title_id>
        std::string romfs_root;     // <content_root>/romfs
        std::string titles_dir;     // <romfs_root>/titles
        std::string database_path;  // <titles_dir>/lclassics.titlesdb
        std::string dbinfo_path;    // <romfs_root>/DBINFO ("" when absent)
        std::vector<std::string> strings_files;
        std::vector<std::string> strings_languages;
        bool has_exefs_mod = false; // exefs/subsdk9 present
        // The database file exists. False means the paths above are a target
        // romm-nx can create rather than one it found — a modded app with no
        // LayeredFS yet is a perfectly good place to inject into, it just needs
        // an empty database written first.
        bool database_exists = false;
        size_t entry_count = 0;
        size_t injected_asset_dirs = 0;
        std::string error;
    };

    // Scans /atmosphere/contents and /atmosphere/titles for a title whose
    // romfs carries an lclassics.titlesdb full of "S-*" codes. Prefers the
    // known SNES title id when several candidates exist.
    NsoSnesInstall DetectNsoSnes();

    // Writes an empty database, creating the directories on the way. Used when
    // the user has the app and the mod but no LayeredFS yet.
    //
    // Deliberately writes ONLY the database: LayeredFS replaces files wholesale,
    // so a partial strings.lng would blank every other string in the app and
    // leave raw SYS_MENU_* keys across the whole UI. Without one, the app keeps
    // its own — the only cost is that injected games show no description text.
    bool CreateEmptyDatabase(const NsoSnesInstall& target, std::string& error);

    struct TitlesDb {
        bool loaded = false;
        std::string text;             // the file, byte for byte
        size_t root_object_start = 0;
        size_t titles_object_start = 0;
        // A database CaVE created but never populated can legitimately have an
        // empty "titles" object, or no "titles" member at all. Both are valid
        // starting points, not errors.
        bool has_titles_object = false;
        std::vector<std::string> codes; // in file order
        std::string error;
    };

    bool LoadTitlesDb(const std::string& path, TitlesDb& out);
    bool HasCode(const TitlesDb& db, const std::string& code);

    // Reads one string field out of an existing entry. Used to confirm that a
    // code romm-nx remembers assigning still belongs to the game it assigned it
    // to before that entry is overwritten.
    bool GetEntryField(const TitlesDb& db, const std::string& code,
                       const std::string& field, std::string& out);

    struct NsoTitleMeta {
        std::string code;
        std::string title;
        std::string sort_title;
        std::string publisher = "Unknown";
        std::string sort_publisher = "unknown";
        std::string copyright;
        std::string release_date;      // YYYY-MM-DD
        std::string lcla6_release_date; // "added to the service on" date
        int players_count = 1;
        bool simultaneous = false;
        int save_count = 1;
        int volume = 100;
    };

    // Serializes one database entry with exactly the field set, order and
    // formatting the reference entries use (alphabetical keys, minified,
    // literal UTF-8, rewind_interval rendered as 1.5).
    std::string BuildTitleEntryJson(const NsoTitleMeta& meta);

    // Adds (or, for a reinstall of the same code, replaces) one entry.
    // Returns the complete new document; never mutates the file on disk.
    bool UpsertTitleEntry(const TitlesDb& db, const std::string& code,
                          const std::string& entry_json,
                          std::string& out_text, std::string& error);

    // Removes one entry. Returns false if the code is absent (nothing to do is
    // reported by `found`, not as an error).
    bool RemoveTitleEntry(const TitlesDb& db, const std::string& code,
                          std::string& out_text, bool& found, std::string& error);

    // Removes the per-title keys PatchStringsFile added. Leaves every other key
    // byte-for-byte untouched.
    bool UnpatchStringsFile(const std::string& text, const std::string& code,
                            std::string& out_text, bool& changed, std::string& error);

    // The per-title key set, shared by the patch and unpatch paths so they can
    // never disagree about which keys belong to a title.
    std::vector<std::string> TitleStringKeys(const std::string& code);

    // Re-parses a serialized database and checks that every code in `before`
    // is still present, that `expected_new` exists, and that its asset paths
    // are the ones we generated.
    bool VerifySerializedDb(const std::string& text,
                            const std::vector<std::string>& before,
                            const std::string& expected_new,
                            std::string& error);

    // Derives a stable 4-digit S-code from the ROM hash and probes upward until
    // it does not collide with anything already in the database.
    std::string AllocateGameCode(const TitlesDb& db, const std::string& sha256,
                                 const std::string& preferred_code);

    // The 18 per-title strings CaVE writes into each localized strings.lng
    // (one description plus a key-guide label per button). Returns the new
    // document; a language file that already carries the keys is returned
    // unchanged with `changed` false.
    bool PatchStringsFile(const std::string& text, const std::string& code,
                          const std::string& description,
                          std::string& out_text, bool& changed, std::string& error);

    // Lowercased, ASCII-folded sort key in the shape the database uses.
    std::string MakeSortKey(const std::string& value);

    // "S-3051_e" -> "S_3051_e", the form strings.lng keys are built from.
    std::string CodeToStringKey(const std::string& code);

    // Key ordering used by strings.lng: punctuation < digits < letters, letters
    // compared case-folded. Reproduces the order the (.NET-written) stock file
    // is already in; a plain strcmp does not. <0, 0 or >0 like strcmp.
    int CollateCompare(const std::string& a, const std::string& b);

}
