#include "AudioManager.hpp"
#include "ConfigManager.hpp"
#include <iostream>
#include <sys/stat.h>
#include <cstdio>

namespace romm::model {

    namespace {
        void EnsureAudioCacheDir(const std::string& kind) {
            mkdir("sdmc:/switch", 0777);
            mkdir("sdmc:/switch/romm-nx", 0777);
            mkdir("sdmc:/switch/romm-nx/cache", 0777);
            mkdir("sdmc:/switch/romm-nx/cache/audio", 0777);
            mkdir(("sdmc:/switch/romm-nx/cache/audio/" + kind).c_str(), 0777);
        }

        // ConfigManager stores volume as a 0-100 percentage (matches how it's
        // shown in Settings); SDL_mixer wants 0-MIX_MAX_VOLUME.
        int PercentToMixVolume(int percent) {
            if (percent < 0) percent = 0;
            if (percent > 100) percent = 100;
            return (percent * MIX_MAX_VOLUME) / 100;
        }
    }

    AudioManager& AudioManager::Instance() {
        static AudioManager instance;
        return instance;
    }

    AudioManager::~AudioManager() {}

    void AudioManager::Init() {
        int inited = Mix_Init(MIX_INIT_MP3);
        if (!(inited & MIX_INIT_MP3)) {
            // devkitPro's SDL2_mixer statically links mpg123 rather than
            // dynamically loading it, so MP3 playback can still work even if
            // this flag-registration step reports failure — keep going.
            std::cerr << "[AUDIO] Mix_Init(MIX_INIT_MP3) reported failure: " << Mix_GetError() << std::endl;
        }

        if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 2048) != 0) {
            std::cerr << "[AUDIO] Mix_OpenAudio failed: " << Mix_GetError() << std::endl;
            audio_ok = false;
            return;
        }

        Mix_HookMusicFinished(&AudioManager::OnMusicFinishedTrampoline);
        audio_ok = true;
    }

    void AudioManager::Shutdown() {
        if (!audio_ok) return;
        Mix_HookMusicFinished(nullptr);
        if (startup_music) { Mix_FreeMusic(startup_music); startup_music = nullptr; }
        if (theme_music) { Mix_FreeMusic(theme_music); theme_music = nullptr; }
        Mix_CloseAudio();
        Mix_Quit();
        audio_ok = false;
    }

    void AudioManager::OnMusicFinishedTrampoline() {
        // Runs on SDL_mixer's audio callback thread — must not touch
        // Mix_Music/Mix_PlayMusic here (SDL_mixer documents calling
        // Mix_PlayMusic from within this hook as unsafe). Just flag it;
        // Poll() handles the actual transition on the main thread.
        Instance().startup_finished.store(true, std::memory_order_release);
    }

    std::string AudioManager::CachePathFor(const std::string& kind, const std::string& key) {
        return "sdmc:/switch/romm-nx/cache/audio/" + kind + "/" + key + ".mp3";
    }

    std::string AudioManager::UrlFor(const std::string& kind, const std::string& key) {
        std::string base = ConfigManager::Instance().GetAudioBaseUrl();
        if (base.empty()) return "";
        if (base.back() != '/') base += "/";
        return base + kind + "/" + key + ".mp3";
    }

    bool AudioManager::TryLoadCached(const std::string& kind, const std::string& key, Mix_Music*& out) {
        std::string cache_path = CachePathFor(kind, key);
        struct stat st;
        if (stat(cache_path.c_str(), &st) == 0 && st.st_size > 0) {
            out = Mix_LoadMUS(cache_path.c_str());
            if (!out) {
                std::cerr << "[AUDIO] Failed to decode cached track '" << cache_path << "': " << Mix_GetError() << std::endl;
            }
            return out != nullptr;
        }

        // Not cached on disk — start a download unless one's already in
        // flight for this exact (kind,key).
        bool already_pending = pending_download && !pending_download->completed &&
                               pending_kind == kind && pending_key == key;
        if (!already_pending) {
            std::string url = UrlFor(kind, key);
            if (url.empty()) {
                std::cerr << "[AUDIO] Audio base URL not set." << std::endl;
                return false;
            }
            EnsureAudioCacheDir(kind);
            pending_kind = kind;
            pending_key = key;
            pending_tmp_path = cache_path + ".tmp";
            pending_download = HttpClient::downloadFileAsync(url, {}, pending_tmp_path);
            std::cout << "[AUDIO] Downloading " << kind << " track '" << key << "' from " << url << std::endl;
        }
        return false;
    }

    void AudioManager::PlayStartupSound() {
        if (!audio_ok) return;

        std::string key = ConfigManager::Instance().GetStartupSound();
        if (key == "none") {
            ApplyThemeAmbienceFromConfig();
            return;
        }

        Mix_Music* mus = nullptr;
        if (TryLoadCached("startup", key, mus)) {
            if (startup_music) Mix_FreeMusic(startup_music);
            startup_music = mus;
            Mix_VolumeMusic(PercentToMixVolume(ConfigManager::Instance().GetStartupVolume()));
            Mix_PlayMusic(startup_music, 0); // 0 = play once, no loop
            music_state = MusicState::PlayingStartup;
            std::cout << "[AUDIO] Playing startup sound: " << key << std::endl;
            return;
        }

        // Not ready synchronously. If a download just started for exactly
        // this key, let it ride — Poll() retries once it lands. Otherwise
        // (e.g. no base URL configured) fall through to ambience so boot
        // isn't silently stuck waiting on nothing.
        if (!(pending_download && pending_kind == "startup" && pending_key == key)) {
            ApplyThemeAmbienceFromConfig();
        }
    }

    void AudioManager::ApplyThemeAmbienceFromConfig() {
        if (!audio_ok) return;

        std::string key = ConfigManager::Instance().GetThemeSound();

        if (key == "none") {
            if (theme_music) { Mix_FreeMusic(theme_music); theme_music = nullptr; }
            loaded_theme_key = key;
            if (music_state != MusicState::PlayingStartup) {
                Mix_HaltMusic();
                music_state = MusicState::Idle;
            }
            return;
        }

        if (loaded_theme_key != key) {
            Mix_Music* mus = nullptr;
            if (TryLoadCached("theme", key, mus)) {
                if (theme_music) Mix_FreeMusic(theme_music);
                theme_music = mus;
                loaded_theme_key = key;
            } else {
                // Downloading (or couldn't start) — Poll() re-calls this once
                // the download lands. Leave whatever's currently
                // loaded/playing alone in the meantime.
                return;
            }
        }

        if (music_state == MusicState::PlayingStartup) {
            return; // don't cut off the chime; Poll() re-applies once it ends
        }

        if (theme_music) {
            Mix_VolumeMusic(PercentToMixVolume(ConfigManager::Instance().GetAmbientVolume()));
            Mix_PlayMusic(theme_music, -1); // -1 = loop forever
            music_state = MusicState::PlayingAmbience;
            std::cout << "[AUDIO] Playing menu ambience: " << key << std::endl;
        }
    }

    void AudioManager::RefreshVolume() {
        if (!audio_ok) return;
        if (music_state == MusicState::PlayingStartup) {
            Mix_VolumeMusic(PercentToMixVolume(ConfigManager::Instance().GetStartupVolume()));
        } else if (music_state == MusicState::PlayingAmbience) {
            Mix_VolumeMusic(PercentToMixVolume(ConfigManager::Instance().GetAmbientVolume()));
        }
    }

    void AudioManager::Poll() {
        if (startup_finished.exchange(false, std::memory_order_acquire)) {
            if (music_state == MusicState::PlayingStartup) {
                // Must clear this before calling in: ApplyThemeAmbienceFromConfig()
                // itself checks music_state == PlayingStartup to avoid cutting off
                // a still-playing chime, and would otherwise see its own stale
                // state and bail out, leaving no ambience playing after the
                // startup sound ends.
                music_state = MusicState::Idle;
                ApplyThemeAmbienceFromConfig();
            }
        }

        if (pending_download && pending_download->completed) {
            auto result = pending_download;
            std::string kind = pending_kind;
            std::string key = pending_key;
            std::string tmp_path = pending_tmp_path;
            pending_download.reset();

            // This completion belongs to the running batch iff it's what the
            // batch is currently waiting on — advancing the queue takes over
            // from the normal single-download dispatch below so an 11-file
            // batch never fires PlayStartupSound()/ApplyThemeAmbienceFromConfig()
            // for tracks the user hasn't actually selected yet.
            bool in_batch = batch_total > 0 && !batch_queue.empty() &&
                            batch_queue.front().first == kind && batch_queue.front().second == key;

            if (result->success) {
                std::string final_path = CachePathFor(kind, key);
                // The Switch's sdmc: fs driver, unlike POSIX rename(), does
                // not overwrite an existing destination — clear the way
                // first (a stale leftover from an earlier run, if any).
                remove(final_path.c_str());
                if (rename(tmp_path.c_str(), final_path.c_str()) == 0) {
                    std::cout << "[AUDIO] Downloaded " << kind << " track '" << key << "'" << std::endl;
                    if (in_batch) {
                        batch_queue.erase(batch_queue.begin());
                        StartNextBatchDownload();
                    } else if (kind == "startup") {
                        PlayStartupSound();
                    } else {
                        ApplyThemeAmbienceFromConfig();
                    }
                } else {
                    std::cerr << "[AUDIO] Failed to move downloaded track into cache: " << tmp_path << " -> " << final_path << std::endl;
                    remove(tmp_path.c_str());
                    if (in_batch) {
                        batch_queue.erase(batch_queue.begin());
                        StartNextBatchDownload();
                    }
                }
            } else {
                std::cerr << "[AUDIO] Download failed for " << kind << " '" << key << "': "
                          << (result->error.empty() ? result->body : result->error) << std::endl;
                remove(tmp_path.c_str());
                if (in_batch) {
                    batch_queue.erase(batch_queue.begin());
                    StartNextBatchDownload();
                }
            }
        }
    }

    bool AudioManager::IsCached(const std::string& kind, const std::string& key) const {
        struct stat st;
        return stat(CachePathFor(kind, key).c_str(), &st) == 0 && st.st_size > 0;
    }

    void AudioManager::StartNextBatchDownload() {
        if (batch_queue.empty()) {
            batch_total = 0;
            return;
        }
        const auto& next = batch_queue.front();
        Mix_Music* mus = nullptr;
        if (TryLoadCached(next.first, next.second, mus)) {
            // Already cached by the time its turn came up (e.g. someone else
            // fetched it in the meantime) — discard this probe load and move
            // straight to the next one.
            if (mus) Mix_FreeMusic(mus);
            batch_queue.erase(batch_queue.begin());
            StartNextBatchDownload();
            return;
        }
        // Not cached: TryLoadCached() has kicked off the download. Poll()
        // pops this entry and continues the queue once it lands.
    }

    void AudioManager::DownloadMissingSoundPacks(const std::vector<std::pair<std::string, std::string>>& packs) {
        if (!audio_ok || batch_total > 0) return; // already running

        batch_queue.clear();
        for (const auto& p : packs) {
            if (!IsCached(p.first, p.second)) {
                batch_queue.push_back(p);
            }
        }
        batch_total = batch_queue.size();
        if (batch_total > 0) {
            std::cout << "[AUDIO] Starting sound pack batch download: " << batch_total << " pack(s)" << std::endl;
            StartNextBatchDownload();
        }
    }

}
