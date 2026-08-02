# SNES Switch Online (Canoe) LayeredFS format

Everything below was established by inspecting a real, working CaVE-generated
installation — CaVE 1.5.2.0, database version 5.1.0.0, 28 injected titles — not
from documentation. Where this contradicts the assumptions people usually carry
over from other Switch Online apps, the real files win.

## Where it lives

```
sdmc:/atmosphere/contents/01008D300C50C000/
  exefs/main.npdm
  exefs/subsdk9                              <- the unlock/mod; romm-nx never touches it
  romfs/DBINFO                               <- CaVE bookkeeping; romm-nx never touches it
  romfs/titles/lclassics.titlesdb            <- the database
  romfs/titles/<CODE>/<CODE>.sfrom
  romfs/titles/<CODE>/<CODE>.sfromsig
  romfs/titles/<CODE>/<CODE>.png             <- cover, 512x374
  romfs/titles/<CODE>/<CODE>-details.png     <- details screen, 400x300
  romfs/bootapp/resources/strings/<lang>/strings.lng
```

`01008D300C50C000` is the SNES entry in the Switch Online family. romm-nx
prefers it but does not hard-code it: it scans `/atmosphere/contents` (and the
legacy `/atmosphere/titles`) for any title whose `romfs/titles/lclassics.titlesdb`
is full of `S-*` codes, so another region's build is still found.

`<CODE>` is `S-<4 digits>_e`. The stock titles shipped with the app use `S-2xxx`;
CaVE's injected ones on this console span `S-3051` to `S-9798`. At least one
stock entry uses a suffixed form (`S-2029_e-sp1`), so code parsing must not
assume the `_e` ending.

## Covers are PNG, not `.xtx.z`

There is no XTX texture, no Tegra block-linear swizzle and no zlib-wrapped
`.xtx.z` anywhere in the SNES title. Both images are ordinary PNG:

| file | size | bit depth | colour type | interlace |
|---|---|---|---|---|
| `<CODE>.png` | 512x374 | 8 | 6 (RGBA) | none |
| `<CODE>-details.png` | 400x300 | 8 | 2 (RGB) | none |

All 28 CaVE-injected `-details.png` files on this console are byte-identical to
`CaVE Database/resources/S-XXXX_e-details.png` — a generic Nintendo-branded
placeholder, not per-game art. romm-nx does not redistribute it; it renders its
own 400x300 details image from the same source cover instead.

27 of the 28 covers are exactly 512x374 (one sloppy entry is 512x375).

## `<CODE>.sfromsig`

256 bytes, all zero, identical across all 28 titles. Generated, not shipped.

## `.sfrom` = raw ROM + Canoe trailer

There is no header. The file is the headerless SNES ROM, unpadded, followed by a
trailer:

```
[ raw SNES ROM, copier header stripped, no padding to a power of two ]
[ Canoe configuration payload, N bytes                               ]
[ uint32 little-endian N                                             ]
[ 'C' 'a' 'n' '1'                                                    ]
```

The payload is a list of little-endian `uint16` tokens. The default, emitted by
romm-nx, is:

```
0x0247, 0x0000, 0x1000, 0x0774, 0x0270
```

which serializes to the full 18-byte trailer:

```
47 02 00 00 00 10 74 07 70 02 0A 00 00 00 43 61 6E 31
```

**Verified:** 22 of the 28 reference `.sfrom` files in `CaVE Database/roms/snes`
end in exactly these 18 bytes. That set spans LoROM and HiROM, 512 KiB through
4 MiB, PAL and NTSC — the trailer does not vary with mapping or size.

The six that differ vary the **third** token, which is the per-game preset slot
(`0x1000` = no preset), and two of them append further tokens:

| game | payload |
|---|---|
| Final Fantasy: Mystic Quest | `0247 0000 1103 0774 0270` |
| Final Fantasy II | `0247 0000 1095 0774 0270` |
| Castlevania: Vampire's Kiss | `0247 0000 1132 0774 0270` |
| Doom | `0247 0000 1000 0C65 0774 0270` |
| Final Fantasy III | `0247 0000 10DC 5A76 0774 0270` |
| Chrono Trigger | `0247 0000 110B 0774 0270 206A` + `u32 0x450` + `u32 0xFF` |

These are CaVE's own per-game emulator presets. romm-nx ships them as a lookup
table keyed by the SHA-256 of the headerless ROM body (`SfromWriter.cpp`), so a
ROM that hashes to one of those six dumps gets CaVE's exact payload rather than
the default. It is a table of measured values, not a heuristic — a hash match
means it is the same dump CaVE converted.

