#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <memory>
#include "../navigation/HttpClient.hpp"

namespace romm::model {

    enum class UpdateState {
        Idle,
        Checking,
        NoUpdateAvailable,
        UpdateAvailable,
        Downloading,
        Verifying,
        Installing,
        InstalledRestartRequired,
        Error
    };

    struct AppManifest {
        int schema = 0;
        std::string app;
        std::string channel;
        std::string version;
        int version_code = 0;
        std::string published_at;
        bool mandatory = false;
        
        struct NroInfo {
            std::string url;
            long long size = 0;
            std::string sha256;
        } nro;

        std::vector<std::string> changelog;
    };

    class UpdateManager {
    public:
        static UpdateManager& Instance();

        void CheckForUpdates();
        void StartDownloadAndInstall();
        void SetExecutablePath(const std::string& path);

        // Drops everything learned about the previously tracked channel and
        // invalidates any check still in flight, so its result can't land as
        // if it belonged to the newly selected channel.
        //
        // Returns false when it declined to reset — an install is running, or
        // one has finished and the app is waiting to be restarted. Both states
        // describe an NRO on disk that the new channel selection can't speak
        // for, so the caller should not start a check off the back of it.
        bool ResetForChannelChange();

        // Getters
        UpdateState GetState();
        std::string GetLatestError();
        AppManifest GetRemoteManifest();
        // Manifest URL the current result was fetched from — the install reuses
        // it so a channel switched mid-flight can't repoint the NRO download.
        std::string GetRemoteManifestUrl();
        // True when the offered build comes from a channel other than the one
        // the running NRO was installed from. It is then not necessarily newer:
        // moving testing -> stable normally means going back a version.
        bool IsOfferedBuildChannelSwitch();
        long long GetDownloadedBytes();
        long long GetTotalBytes();
        
        bool CanRestoreBackup() const;
        bool RestoreBackup();

    private:
        UpdateManager();
        ~UpdateManager();

        void SetState(UpdateState new_state);
        void SetError(const std::string& err);

        std::mutex state_mutex;
        UpdateState state{UpdateState::Idle};
        std::string latest_error;
        std::string executable_path;
        AppManifest remote_manifest;
        std::string remote_manifest_url;
        bool offer_is_channel_switch = false;
        // Bumped on every channel change; a check worker carries the value it
        // started with and abandons its result if it no longer matches.
        unsigned check_generation = 0;

        std::atomic<long long> downloaded_bytes{0};
        std::atomic<long long> total_bytes{0};

        std::shared_ptr<HttpResult> current_download_result;
        bool check_in_progress = false;
        bool install_in_progress = false;
        // Set when a check is asked for while one is already running — cycling
        // the channel does exactly that, and the in-flight check belongs to the
        // channel just left. The running worker picks this up and goes again
        // rather than dropping the request.
        bool recheck_requested = false;

        void WorkerCheckForUpdates();
        void WorkerCheckForUpdatesOnce();
        void WorkerDownloadAndInstall();
    };

}
