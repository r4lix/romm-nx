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

    // Which Switch Online app a job targets. The pipeline itself is one piece of
    // code — staging, backup, atomic write order and rollback are identical —
    // and everything that genuinely differs (ROM container, cover geometry,
    // code shape, entry fields, string keys, install root) is carried by the
    // profile this selects. See docs/nso-nes-format.md for the NES side.
    enum class NsoPlatform {
        Snes,
        Nes,
        GameBoy, // one app for both Game Boy and Game Boy Color
        Gba,
        N64
    };

    // Maps a canonical romm-nx platform id ("snes", "nes") onto the enum.
    // Returns false for anything not injectable.
    bool NsoPlatformForId(const std::string& canonical_platform_id, NsoPlatform& out);

    // Display name for logs and on-screen messages ("SNES Online").
    const char* NsoPlatformName(NsoPlatform platform);

    struct NsoInstallRequest {
        NsoPlatform platform = NsoPlatform::Snes;
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

    // What the pipeline worker was started to do. Was a bool `restore`, which
    // stopped being expressive the moment there were three jobs.
    enum class NsoJobKind {
        Install,
        Restore,
        UninstallAll
    };

    // Whether trying the same ROM again could plausibly give a different
    // answer. Only the fetch-and-write failures qualify: a ROM rejected for its
    // mapping, a malformed database or a failed validation is deterministic, so
    // retrying it just re-downloads the ROM to reach the identical conclusion.
    bool IsRetryableFailure(NsoErrorKind kind);

    // Root of everything romm-nx writes for the SNES feature. NES gets its own
    // tree (`nso-nes`) from the platform profile, so the two platforms' staging,
    // backups and injected-game indexes can never be confused for each other.
    constexpr const char* kNsoRoot = "sdmc:/switch/romm-nx/nso-snes";
    constexpr const char* kStagingDir = "sdmc:/switch/romm-nx/nso-snes/staging";
    constexpr const char* kBackupsDir = "sdmc:/switch/romm-nx/nso-snes/backups";
    constexpr const char* kInjectedIndex = "sdmc:/switch/romm-nx/nso-snes/injected.txt";

    // Whether a platform's injection support is trusted yet.
    //
    // Nintendo 64 is not, and says so rather than being presented as finished:
    // a title only runs if a working MetaPack exists for it, and romm-nx can
    // only derive one when the game's idle loop is an unconditional self-branch
    // in the boot segment. Ocarina of Time gets one; Doom 64 and Resident Evil 2
    // do not, and black-screen. See docs/nso-n64-format.md.
    //
    // Deliberately one predicate rather than a flag spread across the UI, so
    // clearing it when N64 is trusted is a one-line change.
    bool IsNsoPlatformUnstable(NsoPlatform platform);

    // Whether romm-nx knows how to build Switch Online files for a platform.
    // SNES, NES, Game Boy / Game Boy Color, Game Boy Advance and Nintendo 64
    // today; Mega Drive and Virtual Boy still need their ROM container, cover
    // format and per-platform database fields established from real reference
    // files before they can be offered.
    bool PlatformSupportsInjection(const std::string& canonical_platform_id);

    class NsoSnesInstaller {
    public:
        static NsoSnesInstaller& Instance();

        // Re-runs detection on the calling thread (cheap: a directory scan plus
        // one JSON parse) and caches the result. Cached per platform, so the
        // settings screen's SNES view is not overwritten by an NES download
        // running in the background.
        void RefreshDetection(NsoPlatform platform = NsoPlatform::Snes);
        NsoSnesInstall GetDetection(NsoPlatform platform = NsoPlatform::Snes) const;

        void StartInstall(const NsoInstallRequest& request);
        void StartRestore(NsoPlatform platform = NsoPlatform::Snes);
        // Removes every game romm-nx injected, each with its own backup, on the
        // pipeline worker. The progress page is the only report — this can be
        // a hundred megabytes of backup copying, so it must not run on the UI
        // thread.
        void StartUninstallAll(NsoPlatform platform = NsoPlatform::Snes);

        // How many games romm-nx currently has injected, from its own index.
        // Reads a small file, so it is fine to call while drawing a settings row.
        size_t InjectedGameCount(NsoPlatform platform = NsoPlatform::Snes) const;

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
        NsoInstallOutcome UninstallSync(int rom_id, const std::string& title = "",
                                        NsoPlatform platform = NsoPlatform::Snes);

        bool IsBusy() const { return busy.load(); }
        NsoPipelineState GetState() const;
        NsoErrorKind GetErrorKind() const;
        std::string GetError() const;
        std::string GetSummary() const;
        std::vector<NsoStep> GetSteps() const;

        // Newest backup directory for a platform, or "" when none exists.
        std::string LatestBackupPath(NsoPlatform platform = NsoPlatform::Snes) const;
        bool HasBackup(NsoPlatform platform = NsoPlatform::Snes) const {
            return !LatestBackupPath(platform).empty();
        }

        const char* LogPath() const;

    private:
        NsoSnesInstaller() = default;

        void RunInstall(NsoInstallRequest request);
        void RunRestore(NsoPlatform platform);
        void RunUninstallAll(NsoPlatform platform);

        // One game's removal: database entry, per-title strings, asset folder
        // and index line, backed up first. Assumes the caller already holds
        // `busy` and has a log session open, so the bulk path can hold both for
        // a whole run without deadlocking against itself.
        bool RemoveInjectedEntry(NsoPlatform platform,
                                 const std::string& hash, const std::string& code,
                                 const std::string& title, std::string& out_error);

        // The pipeline runs on a pthread with an explicitly sized stack rather
        // than a std::thread. libnx's default thread stack is small, and this
        // worker walks through libpng, SDL surface scaling and a recursive JSON
        // scanner; HttpClient makes the same choice for the same reason. Also,
        // with -fno-exceptions a std::thread that fails to start calls
        // std::terminate, whereas pthread_create just returns an error we can
        // report on screen.
        bool SpawnWorker(NsoJobKind kind, const NsoInstallRequest& request);
        static void* ThreadEntry(void* arg);

        // Index into the per-platform arrays below.
        static size_t PlatformSlot(NsoPlatform platform) {
            switch (platform) {
                case NsoPlatform::Nes:     return 1u;
                case NsoPlatform::GameBoy: return 2u;
                case NsoPlatform::Gba:     return 3u;
                case NsoPlatform::N64:     return 4u;
                default:                   return 0u;
            }
        }

        void ResetSteps(const std::vector<std::string>& names);
        void BeginStep(size_t index);
        void UpdateStep(size_t index, const std::string& detail);
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
        NsoSnesInstall detection[5]; // SNES, NES, GB, GBA, N64 — see PlatformSlot
        // The S-#### slot the last successful run used, for the caller's report.
        std::string install_code;
    };

}
