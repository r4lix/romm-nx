#include "NsoSnesModal.hpp"

#include "../i18n/I18n.hpp"
#include "../model/PlatformCatalog.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../nso/NsoSnesInstaller.hpp"

#include <cstdlib>
#include <iostream>

namespace romm::ui {

    namespace {

        const pu::ui::Color kBorder(45, 50, 62, 255);
        const pu::ui::Color kBackground(16, 18, 22, 255);
        const pu::ui::Color kPanel(24, 27, 33, 255);
        const pu::ui::Color kText(237, 229, 251, 255);
        const pu::ui::Color kDim(150, 150, 162, 255);
        const pu::ui::Color kAccent(190, 180, 225, 255);
        const pu::ui::Color kOk(120, 214, 140, 255);
        const pu::ui::Color kBad(235, 110, 110, 255);
        const pu::ui::Color kWarn(240, 190, 90, 255);
        const pu::ui::Color kSelection(58, 46, 92, 255);

        constexpr s32 kModalW = 1520;
        constexpr s32 kModalH = 880;
        constexpr size_t kPickerVisibleRows = 13;
        constexpr size_t kOverviewRowCount = 2; // install, restore

        void DrawText(pu::ui::render::Renderer::Ref& drawer, const char* font,
                      const std::string& text, s32 x, s32 y, pu::ui::Color color, s32 max_width = 0) {
            if (text.empty()) return;
            auto tex = max_width > 0 ? pu::ui::render::RenderText(font, text, color, max_width)
                                     : pu::ui::render::RenderText(font, text, color);
            if (!tex) return;
            drawer->RenderTexture(tex, x, y);
            pu::ui::render::DeleteTexture(tex);
        }

        // Byte offsets here are cut points for text that goes to SDL_ttf, and a
        // RomM title or publisher is UTF-8 — slicing mid-sequence would hand the
        // renderer a truncated code point. These snap an offset outwards to the
        // nearest sequence boundary (continuation bytes are 10xxxxxx).
        size_t Utf8Floor(const std::string& value, size_t index) {
            while (index > 0 && (static_cast<unsigned char>(value[index]) & 0xC0) == 0x80) --index;
            return index;
        }

        size_t Utf8Ceil(const std::string& value, size_t index) {
            while (index < value.size() && (static_cast<unsigned char>(value[index]) & 0xC0) == 0x80) ++index;
            return index;
        }

        // Keeps a long path or detail line readable on one row by eliding the
        // middle. `max_chars` is a byte budget, so a heavily accented string
        // elides a little sooner than a plain ASCII one — which is fine here.
        std::string Elide(const std::string& value, size_t max_chars) {
            if (value.size() <= max_chars || max_chars < 8) return value;
            const size_t head = Utf8Floor(value, max_chars / 2 - 2);
            const size_t tail_start = Utf8Ceil(value, value.size() - (max_chars - (max_chars / 2 - 2) - 3));
            return value.substr(0, head) + "..." + value.substr(tail_start);
        }

        std::string StatusGlyph(romm::nso::NsoStepStatus status) {
            switch (status) {
                case romm::nso::NsoStepStatus::Done: return "[OK]";
                case romm::nso::NsoStepStatus::Failed: return "[!!]";
                case romm::nso::NsoStepStatus::Running: return "[..]";
                case romm::nso::NsoStepStatus::Skipped: return "[--]";
                default: return "[  ]";
            }
        }

        pu::ui::Color StatusColor(romm::nso::NsoStepStatus status) {
            switch (status) {
                case romm::nso::NsoStepStatus::Done: return kOk;
                case romm::nso::NsoStepStatus::Failed: return kBad;
                case romm::nso::NsoStepStatus::Running: return kAccent;
                case romm::nso::NsoStepStatus::Skipped: return kWarn;
                default: return kDim;
            }
        }

    } // namespace

