#include "CoverCache.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/DataModel.hpp"
#include <iostream>
#include <cstdio>
#include <sys/stat.h>
#include <sys/types.h>
#include <SDL2/SDL_image.h>
#include <algorithm>
#include <unordered_set>

namespace romm::ui {

    const std::string CoverCache::COVER_CACHE_DIR = "sdmc:/switch/romm-nx/cache/covers/small/";

    // ---------------------------------------------------------------------------
    // Helpers
    // ---------------------------------------------------------------------------

    static uint64_t Fnv1aHash(const std::string& str) {
        uint64_t hash = 14695981039346656037ULL;
        for (char c : str) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static std::string HashString(const std::string& str) {
        uint64_t hash_val = Fnv1aHash(str);
        char buf[32];
        sprintf(buf, "%016llx", (unsigned long long)hash_val);
        return std::string(buf);
    }

    static std::string DetectImageExtension(const std::string& filepath) {
        FILE* f = fopen(filepath.c_str(), "rb");
        if (!f) return "png"; // fallback
        
        unsigned char header[12];
        size_t read_bytes = fread(header, 1, 12, f);
        fclose(f);
        
        if (read_bytes >= 4 && header[0] == 0x89 && header[1] == 0x50 && header[2] == 0x4E && header[3] == 0x47) {
            return "png";
        }
        if (read_bytes >= 3 && header[0] == 0xFF && header[1] == 0xD8 && header[2] == 0xFF) {
            return "jpg";
        }
        if (read_bytes >= 12 && header[0] == 'R' && header[1] == 'I' && header[2] == 'F' && header[3] == 'F' &&
            header[8] == 'W' && header[9] == 'E' && header[10] == 'B' && header[11] == 'P') {
            return "webp";
        }
        return "png"; // default fallback
    }

    static void GetDimensions(CoverProfileType profile_type, bool is_big, int& w, int& h) {
        if (is_big) {
            if (profile_type == CoverProfileType::PSPPortrait) {
                w = 340; h = 560;
            } else if (profile_type == CoverProfileType::NintendoDS) {
                w = 340; h = 308;
            } else if (profile_type == CoverProfileType::PS1Square) {
                w = 340; h = 340;
            } else if (profile_type == CoverProfileType::GameBoy ||
                       profile_type == CoverProfileType::GameBoyColor ||
                       profile_type == CoverProfileType::GameBoyAdvance) {
                w = 340; h = 340;
            } else {
                w = 360; h = 480;
            }
        } else {
            if (profile_type == CoverProfileType::NintendoDS ||
                profile_type == CoverProfileType::GameBoy ||
                profile_type == CoverProfileType::GameBoyColor ||
                profile_type == CoverProfileType::GameBoyAdvance) {
                w = 380; h = 344;
            } else if (profile_type == CoverProfileType::PSPPortrait) {
                w = 247; h = 378;
            } else if (profile_type == CoverProfileType::PS1Square) {
                w = 310; h = 310;
            } else {
                w = 180; h = 270;
            }
        }
    }

    static std::string SerializeKey(const CoverCacheKey& key) {
        return key.platform_slug + "|" + std::to_string(key.rom_id) + "|" + key.cover_source + "|" + key.variant + "|" + std::to_string(key.requested_width) + "x" + std::to_string(key.requested_height);
    }

    // CPU-only load+resize producing a surface. Safe to run on a worker
    // thread: touches no renderer state, only SDL_image decode and surface
    // blits.
    static SDL_Surface* LoadAndResizeSurface(const std::string& path, int target_w, int target_h) {
        SDL_Surface* orig = IMG_Load(path.c_str());
        if (!orig) {
            std::cerr << "[COVER] Failed to load image for resizing: " << path << std::endl;
            return nullptr;
        }

        float scale = std::min((float)target_w / orig->w, (float)target_h / orig->h);
        int final_w = (int)(orig->w * scale);
        int final_h = (int)(orig->h * scale);
        if (final_w <= 0) final_w = 1;
        if (final_h <= 0) final_h = 1;

        SDL_Surface* resized = SDL_CreateRGBSurfaceWithFormat(0, final_w, final_h, 32, SDL_PIXELFORMAT_RGBA32);
        if (!resized) {
            std::cerr << "[COVER] Failed to create resized surface." << std::endl;
            SDL_FreeSurface(orig);
            return nullptr;
        }

        SDL_BlitScaled(orig, nullptr, resized, nullptr);
        SDL_FreeSurface(orig);
        return resized;
    }

    // Worker-thread half of the cover pipeline: rename the fresh .tmp download
    // to its sniffed extension, then decode (and resize, for the handheld
    // small-cover profiles) to a surface. Everything here is SD-card I/O and
    // CPU decode that used to run on the render thread and stall the UI while
    // covers were first downloading.
    static void DecodeCoverTask(std::shared_ptr<CoverDecodeResult> dec, std::string path, bool resize_handheld) {
        if (path.length() >= 4 && path.compare(path.length() - 4, 4, ".tmp") == 0) {
            const std::string ext = DetectImageExtension(path);
            const std::string final_path = path.substr(0, path.length() - 4) + "." + ext;

            // The Switch's sdmc: fs driver, unlike POSIX rename(), does not
            // overwrite an existing destination — it just fails. Clear the
            // way first (a stale leftover from an earlier run, if any).
            remove(final_path.c_str());

            if (rename(path.c_str(), final_path.c_str()) == 0) {
                path = final_path;
            } else {
                std::cerr << "[COVER] Failed to rename temp file: " << path << " -> " << final_path << std::endl;
            }
        }
        dec->final_path = path;

        if (resize_handheld) {
            dec->surface = LoadAndResizeSurface(path, 380, 344);
        } else {
            dec->surface = IMG_Load(path.c_str());
            if (!dec->surface) {
                std::cerr << "[COVER] Failed to load image: " << path << std::endl;
            }
        }
        dec->completed.store(true, std::memory_order_release);
    }

    static void EnsureDirExists(const std::string& path) {
        for (size_t i = 1; i < path.size(); ++i) {
            if (path[i] == '/') {
                std::string sub = path.substr(0, i);
                mkdir(sub.c_str(), 0777);
            }
        }
        mkdir(path.c_str(), 0777);
    }

    static bool FileExists(const std::string& path) {
        FILE* f = fopen(path.c_str(), "rb");
        if (f) { fclose(f); return true; }
        return false;
    }

    // ---------------------------------------------------------------------------
    // Singleton
    // ---------------------------------------------------------------------------

    CoverCache& CoverCache::Instance() {
        static CoverCache instance;
        return instance;
    }

    // ---------------------------------------------------------------------------
    // GetOrRequest
    // ---------------------------------------------------------------------------

    CoverCacheResult CoverCache::GetOrRequest(int64_t rom_id, const std::string& platform_slug, const std::string& cover_path_rel, CoverProfileType profile_type, bool is_big, bool allow_download) {
        std::string variant = is_big ? "big" : "small";
        return GetOrRequest(rom_id, platform_slug, cover_path_rel, profile_type, variant, allow_download);
    }

    CoverCacheResult CoverCache::GetOrRequest(int64_t rom_id, const std::string& platform_slug, const std::string& cover_path_rel, CoverProfileType profile_type, const std::string& variant, bool allow_download) {
        CoverCacheResult result;
        if (rom_id <= 0 || cover_path_rel.empty()) {
            return result;
        }

        std::string norm_slug = romm::model::NormalizePlatformSlug(platform_slug);
        bool is_big = (variant == "big" || variant == "miximage_v2");
        int req_w = 0, req_h = 0;
        GetDimensions(profile_type, is_big, req_w, req_h);

        CoverCacheKey key;
        key.platform_slug = norm_slug;
        key.rom_id = rom_id;
        key.cover_source = cover_path_rel;
        key.variant = variant;
        key.requested_width = req_w;
        key.requested_height = req_h;

        result.key = key;

        std::string cache_key = SerializeKey(key);

        // Check if entry exists in cache
        auto it = cache_.find(cache_key);
        if (it != cache_.end()) {
            TouchLru(cache_key);
            auto& entry = it->second;
            if (entry.state == CoverState::FailedTransient && allow_download) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - entry.last_retry_time).count();
                if (elapsed >= 10) {
                    cache_.erase(it);
                    // Fall through to re-request
                } else {
                    result.texture = entry.texture;
                    result.state = entry.state;
                    return result;
                }
            } else {
                result.texture = entry.texture;
                result.state = entry.state;
                return result;
            }
        }