With that table in place, romm-nx reproduces **all 28** reference `.sfrom` files
byte for byte. Any other ROM gets the default payload; if it also has an
enhancement chip, the log says so explicitly.

### Mapping detection does not affect the output

Worth stating plainly, because an early build got this badly wrong: the `.sfrom`
body is the ROM verbatim and Canoe parses the cartridge header itself. Whether
romm-nx labels a ROM LoROM or HiROM changes nothing in the generated file. The
detection exists to confirm the image really is a SNES ROM and to fill in the
log — it must never be used to decide what the emulator can run.

An earlier gate refused map modes outside `0x20/0x21/0x30/0x31` and cartridge
types above `0x02`. It rejected three working games:

| game | why it was rejected | reality |
|---|---|---|
| Contra III | map mode byte reads `0x53` | checksum, reset vector and title at `0x7FC0` all valid; just an odd dump |
| Doom | Super FX | CaVE ships it, and NSO's own Star Fox is Super FX |
| Super Mario Kart | DSP-1 | also a stock NSO title |

Canoe emulates Super FX, DSP, SA-1 and friends — they are stock NSO titles. The
current policy is: convert anything with a credible cartridge header, and record
everything else (chip, odd map mode, failed checksum, odd size) as a warning
shown on screen and written to the log.

## `lclassics.titlesdb`

Minified UTF-8 JSON, no BOM, no trailing newline, literal UTF-8 for non-ASCII
(`©` and `ー` are stored as bytes, not `\u` escapes):

```json
{"titles":{"S-2180_e":{...},"S-2179_e":{...}, ...}}
```

Entries are in insertion order, not sorted. Keys inside an entry are
alphabetical. The file mixes stock titles (whose assets live in the base game's
romfs and have no folder under `titles/`) with injected ones — on this console,
71 entries and 28 asset folders.

Fields on every entry:

```
code copyright cover details_screen lcla6_release_date players_count publisher
release_date rewind_interval rom save_count simultaneous sort_publisher
sort_title title title_ko title_zhHans title_zhHant volume
```

Fields present on only some entries, which romm-nx must not disturb:
`hidden_countries` (2), `mouse_type` (2), `repo_type` (1), `startup_state` (7).

An entry as CaVE writes it for an injected game:

```json
{"code":"S-3051_e","copyright":"©Nintendo","cover":"/titles/S-3051_e/S-3051_e.png",
"details_screen":"/titles/S-3051_e/S-3051_e-details.png","lcla6_release_date":"2026-07-24",
"players_count":1,"publisher":"Nintendo","release_date":"2026-07-24","rewind_interval":1.5,
"rom":"/titles/S-3051_e/S-3051_e.sfrom","save_count":1,"simultaneous":false,
"sort_publisher":"nintendo","sort_title":"the incredible hulk","title":"The Incredible Hulk",
"title_ko":"The Incredible Hulk","title_zhHans":"ー","title_zhHant":"ー","volume":100}
```

`sort_title` / `sort_publisher` are lowercase ASCII with `™`/`®` and accents
dropped; punctuation such as `:` is kept. `volume` is 100 on every injected
entry. `rewind_interval` is always `1.5`.

Because of the unmodelled fields, romm-nx never deserializes and re-serializes
this file. It validates the structure, locates the byte span of the `titles`
object, and splices the new entry in immediately before its closing brace —
every pre-existing byte is copied verbatim. A reinstall replaces one entry's
value span in place.

## `strings.lng`

`romfs/bootapp/resources/strings/<lang>/strings.lng` is JSON with two root
objects, `exploded_strings` and `strings`. The title's description and its
key-guide labels come from here, **not** from the database. CaVE writes 18 keys
per injected title into `strings`, all suffixed with the code in underscore form
(`S-3051_e` -> `S_3051_e`):

```
META_TITLE_COMMENT_<CODE>
META_TITLE_KEY_GUIDE_{a,b,x,y,l,r,select,start,notation,supplementary,
                      mouse_l,mouse_r,dpad,dpad_up,dpad_down,dpad_left,dpad_right}_<CODE>
```

Injected titles get no `exploded_strings` entries.

### Key order is significant

Both objects are stored **sorted**, and appending a key at the end is not
enough: the emulator renders the raw key name on the game's info screen instead
of the string. The first build of this feature appended, and shipped
`META_TITLE_COMMENT_S_9896_e` as the visible description text.

