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

        // Getters
        UpdateState GetState();
        std::string GetLatestError();
        AppManifest GetRemoteManifest();
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

        std::atomic<long long> downloaded_bytes{0};
        std::atomic<long long> total_bytes{0};

        std::shared_ptr<HttpResult> current_download_result;
        bool check_in_progress = false;
        bool install_in_progress = false;

        void WorkerCheckForUpdates();
        void WorkerDownloadAndInstall();
    };

}
