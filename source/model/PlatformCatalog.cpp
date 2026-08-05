#include "PlatformCatalog.hpp"

#include <algorithm>
#include <cctype>
#include <map>

namespace romm::model {

    namespace {

        // "Sony PlayStation 2" / "playstation_2" / "PLAYSTATION-2" all collapse
        // to "playstation-2" before the alias table is consulted, so the table
        // only has to list one spelling per alias.
        std::string Slugify(const std::string& value) {
            std::string out;
            out.reserve(value.size());
            bool pending_dash = false;
            for (char raw : value) {
                const unsigned char c = (unsigned char)raw;
                if (std::isalnum(c)) {
                    if (pending_dash && !out.empty()) out.push_back('-');
                    pending_dash = false;
                    out.push_back((char)std::tolower(c));
                } else {
                    pending_dash = true;
                }
            }
            return out;
        }

        // IGDB disambiguates colliding slugs with a "--<n>" suffix, and RomM can
        // carry that through to the platform slug / library folder name — a real
        // library has "ps4--1" sitting next to "ps2" and "ps3".
        //
        // Only the DOUBLE-dash form is stripped. A single dash before digits is
        // meaningful and must survive: "atari-2600" and "playstation-2" would
        // otherwise collapse to "atari" and "playstation".
        std::string StripIgdbDisambiguator(const std::string& value) {
            const size_t pos = value.rfind("--");
            if (pos == std::string::npos || pos == 0) return value;
            if (pos + 2 >= value.size()) return value; // trailing "--" with no digits
            for (size_t i = pos + 2; i < value.size(); ++i) {
                if (!std::isdigit((unsigned char)value[i])) return value;
            }
            return value.substr(0, pos);
        }

        // slug/name alias -> canonical id. Covers the RomM/IGDB slugs we've seen
        // plus the shorthand spellings a hand-edited config is likely to use.
        const std::map<std::string, std::string>& AliasTable() {
            static const std::map<std::string, std::string> table = {
                // PlayStation family
                {"ps", "psx"}, {"ps1", "psx"}, {"psx", "psx"}, {"psone", "psx"},
                {"playstation", "psx"}, {"playstation-1", "psx"}, {"sony-playstation", "psx"},
                {"ps2", "ps2"}, {"playstation-2", "ps2"}, {"sony-playstation-2", "ps2"},
                {"ps3", "ps3"}, {"playstation-3", "ps3"}, {"sony-playstation-3", "ps3"},
                {"ps4", "ps4"}, {"playstation-4", "ps4"}, {"sony-playstation-4", "ps4"},
                {"psp", "psp"}, {"playstation-portable", "psp"}, {"sony-psp", "psp"},

                // Nintendo handhelds
                {"gb", "gb"}, {"gameboy", "gb"}, {"game-boy", "gb"}, {"nintendo-game-boy", "gb"},
                {"gbc", "gbc"}, {"gameboy-color", "gbc"}, {"game-boy-color", "gbc"},
                {"nintendo-game-boy-color", "gbc"},
                {"gba", "gba"}, {"gameboy-advance", "gba"}, {"game-boy-advance", "gba"},
                {"nintendo-game-boy-advance", "gba"},
                {"ds", "nds"}, {"nds", "nds"}, {"nintendo-ds", "nds"},
                {"3ds", "3ds"}, {"n3ds", "3ds"}, {"nintendo-3ds", "3ds"},

                // Nintendo consoles
                {"nes", "nes"}, {"nintendo-entertainment-system", "nes"},
                {"nintendo-nes", "nes"}, {"famicom", "nes"},
                {"snes", "snes"}, {"super-nes", "snes"}, {"super-nintendo", "snes"},
                {"super-nintendo-entertainment-system", "snes"}, {"sfc", "snes"},
                {"super-famicom", "snes"},
                {"n64", "n64"}, {"nintendo-64", "n64"},
                {"wii", "wii"}, {"nintendo-wii", "wii"},
                {"wiiu", "wiiu"}, {"wii-u", "wiiu"}, {"nintendo-wii-u", "wiiu"},
                {"switch", "switch"}, {"nsw", "switch"}, {"nintendo-switch", "switch"},

                // Sega
                {"genesis", "genesis"}, {"megadrive", "genesis"}, {"mega-drive", "genesis"},
                {"sega-genesis", "genesis"}, {"sega-mega-drive", "genesis"},
                // IGDB (and therefore RomM) spells this one with a literal "slash".
                {"genesis-slash-megadrive", "genesis"}, {"sega-mega-drive-genesis", "genesis"},
                // Round-trips this platform's own display name, for the case
                // where a server sends a name but no slug.
                {"genesis-mega-drive", "genesis"}, {"mega-drive-genesis", "genesis"},
                {"saturn", "saturn"}, {"sega-saturn", "saturn"},

                // Other
                {"arcade", "arcade"}, {"mame", "arcade"}, {"arcade-machine", "arcade"},
                {"atari2600", "atari2600"}, {"atari-2600", "atari2600"},
                {"2600", "atari2600"}, {"atari-vcs", "atari2600"},
            };
            return table;
        }

    }

