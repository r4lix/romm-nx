#pragma once

#include <string>

namespace romm::model {

    class RomPathManager {
    public:
        static bool ValidatePath(const std::string& path);
        static bool CreateFolderIfMissing(const std::string& path);
    };

}
