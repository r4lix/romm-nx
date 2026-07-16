#include "ScreenWakeManager.hpp"
#include "ConfigManager.hpp"
#include "DownloadManager.hpp"
#include <switch.h>
#include <iostream>

namespace romm::model {

    ScreenWakeManager& ScreenWakeManager::Instance() {
        static ScreenWakeManager inst;
        return inst;
    }

    void ScreenWakeManager::RequestUpdate() {
        pending_update.store(true);
    }

    void ScreenWakeManager::Poll() {
        if (pending_update.exchange(false)) {
            bool desired = ConfigManager::Instance().ScreenAlwaysOn() || DownloadManager::Instance().HasActiveDownload();
            if (desired != current_state) {
                appletSetMediaPlaybackState(desired);
                current_state = desired;
                std::cout << "[WAKE] Screen wake state updated to: " << (desired ? "AWAKE" : "NORMAL") << std::endl;
            }
        }
    }

    void ScreenWakeManager::Restore() {
        if (current_state) {
            appletSetMediaPlaybackState(false);
            current_state = false;
            std::cout << "[WAKE] Screen wake state restored to NORMAL on exit." << std::endl;
        }
    }

}
