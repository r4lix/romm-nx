#include "app.hpp"

#include "config.hpp"
#include "http_client.hpp"
#include "romm_api.hpp"

#include <algorithm>
#include <sstream>

namespace {

constexpr pu::ui::Color kBackground = {17, 9, 31, 255};
constexpr pu::ui::Color kText = {235, 235, 245, 255};
constexpr pu::ui::Color kMuted = {170, 160, 190, 255};
constexpr pu::ui::Color kMenuItem = {40, 30, 60, 255};
constexpr pu::ui::Color kMenuFocus = {131, 71, 230, 255};

pu::ui::elm::Menu::Ref makeMenu(const s32 y) {
    return pu::ui::elm::Menu::New(
        30, y, 1220, kMenuItem, kMenuFocus, 58, 8);
}

pu::ui::elm::TextBlock::Ref makeTitle(const s32 y, const std::string& text) {
    auto block = pu::ui::elm::TextBlock::New(40, y, text);
    block->SetColor(kText);
    block->SetFontSize(34);
    return block;
}

pu::ui::elm::TextBlock::Ref makeStatus(const s32 y, const std::string& text) {
    auto block = pu::ui::elm::TextBlock::New(40, y, text);
    block->SetColor(kMuted);
    block->SetFontSize(22);
    return block;
}

} // namespace

enum class PendingKind {
    None,
    FetchPlatforms,
    FetchRoms,
    FetchRomDetail,
    DownloadFile
};

struct PendingState {
    PendingKind kind = PendingKind::None;
    int romId = 0;
    std::string fileName;
    std::string outputPath;
};

static PendingState gPending;

RommApp::RommApp(pu::ui::render::Renderer::Ref renderer)
    : pu::ui::Application(renderer) {}

void RommApp::showToast(const std::string& message) {
    auto text = pu::ui::elm::TextBlock::New(0, 0, message);
    text->SetColor(kText);
    text->SetFontSize(24);
    auto toast = pu::ui::extras::Toast::New(text, {20, 20, 20, 220});
    this->StartOverlayWithTimeout(toast, 2500);
}

void RommApp::setPlatformsStatus(const std::string& message) {
    if (this->platformsStatus_) {
        this->platformsStatus_->SetText(message);
    }
}

void RommApp::setRomsStatus(const std::string& message) {
    if (this->romsStatus_) {
        this->romsStatus_->SetText(message);
    }
}

void RommApp::showScreen(const Screen screen) {
    this->currentScreen_ = screen;
    switch (screen) {
        case Screen::Main:
            this->LoadLayout(this->mainLayout_);
            break;
        case Screen::Settings:
            this->LoadLayout(this->settingsLayout_);
            break;
        case Screen::Platforms:
            this->LoadLayout(this->platformsLayout_);
            break;
        case Screen::Roms:
            this->LoadLayout(this->romsLayout_);
            break;
    }
}

void RommApp::handleBack() {
    if (this->downloadInProgress_) {
        return;
    }

    switch (this->currentScreen_) {
        case Screen::Main:
            this->CloseWithFadeOut(true);
            break;
        case Screen::Settings:
        case Screen::Platforms:
            this->showScreen(Screen::Main);
            break;
        case Screen::Roms:
            this->showScreen(Screen::Platforms);
            break;
    }
}

void RommApp::populatePlatformsMenu() {
    this->platformsMenu_->ClearItems();
    for (const PlatformEntry& platform : this->platforms_) {
        auto item = pu::ui::elm::MenuItem::New(platform.name);
        const int platformId = platform.id;
        const std::string platformName = platform.name;
        item->AddOnKey([this, platformId, platformName]() {
            this->selectedPlatformId_ = platformId;
            this->selectedPlatformName_ = platformName;
            this->showScreen(Screen::Roms);
            this->beginFetchRoms();
        });
        this->platformsMenu_->AddItem(item);
    }
    this->platformsMenu_->ForceReloadItems();
}

