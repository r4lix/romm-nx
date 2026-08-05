# Nintendo 64 Switch Online LayeredFS format

> **N64 is flagged UNSTABLE in the UI** (`IsNsoPlatformUnstable`, one predicate
> in `NsoSnesInstaller.cpp`). A title only runs if a working MetaPack exists for
> it, and a generated one only carries an `Idle` entry when the game's idle loop
> is an unconditional self-branch in the boot segment — true for Ocarina of Time,
> not for Doom 64 or Resident Evil 2. Clearing the flag is a one-line change once
> that stops being a coin toss.

Fifth in the series, and the one that breaks the pattern in four places. From
CaVE's stock databases (four title IDs) and one real CaVE-written injection.

## Read this first: the ROM is not enough

**An injected N64 title also needs a per-game MetaPack (`.dtz`).** CaVE's own UI
carries "Add a .dtz MetaPack file for this game", an `N64 MetaPack File (*.dtz)`
picker and Pack/Unpack tools. Without a MetaPack the title installs correctly —
right code, right entry, right files — and **crashes on launch**. This is the
single reason an N64 injection that looks perfect does not run, and it is what
every "romm-nx downloaded it but it crashes" report has turned out to be.

Earlier revisions of this document said romm-nx could not generate one. That was
wrong twice over: the container is fully reverse-engineered *and* the two fields
that decide whether a title runs — `OptionInfo.PAL` and a real `Idle` entry —
both turn out to be derivable from the ROM. romm-nx writes a `.dtz` for every
N64 title it injects, and two games have been confirmed running on one.

It is not guaranteed. `ScanN64BootIdle` reports nothing for a game whose idle
loop is a conditional branch or lives in an overlay (Doom 64 and Resident Evil 2
are both examples), and such a title may not start. A community pack also
carries per-game tuning nothing can derive. So romm-nx still prefers one
whenever it finds one — drop `.dtz` files in:

```
sdmc:/switch/romm-nx/nso-n64/metapacks/
```

named after the ROM file, the game's title, or — best, because it survives any
rename and cannot match the wrong region — the cartridge's **CRC1** in hex, e.g.
`9B500E8E.dtz`. The install log names the CRC1 it looked for.

The community compatibility list tracks which games have one, and it is split by
region — which turns out to matter far more than anything in the file format:

| region | titles with a MetaPack |
|---|---|
| **US (NTSC-U)** | ~135 |
| **Europe (PAL)** | ~9 |

So the practical rule for N64 is **use the US dump**. A European ROM of the same
game is usually not injectable at all, not because the app rejects it — it runs
any region — but because nobody has published a MetaPack for it.

romm-nx reads the country byte at 0x3E and says so: the analyse step reports the
region, adds "few MetaPacks exist" for anything but US, and the success page
names the region with a note that the US build is far more likely to have one.

Confirmed on hardware: Resident Evil 2 injected by romm-nx and by CaVE crash
identically, and its row in that list has no MetaPack. A crash on launch is
therefore not evidence of a bad injection, and romm-nx says so on the success
page and in the download prompt rather than leaving it looking like its own
failure.

> **Settled on hardware, 2026-08-05.**
>
> `titles/<CODE>/<CODE>.dtz` is where the app looks, and **a romm-nx-generated
> MetaPack runs a game.** Ocarina of Time (Europe, Rev 1) and Ocarina of Time
> Master Quest (Europe) both boot and play with a generated pack and no
> community `.dtz` anywhere on the card.
>
> Getting there took two fields, in this order:
>
> 1. `OptionInfo.PAL` — without it a 50 Hz build gets NTSC timing. Reaches the
>    emulator, then black screen. (Mario Kart 64 first showed this.)
> 2. A real `Idle` entry — an empty array is not enough.
>
> **Everything else in a community pack is tuning, not a prerequisite.** The OoT
> pack sets `RAMSize`, `SIDevice_PakType`, `RSP` and `RendererSetting`; a
> generated pack sets none of them, and Master Quest — the title whose pack asks
> for `"RAMSize": "16M"` — runs anyway.

### Region: `OptionInfo.PAL`

A second community pack — Ocarina of Time (Master Quest, Europe) — settled the
"does region matter" question, and it does, but not in the way the availability
table above suggests:

