# Game Boy / Game Boy Color Switch Online LayeredFS format

Third in the series after `nso-snes-format.md` and `nso-nes-format.md`.
Established from CaVE's stock database (`0100C62011050000.json`, 14 app
versions), its image templates and one real CaVE-written injection.

## One app, both systems

The Game Boy app carries Game Boy and Game Boy Color titles together, and each
entry declares which:

| | Game Boy | Game Boy Color |
|---|---|---|
| `platform` field | `"DMG"` (20 stock entries) | `"CGB"` (22 stock entries) |
| code | `D-3DB7_e` | `C-7224_e` |
| rom | `<CODE>.gb` | `<CODE>.gbc` |
| palette fields | `CGB-*` **and** `DMG_MGB-*` | `CGB-*` only |

Title IDs: `0100C62011050000` (Americas/Europe), `0100395011044000` (Japan).

**The mode comes from the ROM, not from RomM.** Byte `0x143` of the cartridge
header is `0x80` for a Color-enhanced game and `0xC0` for Color-only; anything
else is DMG. A library files games under `gb` and `gbc` by shelf, and a
Color-enhanced cartridge can sit in either, so romm-nx maps **both** RomM
platforms to this one app and lets the header decide.

## Where it lives

```
sdmc:/atmosphere/contents/0100C62011050000/
  romfs/titles/lclassics.titlesdb
  romfs/titles/<CODE>/<CODE>.gb  or  <CODE>.gbc
  romfs/titles/<CODE>/<CODE>.png             <- cover, 512x512
  romfs/titles/<CODE>/<CODE>-details.png     <- details, 1069x802
  romfs/bootapp/resources/strings/<lang>/strings.lng
```

No `exefs`: like NES, no unlock mod is needed.

## The code is the ROM's own checksum

`D-3DB7_e` is not an allocation. The injected ROM's global checksum — the
big-endian word at `0x14E` — is `0x3DB7`:

```
title  : ADDAMS FAMILY 2
CGB    : 0x00      -> DMG, hence the "D-" prefix
global checksum: 0x3DB7
code   : D-3DB7_e
```

So the code is `<D|C>-<global checksum in uppercase hex>_e`, which makes it
deterministic: the same dump lands on the same entry in CaVE and in romm-nx
alike, and a reinstall replaces its own entry rather than accumulating copies.
Stock codes use `_e` and `_p` region suffixes (`_p` also carrying
`"display_version":"European"`); an injection gets `_e`.

If the code is already taken — a checksum collision, or CaVE's copy of the same
game — romm-nx probes the low nibble upward instead of overwriting, so it can
never silently replace someone else's title.

## Entry

Same container and splice rules as the other platforms. ASCII key order, which
puts the capitalised palette keys first. Exactly as CaVE wrote it:

```json
{"CGB-Default":"None","CGB-Nostalgic":"None","DMG_MGB-Default":"None",
"DMG_MGB-Nostalgic":"High","code":"D-3DB7_e","copyright":"©Nintendo",
"cover":"/titles/D-3DB7_e/D-3DB7_e.png",
"details_screen":"/titles/D-3DB7_e/D-3DB7_e-details.png",
"lcla6_release_date":"2026-08-03","platform":"DMG","players_count":1,
"publisher":"Nintendo","release_date":"2026-08-03","rewind_interval":1.5,
"rom":"/titles/D-3DB7_e/D-3DB7_e.gb","save_count":1,"sort_publisher":"nintendo",
"sort_title":"the addams family - pugsley's scavenger hunt",
"title":"The Addams Family - Pugsley's Scavenger Hunt","title_ko":"…","volume":100}
```

**Verified:** `BuildGbTitleEntryJson` reproduces that byte for byte, all 644.

Differences from the platforms already implemented:

- **No `title_zhHans` / `title_zhHant`.** SNES and NES entries carry them; not
  one Game Boy entry in the stock database does.
- `save_count` 1 and `volume` 100 — the SNES defaults, not NES's 0 and 80.
- Palettes instead of NES's filter fields. `None` / `Min` / `High`.
- Optional fields to leave untouched where present: `compatible_titles`,
  `connect_guides`, `emu_option`, `enable_ir`, `display_version`, `fadein`,
  `anothertitle_guides`.

## Images

| file | size | colour type |
|---|---|---|
| cover | 512x512 | RGBA |
| details | 1069x802 | RGB |

The details file CaVE wrote is byte-identical to its `D-XXXX_e-details.png`
template — the same generic-placeholder situation as SNES and NES — so romm-nx
renders its own instead of redistributing it.

The cover came out at exactly 512x512 with image content to all four edges: no
padding, no letterbox. Game Boy box art is close to square, so this is
consistent with the same height-locked rule NES uses (scale to 512 tall, keep
the aspect, cap the width at 512) landing on a square for a square source.
romm-nx applies that rule rather than forcing a square, since stretching a
non-square cover into one would distort it. If a future reference shows CaVE
padding to a fixed square instead, this is the line to revisit.

## `strings.lng`

One key per injected title, as on NES:

```
META_TITLE_COMMENT_<CODE>
```

Stock titles carry up to 11 key-guide keys plus `META_TITLE_CONNECT_GUIDE_01..03`
(link-cable instructions), but CaVE writes none of them for an injection, and
neither does romm-nx. Same 50-column / 10-line description budget.
