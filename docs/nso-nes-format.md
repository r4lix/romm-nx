# NES Switch Online ("Nintendo Classics") LayeredFS format

Companion to `nso-snes-format.md`. Everything below comes from CaVE's own files
on this machine — its stock database (`CaVE Database/stock_database/0100D870045B6000.json`,
21 app versions), its image templates (`CaVE Database/resources/CLV-P-NXXXE*.png`)
and one real CaVE-written entry — not from documentation.

Where this contradicts the "if this is ever extended" table at the bottom of the
SNES document, this wins: that table was written from the original brief, and it
had NES covers as `.xtx.z`. **They are ordinary PNG.**

## Where it lives

```
sdmc:/atmosphere/contents/0100D870045B6000/
  romfs/DBINFO                               <- CaVE bookkeeping; never touched
  romfs/titles/lclassics.titlesdb            <- same filename as SNES
  romfs/titles/<CODE>/<CODE>.nes
  romfs/titles/<CODE>/<CODE>.png             <- cover, 359x512 (portrait)
  romfs/titles/<CODE>/<CODE>00.png           <- details screen, 400x300
  romfs/bootapp/resources/strings/<lang>/strings.lng
```

`0100D870045B6000` is the NES app; `0100B4E00444C000` is the Famicom one, and
CaVE's mods declare both together. No `exefs/` at all: **NES needs no unlock
mod.** The SNES signature check is what made `exefs/subsdk9` mandatory there,
and there is no NES equivalent — CaVE ships `Full_Unlock` for SNES only.

Differences from SNES worth being explicit about, because they are the kind of
thing that gets assumed rather than checked:

| | SNES | NES |
|---|---|---|
| code | `S-3051_e` | `CLV-P-NABCE` |
| rom | `<CODE>.sfrom` + 256-byte zero `<CODE>.sfromsig` | `<CODE>.nes`, nothing else |
| details file | `<CODE>-details.png` | `<CODE>00.png` |
| cover | 512x374 landscape | 359x512 portrait |
| unlock mod | required | not needed |

## `<CODE>`

`CLV-P-N` + two letters + `E`, e.g. `CLV-P-NABCE` — the NES Classic Edition
product codes, carried over. Alternate versions of a game take a `-sp1`/`-sp2`
suffix (`CLV-P-NAANE-sp1`), so code parsing must not assume a fixed length.

The stock catalogue occupies a large, non-contiguous slice of that space: 96
codes in the current `default_order`, spanning `NAAAE` through `NAJVE` with
gaps. An allocator therefore cannot just walk upward from a base — it has to
probe against the database the way the SNES one does, and it should start well
clear of the stock block.

## Images

Measured on CaVE's own output for an injected title, not just its templates:

| file | size | colour type |
|---|---|---|
| `CLV-P-NQCLE.png` (cover) | **374x512** | RGBA |
| `CLV-P-NQCLE00.png` (details) | 400x300 | RGB |

**The height is the fixed quantity, not the width.** The source art supplied to
CaVE was 497x680; 497 x 512/680 = 374. So the cover is the art scaled to 512
tall with its aspect kept, and the 359x512 template is simply the placeholder's
own shape — reading it as a target size would squash every cover that is not
0.70:1. romm-nx caps width at 512 as its own safeguard; nothing observed needs
it.

The details screen is **byte-identical to `CaVE Database/resources/CLV-P-NXXXE00.png`**
— the same generic placeholder situation as SNES, where all 28 injected details
files matched the template. romm-nx does not redistribute it; it renders its own
400x300 from the same cover source.

## `lclassics.titlesdb`

Identical container to SNES: minified UTF-8 JSON, no BOM, no trailing newline,
`{"titles":{"<CODE>":{...}}}`, entries in insertion order, keys inside an entry
alphabetical. The same byte-span splice approach applies, and for the same
reason — unmodelled fields must survive.

One entry, exactly as CaVE wrote it (Ghosts'n Goblins, re-added to an empty
database on this console):

```json
{"armet_threshold":85,"armet_version":"v1","code":"CLV-P-NABCE",
"copyright":"©CAPCOM CO., LTD. 1986, 2018 ALL RIGHTS RESERVED.",
"cover":"/titles/CLV-P-NABCE/CLV-P-NABCE.png",
"details_screen":"/titles/CLV-P-NABCE/CLV-P-NABCE00.png","fadein":[3,2],
"lcla6_release_date":"2018-09-01","overscan":[0,0,9,3],"players_count":2,
"publisher":"CAPCOM","release_date":"1986-11-01","rewind_interval":1.5,
"rom":"/titles/CLV-P-NABCE/CLV-P-NABCE.nes","save_count":0,"simultaneous":false,
"sort_publisher":"capcom","sort_title":"ghosts'n goblins","title":"Ghosts'n Goblins™",
"title_ko":"Ghosts'n Goblins™","title_zhHans":"ー","title_zhHant":"ー","volume":69}
```

Everything SNES has is here, plus four NES-only fields:

| field | meaning | observed values |
|---|---|---|
| `armet_threshold` | strength of the app's scanline/blend filter | 85 on this entry, 100 on most stock ones |
| `armet_version` | which filter revision | `"v1"`, `"v2_blend"` |
| `fadein` | title fade timing | `[3,2]` on every entry seen |
| `overscan` | pixels cropped, presumably T/B/L/R | `[0,0,9,3]` on every entry seen |

