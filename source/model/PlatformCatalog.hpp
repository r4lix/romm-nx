#pragma once

#include <string>
#include <vector>

namespace romm::model {

    // Canonical platform identity, used ONLY for visibility logic, config
    // storage and the Settings > Platforms rows.
    //
    // Deliberately separate from NormalizePlatformSlug() (DataModel.hpp), which
    // keys ROM download folders, the cover cache and installed-game detection —
    // widening that one would silently move existing files. The slug RomM
    // returns is never rewritten: it stays on Platform::slug and is what every
    // API call keeps using. This is an identity layer on top of it.
    struct PlatformCatalogEntry {
        std::string id;           // canonical id, e.g. "psx"
        std::string display_name; // e.g. "PlayStation"
        std::string brand;        // manufacturer, e.g. "Sony"; empty when there isn't one
        bool visible_by_default;
    };

    // How the platform list is ordered, everywhere it is shown: the library
    // sidebar, the banner column and the Settings > Platforms list all sort
    // through PlatformSortsBefore() below. Stored in config.json.
    enum class PlatformSortMode {
        Name,   // alphabetical, by the name each list displays
        Brand,  // grouped by manufacturer, catalogue order inside a group
        Custom  // the order the user arranged in Settings > Platforms
    };

    // Folds a RomM/IGDB slug OR a display name down to one canonical id, so
    // aliases ("ps1", "playstation", "PlayStation", "sony-playstation") all
    // resolve to the same entry. Unrecognised values come back slugified
    // (lowercase, dash-separated) rather than empty, so a platform romm-nx has
    // never heard of still gets a stable identifier.
    std::string NormalizePlatformId(const std::string& value);

    // Canonical id for a platform as the server reports it, using the display
    // name as a second opinion when the slug alone doesn't land on a known
    // platform.
    //
    // RomM binds a library folder to a platform, and the two can diverge: a
    // folder can be "Auto-detected" (folder name == canonical slug) or a
    // "Folder alias" (any folder name at all, bound to a real platform). An old
    // instance carries "ps4--1" where a current one has "ps4", and an admin can
    // bind something arbitrary. Whatever the folder is called, RomM still
    // reports the bound platform's name — so falling back to the name recovers
    // the identity that the slug lost.
    std::string ResolvePlatformIdentity(const std::string& slug, const std::string& name);

    // Every platform romm-nx knows about, in display order.
    const std::vector<PlatformCatalogEntry>& GetPlatformCatalog();

    // nullptr for ids outside the catalogue (i.e. server-only platforms).
    const PlatformCatalogEntry* FindPlatformCatalogEntry(const std::string& canonical_id);

    // Unknown / newly detected platforms are hidden by default: only ids
    // explicitly listed as visible in the catalogue return true.
    bool IsPlatformVisibleByDefault(const std::string& canonical_id);

    // Catalogue entries that ship hidden. Used to seed a config that has no
    // visibility data yet, and by Reset Defaults.
    std::vector<std::string> GetDefaultHiddenPlatformIds();

    // Catalogue display name if known, otherwise the server-provided name,
    // otherwise the canonical id itself.
    std::string GetPlatformDisplayName(const std::string& canonical_id, const std::string& server_name);

    // Manufacturer for a canonical id ("Nintendo", "Sony", ...). Empty for
    // anything outside the catalogue, and for platforms that have no single
    // manufacturer (Arcade) — those sort last under PlatformSortMode::Brand.
    std::string GetPlatformBrand(const std::string& canonical_id);

    // The one place the ordering rules live, so the library and Settings can't
    // disagree about what "sorted" means. Callers pass the name each of them
    // actually displays — the sidebar draws RomM's name, Settings draws the
    // catalogue's — so an alphabetical list reads alphabetically wherever it
    // is shown, even when the two names differ.
    //
    // `custom_order` is consulted only in Custom mode; ids missing from it (a
    // platform detected after the order was saved) follow every listed one.
    // The comparison is a strict total order, so std::sort is well-defined
    // whichever mode is active.
    bool PlatformSortsBefore(PlatformSortMode mode,
                             const std::vector<std::string>& custom_order,
                             const std::string& id_a, const std::string& name_a,
                             const std::string& id_b, const std::string& name_b);

}
