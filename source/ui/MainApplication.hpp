#pragma once

#include <pu/Plutonium>
#include "../model/DataModel.hpp"
#include "../model/RommApi.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../navigation/HttpClient.hpp"
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

        void PollNetworkRequests();

    public:
        using Application::Application;
        ~MainApplication() override = default;

        void OnLoad() override;
        void TriggerFetchRoms(int platform_id);
        void TriggerFetchPlatforms();
        void TriggerFetchRomDetail(int rom_id, uint64_t generation = 0, const std::string& platform_slug = "");
        int GetCurrentRomsRequestId() const { return current_roms_request_id; }

        PU_SMART_CTOR(MainApplication)
    };

}