The order is **not** `strcmp`. It is the ordering .NET's default culture-aware
comparer produces (CaVE is a C# application): **punctuation sorts before digits,
digits before letters, and letters compare case-folded**. Consequences a byte
comparison gets backwards:

| lower | higher | why strcmp disagrees |
|---|---|---|
| `SYS_DIALOG_..._WITH_PASSWORD` | `SYS_DIALOG_..._WITHOUT_PASSWORD` | `_` (0x5F) > `O` (0x4F) |
| `SYS_DIALOG_..._CREATE_SESSION` | `SYS_DIALOG_..._CREATED_SESSION_TEXT` | `_` > `D` |
| `META_TITLE_KEY_GUIDE_dpad_right_S_2152_e` | `META_TITLE_KEY_GUIDE_dpad_S_2002_e` | `r` (0x72) > `S` (0x53) |
| `SYS_HUD_GUIDE_LARK_VOICE_CHAT` | `SYS_HUD_GUIDE_LARK3_CONFIG_ADDITIONAL` | `_` > `3` |

`NsoSnesDb::CollateCompare` implements that rule. Verified against a stock
`strings.lng`: it reproduces the order of all 2438 keys across both objects with
zero violations, where an ordinal comparison produces violations immediately.
romm-nx inserts each key at its collated position, and moves a key it finds
sitting in the wrong place (i.e. one an earlier build appended).

Only the languages actually present in the LayeredFS are overridden — on this
console that is `fr` alone. romm-nx patches every `<lang>/strings.lng` it finds
and leaves the rest to the base game's romfs.

The NSO info screen does no word wrapping; CaVE's own placeholder text documents
a budget of roughly 50 columns and 10 lines per page, so romm-nx hard-wraps the
RomM summary at 50 columns.

## Prerequisite: the Full Unlock mod

romm-nx assumes the console already has a modded SNES Online. It does not ship,
generate or install the mod, and it never writes to `exefs/`.

The mod is `NSO-SNES-Full_Unlock.nsom` by DarkAkuma — a ZIP carrying a prebuilt
`exefs/main.npdm` and `exefs/subsdk9`, whose own `info.json` describes it as:

> Disables the SFROM Signature check, allowing the entire SNES/SFC ROM catalog
> to be used. Enables support for SNES Classic format SFROMs. Enables ExHiROM
> support. Enables SNES Rumble support.

It declares support for title IDs `01008D300C50C000` / `0100E8600C504000` across
app versions 1.0.0.0–5.1.0.0.

That signature check is why the mod is mandatory *for SNES specifically*: every
injected title carries a 256-byte, all-zero `.sfromsig`, i.e. an unsigned
RSA-2048 slot that only passes because the check is gone.

Detection reports the mod's presence (`exefs/subsdk9`) but treats its absence as
a warning, not a blocker — installing is allowed either way, and whether to run
the mod is the user's decision.

**SNES is the only NSO platform with such a check.** CaVE ships a `Full_Unlock`
for SNES and for nothing else; NES, GB/GBC, GBA, N64, Mega Drive and Virtual Boy
get only cosmetic mods (`CaVE_Info`, `Hide_Background`, `Hide_Player_Icon`,
`DisplayEx`). Those platforms should therefore be injectable on a stock,
unmodded app — see the table below if this is ever extended.

| platform | title ID | ROM | cover |
|---|---|---|---|
| GBA | `010012F017576000` | `.gba` (raw) | `.png` |
| GB/GBC | `0100C62011050000` | `.gbc` (raw) | `.png` |
| Virtual Boy | `0100BFC01D976000` | `.vb` (raw) | `.png` |
| N64 | `0100C9A00ECE6000` | `.bin` (raw) | `.png` |
| NES | `0100D870045B6000` | `.nes` (raw) | `.xtx.z` |
| Mega Drive | `0100B3C014BDA000` | `.bin` (raw) | `.xtx.z` |
| SNES | `01008D300C50C000` | `.sfrom` | `.png` |

SNES is the hardest of the set: the only one needing a container format and the
only one needing a mod. The `.xtx.z` covers the original brief expected belong to
NES and Mega Drive.

## What romm-nx deliberately does not touch

- `exefs/main.npdm`, `exefs/subsdk9` — the unlock/mod, out of scope.
- `romfs/DBINFO` — CaVE's own file-timestamp manifest. Not read by the emulator.
  Leaving it stale is harmless; rewriting it risks breaking CaVE's own tooling
  for no gain. A later CaVE sync may therefore not know about romm-nx's files.
- `romfs/bootapp/resources/{prefabs,scenes,scripts}` — untouched.