```json
"OptionInfo": { "PAL": 1, "SndSampleRate": 32016 }
```

**A 50 Hz build needs `"PAL": 1` or the emulator gives it NTSC video timing.**
This is the one field that genuinely differs between a US and a European pack,
and the first generated packs never wrote it — so every European injection got
NTSC timing. `IsN64PalRegion` decides it from the country byte at 0x3E: `P D F I
S U H X Y` are PAL; `E J N B A K C` are not. Brazil is PAL-M at 60 Hz and Canada
is NTSC, so neither counts despite not being `E`.

### `Idle`: derivable more often than it first looked

The Doom 64 pack's entry is `JmpInst 0x1560FFE0` — a conditional `bne` branching
~31 instructions back, at `0x800000D0`, which is *below* the entry point and so
not in the game segment at all. That looked like proof that idle addresses are
debugger-only.

The Ocarina of Time pack says otherwise: `JmpAddr 0x80000810`, `JmpInst
0x1000FFFF` — the plain unconditional self-branch, right in the boot segment.
Doom 64 is the exception, not the rule.

So `ScanN64BootIdle` looks for exactly one pattern:

```
1000FFFF   beq $zero,$zero,-1
00000000   nop
```

Nothing else. That restriction is what makes emitting the result **derived
rather than guessed**: such a loop has no exit except an interrupt, so
fast-forwarding out of it cannot skip work the game needed. A conditional branch
can leave on its own and is never emitted.

Measured against the ROMs on hand — the scan reports the community pack's exact
answer where one exists, and nothing at all where it cannot justify an answer:

| ROM | candidates | community pack says |
|---|---|---|
| OoT Master Quest (Europe) | `0x80000810`, `0x80002088` | **`0x80000810`** — first hit |
| OoT (Europe) (Rev 1) | `0x800007C0`, `0x80003448` | no pack |
| Doom 64 (USA) | `0x8004C4A8` | `0x800000D0` — **not found**, conditional |
| Resident Evil 2 (USA) | none | no pack |

An empty result writes an empty array. That is the honest answer, not a failure.

### Still missing from a generated pack

Everything below is in the OoT pack and not in a generated one, and none of it
can be derived from a ROM. **None of it is needed to boot** — both European
Zeldas run without any of it — so treat this as the list of reasons a community
pack may still play *better*, not as a list of blockers:

* **`RomOption.RAMSize`** — `"16M"` for Expansion Pak titles.
* **`SIDevice_PakType`** — `"Rumble Pak"` for OoT, `"Controller Pak"` for Doom 64.
  Generated packs write `"Controller Pak"`, the more generally-safe default.
* **`RSP`** — `{"RSPProcUndefineTask": 1}` for OoT; generated packs write `{}`.
* **`RendererSetting`** — eight keys for OoT (`CopyAtEnd`, `CopyAtFullSync`,
  `CopyTextureBackup`, `PreparseTMEMBlock`, `TMEMOverflowCheck`,
  `NeedTileSizeCheck`, `TexMirrorDoubleSize`, `ZClip`); generated packs write `{}`.
* **`BackupType`** — hardcoded `"SRAM"`; the header does not record the save
  type. **No pack using EEPROM or FlashRAM has been seen, so the string the app
  expects for those is still unknown** — which is why this is not simply
  "detect it and write it".

### The directory name inside the archive is arbitrary

Doom 64's members are `metapack\NN_...`; the OoT pack's are
`Oot Master Quest (EUR)\NN_...`. The app clearly keys off the `NN_` prefix and
the extension, not the path. romm-nx writes `metapack\`.

### The `.lua` has more than one entry point

Doom 64 defines `RomPatch()` and calls `n64RomWrite8` (patches the ROM image
once, then fixes the CIC checksum). The OoT pack instead defines `FrameBegin()`
and calls **`n64MemWrite8`** — a per-frame write into RDRAM. Generated packs
write an empty `RomPatch()`.

The list also records per-game requirements that map onto fields in the entry
below — "needs cold reset" appears on 20+ titles, which is exactly the
`cold_reset` flag — plus MetaPack-level settings romm-nx does not write at all
(RAM size, rumble, motion blur). romm-nx writes CaVE's injected defaults; adding
the MetaPack in CaVE is also where those per-game flags get set.

## Where it lives

```
sdmc:/atmosphere/contents/0100C9A00ECE6000/
  romfs/titles/lclassics.titlesdb
  romfs/titles/<CODE>/<CODE>.bnz            <- zlib stream; the ENTRY says .bin
  romfs/titles/<CODE>/<CODE>.dtz            <- MetaPack; nothing references it
  romfs/titles/<CODE>/<CODE>.png            <- cover, 512x374
  romfs/titles/<CODE>/<CODE>-details.png    <- details, 400x300
