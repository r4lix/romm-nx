#pragma once

#include <string>
#include <vector>

#include "GbaRom.hpp"
#include "NsoSnesDb.hpp"

// The Game Boy Advance half of the Switch Online LayeredFS support.
//
//   sdmc:/atmosphere/contents/010012F017576000/
//     romfs/titles/lclassics.titlesdb
//     romfs/titles/<CODE>/<CODE>.gba            <- raw, verbatim
//     romfs/titles/<CODE>/<CODE>.png            <- cover, 512 box
//     romfs/titles/<CODE>/<CODE>-details.png    <- details, 400x300
//
// See docs/nso-gba-format.md.
namespace romm::nso {

    constexpr const char* kKnownGbaTitleId = "010012F017576000";
    constexpr const char* kKnownGbaTitleIdAlt = "0100555017574000";

    NsoSnesInstall DetectNsoGba();

    // Fields a GBA entry carries that the others do not.
    struct NsoGbaExtras {
        int fadein_a = 3;
        int fadein_b = 0; // 0 here, where NES uses 2
        // Save-file size the app allocates. Stock entries carry the cartridge's
        // real size (512 B to 128 KiB); CaVE writes one value big enough for any
        // of them on an injected title, and so does romm-nx — the alternative is
        // guessing a save type from the ROM and getting it wrong.
        int sram_file_size = 164096;
    };

    std::string BuildGbaTitleEntryJson(const NsoTitleMeta& meta, const NsoGbaExtras& extras);

    // save_count 1, volume 100 — as CaVE writes for an injected title.
    NsoTitleMeta GbaDefaultTitleMeta();

    // Empty, as on NES and Game Boy.
    const std::vector<NsoGuideKey>& GbaGuideKeys();

    bool VerifySerializedGbaDb(const std::string& text,
                               const std::vector<std::string>& before,
                               const std::string& expected_new,
                               std::string& error);

    // "A-AG5E_e": the prefix, the cartridge's own four-character game code, and
    // the region suffix — exactly how CaVE names an injected GBA title. A code
    // already in the database (a second dump of the same game, or CaVE's copy)
    // falls back to probing the last character so nothing is overwritten.
    std::string AllocateGbaGameCode(const TitlesDb& db, const GbaRomInfo& rom,
                                    const std::string& preferred_code);

}