        std::string hash_str = HashString(cover_path_rel);
        std::string base_path = std::string("sdmc:/switch/romm-nx/cache/covers/") + norm_slug + "/" + variant + "/" + std::to_string(rom_id) + "_" + hash_str;
        std::string cache_path = "";
        // Only a genuine on-SD absolute path (sdmc:/...) points straight at a local
        // file. RomM cover paths are server-relative and start with "/" (e.g.
        // "/assets/romm/resources/roms/1/47/cover/small.png") — those, and http(s)
        // URLs, must go through our own disk cache keyed by rom_id+hash. (A previous
        // "|| starts-with '/'" check sent every server path down the local-file
        // branch, so the disk cache was never consulted and covers re-downloaded on
        // every launch.)
        if (cover_path_rel.rfind("sdmc:/", 0) == 0) {
            if (FileExists(cover_path_rel)) {
                cache_path = cover_path_rel;
            }
        } else {
            // Determine cache path by checking for existences of .png, .jpg, .webp
            if (FileExists(base_path + ".png")) {
                cache_path = base_path + ".png";
            } else if (FileExists(base_path + ".jpg")) {
                cache_path = base_path + ".jpg";
            } else if (FileExists(base_path + ".webp")) {
                cache_path = base_path + ".webp";
            }
        }

