#pragma once

#include <string>
#include <vector>
#include <map>

namespace romm::model {

    enum class ApiState {
        Idle,
        Loading,
        Success,
        FailedConnect,
        Unauthorized,
        NoData
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
        std::string path_cover_large;
        std::string path_cover_small;
        std::string miximage_v2_url;
        std::string error_message;
    };

    class DataModel {
    private:
        std::vector<Platform> platforms;
        ApiState platform_state;
        ApiState roms_state;

        std::map<int, DetailLoadState> detail_states;
        std::map<int, GameDetail> cached_rom_details;

    public:
        DataModel();
        
        const std::vector<Platform>& GetPlatforms() const;
        void SetPlatforms(const std::vector<Platform>& new_plats);
        const Platform* GetPlatformById(const std::string& id) const;
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
