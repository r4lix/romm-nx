#include "MainApplication.hpp"
#include "DetailLayout.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/RommApi.hpp"
#include "../model/JsonUtil.hpp"
#include "../model/UpdateManager.hpp"
#include "../model/ScreenWakeManager.hpp"
#include "LibraryLayout.hpp"
#include <iostream>
#include <chrono>

namespace romm::ui {

    void MainApplication::OnLoad() {
        std::cout << "[LOG] Loading Orbitron and Ubuntu fonts from RomFS" << std::endl;
        
        auto orbitron_24 = std::make_shared<pu::ttf::Font>(24);
        orbitron_24->LoadFromFile("romfs:/Orbitron-Black.ttf");
        pu::ui::render::AddFont("Orbitron@24", orbitron_24);

        auto orbitron_30 = std::make_shared<pu::ttf::Font>(30);
        orbitron_30->LoadFromFile("romfs:/Orbitron-Black.ttf");
        pu::ui::render::AddFont("Orbitron@30", orbitron_30);

        auto orbitron_37 = std::make_shared<pu::ttf::Font>(37);
        orbitron_37->LoadFromFile("romfs:/Orbitron-Black.ttf");
        pu::ui::render::AddFont("Orbitron@37", orbitron_37);

        auto orbitron_45 = std::make_shared<pu::ttf::Font>(45);
        orbitron_45->LoadFromFile("romfs:/Orbitron-Black.ttf");
        pu::ui::render::AddFont("Orbitron@45", orbitron_45);

        auto ubuntu_18 = std::make_shared<pu::ttf::Font>(18);
        ubuntu_18->LoadFromFile("romfs:/Ubuntu-Regular.ttf");
        pu::ui::render::AddFont("Ubuntu@18", ubuntu_18);

        auto ubuntu_20 = std::make_shared<pu::ttf::Font>(20);
        ubuntu_20->LoadFromFile("romfs:/Ubuntu-Regular.ttf");
        pu::ui::render::AddFont("Ubuntu@20", ubuntu_20);

        auto ubuntu_24 = std::make_shared<pu::ttf::Font>(24);
        ubuntu_24->LoadFromFile("romfs:/Ubuntu-Regular.ttf");
        pu::ui::render::AddFont("Ubuntu@24", ubuntu_24);

        auto ubuntu_30 = std::make_shared<pu::ttf::Font>(30);
        ubuntu_30->LoadFromFile("romfs:/Ubuntu-Regular.ttf");
        pu::ui::render::AddFont("Ubuntu@30", ubuntu_30);

        auto ubuntu_37 = std::make_shared<pu::ttf::Font>(37);
        ubuntu_37->LoadFromFile("romfs:/Ubuntu-Regular.ttf");
        pu::ui::render::AddFont("Ubuntu@37", ubuntu_37);

        // Create the data model
        data_model = std::make_shared<romm::model::DataModel>();

        // Create the navigation manager
        nav_mgr = std::make_shared<romm::navigation::NavigationManager>(this, data_model);
        
        // Initialize navigation manager
        nav_mgr->Initialize();

        // Check config validity on startup
        auto& config = romm::model::ConfigManager::Instance();
        if (!config.IsValid()) {
            std::cerr << "[app] Configuration invalid: " << config.GetErrorMessage() << std::endl;
            data_model->SetPlatformState(romm::model::ApiState::FailedConnect);
        } else {
            // Trigger async platform fetch
            std::cout << "[app] Config is valid, fetching platforms..." << std::endl;
            data_model->SetPlatformState(romm::model::ApiState::Loading);
            pending_platforms_req = romm::model::RommApi::fetchPlatformsAsync();

            if (config.CheckUpdatesOnStartup()) {
                romm::model::UpdateManager::Instance().CheckForUpdates();
            }
        }

        // Add a frame callback to poll requests on the main thread
        this->AddRenderCallback([this]() {
            this->PollNetworkRequests();
            romm::model::ScreenWakeManager::Instance().Poll();
        });

        // Set inputs callback
        this->SetOnInput([this](const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) {
            // Exit on (+)
            if (keys_down & HidNpadButton_Plus) {
                std::cout << "[LOG] Plus button pressed: Closing Application" << std::endl;
                this->Close();
                return;
            }

            if (keys_down != 0) {
                auto start = std::chrono::high_resolution_clock::now();
                nav_mgr->HandleInput(keys_down, keys_held);
                auto end = std::chrono::high_resolution_clock::now();
                auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
                std::cout << "[PERF] Input handling: " << elapsed_ms << " ms" << std::endl;
            } else if (keys_held != 0) {
                // No fresh press, but check for held repeat (directional hold scrolling)
                nav_mgr->HandleInput(0, keys_held);
            }
        });

    }

