#pragma once

#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <deque>
#include <vector>
#include <unordered_map>
#include "DataModel.hpp"
#include <map>

namespace romm::model {

    struct InstalledIndexEntry {
        std::string platform_slug;
        int rom_id = 0;
        std::string title;
        std::string original_filename;
        std::string install_path;
        std::string cover_path;
        long long size = 0;
        std::string installed_at;
        // The game lives in the Switch Online app and nowhere else — the
        // downloaded ROM was deleted once the injection succeeded. The entry
        // stays so the Installed screen can still list it and, more
        // importantly, so uninstalling from there still finds the rom_id it
        // needs to remove the injected title.
        bool switch_online_only = false;
    };

    enum class DownloadState {
        Idle,
        Queued,
        Preparing,
        DownloadingGame,
        DownloadingCover,
        SyncingCover,
        // Building and installing this game's Switch Online files. Runs inline
        // on the download worker, so a marked batch injects in queue order and
        // can never outrun the single-flight installer.
        Injecting,
        Completed,
        Failed,
        Cancelled
    };

    // One physical file to fetch as part of a task. A single-disc game has one of
    // these; a multi-disc game has several, all under the same task/rom_id.
    struct DownloadFile {
        int file_id = 0;
        std::string filename;          // sanitized, as written to disk
        std::string original_filename; // as sent to the RomM content endpoint
        std::string final_path;
        std::string part_path;
        long long total_bytes = 0;
    };

    struct DownloadTask {
        int rom_id = 0;
        int file_id = 0;
        std::string filename;
        std::string original_filename;
        std::string final_path;
        std::string part_path;

        // Every physical file this task installs. Always populated (size 1 for a
        // single-disc game). The scalar fields above mirror whichever file the
        // worker is currently processing, for the cover/index/UI code that still
        // reasons about one representative file.
        std::vector<DownloadFile> files;

        std::string cover_url;
        std::string cover_cache_path;
        std::string cover_path_rel;
        std::string title;
        std::string platform_slug;

        // Also install into the matching Switch Online app once the ROM lands.
        // Decided at enqueue time from the per-platform setting (and the Ask
        // prompt), never re-read later, so changing the setting mid-queue can't
        // retroactively change what a queued job does.
        bool inject_nso = false;
        // Delete the downloaded ROM once the injection has succeeded, so the
        // game is not stored twice. Deliberately NOT named inject_only — that
        // is the field right below and means something else entirely.
        bool discard_rom_after_inject = false;
        // Requeued injection retry: the ROM is already on the SD card, so only
        // the injection is redone.
        bool inject_only = false;
        int inject_attempts = 0;

        long long total_bytes = 0;
        std::atomic<long long> downloaded_bytes{0};
        std::atomic<size_t> download_speed_bps{0}; // Bytes per second
        std::atomic<DownloadState> state{DownloadState::Queued};
        std::string error_message;

        DownloadTask() = default;
        DownloadTask(const DownloadTask& other) {
            rom_id = other.rom_id;
            file_id = other.file_id;
            filename = other.filename;
            original_filename = other.original_filename;
            final_path = other.final_path;
            part_path = other.part_path;
            files = other.files;
            cover_url = other.cover_url;
            cover_cache_path = other.cover_cache_path;
            cover_path_rel = other.cover_path_rel;
            title = other.title;
            platform_slug = other.platform_slug;
            inject_nso = other.inject_nso;
            discard_rom_after_inject = other.discard_rom_after_inject;
            inject_only = other.inject_only;
            inject_attempts = other.inject_attempts;
            total_bytes = other.total_bytes;
            downloaded_bytes.store(other.downloaded_bytes.load());
            download_speed_bps.store(other.download_speed_bps.load());
            state.store(other.state.load());
            error_message = other.error_message;
        }
        DownloadTask& operator=(const DownloadTask& other) {
            rom_id = other.rom_id;
            file_id = other.file_id;
            filename = other.filename;
            original_filename = other.original_filename;
            final_path = other.final_path;
            part_path = other.part_path;
            files = other.files;
            cover_url = other.cover_url;
            cover_cache_path = other.cover_cache_path;
            cover_path_rel = other.cover_path_rel;
            title = other.title;
            platform_slug = other.platform_slug;
            inject_nso = other.inject_nso;
            discard_rom_after_inject = other.discard_rom_after_inject;
            inject_only = other.inject_only;
            inject_attempts = other.inject_attempts;
            total_bytes = other.total_bytes;
            downloaded_bytes.store(other.downloaded_bytes.load());
            download_speed_bps.store(other.download_speed_bps.load());
            state.store(other.state.load());
            error_message = other.error_message;
            return *this;
        }
    };

