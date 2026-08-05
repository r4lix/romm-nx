#pragma once

#include <cstdint>
#include <string>
#include <vector>

// The N64 MetaPack (".dtz") — the file that decides whether an injected
// Nintendo 64 title boots at all.
//
// Everything else romm-nx writes for N64 is correct without it: the code, the
// entry, the compressed ROM, the covers. The title still crashes the instant it
// is launched, because the app runs each game from a per-game MetaPack and has
// none for a title it has never seen.
//
// The container is not opaque. It is **zlib (level 9) over an old binary cpio
// archive** — magic 0o070707 stored little-endian, a 26-byte header, name and
// data each padded to an even length. Three members, all plain text:
//
//     metapack\00_XXXXXX.000.meta    JSON: Hardware, TitleCode, OptionInfo
//     metapack\02_XXXXXX.000.cfg     JSON: RomOption, RSP, Idle, SpecialInst,
//                                          RendererSetting
//     metapack\06_XXXXXX.000.lua     Lua source: RomPatch(), n64RomWrite8()
//
// The backslash is literal and "XXXXXX" is a literal placeholder rather than
// the game's code — which is the reason a MetaPack can be written generically
// at all. Header fields are zero except dev=0x8080 and a real mtime; the
// TRAILER!!! record uses dev=0.
//
// Verified by unpacking a known-good community pack (Doom 64) and rebuilding it
// byte-identical, cpio and zlib stream alike.
//
// See docs/nso-n64-format.md.
namespace romm::nso {

    // Sits beside "<CODE>.bnz" in the title's asset folder.
    constexpr const char* kN64MetaPackSuffix = ".dtz";

    // Where a user drops community MetaPacks for romm-nx to pick up. A pack
    // published for a game is always a better answer than a generated one: it
    // carries a real idle address, the right save type and any per-game ROM
    // patches, none of which can be derived from the cartridge alone.
    constexpr const char* kN64MetaPackDir = "sdmc:/switch/romm-nx/nso-n64/metapacks";

    // One "Idle" array entry. Comes from ScanN64BootIdle, which only ever
    // reports loops that provably cannot be left except by an interrupt.
    struct N64MetaPackIdle {
        uint32_t jmp_addr = 0;
        uint32_t jmp_inst = 0;
    };

    struct N64MetaPackOptions {
        // OptionInfo.PAL. A 50 Hz build needs this or the emulator gives it NTSC
        // video timing. The community pack for the European Ocarina of Time
        // carries it and is otherwise unremarkable, which makes it the one
        // field that genuinely differs between a US and a European MetaPack.
        bool pal = false;

        // RomOption. "SRAM" is what the reference pack carries; the cartridge
        // header does not record its save type, so this cannot be detected and
        // a user-supplied pack is the only way to get it right for certain.
        // Games that actually use EEPROM or FlashRAM are a known gap — the
        // string the app expects for those has never been seen in a real pack.
        std::string backup_type = "SRAM";
        int tick_per_inst = 8;

        // SIDevice_PakType; the key is omitted entirely when empty, which is
        // what the first generated packs did — and Mario Kart 64 booted to a
        // black screen with one. The reference pack sets it, and a title whose
        // controller device is never configured is a plausible way to sit
        // forever in osContInit, so it is written by default now.
        std::string pak_type = "Controller Pak";

        // OptionInfo.
        int snd_sample_rate = 32016;

        // The emulator's idle-loop skip. An empty vector writes an empty array,
        // which hardware has shown is not enough to run a game — but a *wrong*
        // JmpAddr is worse than none, so this is only ever filled from
        // ScanN64BootIdle's provably-safe candidates. Several entries are fine;
        // the field is an array in real packs too.
        std::vector<N64MetaPackIdle> idle;
    };

    // Serializes a complete .dtz. `mtime` is stamped into every member header;
    // pass a fixed value to make the output reproducible.
    std::string BuildN64MetaPack(const N64MetaPackOptions& options, uint32_t mtime);

    bool WriteN64MetaPack(const std::string& path, const N64MetaPackOptions& options,
                          std::string& error);

    // Inflates and walks the cpio, checking the three members are present and
    // their sizes agree with the archive. Run against a user-supplied pack
    // before it is copied anywhere, so a truncated download is caught here
    // rather than by the console at launch.
    bool VerifyN64MetaPack(const std::string& bytes, std::vector<std::string>& out_members,
                           std::string& error);

    // Looks in kN64MetaPackDir for a pack belonging to this game. Matched, in
    // order: "<CRC1>.dtz" (the cartridge's own checksum, so it survives any
    // rename), the ROM's filename with the extension replaced, and the title.
    // Returns "" when nothing matches; `out_matched_by` names the rule that hit.
    std::string FindUserN64MetaPack(const std::string& rom_filename,
                                    const std::string& title,
                                    uint32_t crc1,
                                    std::string& out_matched_by);

}
