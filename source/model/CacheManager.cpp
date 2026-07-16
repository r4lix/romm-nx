#include "CacheManager.hpp"
#include "ConfigManager.hpp"
#include "../navigation/HttpClient.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include <ctime>

namespace romm::model {

    struct CacheFileEntry {
        std::string path;
        long long size;
        time_t mtime;
    };

    namespace {
        void collectFiles(const std::string& path, std::vector<CacheFileEntry>& out_files) {
            DIR* dir = opendir(path.c_str());
            if (!dir) return;

            struct dirent* ent;
            while ((ent = readdir(dir)) != NULL) {
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;

                std::string full_path = path;
                if (full_path.back() != '/') full_path += "/";
                full_path += name;

                struct stat st;
                if (stat(full_path.c_str(), &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        collectFiles(full_path, out_files);
                    } else if (S_ISREG(st.st_mode)) {
                        CacheFileEntry entry;
                        entry.path = full_path;
                        entry.size = st.st_size;
                        entry.mtime = st.st_mtime;
                        out_files.push_back(entry);
                    }
                }
            }
            closedir(dir);
        }

        void clearDir(const std::string& path, long long& deleted_bytes, int& deleted_count) {
            DIR* dir = opendir(path.c_str());
            if (!dir) return;

            struct dirent* ent;
            while ((ent = readdir(dir)) != NULL) {
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;

                std::string full_path = path;
                if (full_path.back() != '/') full_path += "/";
                full_path += name;

                struct stat st;
                if (stat(full_path.c_str(), &st) == 0) {
                    if (S_ISDIR(st.st_mode)) {
                        clearDir(full_path, deleted_bytes, deleted_count);
                        rmdir(full_path.c_str());
                    } else if (S_ISREG(st.st_mode)) {
                        deleted_bytes += st.st_size;
                        if (unlink(full_path.c_str()) == 0) {
                            deleted_count++;
                        }
                    }
                }
            }
            closedir(dir);
        }
    }

    CacheManager& CacheManager::Instance() {
        static CacheManager inst;
        return inst;
    }

    CacheStats CacheManager::CalculateSize() {
        std::vector<CacheFileEntry> files;
        collectFiles("sdmc:/switch/romm-nx/cache", files);

        CacheStats stats;
        for (const auto& f : files) {
            stats.total_size += f.size;
            stats.total_count++;
            
            if (f.path.find("cache/covers/") != std::string::npos) {
                stats.cover_size += f.size;
                stats.cover_count++;
            }
        }

        std::cout << "[CACHE] Calculating cache size" << std::endl;
        std::cout << "[CACHE] Total size=" << stats.total_size << std::endl;
        return stats;
    }

    bool CacheManager::ClearCache(long long& out_bytes, int& out_count) {
        std::cout << "[CACHE] Clear requested type=all" << std::endl;
        out_bytes = 0;
        out_count = 0;
        clearDir("sdmc:/switch/romm-nx/cache", out_bytes, out_count);
        std::cout << "[CACHE] Clear completed files=" << out_count << " bytes=" << out_bytes << std::endl;
        return true;
    }

    void CacheManager::AutoPrune() {
        auto& config = ConfigManager::Instance();
        bool enabled = config.IsAutoClearEnabled();
        int max_mb = config.GetMaxSizeMb();
        int max_days = config.GetMaxAgeDays();

        std::cout << "[CACHE] Auto prune enabled=" << (enabled ? "true" : "false")
                  << " max_mb=" << max_mb << " max_days=" << max_days << std::endl;

        if (!enabled) return;

        HttpClient::runAsync([=]() {
            std::vector<CacheFileEntry> files;
            collectFiles("sdmc:/switch/romm-nx/cache", files);

            time_t now = std::time(nullptr);
            long long deleted_bytes = 0;
            int deleted_count = 0;

            std::vector<CacheFileEntry> remaining;

            for (const auto& f : files) {
                double diff_sec = std::difftime(now, f.mtime);
                double max_sec = max_days * 24.0 * 3600.0;
                if (diff_sec > max_sec) {
                    if (unlink(f.path.c_str()) == 0) {
                        deleted_bytes += f.size;
                        deleted_count++;
                    }
                } else {
                    remaining.push_back(f);
                }
            }

            long long current_size = 0;
            for (const auto& f : remaining) {
                current_size += f.size;
            }

            long long limit_bytes = (long long)max_mb * 1024 * 1024;
            if (current_size > limit_bytes) {
                std::sort(remaining.begin(), remaining.end(), [](const CacheFileEntry& a, const CacheFileEntry& b) {
                    return a.mtime < b.mtime;
                });

                for (const auto& f : remaining) {
                    if (current_size <= limit_bytes) break;
                    if (unlink(f.path.c_str()) == 0) {
                        deleted_bytes += f.size;
                        deleted_count++;
                        current_size -= f.size;
                    }
                }
            }

            if (deleted_count > 0) {
                std::cout << "[CACHE] Pruned files=" << deleted_count << " bytes=" << deleted_bytes << std::endl;
            }
        });
    }

}