    std::string NormalizePlatformId(const std::string& value) {
        // Strip before slugifying: Slugify() collapses "--" to "-", which would
        // turn "ps4--1" into "ps4-1" and hide the suffix from this check.
        const std::string slug = Slugify(StripIgdbDisambiguator(value));
        if (slug.empty()) return slug;

        const auto& table = AliasTable();
        auto it = table.find(slug);
        if (it != table.end()) return it->second;

        // Unknown platform: keep the slugified form as its identity so it still
        // stores, looks up and de-duplicates consistently.
        return slug;
    }

    std::string ResolvePlatformIdentity(const std::string& slug, const std::string& name) {
        const std::string from_slug = NormalizePlatformId(slug);
        if (FindPlatformCatalogEntry(from_slug) != nullptr) {
            return from_slug; // "ps4", "ps4--1", "genesis-slash-megadrive", ...
        }

        // Slug didn't resolve — try the display name. This is what rescues a
        // Folder alias whose folder name says nothing about the platform.
        const std::string from_name = NormalizePlatformId(name);
        if (FindPlatformCatalogEntry(from_name) != nullptr) {
            return from_name;
        }

        // Genuinely unknown to romm-nx. Prefer the slug as its identity (stable
        // across a rename); fall back to the name only if there's no slug.
        return from_slug.empty() ? from_name : from_slug;
    }

    const std::vector<PlatformCatalogEntry>& GetPlatformCatalog() {
        // Grouped by manufacturer, oldest first inside a group — this order is
        // what PlatformSortMode::Brand shows, so it is release chronology
        // rather than anything alphabetical. The last column is the shipped
        // default visibility, which no longer has to be a contiguous block:
        // both lists that used to lean on this order now sort explicitly.
        static const std::vector<PlatformCatalogEntry> catalog = {
            {"nes",       "NES",                  "Nintendo", true},
            {"gb",        "Game Boy",             "Nintendo", true},
            {"snes",      "SNES",                 "Nintendo", true},
            {"n64",       "Nintendo 64",          "Nintendo", true},
            {"gbc",       "Game Boy Color",       "Nintendo", true},
            {"gba",       "Game Boy Advance",     "Nintendo", true},
            {"nds",       "Nintendo DS",          "Nintendo", true},
            {"wii",       "Nintendo Wii",         "Nintendo", false},
            {"3ds",       "Nintendo 3DS",         "Nintendo", true},
            {"wiiu",      "Nintendo Wii U",       "Nintendo", false},
            {"switch",    "Nintendo Switch",      "Nintendo", false},

            {"psx",       "PlayStation",          "Sony",     true},
            {"ps2",       "PlayStation 2",        "Sony",     true},
            {"psp",       "PlayStation Portable", "Sony",     true},
            {"ps3",       "PlayStation 3",        "Sony",     false},
            {"ps4",       "PlayStation 4",        "Sony",     false},

            {"genesis",   "Genesis / Mega Drive", "Sega",     false},
            {"saturn",    "Sega Saturn",          "Sega",     false},

            {"atari2600", "Atari 2600",           "Atari",    false},

            // Arcade has no single manufacturer, so it carries no brand and
            // sorts with the server-only platforms at the end of a brand list.
            {"arcade",    "Arcade",               "",         false},
        };
        return catalog;
    }

