#include <iostream>
#include <switch.h>
#include <pu/Plutonium>
#include "ui/MainApplication.hpp"
#include "navigation/HttpClient.hpp"
#include "model/ConfigManager.hpp"
#include "model/CacheManager.hpp"
#include "model/UpdateManager.hpp"
#include "model/ScreenWakeManager.hpp"
#include "model/AudioManager.hpp"
#include "Version.hpp"

namespace {

bool waitForNetwork() {
    for (int attempt = 0; attempt < 30; ++attempt) {
        NifmInternetConnectionType type = (NifmInternetConnectionType)0;
        u32 wifi = 0;
        NifmInternetConnectionStatus status = (NifmInternetConnectionStatus)0;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &wifi, &status)) &&
            status == NifmInternetConnectionStatus_Connected) {
            return true;
        }
        svcSleepThread(500000000ULL);
    }
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    // Setup unbuffered stdout/stderr to ensure log outputs are flushed immediately
    std::setvbuf(stdout, NULL, _IONBF, 0);
    std::setvbuf(stderr, NULL, _IONBF, 0);

    std::cout << "[BUILD] romm-nx build v" << romm::ROMM_NX_VERSION << std::endl;
    std::cout << "[LOG] main: Initializing Plutonium ROM Manager Client" << std::endl;

    // 1. Safe Socket and Network Initialization inside main()
    // libnx's stock socket config uses small, fixed-ish TCP buffers, which caps
    // the effective receive window and throttles throughput on higher-latency
    // Wi-Fi links (RomM downloads are one big long-lived transfer, so a larger
    // window matters far more here than session/connection overhead). Start
    // from the documented defaults and only raise the buffer/window knobs.
    SocketInitConfig socket_cfg = *socketGetDefaultInitConfig();
    socket_cfg.tcp_tx_buf_size = 0x40000;       // 256 KiB initial send buffer
    socket_cfg.tcp_rx_buf_size = 0x40000;       // 256 KiB initial receive buffer
    socket_cfg.tcp_tx_buf_max_size = 0x200000;  // let the send window grow to 2 MiB
    socket_cfg.tcp_rx_buf_max_size = 0x200000;  // let the receive window grow to 2 MiB
    socket_cfg.sb_efficiency = 8;               // max per libnx docs; more buffers/socket
    bool socket_ok = R_SUCCEEDED(socketInitialize(&socket_cfg));
    if (socket_ok) {
        nxlinkStdio();
        std::cout << "[LOG] Sockets initialized successfully." << std::endl;
    } else {
        std::cerr << "[LOG] Sockets failed to initialize!" << std::endl;
    }

    bool nifm_ok = false;
    if (socket_ok) {
        nifm_ok = R_SUCCEEDED(nifmInitialize(NifmServiceType_User));
        if (nifm_ok) {
            std::cout << "[LOG] Nifm service initialized." << std::endl;
            if (waitForNetwork()) {
                std::cout << "[LOG] Network is ready." << std::endl;
            } else {
                std::cerr << "[LOG] Network not ready after timeout." << std::endl;
            }
        } else {
            std::cerr << "[LOG] Nifm failed to initialize!" << std::endl;
        }
    }

    if (socket_ok) {
        HttpClient::init();
    }

    // Load config on startup
    romm::model::ConfigManager::Instance().Load();
    romm::model::ScreenWakeManager::Instance().RequestUpdate();
    romm::model::UpdateManager::Instance().SetExecutablePath(argv[0]);
    romm::model::CacheManager::Instance().AutoPrune();

    // Configure renderer options
    auto renderer_opts = pu::ui::render::RendererInitOptions(SDL_INIT_EVERYTHING, pu::ui::render::RendererHardwareFlags);
    renderer_opts.UseImage(pu::ui::render::ImgAllFlags);
    renderer_opts.UseRomfs();
    
    // Text rendering via system shared fonts
    renderer_opts.SetPlServiceType(PlServiceType_User);
    renderer_opts.AddDefaultAllSharedFonts();

    // Player input: Player 1 standard handheld/controllers only
    renderer_opts.SetInputPlayerCount(1);
    renderer_opts.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
    renderer_opts.AddInputNpadIdType(HidNpadIdType_Handheld);
    renderer_opts.AddInputNpadIdType(HidNpadIdType_No1);

    // Instantiate renderer
    auto renderer = pu::ui::render::Renderer::New(renderer_opts);

    // Audio (boot chime + looping menu ambience) needs SDL_INIT_AUDIO, which
    // the renderer above just brought up via SDL_INIT_EVERYTHING — must come
    // after it, not before.
    romm::model::AudioManager::Instance().Init();
    romm::model::AudioManager::Instance().PlayStartupSound();

    // Create main application from the renderer
    auto app = romm::ui::MainApplication::New(renderer);

    // Load the application (this triggers OnLoad and layouts creation)
    const auto rc = app->Load();
    if (R_FAILED(rc)) {
        std::cerr << "[CRASH] Failed to load application: " << rc << std::endl;
        diagAbortWithResult(rc);
    }

    // Show: start rendering loop
    std::cout << "[LOG] main: Starting main UI loop" << std::endl;
    app->Show();
    std::cout << "[LOG] main: Application closed cleanly" << std::endl;

    // Restore screen sleep behavior
    romm::model::ScreenWakeManager::Instance().Restore();

    romm::model::AudioManager::Instance().Shutdown();

    // 2. Cleanups
    if (socket_ok) {
        HttpClient::shutdown();
    }
    if (nifm_ok) {
        nifmExit();
    }
    if (socket_ok) {
        socketExit();
    }

    return 0;
}
