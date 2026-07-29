#include "DataModel.hpp"
#include "ConfigManager.hpp"
#include "PlatformCatalog.hpp"
#include <iostream>

namespace romm::model {

    DataModel::DataModel()
        : platform_state(ApiState::Idle), roms_state(ApiState::Idle) {}

    const std::vector<Platform>& DataModel::GetPlatforms() const {
        return this->platforms;
    }

    void DataModel::SetPlatforms(const std::vector<Platform>& new_plats) {
        this->all_platforms = new_plats;
        this->platforms.clear(); // fresh server list: nothing cached to carry over

        // First sighting of a platform decides its default visibility. Persist
        // straight away, so the default survives even if the app never reaches
        // Settings — but only when something actually changed, to keep this off
        // the hot path of every refetch.
        std::vector<std::string> slugs;
        slugs.reserve(new_plats.size());
        for (const auto& plat : new_plats) {
            slugs.push_back(ResolvePlatformIdentity(plat.slug, plat.name));
        }
        auto& config = ConfigManager::Instance();
        if (config.RegisterDetectedPlatforms(slugs)) {
            config.Save();
        }

        RebuildVisiblePlatforms();
    }

    void DataModel::RebuildVisiblePlatforms() {
        auto& config = ConfigManager::Instance();

        // Park whatever the visible list has already fetched back onto the
        // master list first. Hiding a platform must not throw away its ROMs:
        // re-enabling it has to come back without another request.
        for (auto& loaded : this->platforms) {
            for (auto& meta : this->all_platforms) {
                if (meta.id == loaded.id) {
                    meta.games = std::move(loaded.games);
                    meta.roms_state = loaded.roms_state;
                    break;
                }
            }
        }

        std::vector<Platform> next;
        next.reserve(this->all_platforms.size());
        for (auto& meta : this->all_platforms) {
            if (!config.IsPlatformVisible(ResolvePlatformIdentity(meta.slug, meta.name))) continue;
            Platform visible;
            visible.name = meta.name;
            visible.id = meta.id;
            visible.slug = meta.slug;
            visible.games = std::move(meta.games); // ownership hops back on hide
            visible.roms_state = meta.roms_state;
            next.push_back(std::move(visible));
        }

        this->platforms = std::move(next);
        this->platforms_generation++;

        std::cout << "[PLATFORMS] Visible " << this->platforms.size() << "/"
                  << this->all_platforms.size() << " platforms" << std::endl;
    }

    const Platform* DataModel::GetPlatformById(const std::string& id) const {
        for (const auto& plat : this->platforms) {
            if (plat.id == id) {
                return &plat;
            }
        }
        return nullptr;
    }

    const Game* DataModel::FindGameByRomId(int rom_id) const {
        if (rom_id <= 0) return nullptr;
        for (const auto* list : {&this->platforms, &this->all_platforms}) {
            for (const auto& plat : *list) {
                for (const auto& game : plat.games) {
                    if (game.id == rom_id) return &game;
                }
            }
        }
        return nullptr;
    }

    void DataModel::UpdatePlatformGames(const std::string& platform_id, const std::vector<Game>& games) {
        for (auto& plat : this->platforms) {
            if (plat.id == platform_id) {
                plat.games = games;
                if (games.empty()) {
                    plat.roms_state = ApiState::NoData;
                } else {
                    plat.roms_state = ApiState::Success;
                }
                break;
            }
        }
    }

    void DataModel::SetPlatformRomsState(const std::string& platform_id, ApiState state) {
        for (auto& plat : this->platforms) {
            if (plat.id == platform_id) {
                plat.roms_state = state;
                break;
            }
        }
    }

    DetailLoadState DataModel::GetDetailState(int rom_id) const {
        auto it = detail_states.find(rom_id);
        if (it != detail_states.end()) {
            return it->second;
        }
        return DetailLoadState::NotLoaded;
    }

    void DataModel::SetDetailState(int rom_id, DetailLoadState state) {
        detail_states[rom_id] = state;
    }

    const GameDetail* DataModel::GetCachedDetail(int rom_id) const {
        auto it = cached_rom_details.find(rom_id);
        if (it != cached_rom_details.end()) {
            return &it->second;
        }
        return nullptr;
    }

    void DataModel::SetCachedDetail(int rom_id, const GameDetail& detail) {
        cached_rom_details[rom_id] = detail;
        detail_states[rom_id] = DetailLoadState::Loaded;
    }

}