    // What the caller has already decided about Switch Online injection for
    // this one enqueue. An explicit parameter rather than shared state:
    // inject_nso is fixed at enqueue time and never re-read, so a resolved Ask
    // has to arrive with the call, and a caller that never asked (bulk
    // download) must not be able to accidentally inherit someone else's answer.
    // What the user (or the per-platform setting) decided this download should
    // produce. Mirrors NsoInjectionMode minus Ask, which is resolved before a
    // task is ever enqueued.
    enum class InjectChoice {
        // Read the per-platform setting. Both and InjectOnly inject; RomOnly and
        // an unresolved Ask do not.
        UseSetting,
        RomOnly,
        InjectOnly, // inject, then delete the downloaded ROM
        Both
    };

    class DownloadManager {
    public:
        static DownloadManager& Instance();

        // Queue control
        void EnqueueDownload(const GameDetail& detail, const std::string& platform_slug, const std::string& title,
                             InjectChoice inject = InjectChoice::UseSetting);
        void RemoveFromQueue(int rom_id);
        void ClearCompleted();
        void RetryFailed(int rom_id);
        
        // Single Cancel for backward compatibility (cancels current active task)
        void CancelDownload();

        // State snapshots
        DownloadTask GetActiveDownloadSnapshot(); // Returns a copy of the active task, or empty task if none
        bool HasActiveDownload();
        std::vector<DownloadTask> GetQueueSnapshot(); // Returns a copy of the whole queue
        DownloadTask GetTaskSnapshot(int rom_id); // Returns specific task if exists

        bool IsCancelRequested() const { return cancel_requested.load(); }
        void SetCurrentDownloadedBytes(long long bytes);

        // Utilities
        std::string SanitizeFilename(const std::string& filename);
        std::string ResolveGameInstallPath(const std::string& platform_slug, const std::string& filename);
        bool IsGameInstalled(const std::string& platform_slug, const std::string& filename);
        void UninstallGame(const std::string& platform_slug, const std::string& filename, const std::string& cover_path);

        // The filename whose on-disk presence represents this ROM being installed.
        // For a multi-disc PS1 set that's the root ".m3u"; otherwise the first file.
        // Used by the detail view (install badge) and uninstall so both agree with
        // how EnqueueDownload/the worker lay a multi-disc game out on disk.
        std::string InstallIdentityFilename(const std::string& platform_slug,
                                            const std::vector<RomFileEntry>& files,
                                            const std::string& fallback);

        // Install state cache (avoids hitting the filesystem every frame)
        // Call after selection changes; safe to call from any thread.
        void InvalidateInstallCache();
        // Warm the cache for a specific game (used by the detail view polling loop).
        bool GetCachedInstallState(const std::string& platform_slug, const std::string& filename);
        void RefreshInstallCache(const std::string& platform_slug, const std::string& filename);

        // Index management
        void LoadInstalledIndex();
        void SaveInstalledIndex();
        void ReconcileInstalledIndex();
        std::map<std::string, InstalledIndexEntry> GetInstalledIndex() const;

        ~DownloadManager();

    private:
        DownloadManager() = default;

        void DownloadWorker();

        // Result of fetching one physical file within a task.
        struct FileDownloadOutcome {
            bool success = false;
            bool cancelled = false;
            long long final_size = 0;
            std::string error_message;
        };
        // Downloads a single file (df) belonging to task. base_bytes is the number
        // of bytes already completed by earlier files in the same task, so the
        // shared progress counter reads cumulatively across a multi-disc set.
        FileDownloadOutcome DownloadFilePhysical(int rom_id, DownloadTask& task,
                                                 const DownloadFile& df, long long base_bytes);

        std::string NormalizePath(const std::string& path);
        bool IsInsideAllowedRoot(const std::string& path, const std::string& root);

        // The download worker never holds a raw DownloadTask* across a long-running
        // operation: std::deque::erase() on a non-front/back element invalidates
        // references/pointers to EVERY element in the deque (not just the erased
        // one), and RemoveFromQueue/ClearCompleted/UninstallGame can all erase other
        // queue entries while a download is in flight on the worker thread. These
        // helpers re-look-up the task by rom_id under task_mutex each time instead.
        void SetTaskState(int rom_id, DownloadState state);
        void FailTask(int rom_id, DownloadState state, const std::string& error_message);

        std::deque<DownloadTask> download_queue;
        std::thread worker_thread;
        std::mutex task_mutex;
        std::mutex index_mutex;
        std::atomic<bool> cancel_requested{false};
        std::atomic<bool> worker_running{false};
        // Bytes completed by earlier files of the in-flight multi-disc task; added
        // to curl's per-file dlnow so the shared progress counter stays cumulative.
        std::atomic<long long> active_base_bytes{0};
        
        std::map<std::string, InstalledIndexEntry> installed_index;

        // Install state cache: keyed by "slug|filename"
        std::unordered_map<std::string, bool> install_cache;
        std::mutex install_cache_mutex;
    };

}
