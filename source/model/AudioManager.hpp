#pragma once

#include <SDL2/SDL_mixer.h>
#include <string>
#include <atomic>
#include <memory>
#include <utility>
#include <vector>
#include "../navigation/HttpClient.hpp"

namespace romm::model {

    // Boot chime ("Startup Sound") + looping background track ("Menu
    // Ambience") for the app's Theme settings. Tracks are NOT bundled in the
    // NRO — they're fetched on demand from ConfigManager's audio base URL
    // (expected to serve <base>/startup/<key>.mp3 and <base>/theme/<key>.mp3,
    // not currently exposed as a Settings row) and cached to
    // sdmc:/switch/romm-nx/cache/audio/, mirroring how CoverCache handles
    // cover art: first play costs a download, every play after is instant
    // from the SD card.
    class AudioManager {
    public:
        static AudioManager& Instance();

        // Call once after the renderer (and therefore SDL_INIT_AUDIO) is up.
        void Init();
        // Call once at shutdown, before tearing down SDL.
        void Shutdown();

        // Plays the configured startup track once (downloading it first if
        // not already cached). If none configured, download fails to start,
        // or the file fails to decode, falls through to ambience instead of
        // leaving boot silent.
        void PlayStartupSound();

        // (Re)loads and plays/stops the configured ambience track to match
        // ConfigManager's current value. Downloads it first if not cached.
        // Safe to call repeatedly — skips redundant work if the selection
        // hasn't changed. Won't cut off a still-playing startup chime;
        // Poll() re-applies once it finishes.
        void ApplyThemeAmbienceFromConfig();

        // Re-applies whichever volume (Startup or Ambient) matches what's
        // currently playing. Call after the user adjusts a Settings volume
        // slider so a change is audible immediately if that track is live —
        // otherwise it just takes effect the next time that track starts.
        void RefreshVolume();

        // Must be called once per frame from the main thread: drains the
        // music-finished signal (fires from SDL_mixer's audio callback
        // thread, where starting new playback is unsafe) and checks
        // in-flight downloads.
        void Poll();

        // True if (kind,key) is already on the SD cache — checking costs a
        // stat(), not a download, so Settings can use it to decide whether
        // "Download sound pack?" even needs to be asked.
        bool IsCached(const std::string& kind, const std::string& key) const;

        // Downloads every (kind,key) pack in `packs` that isn't already
        // cached, one at a time (reusing the single in-flight download
        // slot). Runs silently — no playback/ambience side effects fire for
        // packs landing mid-batch, so fetching the whole catalogue doesn't
        // interrupt whatever's currently playing. No-op if a batch is
        // already running (check IsBatchDownloading() first).
        void DownloadMissingSoundPacks(const std::vector<std::pair<std::string, std::string>>& packs);

        // Batch progress for Settings UI. GetBatchTotal() == 0 means no
        // batch is running (either none started, or the last one finished).
        bool IsBatchDownloading() const { return batch_total > 0; }
        size_t GetBatchTotal() const { return batch_total; }
        size_t GetBatchRemaining() const { return batch_queue.size(); }

    private:
        AudioManager() = default;
        ~AudioManager();
        AudioManager(const AudioManager&) = delete;
        AudioManager& operator=(const AudioManager&) = delete;

        enum class MusicState { Idle, PlayingStartup, PlayingAmbience };

        static void OnMusicFinishedTrampoline();
        static std::string CachePathFor(const std::string& kind, const std::string& key);
        static std::string UrlFor(const std::string& kind, const std::string& key);

        // Loads (kind,key) from the SD cache into `out` if present on disk.
        // If not present, starts (or lets ride) a background download and
        // returns false — the caller's request will be retried from Poll()
        // once that download lands.
        bool TryLoadCached(const std::string& kind, const std::string& key, Mix_Music*& out);

        // Kicks off (or continues) fetching batch_queue.front(), advancing
        // past any entry that's already cached by the time its turn comes up.
        void StartNextBatchDownload();

        bool audio_ok = false;
        Mix_Music* startup_music = nullptr;
        Mix_Music* theme_music = nullptr;
        std::string loaded_theme_key = "\x01"; // sentinel: force first load
        MusicState music_state = MusicState::Idle;
        std::atomic<bool> startup_finished{false};

        // Single in-flight download slot. A new request for a different
        // (kind,key) simply stops tracking the old one — its background
        // download (if still running) completes harmlessly into an orphaned
        // .tmp file that gets overwritten next time that key is requested.
        std::shared_ptr<HttpResult> pending_download;
        std::string pending_kind;
        std::string pending_key;
        std::string pending_tmp_path;

        // Packs still to fetch for the in-progress "Download Sound Pack"
        // batch (front() is the one currently downloading, or about to
        // start). Popped only once Poll() confirms that entry's completion.
        std::vector<std::pair<std::string, std::string>> batch_queue;
        size_t batch_total = 0;
    };

}
