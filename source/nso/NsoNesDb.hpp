#pragma once

#include <string>
#include <vector>

#include "NsoSnesDb.hpp"

// The NES ("Nintendo Classics") half of the Switch Online LayeredFS support.
//
// Everything structural is shared with SNES and lives in NsoSnesDb: the same
// lclassics.titlesdb container, the same byte-span splice, the same .NET
// collation for strings.lng. Only what genuinely differs is here — and it is
// more than it looks:
//
//   sdmc:/atmosphere/contents/0100D870045B6000/
//     romfs/titles/lclassics.titlesdb
//     romfs/titles/<CODE>/<CODE>.nes          <- raw iNES, no container, no sig
//     romfs/titles/<CODE>/<CODE>.png          <- cover, 359x512 PORTRAIT
//     romfs/titles/<CODE>/<CODE>00.png        <- details, 400x300 (not -details)
//
// See docs/nso-nes-format.md for how each of those was established. Notably
// there is no exefs mod: the signature check that makes one mandatory on SNES
// has no NES equivalent.
namespace romm::nso {

    // Both regional builds. The Famicom app carries the same database layout,
    // and CaVE's mods declare the pair together.
    constexpr const char* kKnownNesTitleId = "0100D870045B6000";
    constexpr const char* kKnownFamicomTitleId = "0100B4E00444C000";

    // Cover geometry. Portrait, unlike every other injectable platform so far —
    // NES box art is taller than wide and the app's grid expects it.
    //
    // The HEIGHT is the fixed quantity, not the width: CaVE scales the supplied
    // art to 512 tall and lets the width fall where the aspect puts it. Measured
    // on its own output — a 497x680 cover came out 374x512, and its 359x512
    // template is simply the placeholder's own aspect, not a target size. An
    // earlier draft of this file had 359 as a fixed width, which would have
    // squashed every cover that is not exactly 0.70:1.
    constexpr int kNesCoverHeight = 512;
    // romm-nx's own cap, for a cover wide enough that 512 tall would overflow
    // the app's slot. Nothing observed needs it; it exists so an unusual RomM
    // image cannot produce a 2000px-wide texture.
    constexpr int kNesCoverMaxWidth = 512;
    constexpr int kNesDetailsWidth = 400;
    constexpr int kNesDetailsHeight = 300;

    // Scans /atmosphere/contents (and legacy /atmosphere/titles) for a title
    // whose database holds CLV-P-* codes. Returns the same install description
    // the SNES side uses; `has_exefs_mod` is reported but means nothing here.
    NsoSnesInstall DetectNsoNes();

    // Fields an NES entry carries that a SNES one does not. The defaults are
    // what CaVE itself wrote for a ROM outside the stock catalogue, not what
    // stock entries happen to carry: `armet_version` is "off" there, i.e. the
    // scanline filter disabled, because CaVE has no per-game filter profile for
    // a game it has never seen. Stock entries use "v1"/"v2_blend" with tuned
    // thresholds; copying those onto an arbitrary ROM would be guessing.
    struct NsoNesExtras {
        int armet_threshold = 85;
        std::string armet_version = "off";
        int fadein_a = 3;
        int fadein_b = 2;
        int overscan_top = 0;
        int overscan_bottom = 0;
        int overscan_left = 9;
        int overscan_right = 3;
    };

    // Entry values CaVE uses for an injected NES title where SNES differs:
    // volume 80 rather than 100, and save_count 0 rather than 1. Applied by the
    // pipeline on top of whatever RomM supplies for the rest.
    NsoTitleMeta NesDefaultTitleMeta();

    // Serializes one NES entry: the shared field set plus the four NES-only
    // ones, in the same alphabetical, minified form the reference entry uses.
    std::string BuildNesTitleEntryJson(const NsoTitleMeta& meta, const NsoNesExtras& extras);

    // The NES key-guide set, and it is EMPTY — deliberately.
    //
    // CaVE writes 18 keys per injected SNES title, but for the NES title it
    // injected here it wrote exactly one: META_TITLE_COMMENT. No key-guide keys
    // at all. Since the guide screen is Nintendo's and romm-nx has no real
    // per-game control text, following CaVE is both the evidenced choice and the
    // one that cannot put placeholder rows ("A", "B", "D-Pad") on screen.
    //
    // Kept as a function rather than an empty literal at the call site so this
    // reasoning lives in one place if a hardware test ever contradicts it.
    const std::vector<NsoGuideKey>& NesGuideKeys();

    // Same contract as VerifySerializedDb, checking the NES file names
    // (`.nes` / `.png` / `00.png`) instead of the SNES ones.
    bool VerifySerializedNesDb(const std::string& text,
                               const std::vector<std::string>& before,
                               const std::string& expected_new,
                               std::string& error);

    // Derives a stable CLV-P code from the ROM hash and probes upward until it
    // does not collide.
    //
    // Deliberately allocates outside Nintendo's block: every stock code is
    // CLV-P-NA__E (NAAAE through NAJTE today), so injected ones take
    // CLV-P-NZ__E. Nintendo adding games to the service can then never land on
    // a code romm-nx has already used — which on this database would not be a
    // conflict the app reports, it would be an injected ROM quietly shadowing a
    // real title.
    std::string AllocateNesGameCode(const TitlesDb& db, const std::string& sha256,
                                    const std::string& preferred_code);

    // Whether a code belongs to the injected block rather than the stock
    // catalogue. Used before removing anything, so an uninstall can never take
    // out a stock entry.
    bool IsInjectedNesCode(const std::string& code);

}
