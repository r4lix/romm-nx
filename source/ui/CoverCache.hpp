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

        // Where this cover would be fetched from, kept for the entry's whole
        // life rather than consumed on first use. An entry can sit in Missing
        // for a while — the download slots were all taken, or it was first
        // requested with allow_download=false — and needs to be startable
        // later without redoing the disk-cache probe.
        std::string download_url;
        bool download_needs_auth = false;

        // True only between StartDownload and ReleaseInflight. Tracked
        // explicitly instead of inferred from download_result, because the
        // disk-cache-hit path also parks a (pre-completed) result there
        // without ever having taken a slot.
        bool holds_download_slot = false;

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

        // Ceiling on cover downloads that may be queued or running at once.
        //
        // The grid re-requests every visible and prefetched tile each frame, so
        // without a ceiling a scroll through a large library queues a download
        // per tile it passes — thousands of them, none cancellable. The covers
        // on screen then sit behind minutes of covers the user already scrolled
        // past. Refusing to start beyond this many leaves the entry in Missing,
        // and because the grid asks again next frame, whatever is on screen
        // *now* claims the slots as they free up.
        static constexpr int MAX_INFLIGHT_DOWNLOADS = 6;

        static const std::string COVER_CACHE_DIR;

        static CoverCache& Instance();

        // Returns CoverCacheResult. Kicks off download if Idle.
        //
        // decode_w/decode_h override the profile's render size as the decode
        // target. Callers that draw far larger than the grid does — the
        // fullscreen viewer, at up to 1080p — must pass their own size, or the
        // profile default would shrink the image to a grid tile's worth of
        // pixels. The target is part of the cache key, so the same cover at two
        // display sizes is two entries backed by one downloaded file.
        CoverCacheResult GetOrRequest(int64_t rom_id, const std::string& platform_slug, const std::string& cover_path_rel, CoverProfileType profile_type = CoverProfileType::DefaultPortrait, bool is_big = false, bool allow_download = true);
        CoverCacheResult GetOrRequest(int64_t rom_id, const std::string& platform_slug, const std::string& cover_path_rel, CoverProfileType profile_type, const std::string& variant, bool allow_download = true, int decode_w = 0, int decode_h = 0);

        // Any decoded texture for this image, whatever size it was decoded at,
        // or nullptr if none is ready.
        //
        // Decode size is part of the cache key, so the same picture at two
        // display sizes is two entries. That is right for memory and wrong for
        // the moment a viewer opens: the fullscreen viewer decodes at 1080p,
        // finds its own entry cold even though the detail panel is displaying
        // that very image, and would otherwise fall back to a DIFFERENT picture
        // until the big decode lands. Showing the same image at the size we
        // already have — softer for a moment, then replaced — is what the user
        // asked for by opening it.
        pu::sdl2::Texture FindReadyAnySize(int64_t rom_id, const std::string& platform_slug,
                                           const std::string& cover_source,
                                           const std::string& variant) const;

        // Drive the two-phase cover pipeline: hand completed downloads (or
        // disk-cache hits) to a worker thread for decoding, and upload
        // finished decodes to GPU textures (at most 2 uploads per call).
        // Must be called on the main/render thread.
        void PollCompleted();

        // Release all GPU textures (keeps disk cache).
        void Clear();

     private:
        CoverCache() = default;
        ~CoverCache() { Clear(); }

        void TouchLru(const std::string& key);
        void EvictIfNeeded();

        // Kick off entry.download_url if a slot is free, moving the entry to
        // Loading. Returns false (leaving it in Missing) when at capacity.
        bool StartDownload(CoverEntry& entry);
        // Give back the slot an entry holds, if any. Every path that drops or
        // finishes a download must go through this or the counters drift.
        void ReleaseInflight(CoverEntry& entry);

        std::unordered_map<std::string, CoverEntry> cache_;
        std::list<std::string> lru_order_;

        int inflight_downloads_ = 0;
    };

} // namespace romm::ui
