# Game Boy Advance Switch Online LayeredFS format

Fourth in the series. Established from CaVE's stock database
(`010012F017576000.json`) and **two** real CaVE-written injections, which is the
first time a platform came with more than one reference entry.

## Where it lives

```
sdmc:/atmosphere/contents/010012F017576000/
  romfs/titles/lclassics.titlesdb
  romfs/titles/<CODE>/<CODE>.gba            <- raw, verbatim
  romfs/titles/<CODE>/<CODE>.png            <- cover, 512 box
  romfs/titles/<CODE>/<CODE>-details.png    <- details, 400x300
```

`0100555017574000` is the second GBA title in the family and carries the same
layout. No `exefs`: no unlock mod needed, as on NES and Game Boy.

## The code is the cartridge's game code

`A-AG5E_e` — `AG5E` is the four-character game code at offset `0xAC` of the GBA
header, read straight out of the ROM:

```
A-AG5E_e.gba   title "SUPER GHOULS"   game code AG5E   maker 08
A-ASIE_e.gba   title "THE SIMS ADV"   game code ASIE   maker 69
```

So the pattern is `A-<game code>_e`, the same "derive, don't allocate" approach
Game Boy uses with its global checksum. Stock codes are numeric (`A-7279_p`) and
injected ones alphabetic, so the two do not compete for the same names. A code
already present falls back to varying the last character rather than replacing
whatever is there.

Homebrew with a blank game-code field gets four characters from the ROM hash
instead, so an entry can never be written with an empty code.

## Entry

```json
{"code":"A-AG5E_e","copyright":"©Nintendo","cover":"/titles/A-AG5E_e/A-AG5E_e.png",
"details_screen":"/titles/A-AG5E_e/A-AG5E_e-details.png","fadein":[3,0],
"lcla6_release_date":"2026-08-03","players_count":1,"publisher":"Nintendo",
"release_date":"2026-08-03","rewind_interval":1.5,"rom":"/titles/A-AG5E_e/A-AG5E_e.gba",
"save_count":1,"simultaneous":false,"sort_publisher":"nintendo",
"sort_title":"super ghouls'n ghosts","sram_file_size":164096,"title":"Super Ghouls'n Ghosts",
"title_ko":"Super Ghouls'n Ghosts","title_zhHans":"ー","title_zhHant":"ー","volume":100}
```

**Verified:** `BuildGbaTitleEntryJson` reproduces both reference entries byte for
byte — 561 and 635 bytes.

GBA-specific fields:

| field | injected value | notes |
|---|---|---|
| `sram_file_size` | `164096` | Stock entries carry the cartridge's real save size (512 B, 8 K, 32 K, 64 K, 128 K). CaVE writes one value large enough for any of them on an injected title, and so does romm-nx — inferring a save type from the ROM and getting it wrong costs the player their save. |
| `fadein` | `[3,0]` | `[3,2]` on NES, `[3,9]` on some stock GBA entries. |

`title_zhHans`/`title_zhHant` are present here (as on SNES and NES, unlike Game
Boy), `simultaneous` is present (unlike Game Boy), and `save_count` is 1 with
`volume` 100.

Optional stock fields romm-nx never writes and the splice preserves:
`compatible_titles`, `connect_guides`, `onecartridge_guides`, `transfer_title`,
`adjust_colors`, `hidden_countries`, `display_version`.

## Images

| file | size | colour type |
|---|---|---|
| cover | **512x512** and **512x508** | RGBA |
| details | 400x300 | RGB |

Two covers, two different sizes — which is the useful part. It confirms the rule
inferred for Game Boy from a single square sample: the art is fitted inside a
512x512 box with its aspect kept and **no padding**, so the canvas is whatever
the scaled art measures. A square source lands on 512x512; a source 1.008:1 wide
lands on 512x508. romm-nx already implements exactly that.

Both details files are byte-identical to CaVE's `A-XXXX_e-details.png` template,
the same placeholder situation as every other platform, so romm-nx renders its
own.

## `strings.lng`

One key per injected title:

```
META_TITLE_COMMENT_<CODE>
```

as on NES and Game Boy.