    void MainApplication::TriggerFetchRoms(int platform_id) {
        if (pending_roms_req) {
            pending_roms_req.reset();
        }

        auto& config = romm::model::ConfigManager::Instance();
        if (!config.IsValid()) {
            data_model->SetRomsState(romm::model::ApiState::FailedConnect);
            return;
        }

        const auto* plat = data_model->GetPlatformById(std::to_string(platform_id));
        std::string plat_name = plat ? plat->name : ("Platform #" + std::to_string(platform_id));

        if (plat && (plat->roms_state == romm::model::ApiState::Success || plat->roms_state == romm::model::ApiState::NoData)) {
            std::cout << "[PERF] ROMs loaded from cache: yes" << std::endl;
            data_model->SetRomsState(plat->roms_state);

            auto lib_lyt = nav_mgr->GetLibraryLayout();
            if (lib_lyt) {
                lib_lyt->OnSelectionUpdated();
            }
            return;
        }

        std::cout << "[PERF] ROMs loaded from cache: no" << std::endl;

        selected_platform_id_fetching = platform_id;
        current_roms_request_id++;

        data_model->SetPlatformRomsState(std::to_string(platform_id), romm::model::ApiState::Loading);
        data_model->SetRomsState(romm::model::ApiState::Loading);
        pending_roms_req = romm::model::RommApi::fetchRomsAsync(platform_id, current_roms_request_id);

        // Instantly refresh UI to show "Loading ROMs..."
        auto lib_lyt = nav_mgr->GetLibraryLayout();
        if (lib_lyt) {
            lib_lyt->OnSelectionUpdated();
        }
    }

    void MainApplication::TriggerFetchPlatforms() {
        if (pending_platforms_req) {
            pending_platforms_req.reset();
        }
        if (pending_roms_req) {
            pending_roms_req.reset();
        }

        // Clear existing platforms & games in model
        data_model->SetPlatforms({});
        data_model->SetPlatformState(romm::model::ApiState::Loading);
        data_model->SetRomsState(romm::model::ApiState::Loading);

        auto& config = romm::model::ConfigManager::Instance();
        if (!config.IsValid()) {
            data_model->SetPlatformState(romm::model::ApiState::FailedConnect);
            data_model->SetRomsState(romm::model::ApiState::FailedConnect);
            
            auto lib_lyt = nav_mgr->GetLibraryLayout();
            if (lib_lyt) {
                lib_lyt->OnSelectionUpdated();
            }
            return;
        }

        std::cout << "[app] Fetching platforms after config update..." << std::endl;
        pending_platforms_req = romm::model::RommApi::fetchPlatformsAsync();

        // Refresh UI
        auto lib_lyt = nav_mgr->GetLibraryLayout();
        if (lib_lyt) {
            lib_lyt->OnSelectionUpdated();
        }
    }

