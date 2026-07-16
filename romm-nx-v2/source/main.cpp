#include "app.hpp"

#include "config.hpp"
#include "http_client.hpp"

#include <iostream>

namespace {

bool waitForNetwork() {
    NifmInternetConnectionType connectionType = NifmInternetConnectionType_None;
    for (int attempt = 0; attempt < 30; ++attempt) {
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(nullptr, nullptr, &connectionType)) &&
            connectionType != NifmInternetConnectionType_None) {
            return true;
        }
        svcSleepThread(500000000ULL);
    }
    return false;
}

} // namespace

extern "C" void userAppInit() {
    socketInitializeDefault();
    nxlinkStdio();

    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);

    if (R_FAILED(nifmInitialize(NifmServiceType_User))) {
        std::cerr << "[romm-nx-v2] nifmInitialize failed" << std::endl;
    } else if (!waitForNetwork()) {
        std::cerr << "[romm-nx-v2] network not ready" << std::endl;
    }

    HttpClient::init();
    Config::instance().load();
}

extern "C" void userAppExit() {
    HttpClient::shutdown();
    nifmExit();
    socketExit();
}

int main() {
    auto rendererOpts = pu::ui::render::RendererInitOptions(SDL_INIT_EVERYTHING, pu::ui::render::RendererHardwareFlags);
    rendererOpts.UseImage(pu::ui::render::ImgAllFlags);
    rendererOpts.SetPlServiceType(PlServiceType_User);
    rendererOpts.AddDefaultAllSharedFonts();
    rendererOpts.SetInputPlayerCount(1);
    rendererOpts.AddInputNpadStyleTag(HidNpadStyleSet_NpadStandard);
    rendererOpts.AddInputNpadIdType(HidNpadIdType_Handheld);
    rendererOpts.AddInputNpadIdType(HidNpadIdType_No1);

    auto renderer = pu::ui::render::Renderer::New(rendererOpts);
    auto app = RommApp::New(renderer);

    const Result rc = app->Load();
    if (R_FAILED(rc)) {
        diagAbortWithResult(rc);
    }

    app->ShowWithFadeIn();
    return 0;
}
