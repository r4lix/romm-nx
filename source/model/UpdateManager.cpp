#include "UpdateManager.hpp"
#include "ConfigManager.hpp"
#include "DownloadManager.hpp"
#include "JsonUtil.hpp"
#include "../Version.hpp"
extern "C" {
#include <switch.h>
#include <switch/crypto/sha256.h>
}
#include <curl/curl.h>
#include <cstdio>
#include <iostream>
#include <algorithm>
#include <thread>
#include <chrono>
#include <sys/stat.h>

namespace romm::model {

    namespace {
        bool FileExists(const std::string& path) {
            FILE* f = fopen(path.c_str(), "rb");
            if (f) {
                fclose(f);
                return true;
            }
            return false;
        }

        std::string CalculateFileSHA256(const std::string& filepath) {
            FILE* f = fopen(filepath.c_str(), "rb");
            if (!f) return "";
            
            Sha256Context ctx;
            sha256ContextCreate(&ctx);
            
            char buffer[8192];
            size_t bytes_read;
            while ((bytes_read = fread(buffer, 1, sizeof(buffer), f)) > 0) {
                sha256ContextUpdate(&ctx, buffer, bytes_read);
            }
            fclose(f);
            
            unsigned char hash[32];
            sha256ContextGetHash(&ctx, hash);
            
            char hex[65];
            for (int i = 0; i < 32; ++i) {
                sprintf(hex + (i * 2), "%02x", hash[i]);
            }
            hex[64] = '\0';
            return std::string(hex);
        }

        struct UpdateDownloadContext {
            FILE* file = nullptr;
            std::atomic<long long>* downloaded = nullptr;
            long long last_logged = 0;
        };

        size_t writeUpdateFile(void* contents, size_t size, size_t nmemb, void* userp) {
            auto* ctx = static_cast<UpdateDownloadContext*>(userp);
            size_t written = ctx->file ? fwrite(contents, size, nmemb, ctx->file) : 0;
            if (written > 0 && ctx->downloaded) {
                *ctx->downloaded += (long long)written;
            }
            return written;
        }

        int updateProgressCallback(void* clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
            auto* ctx = static_cast<UpdateDownloadContext*>(clientp);
            if (ctx && dlnow > 0) {
                if (dlnow - ctx->last_logged >= 1024 * 1024) {
                    std::cout << "[UPDATE_DOWNLOAD_PROGRESS] downloaded=" << dlnow << "/" << dltotal << std::endl;
                    ctx->last_logged = dlnow;
                }
            }
            return 0;
        }

        std::string ResolveUrl(const std::string& manifest_url, const std::string& nro_url) {
            if (nro_url.rfind("http://", 0) == 0 || nro_url.rfind("https://", 0) == 0) {
                return nro_url;
            }
            size_t last_slash = manifest_url.find_last_of('/');
            if (last_slash != std::string::npos) {
                return manifest_url.substr(0, last_slash + 1) + nro_url;
            }
            return nro_url;
        }

