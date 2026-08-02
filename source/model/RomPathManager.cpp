#include "RomPathManager.hpp"
#include <sys/stat.h>
#include <sys/types.h>
#include <algorithm>
#include <iostream>
#include <errno.h>

namespace romm::model {

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
