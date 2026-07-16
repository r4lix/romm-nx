#pragma once

#include <atomic>

namespace romm::model {

    class ScreenWakeManager {
    public:
        static ScreenWakeManager& Instance();

        // Called by any thread to request an update (sets a dirty flag)
        void RequestUpdate();

        // Called by the main thread in the render callback / update loop
        void Poll();

        // Forces normal behavior on exit
        void Restore();

    private:
        ScreenWakeManager() = default;

        bool current_state = false;
        std::atomic<bool> pending_update{false};
    };

}