        bool IsSafeNroUrl(const std::string& url) {
            if (url.find("..") != std::string::npos) return false;
            if (url.find("file:") != std::string::npos) return false;
            if (url.find("sdmc:") != std::string::npos) return false;
            if (url.find("romfs:") != std::string::npos) return false;
            if (url.find(":") != std::string::npos) {
                if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
                    return false;
                }
            }
            if (url.rfind("/", 0) == 0) return false;
            return true;
        }
    }

    UpdateManager& UpdateManager::Instance() {
        static UpdateManager instance;
        return instance;
    }

    UpdateManager::UpdateManager() {}
    UpdateManager::~UpdateManager() {}

    void UpdateManager::SetExecutablePath(const std::string& path) {
        std::lock_guard<std::mutex> lock(state_mutex);
        executable_path = path;
    }

    UpdateState UpdateManager::GetState() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return state;
    }

    void UpdateManager::SetState(UpdateState new_state) {
        std::lock_guard<std::mutex> lock(state_mutex);
        state = new_state;
    }

    std::string UpdateManager::GetLatestError() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return latest_error;
    }

    void UpdateManager::SetError(const std::string& err) {
        std::lock_guard<std::mutex> lock(state_mutex);
        latest_error = err;
        if (!err.empty()) {
            std::cout << "[UPDATE_ERROR] reason=" << err << std::endl;
        }
    }

    AppManifest UpdateManager::GetRemoteManifest() {
        std::lock_guard<std::mutex> lock(state_mutex);
        return remote_manifest;
    }

    long long UpdateManager::GetDownloadedBytes() {
        return downloaded_bytes.load();
    }

    long long UpdateManager::GetTotalBytes() {
        return total_bytes.load();
    }

    bool UpdateManager::CanRestoreBackup() const {
        std::string current_path = executable_path;
        if (current_path.empty()) {
            current_path = "sdmc:/switch/romm-nx/romm-nx.nro";
        }
        std::string backup_path = current_path + ".bak";
        return FileExists(backup_path);
    }

    bool UpdateManager::RestoreBackup() {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (state == UpdateState::Checking || state == UpdateState::Downloading || state == UpdateState::Installing) {
            return false;
        }

        std::string current_path = executable_path;
        if (current_path.empty()) {
            current_path = "sdmc:/switch/romm-nx/romm-nx.nro";
        }
        std::string backup_path = current_path + ".bak";
        std::string temp_new_path = current_path + ".new_tmp";

        if (!FileExists(backup_path)) {
            SetError("Backup file does not exist");
            return false;
        }

        // Unmount RomFS to release file lock on current NRO
        romfsExit();

        // Rename current to temp
        remove(temp_new_path.c_str());
        if (FileExists(current_path)) {
            if (rename(current_path.c_str(), temp_new_path.c_str()) != 0) {
                romfsInit();
                SetError("Failed to rename current NRO to temporary path");
                return false;
            }
        }

        // Rename backup to current
        if (rename(backup_path.c_str(), current_path.c_str()) != 0) {
            // Restore from temp
            if (FileExists(temp_new_path)) {
                rename(temp_new_path.c_str(), current_path.c_str());
            }
            romfsInit();
            SetError("Failed to restore backup NRO");
            return false;
        }

        romfsInit();
        remove(temp_new_path.c_str());
        state = UpdateState::InstalledRestartRequired;
        return true;
    }

    void UpdateManager::CheckForUpdates() {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (check_in_progress || install_in_progress) return;
            check_in_progress = true;
        }
        SetState(UpdateState::Checking);
        SetError("");
        HttpClient::runAsync([this]() {
            WorkerCheckForUpdates();
        });
    }

    void UpdateManager::WorkerCheckForUpdates() {
        std::string manifest_url = ConfigManager::Instance().GetUpdateManifestUrl();
        std::cout << "[UPDATE_CHECK_START] manifest_url=" << manifest_url << std::endl;

        auto http_res = HttpClient::getSync(manifest_url, {});
        if (!http_res.success) {
            SetError("Failed to fetch manifest: " + (http_res.error.empty() ? "HTTP " + std::to_string(http_res.statusCode) : http_res.error));
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        // Parse json manifest
        AppManifest manifest;
        bool parse_ok = true;
        
        parse_ok &= jsonExtractInt(http_res.body, "schema", manifest.schema);
        parse_ok &= jsonExtractString(http_res.body, "app", manifest.app);
        parse_ok &= jsonExtractString(http_res.body, "channel", manifest.channel);
        parse_ok &= jsonExtractString(http_res.body, "version", manifest.version);
        parse_ok &= jsonExtractInt(http_res.body, "version_code", manifest.version_code);
        jsonExtractBool(http_res.body, "mandatory", manifest.mandatory);

        size_t nro_pos = http_res.body.find("\"nro\"");
        if (nro_pos != std::string::npos) {
            std::string nro_sub = http_res.body.substr(nro_pos);
            parse_ok &= jsonExtractString(nro_sub, "url", manifest.nro.url);
            int size_val = 0;
            parse_ok &= jsonExtractInt(nro_sub, "size", size_val);
            manifest.nro.size = size_val;
            parse_ok &= jsonExtractString(nro_sub, "sha256", manifest.nro.sha256);
        } else {
            parse_ok = false;
        }

        jsonExtractStringArray(http_res.body, "changelog", manifest.changelog);

        if (!parse_ok) {
            SetError("Failed to parse update manifest JSON");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        // Validate manifest content
        if (manifest.schema != 1) {
            SetError("Unsupported manifest schema version: " + std::to_string(manifest.schema));
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        if (manifest.app != "romm-nx") {
            SetError("Manifest app mismatch: " + manifest.app);
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        std::string expected_channel = ConfigManager::Instance().GetUpdateChannel();
        if (manifest.channel != expected_channel) {
            SetError("Manifest channel mismatch: " + manifest.channel + " (expected " + expected_channel + ")");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        if (manifest.nro.url.empty()) {
            SetError("NRO URL is missing in update manifest");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        if (manifest.nro.size <= 0) {
            SetError("NRO size must be greater than zero");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        if (manifest.nro.sha256.size() != 64) {
            SetError("Invalid SHA-256 hash length in manifest");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        if (!IsSafeNroUrl(manifest.nro.url)) {
            SetError("Unsafe NRO URL in update manifest");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
            return;
        }

        std::cout << "[UPDATE_MANIFEST_OK] remote_version=" << manifest.version << " version_code=" << manifest.version_code << std::endl;

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            remote_manifest = manifest;
        }

        // Compare versions
        if (manifest.version_code > ROMM_NX_VERSION_CODE) {
            std::cout << "[UPDATE_AVAILABLE] local=" << ROMM_NX_VERSION_CODE << " remote=" << manifest.version_code << std::endl;
            SetState(UpdateState::UpdateAvailable);
        } else {
            SetState(UpdateState::NoUpdateAvailable);
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            check_in_progress = false;
        }
    }

    void UpdateManager::StartDownloadAndInstall() {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            if (install_in_progress || check_in_progress) return;
            if (state != UpdateState::UpdateAvailable) return;
            install_in_progress = true;
        }
        SetError("");
        HttpClient::runAsync([this]() {
            WorkerDownloadAndInstall();
        });
    }

    void UpdateManager::WorkerDownloadAndInstall() {
        // Block if game download active or queue running
        auto active_dl = DownloadManager::Instance().GetActiveDownloadSnapshot();
        auto dl_queue = DownloadManager::Instance().GetQueueSnapshot();
        if (active_dl.rom_id > 0 || !dl_queue.empty()) {
            SetError("Stop current downloads before updating romm-nx.");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        // Ensure directories exist
        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/romm-nx", 0777);
        mkdir("sdmc:/switch/romm-nx/update", 0777);

        std::string part_path = "sdmc:/switch/romm-nx/update/romm-nx.nro.part";
        std::string new_path = "sdmc:/switch/romm-nx/update/romm-nx.nro.new";
        
        remove(part_path.c_str());
        remove(new_path.c_str());

        AppManifest manifest = GetRemoteManifest();
        
        std::string manifest_url = ConfigManager::Instance().GetUpdateManifestUrl();
        std::string nro_url = ResolveUrl(manifest_url, manifest.nro.url);

        std::cout << "[UPDATE_MANIFEST_URL] url=" << manifest_url << std::endl;
        size_t last_slash = manifest_url.find_last_of('/');
        std::string manifest_dir = (last_slash != std::string::npos) ? manifest_url.substr(0, last_slash + 1) : "";
        std::cout << "[UPDATE_MANIFEST_DIR] dir=" << manifest_dir << std::endl;
        std::cout << "[UPDATE_NRO_URL_RAW] url=" << manifest.nro.url << std::endl;
        std::cout << "[UPDATE_NRO_URL_RESOLVED] url=" << nro_url << std::endl;

        if (nro_url.rfind("http://", 0) != 0 && nro_url.rfind("https://", 0) != 0) {
            SetError("Invalid NRO URL: must start with http:// or https://");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        std::cout << "[UPDATE_DOWNLOAD_START] url=" << nro_url << " size=" << manifest.nro.size << std::endl;

        downloaded_bytes = 0;
        total_bytes = manifest.nro.size;

        SetState(UpdateState::Downloading);

        // Before starting curl, open file for binary writing
        FILE* file = fopen(part_path.c_str(), "wb");
        if (!file) {
            SetError("Failed to open update NRO part file for writing");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            fclose(file);
            SetError("Failed to initialize curl for NRO download");
            SetState(UpdateState::Error);
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        UpdateDownloadContext dl_ctx;
        dl_ctx.file = file;
        dl_ctx.downloaded = &downloaded_bytes;

        curl_easy_setopt(curl, CURLOPT_URL, nro_url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeUpdateFile);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &dl_ctx);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
        // There's no cancel button anywhere in the UI for an in-progress
        // update download/install, and no overall timeout — a connection that
        // stalls after the initial connect (server hangs mid-transfer) would
        // otherwise wedge the state machine in "Downloading" forever: the app
        // shows no error, and CheckForUpdates()/StartDownloadAndInstall() both
        // bail out early while install_in_progress stays true until restart.
        // Abort if the transfer sustains under 1 KB/s for 30 seconds.
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, updateProgressCallback);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &dl_ctx);

        const CURLcode code = curl_easy_perform(curl);
        fclose(file);

        long http_code = 0;
        if (code == CURLE_OK) {
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        }

        curl_easy_cleanup(curl);

        if (code != CURLE_OK || http_code < 200 || http_code >= 300) {
            std::string curl_err = curl_easy_strerror(code);
            std::string err_msg = "Download failed: " + curl_err + " (HTTP " + std::to_string(http_code) + ")";
            
            std::cout << "[UPDATE_DOWNLOAD_ERROR] curl_code=" << (int)code
                      << " message=" << curl_err
                      << " http_code=" << http_code << std::endl;

            SetError(err_msg);
            SetState(UpdateState::Error);
            remove(part_path.c_str());
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        // Verify final download size
        long long final_size = 0;
        FILE* f = fopen(part_path.c_str(), "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            final_size = ftell(f);
            fclose(f);
        }

        if (final_size != total_bytes.load()) {
            SetError("Downloaded NRO file size mismatch");
            SetState(UpdateState::Error);
            remove(part_path.c_str());
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        // Verify SHA-256
        SetState(UpdateState::Verifying);
        std::string calculated_sha = CalculateFileSHA256(part_path);
        std::string expected_sha = manifest.nro.sha256;

        std::transform(calculated_sha.begin(), calculated_sha.end(), calculated_sha.begin(), ::tolower);
        std::transform(expected_sha.begin(), expected_sha.end(), expected_sha.begin(), ::tolower);

        if (calculated_sha != expected_sha) {
            SetError("SHA-256 hash mismatch");
            SetState(UpdateState::Error);
            remove(part_path.c_str());
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        std::cout << "[UPDATE_VERIFY_OK] sha256=" << calculated_sha << std::endl;

        // Rename verified part path to new path
        if (rename(part_path.c_str(), new_path.c_str()) != 0) {
            SetError("Failed to rename temporary download file");
            SetState(UpdateState::Error);
            remove(part_path.c_str());
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        // Install NRO
        std::cout << "[UPDATE_INSTALL_START]" << std::endl;
        SetState(UpdateState::Installing);

        std::string current_app_path = executable_path;
        if (current_app_path.empty()) {
            current_app_path = "sdmc:/switch/romm-nx/romm-nx.nro";
        }
        std::string backup_path = current_app_path + ".bak";

        // Remove old backup if exists
        remove(backup_path.c_str());

        // Unmount RomFS to release file lock on the running NRO
        romfsExit();

        bool backup_created = false;
        if (FileExists(current_app_path)) {
            if (rename(current_app_path.c_str(), backup_path.c_str()) == 0) {
                backup_created = true;
            } else {
                romfsInit();
                SetError("Failed to backup current application executable");
                SetState(UpdateState::Error);
                remove(new_path.c_str());
                std::lock_guard<std::mutex> lock(state_mutex);
                install_in_progress = false;
                return;
            }
        }

        // Place new NRO in position
        if (rename(new_path.c_str(), current_app_path.c_str()) != 0) {
            // Restore from backup
            if (backup_created) {
                rename(backup_path.c_str(), current_app_path.c_str());
            }
            romfsInit();
            SetError("Failed to install the new application executable");
            SetState(UpdateState::Error);
            remove(new_path.c_str());
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
            return;
        }

        // Remount RomFS
        romfsInit();

        // Write update_result.json
        FILE* result_f = fopen("sdmc:/switch/romm-nx/update/update_result.json", "w");
        if (result_f) {
            std::string res_json = "{\n";
            res_json += "  \"status\": \"success\",\n";
            res_json += "  \"version\": \"" + manifest.version + "\",\n";
            res_json += "  \"version_code\": " + std::to_string(manifest.version_code) + "\n";
            res_json += "}\n";
            fwrite(res_json.c_str(), 1, res_json.size(), result_f);
            fclose(result_f);
        }

        std::cout << "[UPDATE_INSTALL_OK]" << std::endl;
        SetState(UpdateState::InstalledRestartRequired);

        {
            std::lock_guard<std::mutex> lock(state_mutex);
            install_in_progress = false;
        }
    }

}
