#pragma once

#include <string>

namespace romm::model {

    class RomPathManager {
    public:
        static std::string GetDefaultPath(const std::string& platform_slug);
        static bool ValidatePath(const std::string& path);
        static bool CreateFolderIfMissing(const std::string& path);
    };

}
