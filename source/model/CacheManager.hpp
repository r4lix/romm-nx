#pragma once

#include <string>

namespace romm::model {

    struct CacheStats {
        long long cover_size = 0;
        int cover_count = 0;
        long long total_size = 0;
        int total_count = 0;
    };

    class CacheManager {
    public:
        static CacheManager& Instance();

        CacheStats CalculateSize();
        bool ClearCache(long long& out_bytes, int& out_count);
        void AutoPrune();

    private:
        CacheManager() = default;
    };

}