        CoverEntry entry;
        entry.key = key;
        entry.profile_type = profile_type;
        entry.is_big = is_big;
        entry.original_rom_id = rom_id;
        entry.platform_slug = norm_slug;
        entry.cover_path_rel = cover_path_rel;

        if (!cache_path.empty()) {
            entry.cache_path = cache_path;
            // Enqueue GPU texture upload next frame
            auto dummy = std::make_shared<HttpResult>();
            dummy->completed = true;
            dummy->success = true;
            entry.state = CoverState::Loading;
            entry.download_result = dummy;
        } else {
            // Need to download to .tmp file first
            cache_path = base_path + ".tmp";
            entry.cache_path = cache_path;
            entry.state = CoverState::Missing;
            
            if (allow_download) {
                auto& config = romm::model::ConfigManager::Instance();
                if (!config.IsValid()) {
                    entry.state = CoverState::FailedTransient;
                    entry.last_retry_time = std::chrono::steady_clock::now();
                } else {
                    std::string url;
                    std::map<std::string, std::string> headers;
                    if (cover_path_rel.rfind("http://", 0) == 0 || cover_path_rel.rfind("https://", 0) == 0) {
                        url = cover_path_rel;
                    } else {
                        std::string url_path = cover_path_rel;
                        bool is_handheld_platform = (profile_type == CoverProfileType::PSPPortrait || 
                                                     profile_type == CoverProfileType::NintendoDS ||
                                                     profile_type == CoverProfileType::GameBoy ||
                                                     profile_type == CoverProfileType::GameBoyColor ||
                                                     profile_type == CoverProfileType::GameBoyAdvance);
                        if (is_handheld_platform) {
                            size_t last_slash = url_path.find_last_of('/');
                            if (last_slash != std::string::npos) {
                                if (is_big) {
                                    url_path = url_path.substr(0, last_slash) + "/big.png";
                                } else {
                                    url_path = url_path.substr(0, last_slash) + "/small.png";
                                }
                            }
                        }
                        url = config.GetRommHost() + url_path;
                        headers["Authorization"] = "Bearer " + config.GetApiKey();
                    }

                    std::string dir_base = std::string("sdmc:/switch/romm-nx/cache/covers/") + norm_slug + "/" + variant + "/";
                    // GetOrRequest runs on the render thread; the mkdir chain
                    // is several SD-card syscalls, so only walk it once per
                    // directory per app run (main-thread only, no lock needed).
                    static std::unordered_set<std::string> ensured_dirs;
                    if (ensured_dirs.insert(dir_base).second) {
                        EnsureDirExists(dir_base);
                    }

                    entry.state = CoverState::Loading;
                    entry.download_result = HttpClient::downloadFileAsync(url, headers, cache_path);
                }
            }
        }

        lru_order_.push_front(cache_key);
        result.state = entry.state;
        result.texture = entry.texture;
        cache_[cache_key] = std::move(entry);
        EvictIfNeeded();

