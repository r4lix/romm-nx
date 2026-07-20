#pragma once

#include <pu/Plutonium>
#include "CoverProfile.hpp"
#include "../navigation/HttpClient.hpp"
#include <string>
#include <unordered_map>
#include <list>
#include <memory>
#include <atomic>

namespace romm::ui {

    enum class CoverState {
        Unknown,
        Missing,
        Cached,
        Loading,
        Ready,
        FailedTransient,
        FailedPermanent
    };

    struct CoverCacheKey {
        std::string platform_slug;
        int64_t rom_id = 0;
        std::string cover_source;
        std::string variant;
        int requested_width = 0;
        int requested_height = 0;

        bool operator==(const CoverCacheKey& other) const {
            return platform_slug == other.platform_slug &&
                   rom_id == other.rom_id &&
                   cover_source == other.cover_source &&
                   variant == other.variant &&
                   requested_width == other.requested_width &&
                   requested_height == other.requested_height;
        }
    };

    struct CoverCacheResult {
        pu::sdl2::Texture texture = nullptr;
        CoverCacheKey key;
        CoverState state = CoverState::Unknown;
    };

    // Result of an off-main-thread image decode. Mirrors the HttpResult
    // pattern: a worker thread fills it in and flips `completed`; the main
    // thread polls it from PollCompleted() and takes ownership of `surface`
    // for the GPU upload. If the entry is evicted while a decode is in
    // flight, the worker's shared_ptr keeps this alive and the destructor
    // frees the orphaned surface.
    struct CoverDecodeResult {
        std::atomic<bool> completed{false};
        SDL_Surface* surface = nullptr;   // decoded pixels, ready for GPU upload
        std::string final_path;           // cache path after any .tmp rename
        ~CoverDecodeResult() {
            if (surface) {
                SDL_FreeSurface(surface);
            }
        }
    };

    struct CoverEntry {
        CoverState state = CoverState::Unknown;
        pu::sdl2::Texture texture = nullptr;
        std::shared_ptr<HttpResult> download_result;
        std::shared_ptr<CoverDecodeResult> decode_result;
        std::string cache_path;

        CoverProfileType profile_type = CoverProfileType::DefaultPortrait;
        bool is_big = false;
        int original_rom_id = 0;
        std::string platform_slug;
        std::string cover_path_rel;

        CoverCacheKey key;
        int decode_retry_count = 0;
        std::chrono::steady_clock::time_point last_retry_time = {};
    };

    /**
     * Singleton cover texture cache.
     *
     * Usage:
     *   GetOrRequest(rom_id, platform_slug, cover_path_rel)  — returns result with texture or nullptr
     *   PollCompleted()                                      — call on main thread every frame
     *   Clear()                                              — call when switching platforms
     */
    class CoverCache {
    public:
        static constexpr int MAX_ENTRIES = 256;
        static const std::string COVER_CACHE_DIR;

        static CoverCache& Instance();

        // Returns CoverCacheResult. Kicks off download if Idle.
        CoverCacheResult GetOrRequest(int64_t rom_id, const std::string& platform_slug, const std::string& cover_path_rel, CoverProfileType profile_type = CoverProfileType::DefaultPortrait, bool is_big = false, bool allow_download = true);
        CoverCacheResult GetOrRequest(int64_t rom_id, const std::string& platform_slug, const std::string& cover_path_rel, CoverProfileType profile_type, const std::string& variant, bool allow_download = true);

        // Drive the two-phase cover pipeline: hand completed downloads (or
        // disk-cache hits) to a worker thread for decoding, and upload
        // finished decodes to GPU textures (at most 2 uploads per call).
        // Must be called on the main/render thread.
        void PollCompleted();

        // Release all GPU textures (keeps disk cache).
        void Clear();
 
        int GetPendingBigDownloadsCount() const;

     private:
        CoverCache() = default;
        ~CoverCache() { Clear(); }

        void TouchLru(const std::string& key);
        void EvictIfNeeded();

        std::unordered_map<std::string, CoverEntry> cache_;
        std::list<std::string> lru_order_;
    };

} // namespace romm::ui
