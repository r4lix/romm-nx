#pragma once

#include <string>
#include <vector>
#include <map>
#include <cctype>
#include <cstdint>

namespace romm::model {

    enum class ApiState {
        Idle,
        Loading,
        Success,
        FailedConnect,
        Unauthorized,
        NoData,
        // Config is fine and nothing has failed — the console just isn't on a
        // network yet. Distinct from FailedConnect so the UI can say so and the
        // fetch can start by itself once the connection comes up.
        WaitingNetwork
    };

    inline std::string NormalizePlatformSlug(const std::string& platform_slug) {
        std::string slug = platform_slug;
        if (slug == "playstation" || slug == "ps1" || slug == "psx" || slug == "sony-playstation") {
            return "psx";
        } else if (slug == "psp" || slug == "sony-psp" || slug == "playstation-portable") {
            return "psp";
        } else if (slug == "nds" || slug == "nintendo-ds" || slug == "nintendo_ds" || slug == "Nintendo DS") {
            return "nds";
        } else if (slug == "gb" || slug == "game-boy" || slug == "gameboy" || slug == "nintendo-game-boy") {
            return "gb";
        } else if (slug == "gbc" || slug == "game-boy-color" || slug == "gameboy-color" || slug == "nintendo-game-boy-color") {
            return "gbc";
        } else if (slug == "gba" || slug == "game-boy-advance" || slug == "gameboy-advance" || slug == "nintendo-game-boy-advance") {
            return "gba";
        } else if (slug == "ps2" || slug == "playstation-2" || slug == "playstation2" || slug == "sony-playstation-2") {
            return "ps2";
        } else if (slug == "ps3" || slug == "playstation-3" || slug == "playstation3" || slug == "sony-playstation-3") {
            return "ps3";
        } else if (slug == "3ds" || slug == "nintendo-3ds" || slug == "n3ds" || slug == "nintendo_3ds") {
            return "3ds";
        }
        return slug;
    }

    struct Game {
        std::string title;
        std::string description;
        std::string release_year;
        std::string developer;

        int id = 0;
        std::string fs_name;
        long long fs_size_bytes = 0;
        std::string cover_path;       // path_cover_small — thumbnail for fast initial load
        std::string cover_path_large;  // path_cover_large — HD cover for grid display
    };

    // Indices into `games` whose title starts with the letter selected by
    // letter_idx (0 = no filter / show all, 1-26 = A-Z). Shared by GameGrid
    // (render list) and NavigationManager (selection/input math) so both
    // stay derived from exactly one predicate instead of two hand-written
    // copies that could silently drift apart.
    inline std::string ToLowerAscii(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            out.push_back((char)std::tolower((unsigned char)c));
        }
        return out;
    }

    // Letter filter and search compose: a search narrows whatever the A-Z bar
    // has already selected, so neither has to be cleared to use the other.
    // query is matched case-insensitively as a substring, and is expected to
    // already be lowercased by the caller (it's hoisted out of the loop).
    inline std::vector<size_t> FilterGames(const std::vector<Game>& games, size_t letter_idx, const std::string& lowered_query) {
        std::vector<size_t> indices;
        char target_letter = ' ';
        if (letter_idx > 0) {
            target_letter = 'A' + (char)(letter_idx - 1);
        }
        for (size_t i = 0; i < games.size(); ++i) {
            const auto& game = games[i];
            if (target_letter != ' ') {
                if (game.title.empty()) continue;
                char first_char = (char)std::toupper((unsigned char)game.title[0]);
                if (first_char != target_letter) continue;
            }
            if (!lowered_query.empty() &&
                ToLowerAscii(game.title).find(lowered_query) == std::string::npos) {
                continue;
            }
            indices.push_back(i);
        }
        return indices;
    }

    inline std::vector<size_t> FilterGamesByLetter(const std::vector<Game>& games, size_t letter_idx) {
        return FilterGames(games, letter_idx, "");
    }

    struct Platform {
        std::string name;
        std::string id; // String representation of platform's ID
        std::string slug;
        std::vector<Game> games;
        ApiState roms_state = ApiState::Idle;
    };

    enum class DetailLoadState {
        NotLoaded,
        Loading,
        Loaded,
        Failed
    };

    // One physical file belonging to a ROM. Single-disc games have exactly one;
    // multi-disc games (e.g. a PS1 title stored on RomM as folder > discs) have
    // one entry per disc, each downloadable individually by its own file id.
    struct RomFileEntry {
        int id = 0;
        std::string file_name;
        long long file_size_bytes = 0;
    };

    struct GameDetail {
        int rom_id = 0;
        std::string description;
        std::string release_date;
        std::string developer;
        std::string publisher;
        std::string franchise;
        std::vector<std::string> genres;
        std::vector<std::string> collections;
        std::string players;
        std::string age_rating;
        std::string file_name;
        long long file_size_bytes = 0;
        int file_id = 0;
        int files_count = 0;
        std::vector<RomFileEntry> files; // every physical file (>1 = multi-disc)
        std::string path_cover_large;
        std::string path_cover_small;
        std::string miximage_v2_url;
        std::string error_message;
    };

    class DataModel {
    private:
        // Everything the server returned, in server order. Never filtered —
        // Settings > Platforms enumerates from here, and it's what makes
        // re-enabling a platform free (no second API request).
        std::vector<Platform> all_platforms;
        // The visible subset the whole UI indexes into.
        //
        // Ownership invariant: a platform's fetched games live in exactly ONE
        // of these two vectors — in `platforms` while it's visible, parked back
        // on `all_platforms` while it's hidden. That keeps hiding a platform
        // free of any copy, but it means neither vector alone sees every game;
        // anything searching across platforms must go through FindGameByRomId().
        std::vector<Platform> platforms;
        uint64_t platforms_generation = 0;
        ApiState platform_state;
        ApiState roms_state;

        std::map<int, DetailLoadState> detail_states;
        std::map<int, GameDetail> cached_rom_details;

    public:
        DataModel();

        const std::vector<Platform>& GetPlatforms() const;
        const std::vector<Platform>& GetAllPlatforms() const { return all_platforms; }
        void SetPlatforms(const std::vector<Platform>& new_plats);

        // Re-applies the Settings > Platforms filter to the cached server list.
        // Purely local: no request, and already-fetched ROMs survive a
        // hide/show round trip.
        void RebuildVisiblePlatforms();

        // Bumped on every rebuild. UI elements that cache per-platform state
        // watch this so a visibility change that happens to leave the list
        // length unchanged still invalidates them.
        uint64_t GetPlatformsGeneration() const { return platforms_generation; }
        const Platform* GetPlatformById(const std::string& id) const;

        // Looks a fetched game up across every platform, visible or hidden.
        // Hiding a platform is a browser filter, not a data eviction — callers
        // that resolve a ROM by id (installed-list cover lookup, for one) must
        // keep finding it. nullptr if no platform has fetched that ROM yet.
        const Game* FindGameByRomId(int rom_id) const;
        void UpdatePlatformGames(const std::string& platform_id, const std::vector<Game>& games);
        void SetPlatformRomsState(const std::string& platform_id, ApiState state);

        void SetPlatformState(ApiState state) { platform_state = state; }
        ApiState GetPlatformState() const { return platform_state; }

        void SetRomsState(ApiState state) { roms_state = state; }
        ApiState GetRomsState() const { return roms_state; }

        DetailLoadState GetDetailState(int rom_id) const;
        void SetDetailState(int rom_id, DetailLoadState state);
        const GameDetail* GetCachedDetail(int rom_id) const;
        void SetCachedDetail(int rom_id, const GameDetail& detail);
    };

}
