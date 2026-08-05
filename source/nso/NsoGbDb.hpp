#pragma once

#include <string>
#include <vector>

#include "GbRom.hpp"
#include "NsoSnesDb.hpp"

// The Game Boy / Game Boy Color half of the Switch Online LayeredFS support.
//
//   sdmc:/atmosphere/contents/0100C62011050000/
//     romfs/titles/lclassics.titlesdb
//     romfs/titles/<CODE>/<CODE>.gb            <- DMG, raw
//     romfs/titles/<CODE>/<CODE>.gbc           <- CGB, raw
//     romfs/titles/<CODE>/<CODE>.png           <- cover, 512x512
//     romfs/titles/<CODE>/<CODE>-details.png   <- details, 1069x802
//
// One app, both systems: each entry carries "platform":"DMG" or "CGB", and the
// code prefix and ROM extension follow it. See docs/nso-gb-format.md.
namespace romm::nso {

    constexpr const char* kKnownGbTitleId = "0100C62011050000";  // Americas / Europe
    constexpr const char* kKnownGbTitleIdJp = "0100395011044000"; // Japan

    // Cover geometry, measured on CaVE's output. The details screen is far
    // larger than the 400x300 the SNES and NES apps use.
    constexpr int kGbCoverBox = 512;
    constexpr int kGbDetailsWidth = 1069;
    constexpr int kGbDetailsHeight = 802;

    NsoSnesInstall DetectNsoGb();

    // Fields a Game Boy entry carries that no other platform does. The values
    // are CaVE's own for an injected DMG title; the DMG_MGB pair is written only
    // for DMG entries, matching both CaVE and the stock catalogue.
    struct NsoGbExtras {
        std::string cgb_default = "None";
        std::string cgb_nostalgic = "None";
        std::string dmg_default = "None";
        std::string dmg_nostalgic = "High";
    };

    // Serializes one entry. `mode` decides the "platform" value, the ROM
    // extension and whether the DMG_MGB palette pair is emitted.
    //
    // Note there are no title_zhHans/title_zhHant members here: unlike SNES and
    // NES, no Game Boy entry in the stock database carries them, and adding
    // fields the app has never seen on this platform is not worth the risk.
    std::string BuildGbTitleEntryJson(const NsoTitleMeta& meta, GbMode mode,
                                      const NsoGbExtras& extras);

    // Entry values CaVE uses for an injected Game Boy title: save_count 1 and
    // volume 100, i.e. the SNES defaults rather than the NES ones.
    NsoTitleMeta GbDefaultTitleMeta();

    // Empty, as on NES — CaVE writes only META_TITLE_COMMENT for an injected
    // title, and inventing key-guide rows would put text on Nintendo's screen
    // that CaVE does not.
    const std::vector<NsoGuideKey>& GbGuideKeys();

    bool VerifySerializedGbDb(const std::string& text,
                              const std::vector<std::string>& before,
                              const std::string& expected_new,
                              GbMode mode,
                              std::string& error);

    // "D-3DB7_e" / "C-7224_e" — the prefix from the mode, the body from the
    // ROM's own global checksum in hex, exactly as CaVE builds it. That makes
    // the code deterministic for a given dump rather than an allocation, so
    // reinstalling the same game always lands on the same entry.
    //
    // A collision (two different dumps sharing a checksum, or the slot already
    // holding another game) falls back to probing the low nibble upward, so an
    // occupied code can never silently overwrite someone else's title.
    std::string AllocateGbGameCode(const TitlesDb& db, const GbRomInfo& rom,
                                   const std::string& preferred_code);

    // Extension for the ROM file, ".gb" or ".gbc".
    const char* GbRomSuffix(GbMode mode);

}
