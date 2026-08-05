#pragma once

#include <pu/Plutonium>
#include "../model/DataModel.hpp"
// For InjectChoice: a bulk queue item carries the answer the ZR prompt gave.
#include "../model/DownloadManager.hpp"
#include "../model/RommApi.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../navigation/HttpClient.hpp"
#include <chrono>
#include <memory>

namespace romm::ui {

    class MainApplication : public pu::ui::Application {
    private:
        std::shared_ptr<romm::model::DataModel> data_model;
        std::shared_ptr<romm::navigation::NavigationManager> nav_mgr;

        std::shared_ptr<romm::model::PlatformFetchResult> pending_platforms_req;
        std::shared_ptr<romm::model::RomFetchResult> pending_roms_req;
        std::shared_ptr<romm::model::RomDetailFetchResult> pending_detail_req;

        int selected_platform_id_fetching = -1;
        int current_roms_request_id = 0;

        // Set when the app started with no network: the platform fetch is held
        // back (it would only fail) until the connection appears or we give up.
        bool platforms_fetch_deferred = false;
        std::chrono::steady_clock::time_point network_wait_start;
        std::chrono::steady_clock::time_point last_network_check;

        // How long to keep waiting for a connection before showing a failure
        // the user can retry from. Generous, because joining Wi-Fi after wake
        // routinely takes a good few seconds.
        static constexpr int NETWORK_WAIT_TIMEOUT_SECONDS = 30;

        // Detail-view panel fetch. Only runs while the panel actually has focus
        // (the user pressed A), so browsing the list costs no requests at all.
        int detail_prefetch_rom_id = 0;

        // Bulk download queue. EnqueueDownload needs a GameDetail (multi-disc
        // games resolve their on-disk identity from detail.files), and the ROM
        // list doesn't carry one — so each entry may need a detail fetch first.
        // Worked through one at a time via the existing single-slot detail
        // request rather than firing N fetches at once, which would flood the
        // High lane exactly the way the cover backlog used to flood the queue.
        struct BulkDownloadItem {
            int rom_id = 0;
            std::string platform_slug;
            std::string title;
            bool detail_requested = false;
            // Resolved once for the whole batch, at the moment ZR queued it,
            // and carried per item because the detail fetches land one at a
            // time. Without it every bulk item enqueued as UseSetting, and a
            // platform set to Ask has nobody to ask by then — so a queued
            // selection silently downloaded with no injection.
            romm::model::InjectChoice inject = romm::model::InjectChoice::UseSetting;
        };
        std::vector<BulkDownloadItem> bulk_queue;
        void PollBulkDownload();

        void PollNetworkRequests();
        void PollDeferredNetworkStart();
        void PollDetailPrefetch();
        // Issue the platform request — shared by the immediate path and the
        // deferred one. check_updates is only set for the startup fetch, so a
        // manual reload from Settings doesn't re-trigger the update check.
        void StartPlatformsFetch(bool check_updates);
        void RefreshLibraryLayout();

    public:
        using Application::Application;
        ~MainApplication() override = default;

        void OnLoad() override;
        void TriggerFetchRoms(int platform_id);
        void TriggerFetchPlatforms();
        void TriggerFetchRomDetail(int rom_id, uint64_t generation = 0, const std::string& platform_slug = "");
        // Adds a ROM to the bulk download queue; ignores duplicates.
        void EnqueueBulkDownload(int rom_id, const std::string& platform_slug, const std::string& title,
                                 romm::model::InjectChoice inject = romm::model::InjectChoice::UseSetting);
        size_t GetBulkQueueRemaining() const { return bulk_queue.size(); }
        int GetCurrentRomsRequestId() const { return current_roms_request_id; }

        PU_SMART_CTOR(MainApplication)
    };

}