```

Four title IDs, all carrying the same layout:

| id | app |
|---|---|
| `0100C9A00ECE6000` | Americas / Europe |
| `010057D00ECE4000` | Japan |
| `0100E0601C632000` | Expansion Pak |
| `010037A0170D2000` | Expansion Pak (Japan) |

## `.bnz`: the ROM is compressed, and the entry lies about it

The file CaVE writes is `<CODE>.bnz` while the entry's `rom` field says
`<CODE>.bin`. The `.bnz` is a **raw zlib stream** — `78 DA` — of the ROM:

```
compressed   64,398,069 bytes
decompressed 67,108,864 bytes  (64 MiB)
magic        80 37 12 40  -> big-endian
internal     "Resident Evil II"   cart NR   country P   CRC1 9B500E8E
```

romm-nx writes the same pair: `.bnz` beside an entry naming `.bin`. Compression
is level 6 rather than CaVE's 9 — the app only decompresses, and level 9 on a
64 MiB cartridge costs minutes of console time for a percent of size.

## `.dtz`: the MetaPack container

**zlib (level 9) over an old *binary* cpio archive.** Magic `0o070707` stored
little-endian (`C7 71`), 26-byte header of 13 `u16`, name and data each padded to
an even length. The two 32-bit fields — `mtime` and `filesize` — are stored as
two `u16` **most-significant word first**, the PDP-11 quirk, while each word is
itself little-endian. Every header field is zero except `dev = 0x8080` and a real
`mtime`; the `TRAILER!!!` record uses `dev = 0`.

Three members, **all plain text**:

```
metapack\00_XXXXXX.000.meta    JSON: Hardware "NUS", TitleCode, DataVersion, OptionInfo
metapack\02_XXXXXX.000.cfg     JSON: RomOption, RSP, Idle, SpecialInst, RendererSetting
metapack\06_XXXXXX.000.lua     Lua SOURCE, not bytecode: RomPatch(), n64RomWrite8()
```

The backslash is literal. `XXXXXX` is a **literal placeholder**, not the game's
code — and `TitleCode` inside the `.meta` says `00_XXXXXX.000` too, so the app
cannot be using either to identify the game. That is what makes a MetaPack
writable generically, and it is also why the `.dtz` has to be named after the
title: nothing inside it is game-specific.

Lines are **CRLF** and no member ends with a newline. A JSON or Lua parser would
not care, but `BuildN64MetaPack` matches the one pack confirmed to boot on
hardware byte for byte, so "is the pack malformed" is never a variable.

The `.cfg` is JSON despite the extension, not INI. `Idle` is the emulator's
idle-loop skip:

```json
"Idle": [{ "Comment": "...", "JmpAddr": "0x800000D0",
           "JmpInst": "0x1560FFE0", "IsIdle": "0x1" }]
