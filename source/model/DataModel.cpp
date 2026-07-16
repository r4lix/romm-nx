#include "DataModel.hpp"

namespace romm::model {

    DataModel::DataModel()
        : platform_state(ApiState::Idle), roms_state(ApiState::Idle) {}

    const std::vector<Platform>& DataModel::GetPlatforms() const {
        return this->platforms;
    }

    void DataModel::SetPlatforms(const std::vector<Platform>& new_plats) {
        this->platforms = new_plats;
    }

    const Platform* DataModel::GetPlatformById(const std::string& id) const {
        for (const auto& plat : this->platforms) {
            if (plat.id == id) {
                return &plat;
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