    void MainApplication::PollNetworkRequests() {
        // 1. Poll platforms request
        if (pending_platforms_req && pending_platforms_req->completed) {
            auto req = pending_platforms_req;
            pending_platforms_req.reset();

            std::cout << "[app] Platforms request finished. success=" << req->success << " code=" << req->statusCode << std::endl;

            if (!req->success) {
                if (req->statusCode == 401 || req->statusCode == 403) {
                    data_model->SetPlatformState(romm::model::ApiState::Unauthorized);
                } else {
                    data_model->SetPlatformState(romm::model::ApiState::FailedConnect);
                }
            } else {
                data_model->SetPlatforms(req->platforms);
                data_model->SetPlatformState(romm::model::ApiState::Success);

                // Reset nav indices to 0 to prevent OOB if list changed (e.g. after config edit)
                nav_mgr->SetSelectedPlatformIdx(0);
                nav_mgr->SetLoadedPlatformIdx(0);
                // Also reset roms_state to Idle: user must press A to load ROMs
                data_model->SetRomsState(romm::model::ApiState::Idle);
            }

            auto lib_lyt = nav_mgr->GetLibraryLayout();
            if (lib_lyt) {
                lib_lyt->OnSelectionUpdated();
            }
        }

        // 2. Poll ROMs request
        if (pending_roms_req && pending_roms_req->completed) {
            auto req = pending_roms_req;
            pending_roms_req.reset();

            if (req->request_id != current_roms_request_id) {
                std::cout << "[app] Discarding stale ROMs request ID " << req->request_id 
                          << " (current is " << current_roms_request_id << ")" << std::endl;
                return;
            }

            std::cout << "[app] ROMs request finished. success=" << req->success << " code=" << req->statusCode << std::endl;

            int plat_id = req->platform_id;

            if (!req->success) {
                romm::model::ApiState state = romm::model::ApiState::FailedConnect;
                if (req->statusCode == 401 || req->statusCode == 403) {
                    state = romm::model::ApiState::Unauthorized;
                }
                data_model->SetPlatformRomsState(std::to_string(plat_id), state);
                data_model->SetRomsState(state);
            } else {
                data_model->UpdatePlatformGames(std::to_string(plat_id), req->games);
                if (req->games.empty()) {
                    data_model->SetRomsState(romm::model::ApiState::NoData);
                } else {
                    data_model->SetRomsState(romm::model::ApiState::Success);
                }
            }

            auto lib_lyt = nav_mgr->GetLibraryLayout();
            if (lib_lyt) {
                lib_lyt->OnSelectionUpdated();
            }
        }

        // 3. Poll Detail request
        if (pending_detail_req && pending_detail_req->completed) {
            auto req = pending_detail_req;
            pending_detail_req.reset();

            std::cout << "[app] Detail request finished for ROM " << req->rom_id 
                      << ". success=" << req->success << " code=" << req->statusCode << std::endl;

            if (!req->success) {
                data_model->SetDetailState(req->rom_id, romm::model::DetailLoadState::Failed);
            } else {
                data_model->SetCachedDetail(req->rom_id, req->detail);
            }

            auto detail_lyt = nav_mgr->GetDetailLayout();
            if (detail_lyt) {
                bool active = (nav_mgr->GetScreen() == romm::navigation::Screen::Detail);
                bool rom_match = (req->rom_id == detail_lyt->ctx.rom_id);
                bool plat_match = (req->platform_slug == detail_lyt->ctx.platform_slug);
                bool gen_match = (req->generation == detail_lyt->ctx.generation);

                if (active && rom_match && plat_match && gen_match) {
                    std::cout << "[app] Applying detail result for ROM " << req->rom_id << " (generation " << req->generation << ")" << std::endl;
                    detail_lyt->OnSelectionUpdated();
                } else {
                    std::cout << "[app] Discarding stale detail result for ROM " << req->rom_id 
                              << " (generation " << req->generation << ", platform " << req->platform_slug << "). "
                              << "Active context: ROM " << detail_lyt->ctx.rom_id 
                              << ", platform " << detail_lyt->ctx.platform_slug 
                              << ", generation " << detail_lyt->ctx.generation 
                              << ", screen " << (int)nav_mgr->GetScreen() << std::endl;
                }
            }
        }
    }

    void MainApplication::TriggerFetchRomDetail(int rom_id, uint64_t generation, const std::string& platform_slug) {
        data_model->SetDetailState(rom_id, romm::model::DetailLoadState::Loading);
        pending_detail_req = romm::model::RommApi::fetchRomDetailAsync(rom_id, generation, platform_slug);
    }

}
