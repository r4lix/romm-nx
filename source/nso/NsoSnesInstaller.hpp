#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

#include "NsoSnesDb.hpp"

// Orchestrates the experimental "RomM ROM -> SNES Switch Online" pipeline.
//
// Everything runs on a detached worker thread; the UI only ever reads snapshots
// through the accessors here. Nothing under /atmosphere is touched until every
// artefact has been produced and validated in staging, and the active database
// is always the last file replaced.
namespace romm::nso {

    enum class NsoStepStatus {
        Pending,
        Running,
        Done,
        Failed,
        Skipped
    };

    struct NsoStep {
        std::string name;
        NsoStepStatus status = NsoStepStatus::Pending;
        std::string detail;
    };

    enum class NsoPipelineState {
        Idle,
        Running,
        Success,
        Failed
    };

    // Error classes the UI distinguishes. Kept explicit so a failure message can
    // say which stage of the pipeline gave up rather than just "it failed".
    enum class NsoErrorKind {
        None,
        NotDetected,
        SourceDownload,
        InvalidRom,
        UnsupportedRom,
        SfromGeneration,
        CoverDecode,
        ImageConversion,
        MalformedDatabase,
        DuplicateEntry,
        InsufficientSpace,
        FileWrite,
        Validation,
        Rollback
    };

    struct NsoInstallRequest {
        int rom_id = 0;
        int file_id = 0;
        std::string title;          // display title from RomM
        std::string rom_filename;   // name the RomM content endpoint expects
        std::string cover_url;      // absolute, already resolved
        // Set when the ROM is already on the SD card (the download flow has
        // just fetched it). The pipeline then copies it into staging instead of
        // downloading it again — which also means a retry never re-fetches a
        // multi-megabyte file it already has.
        std::string local_rom_path;
    };

    struct NsoInstallOutcome {
        bool success = false;
        NsoErrorKind error_kind = NsoErrorKind::None;
        std::string error;
        std::string code; // the S-#### slot it landed in, on success
    };

    // Whether trying the same ROM again could plausibly give a different
    // answer. Only the fetch-and-write failures qualify: a ROM rejected for its
    // mapping, a malformed database or a failed validation is deterministic, so
    // retrying it just re-downloads the ROM to reach the identical conclusion.
    bool IsRetryableFailure(NsoErrorKind kind);

    // Root of everything romm-nx writes for this feature.
    constexpr const char* kNsoRoot = "sdmc:/switch/romm-nx/nso-snes";
    constexpr const char* kStagingDir = "sdmc:/switch/romm-nx/nso-snes/staging";
    constexpr const char* kBackupsDir = "sdmc:/switch/romm-nx/nso-snes/backups";
    constexpr const char* kInjectedIndex = "sdmc:/switch/romm-nx/nso-snes/injected.txt";

    // Whether romm-nx knows how to build Switch Online files for a platform.
    // SNES only today: every other NSO app needs its own ROM container, cover
    // format and per-platform database fields established from real reference
    // files before it can be offered.
    bool PlatformSupportsInjection(const std::string& canonical_platform_id);

    class NsoSnesInstaller {
    public:
        static NsoSnesInstaller& Instance();

        // Re-runs detection on the calling thread (cheap: a directory scan plus
        // one JSON parse) and caches the result.
        void RefreshDetection();
        NsoSnesInstall GetDetection() const;

        void StartInstall(const NsoInstallRequest& request);
        void StartRestore();

        // Runs the pipeline on the CALLING thread and returns when it is done.
        // The download worker uses this: its queue is already serialized, so
        // injections serialize with it for free — no second queue, and no way
        // for a batch to outrun a single-flight installer. Blocks while a
        // manual install from the settings screen is in flight.
        NsoInstallOutcome InstallSync(const NsoInstallRequest& request);

        // Removes a previously injected game: its database entry, its per-title
        // strings and its asset folder. Called when the ROM is uninstalled, so
        // the two stay in step instead of leaving an entry pointing at files
        // that are gone. A game romm-nx never injected is a no-op, not an error.
        //
        // `title` is only a fallback, for index lines an earlier build wrote
        // without a rom_id (those load back as 0 and can never match one).
        // It is matched exactly, and only against those rom_id-less entries, so
        // a correctly recorded game is never removed by a title coincidence.
        NsoInstallOutcome UninstallSync(int rom_id, const std::string& title = "");

        bool IsBusy() const { return busy.load(); }
        NsoPipelineState GetState() const;
        NsoErrorKind GetErrorKind() const;
        std::string GetError() const;
        std::string GetSummary() const;
        std::vector<NsoStep> GetSteps() const;

        // Newest backup directory, or "" when none exists.
        std::string LatestBackupPath() const;
        bool HasBackup() const { return !LatestBackupPath().empty(); }

        const char* LogPath() const;

    private:
        NsoSnesInstaller() = default;

        void RunInstall(NsoInstallRequest request);
        void RunRestore();

        // The pipeline runs on a pthread with an explicitly sized stack rather
        // than a std::thread. libnx's default thread stack is small, and this
        // worker walks through libpng, SDL surface scaling and a recursive JSON
        // scanner; HttpClient makes the same choice for the same reason. Also,
        // with -fno-exceptions a std::thread that fails to start calls
        // std::terminate, whereas pthread_create just returns an error we can
        // report on screen.
        bool SpawnWorker(bool restore, const NsoInstallRequest& request);
        static void* ThreadEntry(void* arg);

        void ResetSteps(const std::vector<std::string>& names);
        void BeginStep(size_t index);
        void FinishStep(size_t index, const std::string& detail);
        void SkipStep(size_t index, const std::string& detail);
        void FailStep(size_t index, NsoErrorKind kind, const std::string& detail);
        void SetSummary(const std::string& text);

        mutable std::mutex mutex;
        std::atomic<bool> busy{false};
        NsoPipelineState state = NsoPipelineState::Idle;
        NsoErrorKind error_kind = NsoErrorKind::None;
        std::string error_message;
        std::string summary;
        std::vector<NsoStep> steps;
        NsoSnesInstall detection;
        // The S-#### slot the last successful run used, for the caller's report.
        std::string install_code;
    };

}
