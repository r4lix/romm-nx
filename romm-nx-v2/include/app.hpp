#pragma once

#include <pu/Plutonium>

#include <memory>
#include <string>
#include <vector>

#include "http_client.hpp"
#include "json_util.hpp"

class RommApp : public pu::ui::Application {
public:
    explicit RommApp(pu::ui::render::Renderer::Ref renderer);
    PU_SMART_CTOR(RommApp)

    void OnLoad() override;

private:
    enum class Screen {
        Main,
        Settings,
        Platforms,
        Roms
    };

    pu::ui::Layout::Ref mainLayout_;
    pu::ui::Layout::Ref settingsLayout_;
    pu::ui::Layout::Ref platformsLayout_;
    pu::ui::Layout::Ref romsLayout_;

    pu::ui::elm::TextBlock::Ref mainTitle_;
    pu::ui::elm::Menu::Ref mainMenu_;

    pu::ui::elm::TextBlock::Ref settingsTitle_;
    pu::ui::elm::TextBlock::Ref settingsBody_;

    pu::ui::elm::TextBlock::Ref platformsTitle_;
    pu::ui::elm::TextBlock::Ref platformsStatus_;
    pu::ui::elm::Menu::Ref platformsMenu_;

    pu::ui::elm::TextBlock::Ref romsTitle_;
    pu::ui::elm::TextBlock::Ref romsStatus_;
    pu::ui::elm::Menu::Ref romsMenu_;

    std::shared_ptr<HttpResult> pendingRequest_;
    Screen currentScreen_ = Screen::Main;
    bool downloadInProgress_ = false;

    int selectedPlatformId_ = 0;
    std::string selectedPlatformName_;
    std::vector<PlatformEntry> platforms_;
    std::vector<RomEntry> roms_;

    void showScreen(Screen screen);
    void showToast(const std::string& message);
    void setPlatformsStatus(const std::string& message);
    void setRomsStatus(const std::string& message);

    void pollPendingRequest();
    void beginFetchPlatforms();
    void beginFetchRoms();

    void populatePlatformsMenu();
    void populateRomsMenu();
    void handleBack();
};
