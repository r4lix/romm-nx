#include "MainApplication.hpp"
#include "DetailLayout.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/RommApi.hpp"
#include "../model/JsonUtil.hpp"
#include "../model/UpdateManager.hpp"
#include "../model/ScreenWakeManager.hpp"
#include "../model/AudioManager.hpp"
#include "../model/NetworkStatus.hpp"
#include "../model/DownloadManager.hpp"
#include "LibraryLayout.hpp"
#include "GameGrid.hpp"
#include <iostream>
#include <chrono>

namespace romm::ui {

    void MainApplication::OnLoad() {
        std::cout << "[LOG] Loading Orbitron and Ubuntu fonts from RomFS" << std::endl;
        
        // Registered from a table rather than one block per size. These were
        // hand-written and drifted: "Ubuntu@22" and "Ubuntu@26" were in use on
        // the Installed screen but never registered here, and RenderText
        // returns a null texture for an unknown font name rather than failing
        // loudly — so those rows silently drew nothing at all. Any size a
        // layout asks for must appear in these lists.
        struct FontSpec { const char* prefix; const char* file; };
        const FontSpec faces[] = {
            { "Orbitron", "romfs:/Orbitron-Black.ttf" },
            { "Ubuntu",   "romfs:/Ubuntu-Regular.ttf" }
        };
        const u32 orbitron_sizes[] = { 24, 30, 37, 45 };
        const u32 ubuntu_sizes[]   = { 18, 20, 22, 24, 26, 30, 37 };

        for (const auto& face : faces) {
            const bool is_orbitron = (std::string(face.prefix) == "Orbitron");
            const u32* sizes = is_orbitron ? orbitron_sizes : ubuntu_sizes;
            const size_t count = is_orbitron ? (sizeof(orbitron_sizes) / sizeof(u32))
                                             : (sizeof(ubuntu_sizes) / sizeof(u32));
            for (size_t i = 0; i < count; ++i) {
                auto font = std::make_shared<pu::ttf::Font>(sizes[i]);
                font->LoadFromFile(face.file);
                pu::ui::render::AddFont(std::string(face.prefix) + "@" + std::to_string(sizes[i]), font);
            }
        }

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
        } else if (romm::model::IsNetworkConnected()) {
            StartPlatformsFetch(true);
        } else {
            // Nothing is wrong yet — the console just hasn't finished joining a
            // network. Say that on screen and retry from PollDeferredNetworkStart
            // rather than firing a request that can only fail and leave the user
            // looking at "Failed to connect".
            std::cout << "[app] No network yet, deferring platform fetch." << std::endl;
            data_model->SetPlatformState(romm::model::ApiState::WaitingNetwork);
            data_model->SetRomsState(romm::model::ApiState::WaitingNetwork);
            platforms_fetch_deferred = true;
            network_wait_start = std::chrono::steady_clock::now();
            last_network_check = network_wait_start;
        }

