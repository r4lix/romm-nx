#pragma once

#include <string>
#include <vector>

#include "N64Rom.hpp"
#include "NsoSnesDb.hpp"

// The Nintendo 64 half of the Switch Online LayeredFS support — the awkward
// one, for four reasons no earlier platform had:
//
//   * four title IDs, not one (regional apps plus the Expansion Pak builds)
//   * three interchangeable-looking ROM byte orders, none of which is rejected
//     by anything if wrong (see N64Rom)
//   * the ROM is stored zlib-compressed as "<CODE>.bnz" while the database
//     entry points at "<CODE>.bin"
//   * per-game emulator settings in the entry (overclock, reset behaviour,
//     controller layout) and no rewind_interval at all
//
// See docs/nso-n64-format.md.
namespace romm::nso {

    // All four, in preference order. The first two are the ordinary regional
    // apps; the last two ship the Expansion Pak titles.
    constexpr const char* kKnownN64TitleIds[] = {
        "0100C9A00ECE6000", // Americas / Europe
        "010057D00ECE4000", // Japan
        "0100E0601C632000", // Expansion Pak
        "010037A0170D2000"  // Expansion Pak (Japan)
    };

    NsoSnesInstall DetectNsoN64();

    // Per-game emulator settings. The defaults are what CaVE wrote for a ROM
    // outside the stock catalogue, not what stock entries carry: stock tunes
    // GPU384MHz and the layout pattern per title, and copying one game's tuning
    // onto another is worse than leaving the app on its own defaults.
    struct NsoN64Extras {
        bool gpu_384mhz = false;
        bool cold_reset = false;
        std::string controller_position = "right";
        int control_opt_layout_pattern = 1;
        int sram_file_size = 164096; // the same constant GBA uses
    };

    // Serializes one entry. Note there is no rewind_interval: no N64 entry in
    // the stock database has one, and the app has no rewind.
    //
    // `rom` deliberately names "<CODE>.bin" even though the file written is
    // "<CODE>.bnz" — that is what CaVE does, and the app resolves it.
    std::string BuildN64TitleEntryJson(const NsoTitleMeta& meta, const NsoN64Extras& extras);

    // save_count 1, volume 80.
    NsoTitleMeta N64DefaultTitleMeta();

    // 26 key-guide rows, and this time they are not empty: CaVE writes the full
    // set for an injected N64 title, including the C buttons ("cunit"), the
    // analog stick ("stick") and Z ("z_r"). NES, Game Boy and GBA get only a
    // description; SNES and N64 get the guide.
    const std::vector<NsoGuideKey>& N64GuideKeys();

    bool VerifySerializedN64Db(const std::string& text,
                               const std::vector<std::string>& before,
                               const std::string& expected_new,
                               std::string& error);

    // "N-9416_e": four digits from the ROM hash, probed upward on collision —
    // the SNES approach rather than the Game Boy one, because the stock codes
    // are numeric here and carry no relationship to the cartridge.
    std::string AllocateN64GameCode(const TitlesDb& db, const std::string& sha256,
                                    const std::string& preferred_code);

}