    NsoSnesModal::NsoSnesModal(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element::Element(), nav_mgr(nav) {}

    void NsoSnesModal::Show() {
        active = true;
        page = Page::Overview;
        overview_row = 0;
        picker_row = 0;
        picker_scroll = 0;
        RefreshDetection();
        LoadSnesLibrary();
    }

    void NsoSnesModal::Hide() {
        active = false;
        pending_fetch.reset();
    }

    void NsoSnesModal::RefreshDetection() {
        romm::nso::NsoSnesInstaller::Instance().RefreshDetection();
    }

    void NsoSnesModal::LoadSnesLibrary() {
        snes_games.clear();
        pending_fetch.reset();
        library_status.clear();

        auto nav = nav_mgr.lock();
        if (!nav) return;
        auto model = nav->GetModel();
        if (!model) return;

        const romm::model::Platform* snes = nullptr;
        for (const auto& platform : model->GetAllPlatforms()) {
            if (romm::model::ResolvePlatformIdentity(platform.slug, platform.name) == "snes") {
                snes = &platform;
                break;
            }
        }
        if (!snes) {
            library_status = romm::i18n::tr("nso.snes.library.no_platform");
            return;
        }

        if (!snes->games.empty()) {
            snes_games = snes->games;
            return;
        }

        const long platform_id = std::strtol(snes->id.c_str(), nullptr, 10);
        if (platform_id <= 0) {
            library_status = romm::i18n::tr("nso.snes.library.no_platform");
            return;
        }
        library_status = romm::i18n::tr("nso.snes.library.loading");
        pending_fetch = romm::model::RommApi::fetchRomsAsync((int)platform_id, ++fetch_request_id);
    }

    void NsoSnesModal::PollLibraryFetch() {
        if (!pending_fetch || !pending_fetch->completed) return;
        if (pending_fetch->success) {
            snes_games = pending_fetch->games;
            library_status = snes_games.empty() ? romm::i18n::tr("nso.snes.library.empty") : "";
        } else {
            library_status = romm::i18n::tr("nso.snes.library.failed");
        }
        pending_fetch.reset();
    }

    void NsoSnesModal::StartInstallForSelection() {
        if (picker_row >= snes_games.size()) return;
        const auto& game = snes_games[picker_row];

        romm::nso::NsoInstallRequest request;
        request.rom_id = game.id;
        request.title = game.title;
        // file_id, filename and cover URL are resolved by the pipeline's own
        // metadata fetch, which is the only place that knows the /api/roms
        // response shape for multi-file entries.
        std::cout << "[NSO-SNES] Install requested for rom_id=" << game.id
                  << " title=" << game.title << std::endl;

        page = Page::Progress;
        romm::nso::NsoSnesInstaller::Instance().StartInstall(request);
    }

    void NsoSnesModal::HandleInput(u64 keys_down) {
        if (!active) return;
        auto& installer = romm::nso::NsoSnesInstaller::Instance();

        if (page == Page::Progress) {
            if (installer.IsBusy()) return; // block every input while files are moving
            if (keys_down & (HidNpadButton_B | HidNpadButton_A)) {
                page = Page::Overview;
                RefreshDetection();
            }
            return;
        }

        if (page == Page::Picker) {
            if (keys_down & HidNpadButton_B) {
                page = Page::Overview;
                return;
            }
            if (snes_games.empty()) return;

            if (keys_down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
                if (picker_row > 0) --picker_row;
            } else if (keys_down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
                if (picker_row + 1 < snes_games.size()) ++picker_row;
            } else if (keys_down & (HidNpadButton_L | HidNpadButton_ZL)) {
                picker_row = (picker_row > kPickerVisibleRows) ? picker_row - kPickerVisibleRows : 0;
            } else if (keys_down & (HidNpadButton_R | HidNpadButton_ZR)) {
                picker_row = (picker_row + kPickerVisibleRows < snes_games.size())
                                 ? picker_row + kPickerVisibleRows
                                 : snes_games.size() - 1;
            } else if (keys_down & HidNpadButton_A) {
                StartInstallForSelection();
                return;
            }

            if (picker_row < picker_scroll) picker_scroll = picker_row;
            if (picker_row >= picker_scroll + kPickerVisibleRows) {
                picker_scroll = picker_row - kPickerVisibleRows + 1;
            }
            return;
        }

        // Overview
        if (keys_down & HidNpadButton_B) {
            Hide();
            return;
        }
        if (keys_down & (HidNpadButton_Up | HidNpadButton_StickLUp)) {
            if (overview_row > 0) --overview_row;
        } else if (keys_down & (HidNpadButton_Down | HidNpadButton_StickLDown)) {
            if (overview_row + 1 < kOverviewRowCount) ++overview_row;
        } else if (keys_down & HidNpadButton_X) {
            LoadSnesLibrary();
            RefreshDetection();
        } else if (keys_down & HidNpadButton_A) {
            const auto detection = installer.GetDetection();
            if (overview_row == 0) {
                if (!detection.found) return;
                page = Page::Picker;
                PollLibraryFetch();
            } else if (overview_row == 1) {
                if (!installer.HasBackup()) return;
                page = Page::Progress;
                installer.StartRestore();
            }
        }
    }

    void NsoSnesModal::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32, const s32) {
        if (!active) return;
        PollLibraryFetch();

        auto& installer = romm::nso::NsoSnesInstaller::Instance();
        const auto detection = installer.GetDetection();

        drawer->RenderRectangleFill(pu::ui::Color(0, 0, 0, 205), 0, 0, 1920, 1080);

        const s32 mx = (1920 - kModalW) / 2;
        const s32 my = (1080 - kModalH) / 2;
        drawer->RenderRoundedRectangleFill(kBorder, mx, my, kModalW, kModalH, 16);
        drawer->RenderRoundedRectangleFill(kBackground, mx + 4, my + 4, kModalW - 8, kModalH - 8, 12);

        DrawText(drawer, "Orbitron@30", romm::i18n::tr("nso.snes.title"), mx + 40, my + 32, kText);
        DrawText(drawer, "Ubuntu@20", romm::i18n::tr("nso.snes.experimental"), mx + 40, my + 74, kWarn);

        const s32 body_y = my + 118;

        // --- Detection panel, always visible ------------------------------
        drawer->RenderRoundedRectangleFill(kPanel, mx + 32, body_y, kModalW - 64, 168, 10);
        {
            const s32 col1 = mx + 52;
            const s32 col2 = mx + 330;
            s32 row = body_y + 16;
            const s32 line = 30;

            auto pair = [&](const std::string& label, const std::string& value, pu::ui::Color color) {
                DrawText(drawer, "Ubuntu@20", label, col1, row, kDim);
                DrawText(drawer, "Ubuntu@20", value, col2, row, color);
                row += line;
            };

            if (detection.found) {
                pair(romm::i18n::tr("nso.snes.detect.title_id"), detection.title_id, kOk);
                pair(romm::i18n::tr("nso.snes.detect.database"), Elide(detection.database_path, 96), kText);
                pair(romm::i18n::tr("nso.snes.detect.entries"),
                     romm::i18n::format("nso.snes.detect.entries_value",
                                        {{"entries", std::to_string(detection.entry_count)},
                                         {"assets", std::to_string(detection.injected_asset_dirs)}}),
                     kText);
                std::string langs;
                for (size_t i = 0; i < detection.strings_languages.size(); ++i) {
                    if (i) langs += ", ";
                    langs += detection.strings_languages[i];
                }
                pair(romm::i18n::tr("nso.snes.detect.strings"), langs.empty() ? "-" : langs, kText);
                pair(romm::i18n::tr("nso.snes.detect.mod"),
                     romm::i18n::tr(detection.has_exefs_mod ? "nso.snes.detect.mod_present"
                                                            : "nso.snes.detect.mod_absent"),
                     detection.has_exefs_mod ? kOk : kWarn);
            } else {
                DrawText(drawer, "Ubuntu@22", romm::i18n::tr("nso.snes.detect.not_found"), col1, row, kBad);
                row += line + 6;
                DrawText(drawer, "Ubuntu@20", romm::i18n::tr("nso.snes.detect.not_found_hint"),
                         col1, row, kDim, kModalW - 120);
            }
        }

        const s32 content_y = body_y + 194;
        const s32 content_h = kModalH - (content_y - my) - 84;
        drawer->RenderRoundedRectangleFill(kPanel, mx + 32, content_y, kModalW - 64, content_h, 10);

        std::string hint;

        if (page == Page::Overview) {
            const std::string rows[kOverviewRowCount] = {
                romm::i18n::tr("nso.snes.action.install"),
                romm::i18n::tr("nso.snes.action.restore")
            };
            const std::string backup = installer.LatestBackupPath();
            const std::string values[kOverviewRowCount] = {
                detection.found ? (snes_games.empty()
                                       ? (library_status.empty() ? romm::i18n::tr("nso.snes.library.empty") : library_status)
                                       : romm::i18n::format("nso.snes.library.count", {{"count", std::to_string(snes_games.size())}}))
                                : romm::i18n::tr("nso.snes.detect.not_found"),
                backup.empty() ? romm::i18n::tr("nso.snes.restore.none") : Elide(backup, 64)
            };
            const bool enabled[kOverviewRowCount] = {
                detection.found && !snes_games.empty(),
                !backup.empty()
            };

            s32 row_y = content_y + 20;
            for (size_t i = 0; i < kOverviewRowCount; ++i) {
                if (i == overview_row) {
                    drawer->RenderRoundedRectangleFill(kSelection, mx + 44, row_y - 6, kModalW - 88, 44, 8);
                }
                DrawText(drawer, "Ubuntu@22", rows[i], mx + 64, row_y, enabled[i] ? kText : kDim);
                DrawText(drawer, "Ubuntu@20", values[i], mx + 620, row_y + 2, enabled[i] ? kAccent : kDim);
                row_y += 54;
            }

            row_y += 18;
            DrawText(drawer, "Ubuntu@20", romm::i18n::tr("nso.snes.scope"), mx + 64, row_y, kDim, kModalW - 140);
            row_y += 128;
            DrawText(drawer, "Ubuntu@18",
                     romm::i18n::format("nso.snes.log_path", {{"path", installer.LogPath()}}),
                     mx + 64, row_y, kDim);

            hint = romm::i18n::tr("nso.snes.hint.overview");
        }
        else if (page == Page::Picker) {
            DrawText(drawer, "Ubuntu@22", romm::i18n::tr("nso.snes.picker.header"), mx + 56, content_y + 14, kAccent);

            if (snes_games.empty()) {
                DrawText(drawer, "Ubuntu@22",
                         library_status.empty() ? romm::i18n::tr("nso.snes.library.empty") : library_status,
                         mx + 56, content_y + 64, kWarn);
            } else {
                s32 row_y = content_y + 56;
                for (size_t i = picker_scroll; i < snes_games.size() && i < picker_scroll + kPickerVisibleRows; ++i) {
                    if (i == picker_row) {
                        drawer->RenderRoundedRectangleFill(kSelection, mx + 44, row_y - 5, kModalW - 88, 38, 8);
                    }
                    // RomM data: rendered exactly as the server sent it.
                    DrawText(drawer, "Ubuntu@20", snes_games[i].title, mx + 64, row_y, kText);
                    DrawText(drawer, "Ubuntu@18", Elide(snes_games[i].fs_name, 46), mx + 1000, row_y + 2, kDim);
                    row_y += 42;
                }
                DrawText(drawer, "Ubuntu@18",
                         romm::i18n::format("nso.snes.picker.position",
                                            {{"index", std::to_string(picker_row + 1)},
                                             {"total", std::to_string(snes_games.size())}}),
                         mx + 56, content_y + content_h - 34, kDim);
            }
            hint = romm::i18n::tr("nso.snes.hint.picker");
        }
        else {
            const auto steps = installer.GetSteps();
            const auto state = installer.GetState();

            s32 row_y = content_y + 16;
            for (const auto& step : steps) {
                const pu::ui::Color color = StatusColor(step.status);
                DrawText(drawer, "Ubuntu@18", StatusGlyph(step.status), mx + 56, row_y, color);
                DrawText(drawer, "Ubuntu@18", step.name, mx + 110, row_y, color);
                if (!step.detail.empty() && step.status != romm::nso::NsoStepStatus::Failed) {
                    DrawText(drawer, "Ubuntu@18", Elide(step.detail, 74), mx + 620, row_y, kDim);
                }
                row_y += 28;
            }

            row_y += 12;
            if (state == romm::nso::NsoPipelineState::Failed) {
                DrawText(drawer, "Ubuntu@20", installer.GetError(), mx + 56, row_y, kBad, kModalW - 140);
            } else if (state == romm::nso::NsoPipelineState::Success) {
                DrawText(drawer, "Ubuntu@20", installer.GetSummary(), mx + 56, row_y, kOk, kModalW - 140);
            }

            hint = installer.IsBusy() ? romm::i18n::tr("nso.snes.hint.working")
                                      : romm::i18n::tr("nso.snes.hint.progress");
        }

        DrawText(drawer, "Ubuntu@20", hint, mx + 40, my + kModalH - 56, kAccent);
    }

}