        // Add a frame callback to poll requests on the main thread
        this->AddRenderCallback([this]() {
            this->PollDeferredNetworkStart();
            this->PollNetworkRequests();
            this->PollDetailPrefetch();
            this->PollBulkDownload();
            romm::model::ScreenWakeManager::Instance().Poll();
            romm::model::AudioManager::Instance().Poll();
            nav_mgr->PollUpdateNotification();
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
                nav_mgr->HandleInput(keys_down, keys_held);
            } else if (keys_held != 0) {
                // No fresh press, but check for held repeat (directional hold scrolling)
                nav_mgr->HandleInput(0, keys_held);
            }
        });

    }

    void MainApplication::RefreshLibraryLayout() {
        auto lib_lyt = nav_mgr->GetLibraryLayout();
        if (lib_lyt) {
            lib_lyt->OnSelectionUpdated();
        }
    }

    void MainApplication::StartPlatformsFetch(bool check_updates) {
        std::cout << "[app] Fetching platforms..." << std::endl;
        data_model->SetPlatformState(romm::model::ApiState::Loading);
        pending_platforms_req = romm::model::RommApi::fetchPlatformsAsync();

        if (check_updates && romm::model::ConfigManager::Instance().CheckUpdatesOnStartup()) {
            romm::model::UpdateManager::Instance().CheckForUpdates();
        }
    }

    void MainApplication::PollDeferredNetworkStart() {
        if (!platforms_fetch_deferred) {
            return;
        }

        // Once or twice a second is plenty, and keeps a nifm service call off
        // the per-frame path.
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_network_check).count() < 500) {
            return;
        }
        last_network_check = now;

        if (romm::model::IsNetworkConnected()) {
            std::cout << "[app] Network came up, starting deferred platform fetch." << std::endl;
            platforms_fetch_deferred = false;
            StartPlatformsFetch(false);
            RefreshLibraryLayout();
            return;
        }

        auto waited = std::chrono::duration_cast<std::chrono::seconds>(now - network_wait_start).count();
        if (waited >= NETWORK_WAIT_TIMEOUT_SECONDS) {
            std::cerr << "[app] No network after " << waited << "s, giving up on the deferred fetch." << std::endl;
            platforms_fetch_deferred = false;
            data_model->SetPlatformState(romm::model::ApiState::FailedConnect);
            data_model->SetRomsState(romm::model::ApiState::FailedConnect);
            RefreshLibraryLayout();
        }
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
            RefreshLibraryLayout();
            return;
        }

        if (!romm::model::IsNetworkConnected()) {
            std::cout << "[app] No network, deferring platform fetch after config update." << std::endl;
            data_model->SetPlatformState(romm::model::ApiState::WaitingNetwork);
            data_model->SetRomsState(romm::model::ApiState::WaitingNetwork);
            platforms_fetch_deferred = true;
            network_wait_start = std::chrono::steady_clock::now();
            last_network_check = network_wait_start;
            RefreshLibraryLayout();
            return;
        }

        StartPlatformsFetch(false);
        RefreshLibraryLayout();
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

    void MainApplication::PollDetailPrefetch() {
        if (nav_mgr->GetScreen() != romm::navigation::Screen::Library) {
            detail_prefetch_rom_id = 0;
            return;
        }

        // Only fetch once the user has committed to the panel with A. An
        // earlier version prefetched on a 300ms settle timer while browsing,
        // which meant every pause on a row cost a request and a panel rebuild —
        // the opposite of what this view mode is for. Browsing now costs
        // nothing but the one cover.
        if (nav_mgr->GetLibraryFocus() != romm::navigation::LibraryFocus::Panel) {
            detail_prefetch_rom_id = 0;
            return;
        }

        auto lib_lyt = nav_mgr->GetLibraryLayout();
        if (!lib_lyt) return;
        auto grid = lib_lyt->GetGameGrid();
        // Only the Detail view mode has a panel to fill; the cover grids still
        // fetch details on demand when the Detail screen opens.
        if (!grid || !grid->IsDetailList()) {
            detail_prefetch_rom_id = 0;
            return;
        }

        const int rom_id = grid->GetSelectedRomId();
        if (rom_id <= 0) {
            detail_prefetch_rom_id = 0;
            return;
        }

        detail_prefetch_rom_id = rom_id;

        const auto state = data_model->GetDetailState(rom_id);
        if (state != romm::model::DetailLoadState::NotLoaded &&
            state != romm::model::DetailLoadState::Failed) {
            return; // already cached, or a request is in flight
        }
        // Don't stack a second request on top of one still running.
        if (pending_detail_req && !pending_detail_req->completed) return;

        const auto& platforms = data_model->GetPlatforms();
        const size_t plat_idx = nav_mgr->GetLoadedPlatformIdx();
        if (plat_idx >= platforms.size()) return;

        TriggerFetchRomDetail(rom_id, 0, romm::model::NormalizePlatformSlug(platforms[plat_idx].slug));
    }

    void MainApplication::EnqueueBulkDownload(int rom_id, const std::string& platform_slug, const std::string& title) {
        if (rom_id <= 0) return;
        for (const auto& item : bulk_queue) {
            if (item.rom_id == rom_id) return;
        }
        bulk_queue.push_back({ rom_id, platform_slug, title, false });
    }

    void MainApplication::PollBulkDownload() {
        if (bulk_queue.empty()) return;
        // Share the single detail slot with the detail screen and the panel;
        // whoever is mid-flight finishes first.
        if (pending_detail_req && !pending_detail_req->completed) return;

        auto& item = bulk_queue.front();

        if (const auto* detail = data_model->GetCachedDetail(item.rom_id)) {
            romm::model::DownloadManager::Instance().EnqueueDownload(*detail, item.platform_slug, item.title);
            std::cout << "[BULK] Queued download for rom " << item.rom_id
                      << " (" << bulk_queue.size() - 1 << " remaining)" << std::endl;
            bulk_queue.erase(bulk_queue.begin());
            return;
        }

        const auto state = data_model->GetDetailState(item.rom_id);
        if (state == romm::model::DetailLoadState::Failed || item.detail_requested) {
            // One attempt each. Retrying forever would wedge the queue on a ROM
            // the server won't return, and drop every game behind it.
            std::cerr << "[BULK] Skipping rom " << item.rom_id << ": detail unavailable" << std::endl;
            bulk_queue.erase(bulk_queue.begin());
            return;
        }

        item.detail_requested = true;
        TriggerFetchRomDetail(item.rom_id, 0, item.platform_slug);
    }

    void MainApplication::TriggerFetchRomDetail(int rom_id, uint64_t generation, const std::string& platform_slug) {
        data_model->SetDetailState(rom_id, romm::model::DetailLoadState::Loading);
        pending_detail_req = romm::model::RommApi::fetchRomDetailAsync(rom_id, generation, platform_slug);
    }

}
