#include "RomPathManager.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <iostream>
#include <errno.h>

namespace romm::model {

    std::string RomPathManager::GetDefaultPath(const std::string& platform_slug) {
        if (platform_slug == "psx" || platform_slug == "playstation" || platform_slug == "ps1") {
            return "sdmc:/roms/ps1/";
        } else if (platform_slug == "ps2" || platform_slug == "playstation2") {
            return "sdmc:/roms/ps2/";
        } else if (platform_slug == "psp") {
            return "sdmc:/roms/psp/";
        } else if (platform_slug == "nds" || platform_slug == "nintendo_ds" || platform_slug == "nintendo-ds" || platform_slug == "Nintendo DS") {
            return "sdmc:/roms/nds/";
        } else if (platform_slug == "gb" || platform_slug == "game-boy" || platform_slug == "gameboy" || platform_slug == "nintendo-game-boy") {
            return "sdmc:/roms/gb/";
        } else if (platform_slug == "gbc" || platform_slug == "game-boy-color" || platform_slug == "gameboy-color" || platform_slug == "nintendo-game-boy-color") {
            return "sdmc:/roms/gbc/";
        } else if (platform_slug == "gba" || platform_slug == "game-boy-advance" || platform_slug == "gameboy-advance" || platform_slug == "nintendo-game-boy-advance") {
            return "sdmc:/roms/gba/";
        } else if (platform_slug == "3ds" || platform_slug == "nintendo-3ds" || platform_slug == "n3ds" || platform_slug == "nintendo_3ds") {
            return "sdmc:/roms/3ds/";
        }
        return "sdmc:/roms/" + platform_slug + "/";
    }

    bool RomPathManager::ValidatePath(const std::string& path) {
        if (path.find("sdmc:/") != 0) return false;
        if (path.empty() || path.back() != '/') return false;
        if (path.find("../") != std::string::npos) return false;
        return true;
    }

    bool RomPathManager::CreateFolderIfMissing(const std::string& path) {
        std::string clean_path = path;
        std::replace(clean_path.begin(), clean_path.end(), '\\', '/');
        
        // Walk each '/' boundary and mkdir
        for (size_t i = 1; i < clean_path.size(); ++i) {
            if (clean_path[i] == '/') {
                std::string sub = clean_path.substr(0, i);
                mkdir(sub.c_str(), 0777);
            }
        }
        int rc = mkdir(clean_path.c_str(), 0777);
        bool success = (rc == 0 || errno == EEXIST);
        if (success) {
            std::cout << "[ROM_PATH] Created folder " << clean_path << std::endl;
        } else {
            std::cerr << "[ROM_PATH] Failed to create folder " << clean_path << std::endl;
        }
        return success;
    }

}