```

`JmpAddr` is an RDRAM address (`0x80000000`–`0x807FFFFF`) and cannot be found by
scanning the ROM: static analysis only sees the boot segment, where ROM `0x1000`
maps 1:1 to the header's entry address, and the thread that actually idles is
usually in an overlay. The reliable method is a **Project64 savestate** — magic
`0x23D8A6C8` LE, RdramSize at `0x04`, ROM header at `0x08`, VI regs at `0x3E8`,
RDRAM stored word-swapped — then scan for libultra `OSThread` structs (`priority`
`+0x04`, `state` `+0x10`, `id` `+0x14`, context `+0x20`, so **`pc` at `+0x11C`**)
and read the `pc` of the thread with priority **0** (`OS_PRIORITY_IDLE`).

romm-nx writes an **empty** `Idle` array, because a wrong address does not
degrade gracefully — it tells the emulator to skip instructions the game needs.
Hardware has since shown that empty is not sufficient either (see above), so
this is the known limit of a generated pack rather than a safe default: a
community pack is the only way to get a real `Idle` entry today.

The `.lua` may rewrite ROM bytes through `n64RomWrite8(offset, value)`. The
reference pack patches brightness and then fixes the CIC-6102 checksum that patch
invalidated, writing CRC1/CRC2 back into header bytes `0x12`–`0x17`. A generated
pack changes nothing, so the cartridge checksum stays valid and `RomPatch()` is
empty.

**Verified:** a known-good community pack (Doom 64) unpacks and rebuilds
byte-identical, cpio and zlib stream alike, and `BuildMeta`/`BuildCfg` reproduce
its `.meta` and `.cfg` exactly when handed its own values.

## Byte order

N64 dumps ship in three orders and **nothing rejects the wrong one** — it simply
boots to garbage:

```
80 37 12 40   .z64  big-endian, native
37 80 40 12   .v64  16-bit swapped
40 12 37 80   .n64  32-bit word swapped
```

The pipeline detects the order, converts to big-endian, compresses and hashes in
**one streaming pass** at 256 KiB a chunk: a 64 MiB cartridge held as source
plus converted plus compressed would be ~190 MiB of peak heap, which is not
something to do on a console that may be in applet mode. The hash is of the
normalized bytes, so one cartridge dumped two ways reuses one entry.

> `AnalyzeN64Rom` and `NormalizeN64Rom` must never call each other. They did
> once — each asking the other for the byte order — and recursed until the
> worker's 1 MB stack ran out, crashing the app the instant the analyse step
> began. Detection is now a free function that calls nothing.

## Entry

```json
{"code":"N-9416_e","cold_reset":false,"control_opt_layout_pattern":1,
"controller_position":"right","copyright":"©Nintendo",
"cover":"/titles/N-9416_e/N-9416_e.png",
"details_screen":"/titles/N-9416_e/N-9416_e-details.png","GPU384MHz":false,
"lcla6_release_date":"2026-08-03","players_count":1,"publisher":"Nintendo",
"release_date":"2026-08-03","rom":"/titles/N-9416_e/N-9416_e.bin","save_count":1,
"simultaneous":false,"sort_publisher":"nintendo","sort_title":"resident evil 2 ",
"sram_file_size":164096,"title":"Resident Evil 2 ","title_ko":"Resident Evil 2 ",
"title_zhHans":"ー","title_zhHant":"ー","volume":80}
```

**Verified:** `BuildN64TitleEntryJson` reproduces that byte for byte, all 606.

Two things to notice. There is **no `rewind_interval`** — the only platform
without it, and no stock N64 entry has one either. And the key order is CaVE's
own: `GPU384MHz` folded in at "g" rather than sorted ahead of the lowercase keys
the way the stock entries have it.

Per-game emulator settings, with the values stock titles use:

| field | stock range | injected default |
|---|---|---|
| `GPU384MHz` | false ×59, true ×5 | `false` |
| `cold_reset` | false ×60, true ×4 | `false` |
| `controller_position` | "right" ×62, "home" ×2 | `"right"` |
| `control_opt_layout_pattern` | 1 ×38, 2 ×23, 3, 4 | `1` |
| `save_count` | 1 to 8 | `1` |
| `sram_file_size` | 164096 on all | `164096` |

romm-nx writes the injected defaults, not a stock game's tuning: those values
are per title, and copying one game's onto another is worse than leaving the app
on its own.

Optional stock fields the splice preserves: `icon_process`, `launch_caution`,
`disable_handcursor`, `keyassign_type`, `hidden_countries`, `display_version`.

## Codes

`N-####_e`, four digits allocated from the ROM hash in 5000-9999 and probed
upward on collision — the SNES approach, not the Game Boy one, because stock N64
codes are numeric and have no relationship to the cartridge. Stock codes carry a
language rather than just a region: `_e`, `_p`, `_j`, plus `_pd` `_pf` `_ps`
`_pi` for the German, French, Spanish and Italian PAL builds.

## `strings.lng`

**26 key-guide keys plus the description** — CaVE writes the full set here, as
it does for SNES and unlike NES, Game Boy and GBA which get only the
description. The controller is why: four C buttons (`cunit`), four stick
directions (`stick`) and Z (`z_r`) on top of the usual rows.