    const PlatformCatalogEntry* FindPlatformCatalogEntry(const std::string& canonical_id) {
        for (const auto& entry : GetPlatformCatalog()) {
            if (entry.id == canonical_id) return &entry;
        }
        return nullptr;
    }

    bool IsPlatformVisibleByDefault(const std::string& canonical_id) {
        const auto* entry = FindPlatformCatalogEntry(canonical_id);
        return entry != nullptr && entry->visible_by_default;
    }

    std::vector<std::string> GetDefaultHiddenPlatformIds() {
        std::vector<std::string> out;
        for (const auto& entry : GetPlatformCatalog()) {
            if (!entry.visible_by_default) out.push_back(entry.id);
        }
        return out;
    }

    std::string GetPlatformDisplayName(const std::string& canonical_id, const std::string& server_name) {
        const auto* entry = FindPlatformCatalogEntry(canonical_id);
        if (entry != nullptr) return entry->display_name;
        if (!server_name.empty()) return server_name;
        return canonical_id;
    }

    std::string GetPlatformBrand(const std::string& canonical_id) {
        const auto* entry = FindPlatformCatalogEntry(canonical_id);
        return entry != nullptr ? entry->brand : std::string();
    }

    namespace {

        // Case-insensitive, so a server that reports "playstation 5" doesn't
        // land after every capitalised name in the list.
        int CompareNames(const std::string& a, const std::string& b) {
            const size_t n = std::min(a.size(), b.size());
            for (size_t i = 0; i < n; ++i) {
                const int ca = std::tolower((unsigned char)a[i]);
                const int cb = std::tolower((unsigned char)b[i]);
                if (ca != cb) return ca < cb ? -1 : 1;
            }
            if (a.size() == b.size()) return 0;
            return a.size() < b.size() ? -1 : 1;
        }

        // Position in the catalogue; server-only platforms answer with its size
        // so they follow everything romm-nx ships knowledge of.
        size_t CatalogRank(const std::string& id) {
            const auto& catalog = GetPlatformCatalog();
            for (size_t i = 0; i < catalog.size(); ++i) {
                if (catalog[i].id == id) return i;
            }
            return catalog.size();
        }

        size_t CustomRank(const std::vector<std::string>& order, const std::string& id) {
            for (size_t i = 0; i < order.size(); ++i) {
                if (order[i] == id) return i;
            }
            return order.size();
        }

    }

    bool PlatformSortsBefore(PlatformSortMode mode,
                             const std::vector<std::string>& custom_order,
                             const std::string& id_a, const std::string& name_a,
                             const std::string& id_b, const std::string& name_b) {
        switch (mode) {
        case PlatformSortMode::Brand: {
            const std::string brand_a = GetPlatformBrand(id_a);
            const std::string brand_b = GetPlatformBrand(id_b);
            // No known manufacturer (Arcade, anything the server invented) goes
            // last as a group rather than sorting as an empty string, which
            // would put it first.
            if (brand_a.empty() != brand_b.empty()) return brand_b.empty();
            if (!brand_a.empty()) {
                const int by_brand = CompareNames(brand_a, brand_b);
                if (by_brand != 0) return by_brand < 0;
            }
            const size_t rank_a = CatalogRank(id_a);
            const size_t rank_b = CatalogRank(id_b);
            if (rank_a != rank_b) return rank_a < rank_b;
            break; // both server-only: fall through to the name comparison
        }
        case PlatformSortMode::Custom: {
            const size_t rank_a = CustomRank(custom_order, id_a);
            const size_t rank_b = CustomRank(custom_order, id_b);
            if (rank_a != rank_b) return rank_a < rank_b;
            break; // both unlisted: fall through, so they at least sort sanely
        }
        case PlatformSortMode::Name:
            break;
        }

        const int by_name = CompareNames(name_a, name_b);
        if (by_name != 0) return by_name < 0;
        // Two platforms with the same display name still need a stable answer.
        return id_a < id_b;
    }

}