void RommApp::populateRomsMenu() {
    this->romsMenu_->ClearItems();
    for (const RomEntry& rom : this->roms_) {
        auto item = pu::ui::elm::MenuItem::New(rom.name);
        const int romId = rom.id;
        item->AddOnKey([this, romId]() {
            if (this->downloadInProgress_) {
                return;
            }
            gPending.kind = PendingKind::FetchRomDetail;
            gPending.romId = romId;
            this->downloadInProgress_ = true;
            this->setRomsStatus("Fetching ROM details...");
            this->pendingRequest_ = RommApi::fetchRomDetail(romId);
        });
        this->romsMenu_->AddItem(item);
    }
    this->romsMenu_->ForceReloadItems();
}

void RommApp::beginFetchPlatforms() {
    if (Config::instance().accessToken().empty()) {
        this->setPlatformsStatus("Missing API token. Edit sdmc:/config/romm-nx/config.json");
        return;
    }

    this->setPlatformsStatus("Loading platforms...");
    gPending.kind = PendingKind::FetchPlatforms;
    this->pendingRequest_ = RommApi::fetchPlatforms();
}

void RommApp::beginFetchRoms() {
    if (Config::instance().accessToken().empty()) {
        this->setRomsStatus("Missing API token. Edit sdmc:/config/romm-nx/config.json");
        return;
    }

    this->romsTitle_->SetText("ROMs - " + this->selectedPlatformName_);
    this->setRomsStatus("Loading ROMs...");
    gPending.kind = PendingKind::FetchRoms;
    this->pendingRequest_ = RommApi::fetchRoms(this->selectedPlatformId_);
}

void RommApp::pollPendingRequest() {
    if (!this->pendingRequest_ || !this->pendingRequest_->completed) {
        return;
    }

    const auto request = this->pendingRequest_;
    this->pendingRequest_.reset();

    switch (gPending.kind) {
        case PendingKind::FetchPlatforms: {
            if (!request->success) {
                this->setPlatformsStatus("Failed: " + (request->error.empty() ? request->body : request->error));
                break;
            }
            if (!jsonParsePlatformList(request->body, this->platforms_)) {
                this->setPlatformsStatus("Could not parse platform list.");
                break;
            }
            this->populatePlatformsMenu();
            this->setPlatformsStatus(std::to_string(this->platforms_.size()) + " platforms loaded.");
            break;
        }
        case PendingKind::FetchRoms: {
            if (!request->success) {
                this->setRomsStatus("Failed: " + (request->error.empty() ? request->body : request->error));
                break;
            }
            if (!jsonParseRomItems(request->body, this->roms_)) {
                this->setRomsStatus("Could not parse ROM list.");
                break;
            }
            this->populateRomsMenu();
            this->setRomsStatus(std::to_string(this->roms_.size()) + " ROMs. Press A to download.");
            break;
        }
        case PendingKind::FetchRomDetail: {
            if (!request->success) {
                this->downloadInProgress_ = false;
                gPending = PendingState{};
                this->setRomsStatus("Detail failed: " + (request->error.empty() ? request->body : request->error));
                break;
            }
            if (!jsonExtractFirstFileName(request->body, gPending.fileName) || gPending.fileName.empty()) {
                this->downloadInProgress_ = false;
                gPending = PendingState{};
                this->setRomsStatus("No downloadable file found for this ROM.");
                break;
            }
            gPending.outputPath = RommApi::defaultDownloadPath(this->selectedPlatformId_, gPending.fileName);
            gPending.kind = PendingKind::DownloadFile;
            this->setRomsStatus("Downloading " + gPending.fileName + "...");
            this->pendingRequest_ = HttpClient::downloadFileAsync(
                RommApi::buildContentUrl(gPending.romId, gPending.fileName),
                RommApi::authHeaders(),
                gPending.outputPath);
            return;
        }
        case PendingKind::DownloadFile: {
            this->downloadInProgress_ = false;
            if (request->success) {
                this->setRomsStatus("Saved to " + gPending.outputPath);
                this->showToast("Download complete");
            } else {
                this->setRomsStatus("Download failed: " + (request->error.empty() ? "unknown error" : request->error));
                this->showToast("Download failed");
            }
            gPending = PendingState{};
            break;
        }
        default:
            break;
    }

    if (gPending.kind != PendingKind::DownloadFile && gPending.kind != PendingKind::FetchRomDetail) {
        gPending.kind = PendingKind::None;
    }
}