`volume` is per-game here (69, 74, 85...), not the flat 100 SNES injections use.
`save_count` is legitimately 0 on NES entries. `rewind_interval` is 1.5 as usual.

### What CaVE writes for a ROM it has never seen

The entry above is a *stock* game re-added, so its metadata came out of CaVE's
stock database. The one that matters is an injection of a ROM outside the
catalogue, and its defaults differ:

```json
{"armet_threshold":85,"armet_version":"off","code":"CLV-P-NQCLE",
"copyright":"©Nintendo",...,"players_count":1,"publisher":"Nintendo",
"release_date":"2026-08-03","save_count":0,"volume":80}
```

`armet_version` is **`"off"`** — the scanline filter disabled, because there is
no per-game filter profile for an unknown ROM. `volume` is 80 and `save_count`
0, both different from the SNES injection defaults (100 and 1). Copyright and
publisher fall back to Nintendo, and both dates become the injection date.
`fadein` and `overscan` are unchanged from the stock values.

**Verified:** given CaVE's own field values, `BuildNesTitleEntryJson` reproduces
both entries byte for byte — the stock re-add at 637 bytes and the injected one
at 672, `©`/`™`/`ー` included.

## Codes for injected titles

Every stock code is `CLV-P-NA__E`: `NAAAE` through `NAJTE`, 96 counting the
`-sp` variants. CaVE allocates its own injections in the `NQ` block
(`CLV-P-NQCLE`). romm-nx takes `CLV-P-NZ__E` — 676 slots, seeded from the ROM
hash so a given dump always lands on the same one, probing upward on collision.

Three blocks, three writers, no overlap, and that is the point rather than
tidiness. Nintendo keeps adding games to the service and their codes continue
through `NA`; CaVE keeps injecting into `NQ`. A romm-nx code that collided with
either would not surface as an error — it would silently shadow that title in
the app. `IsInjectedNesCode` gates removal on the `NZ` prefix too, so an
uninstall can never take out a stock entry or one of CaVE's.

## `strings.lng`

Same file, same two root objects, same collation rule (`CollateCompare` — the
.NET culture-aware order, verified against the stock SNES file and equally
applicable here). Per-title keys are suffixed with the code in underscore form,
`CLV-P-NABCE` -> `CLV_P_NABCE`.

Stock NES titles carry up to 11 per-title keys — `META_TITLE_KEY_GUIDE_` for
`a`, `b`, `dpad`, `dpad_up/down/left/right`, `select`, `start`, plus `notation`
(9 of 84 titles) and `supplementary` (35). There is no `x`, `y`, `l`, `r` or
`mouse_*` key anywhere in the NES file.

**But CaVE writes exactly one key for an injected title:**

```
META_TITLE_COMMENT_<CODE>
```

No key-guide keys at all — against 18 per title on SNES. romm-nx follows suit:
`NesGuideKeys()` is empty, which is why `PatchStringsFile` takes the key set as
a parameter rather than assuming the SNES table. Inventing "A"/"B"/"D-Pad"
placeholder rows would put text on Nintendo's guide screen that CaVE does not.

CaVE's placeholder description also documents the text budget, and it is the
same one the SNES side already wraps to:

> Word wrap is not enabled on Switch Online. […] Only 10 lines will be displayed
> at a time.

with a 50-character ruler line under it. romm-nx's existing 50-column hard wrap
therefore applies to NES unchanged.

## How romm-nx implements it

One pipeline, not two. `NsoSnesInstaller` carries a platform profile
(`MakeProfile`) holding the install root, asset suffixes and container policy,
and the four places that genuinely differ — ROM step, cover geometry, code
allocation, entry/verify/strings — branch on it. Staging, backup, atomic write
order, rollback and verification are the same code for both, which is the point:
that machinery was hardened against a real console, and a second copy of it
would drift.

Each platform gets its own tree, so nothing can be confused for the other:

```
sdmc:/switch/romm-nx/nso-snes/{staging,backups,injected.txt}
sdmc:/switch/romm-nx/nso-nes/{staging,backups,injected.txt}
```

Detection, the injected-games index, uninstall and restore are all per platform.

## The `.nes` file keeps its iNES header

Settled by measurement, not inference. CaVE's injected ROM is 262,160 bytes:

```
00000000  4e 45 53 1a 08 10 10 08  00 00 00 00 01 00 00 01
          N  E  S  ^Z
```

262,144 bytes of ROM data plus exactly 16 bytes of header. The file is the
source `.nes` copied verbatim — no container, no stripping, no padding.

Note byte 7 = `0x08`: bits 2-3 mark this as **NES 2.0**, and byte 12 = `0x01`
gives it PAL timing. That matters for parsing, because iNES 1.0 headers are
expected to have bytes 12-15 zeroed and a non-zero tail there is the classic
sign of a dumper signature (`DiskDude!`) polluting the mapper's high nibble. In
NES 2.0 those bytes are real fields. `AnalyzeNesRom` therefore decides NES 2.0
*before* applying the dirty-header rule — an earlier version of it flagged this
very ROM as dirty and would have dropped the mapper's high nibble on any 2.0
image.

## A stock code produces no assets

Worth recording, because it looks like a failed injection: re-adding a **stock**
code (`CLV-P-NABCE`, Ghosts'n Goblins) writes a database entry and nothing else.
Its `cover`/`details_screen`/`rom` paths resolve inside the base game's own
romfs, so there is no folder under `titles/` and cover art supplied for it has
nowhere to go. Only a ROM outside the 96 stock codes produces an asset folder.
