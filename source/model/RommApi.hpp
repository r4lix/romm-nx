#pragma once

#include "../navigation/HttpClient.hpp"
#include "DataModel.hpp"
#include <memory>
#include <string>
#include <vector>

namespace romm::model {

    struct PlatformFetchResult {
        bool completed = false;
        bool success = false;
        long statusCode = 0;
        std::vector<Platform> platforms;
    };

    struct RomFetchResult {
        bool completed = false;
        bool success = false;
        long statusCode = 0;
        int platform_id = 0;
        int request_id = 0;
        std::vector<Game> games;
    };

    struct RomDetailFetchResult {
        bool completed = false;
        bool success = false;
        long statusCode = 0;
        int rom_id = 0;
        uint64_t generation = 0;
        std::string platform_slug;
        GameDetail detail;
    };

    class RommApi {
    public:
        static std::shared_ptr<PlatformFetchResult> fetchPlatformsAsync();
        static std::shared_ptr<RomFetchResult> fetchRomsAsync(int platformId, int requestId);
        static std::shared_ptr<RomDetailFetchResult> fetchRomDetailAsync(int romId, uint64_t generation = 0, const std::string& platform_slug = "");
    };

}