void RommApp::OnLoad() {
    this->mainLayout_ = pu::ui::Layout::New();
    this->mainLayout_->SetBackgroundColor(kBackground);
    this->mainTitle_ = makeTitle(40, "RomM Client (Prototype)");
    this->mainMenu_ = makeMenu(130);

    auto browseItem = pu::ui::elm::MenuItem::New("Browse Platforms");
    browseItem->AddOnKey([this]() {
        this->showScreen(Screen::Platforms);
        this->beginFetchPlatforms();
    });
    this->mainMenu_->AddItem(browseItem);

    auto settingsItem = pu::ui::elm::MenuItem::New("Settings");
    settingsItem->AddOnKey([this]() {
        this->showScreen(Screen::Settings);
    });
    this->mainMenu_->AddItem(settingsItem);

    auto reloadItem = pu::ui::elm::MenuItem::New("Reload Config");
    reloadItem->AddOnKey([this]() {
        Config::instance().load();
        this->showToast("Config reloaded");
    });
    this->mainMenu_->AddItem(reloadItem);

    this->mainLayout_->Add(this->mainTitle_);
    this->mainLayout_->Add(this->mainMenu_);

    this->settingsLayout_ = pu::ui::Layout::New();
    this->settingsLayout_->SetBackgroundColor(kBackground);
    this->settingsTitle_ = makeTitle(40, "Settings");
    this->settingsBody_ = makeStatus(110, "");
    this->settingsLayout_->Add(this->settingsTitle_);
    this->settingsLayout_->Add(this->settingsBody_);

    this->platformsLayout_ = pu::ui::Layout::New();
    this->platformsLayout_->SetBackgroundColor(kBackground);
    this->platformsTitle_ = makeTitle(40, "Platforms");
    this->platformsStatus_ = makeStatus(95, "Press A to open a platform.");
    this->platformsMenu_ = makeMenu(140);
    this->platformsLayout_->Add(this->platformsTitle_);
    this->platformsLayout_->Add(this->platformsStatus_);
    this->platformsLayout_->Add(this->platformsMenu_);

    this->romsLayout_ = pu::ui::Layout::New();
    this->romsLayout_->SetBackgroundColor(kBackground);
    this->romsTitle_ = makeTitle(40, "ROMs");
    this->romsStatus_ = makeStatus(95, "Press A to download.");
    this->romsMenu_ = makeMenu(140);
    this->romsLayout_->Add(this->romsTitle_);
    this->romsLayout_->Add(this->romsStatus_);
    this->romsLayout_->Add(this->romsMenu_);

    this->showScreen(Screen::Main);

    this->AddRenderCallback([this]() {
        if (this->currentScreen_ == Screen::Settings && this->settingsBody_) {
            std::ostringstream body;
            body << "Host: " << Config::instance().hostUrl() << "\n";
            body << "Token: "
                 << (Config::instance().accessToken().empty() ? "(not set)" : "(set)")
                 << "\n\n";
            body << "Edit sdmc:/config/romm-nx/config.json\n";
            body << "Then choose Reload Config on the main menu.";
            this->settingsBody_->SetText(body.str());
        }
        this->pollPendingRequest();
    });

    this->SetOnInput([this](const u64 keys_down, const u64, const u64, const pu::ui::TouchPoint) {
        if (keys_down & HidNpadButton_B) {
            this->handleBack();
        } else if (keys_down & HidNpadButton_Plus) {
            this->CloseWithFadeOut(true);
        }
    });
}