        return result;
    }

    // ---------------------------------------------------------------------------
    // PollCompleted — call on main/render thread each frame
    // ---------------------------------------------------------------------------

    void CoverCache::PollCompleted() {
        int promoted = 0;
        constexpr int MAX_PER_POLL = 2; // GPU uploads per frame

        for (auto& [cache_key, entry] : cache_) {
            if (entry.state != CoverState::Loading) continue;

            // Phase 2: a worker finished decoding — upload to the GPU. This is
            // the only part that must run on the render thread, and it's cheap
            // (texture creation from an already-decoded surface).
            if (entry.decode_result) {
                if (!entry.decode_result->completed.load(std::memory_order_acquire)) continue;
                if (promoted >= MAX_PER_POLL) continue; // spread uploads across frames

                auto dec = entry.decode_result;
                entry.decode_result.reset();
                if (!dec->final_path.empty()) {
                    entry.cache_path = dec->final_path;
                }

                if (dec->surface) {
                    entry.texture = SDL_CreateTextureFromSurface(pu::ui::render::GetMainRenderer(), dec->surface);
                    // dec's destructor frees the surface once we're done here.
                }

                if (entry.texture) {
                    SDL_SetTextureBlendMode(entry.texture, SDL_BLENDMODE_BLEND);
                    entry.state = CoverState::Ready;
                    entry.decode_retry_count = 0;
                    s32 w = pu::ui::render::GetTextureWidth(entry.texture);
                    s32 h = pu::ui::render::GetTextureHeight(entry.texture);
                    std::cout << "[COVER] decoded platform=" << entry.key.platform_slug
                              << " rom_id=" << entry.key.rom_id
                              << " quality=" << entry.key.variant
                              << " size=" << w << "x" << h << std::endl;
                } else {
                    remove(entry.cache_path.c_str()); // delete corrupt cache file
                    if (entry.decode_retry_count < 1) {
                        entry.decode_retry_count++;
                        entry.state = CoverState::Missing; // Allow one retry download
                        std::cerr << "[cover] Decode failed, retrying download for: " << entry.cache_path << std::endl;
                    } else {
                        entry.state = CoverState::FailedPermanent;
                        std::cerr << "[cover] Decode failed permanently: " << entry.cache_path << std::endl;
                    }
                }

                promoted++;
                continue;
            }

            // Phase 1: download (or disk-cache hit) is ready — hand the file
            // off to a worker thread for the rename + decode. Not counted
            // against the upload budget; enqueueing is trivial.
            if (!entry.download_result || !entry.download_result->completed) continue;

            if (entry.download_result->success) {
                const bool resize_handheld = !entry.is_big &&
                                             (entry.profile_type == CoverProfileType::NintendoDS ||
                                              entry.profile_type == CoverProfileType::GameBoy ||
                                              entry.profile_type == CoverProfileType::GameBoyColor ||
                                              entry.profile_type == CoverProfileType::GameBoyAdvance);
                auto dec = std::make_shared<CoverDecodeResult>();
                entry.decode_result = dec;
                const std::string path = entry.cache_path;
                HttpClient::runAsync([dec, path, resize_handheld]() {
                    DecodeCoverTask(dec, path, resize_handheld);
                });
            } else {
                remove(entry.cache_path.c_str());
                long code = entry.download_result->statusCode;
                if (code == 404 || code == 410) {
                    entry.state = CoverState::FailedPermanent;
                } else if (code == 400) {
                    entry.state = CoverState::FailedTransient;
                    entry.last_retry_time = std::chrono::steady_clock::now();
                    std::cerr << "[HTTP 400] Bad Request for key=" << cache_key << std::endl;
                } else {
                    entry.state = CoverState::FailedTransient;
                    entry.last_retry_time = std::chrono::steady_clock::now();
                }
                std::cerr << "[cover] Download failed (HTTP " << code << ") for cache_key=" << cache_key << std::endl;
            }

            entry.download_result.reset();
        }
    }

    // ---------------------------------------------------------------------------
    // Clear — release GPU textures (disk cache stays)
    // ---------------------------------------------------------------------------

    void CoverCache::Clear() {
        for (auto& [key, entry] : cache_) {
            if (entry.texture) {
                pu::ui::render::DeleteTexture(entry.texture);
                entry.texture = nullptr;
            }
        }
        cache_.clear();
        lru_order_.clear();
    }

    // ---------------------------------------------------------------------------
    // LRU helpers
    // ---------------------------------------------------------------------------

    void CoverCache::TouchLru(const std::string& key) {
        lru_order_.remove(key);
        lru_order_.push_front(key);
    }

    void CoverCache::EvictIfNeeded() {
        while ((int)cache_.size() > MAX_ENTRIES && !lru_order_.empty()) {
            std::string lru_key = lru_order_.back();
            lru_order_.pop_back();
            auto it = cache_.find(lru_key);
            if (it != cache_.end()) {
                if (it->second.texture) {
                    pu::ui::render::DeleteTexture(it->second.texture);
                }
                cache_.erase(it);
            }
        }
    }

    int CoverCache::GetPendingBigDownloadsCount() const {
        int count = 0;
        for (const auto& [key, entry] : cache_) {
            if (entry.is_big && entry.state == CoverState::Loading) {
                if (entry.download_result && !entry.download_result->completed) {
                    count++;
                }
            }
        }
        return count;
    }

} // namespace romm::ui
