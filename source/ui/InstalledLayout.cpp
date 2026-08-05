#include "InstalledLayout.hpp"
#include "PlaceholderCover.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../model/DownloadManager.hpp"
#include "GlobalProgressBar.hpp"
#include "UninstallConfirmModal.hpp"
#include "MainMenuLayout.hpp"
#include "../i18n/I18n.hpp"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <SDL2/SDL.h>

namespace romm::ui {

    // -----------------------------------------------------------------------
    // Shared helpers
    // -----------------------------------------------------------------------

    // -----------------------------------------------------------------------
    // File-scope helpers (shared by InstalledTabBar and InstalledLayout)
    // -----------------------------------------------------------------------

    static std::string PlatformDisplayName(const std::string& slug) {
        if (slug == "psx")  return "PlayStation";
        if (slug == "ps2")  return "PlayStation 2";
        if (slug == "psp")  return "PSP";
        if (slug == "nds")  return "Nintendo DS";
        if (slug == "gb")   return "Game Boy";
        if (slug == "gbc")  return "Game Boy Color";
        if (slug == "gba")  return "Game Boy Advance";
        if (slug == "3ds")  return "Nintendo 3DS";
        if (!slug.empty()) {
            std::string s = slug;
            s[0] = (char)toupper((unsigned char)s[0]);
            return s;
        }
        return slug;
    }

    static int PlatformSortOrder(const std::string& slug) {
        if (slug == "psx") return 0;
        if (slug == "ps2") return 1;
        if (slug == "psp") return 2;
        if (slug == "nds") return 3;
        if (slug == "gb")  return 4;
        if (slug == "gbc") return 5;
        if (slug == "gba") return 6;
        if (slug == "3ds") return 7;
        return 99;
    }

    static CoverProfile CoverProfileForSlug(const std::string& slug) {
        romm::model::Platform fake;
        if (slug == "psx") fake.slug = "ps1";
        else if (slug == "ps2") fake.slug = "ps2";
        else if (slug == "psp") fake.slug = "psp";
        else if (slug == "nds") fake.slug = "nds";
        else if (slug == "gb")  fake.slug = "gb";
        else if (slug == "gbc") fake.slug = "gbc";
        else if (slug == "gba") fake.slug = "gba";
        else if (slug == "3ds") fake.slug = "3ds";
        else fake.slug = slug;
        return GetCoverProfile(fake);
    }

    // InstalledLayout member delegation
    std::string InstalledLayout::DisplayName(const std::string& slug) { return PlatformDisplayName(slug); }
    int         InstalledLayout::PlatformOrder(const std::string& slug) { return PlatformSortOrder(slug); }
    CoverProfile InstalledLayout::ProfileForSlug(const std::string& slug) { return CoverProfileForSlug(slug); }

    // -----------------------------------------------------------------------
    // InstalledTabBar
    // -----------------------------------------------------------------------

    InstalledTabBar::InstalledTabBar(s32 x, s32 y, s32 w, s32 h)
        : Element(), x(x), y(y), w(w), h(h) {}

    InstalledTabBar::~InstalledTabBar() {
        for (auto& tab : tabs) {
            if (tab.label_tex_active)   pu::ui::render::DeleteTexture(tab.label_tex_active);
            if (tab.label_tex_inactive) pu::ui::render::DeleteTexture(tab.label_tex_inactive);
        }
    }

    void InstalledTabBar::SetTabs(const std::vector<std::pair<std::string,int>>& slug_counts) {
        for (auto& tab : tabs) {
            if (tab.label_tex_active)   pu::ui::render::DeleteTexture(tab.label_tex_active);
            if (tab.label_tex_inactive) pu::ui::render::DeleteTexture(tab.label_tex_inactive);
        }
        tabs.clear();
        active_tab = 0;
        scroll_offset = 0;

        pu::ui::Color clr_active(237, 229, 251, 255);
        pu::ui::Color clr_inactive(130, 120, 165, 255);

        for (auto& [slug, count] : slug_counts) {
            Tab t;
            t.slug = slug;
            t.display_name = PlatformDisplayName(slug);
            t.game_count = count;
            t.label_tex_active   = pu::ui::render::RenderText("Orbitron@24", t.display_name, clr_active);
            t.label_tex_inactive = pu::ui::render::RenderText("Orbitron@24", t.display_name, clr_inactive);
            tabs.push_back(std::move(t));
        }
    }

    std::string InstalledTabBar::GetActiveSlug() const {
        if (active_tab < tabs.size()) return tabs[active_tab].slug;
        return "";
    }

    void InstalledTabBar::SelectPrev() {
        if (!tabs.empty() && active_tab > 0) active_tab--;
    }

    void InstalledTabBar::SelectNext() {
        if (active_tab + 1 < tabs.size()) active_tab++;
    }

    void InstalledTabBar::OnRender(pu::ui::render::Renderer::Ref& drawer,
                                   const s32 x_coord, const s32 y_coord) {
        if (tabs.empty()) return;

        // Background bar
        pu::ui::Color bar_bg(20, 22, 30, 255);
        drawer->RenderRoundedRectangleFill(bar_bg, x_coord, y_coord, w, h, 10);

        // Tab widths: divide width evenly, max 8 tabs visible at once —
        // scrolled as a window so active_tab is always among them, however
        // many platforms exist.
        size_t visible = std::min(tabs.size(), (size_t)8);
        s32 tab_w = w / (s32)visible;

        if (active_tab < scroll_offset) {
            scroll_offset = active_tab;
        } else if (active_tab >= scroll_offset + visible) {
            scroll_offset = active_tab - visible + 1;
        }
        if (scroll_offset + visible > tabs.size()) {
            scroll_offset = (tabs.size() > visible) ? (tabs.size() - visible) : 0;
        }

        for (size_t i = scroll_offset; i < scroll_offset + visible; ++i) {
            auto& tab = tabs[i];
            s32 tx = x_coord + (s32)(i - scroll_offset) * tab_w;
            bool is_active = (i == active_tab);

            if (is_active) {
                // Active: filled pill with purple accent
                pu::ui::Color active_bg(72, 52, 138, 255);
                pu::ui::Color active_border = focused
                    ? pu::ui::Color(140, 100, 240, 255)
                    : pu::ui::Color(140, 100, 240, 120);
                drawer->RenderRoundedRectangleFill(active_border, tx + 2, y_coord + 2, tab_w - 4, h - 4, 8);
                drawer->RenderRoundedRectangleFill(active_bg,     tx + 4, y_coord + 4, tab_w - 8, h - 8, 6);
                // Bottom indicator line
                drawer->RenderRoundedRectangleFill(active_border, tx + 8, y_coord + h - 4, tab_w - 16, 4, 2);
            } else {
                pu::ui::Color inactive_bg(30, 34, 43, 200);
                drawer->RenderRoundedRectangleFill(inactive_bg, tx + 3, y_coord + 3, tab_w - 6, h - 6, 7);
            }

            // Label texture centred in tab cell
            auto tex = is_active ? tab.label_tex_active : tab.label_tex_inactive;
            if (tex) {
                s32 tw_px = pu::ui::render::GetTextureWidth(tex);
                s32 th_px = pu::ui::render::GetTextureHeight(tex);
                drawer->RenderTexture(tex, tx + (tab_w - tw_px) / 2, y_coord + (h - th_px) / 2);
            }
        }

        // Scroll indicators if more tabs than visible
        if (tabs.size() > visible) {
            pu::ui::Color arrow_clr(190, 180, 225, 180);
            if (scroll_offset > 0) {
                // Left arrow ◀
                drawer->RenderCircleFill(arrow_clr, x_coord + 12, y_coord + h / 2, 8);
            }
            if (scroll_offset + visible < tabs.size()) {
                // Right arrow ▶
                s32 ax = x_coord + w - 20;
                s32 ay = y_coord + h / 2;
                drawer->RenderCircleFill(arrow_clr, ax, ay, 8);
            }
        }
    }

    // -----------------------------------------------------------------------
    // InstalledListPanel
    // -----------------------------------------------------------------------

    static std::string CleanDisplayTitle(const std::string& title) {
        size_t dot = title.find_last_of('.');
        if (dot != std::string::npos) {
            std::string ext = title.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (ext == ".chd" || ext == ".iso" || ext == ".rvz" || ext == ".zip" || 
                ext == ".nds" || ext == ".gb" || ext == ".gbc" || ext == ".gba" || 
                ext == ".bin" || ext == ".cue" || ext == ".rar" || ext == ".7z" || 
                ext == ".nro" || ext == ".elf" || ext == ".pbp") {
                return title.substr(0, dot);
            }
        }
        return title;
    }

    static void SplitRomPath(const std::string& full_path, std::string& parent_dir, std::string& filename) {
        size_t slash = full_path.find_last_of('/');
        if (slash != std::string::npos) {
            parent_dir = full_path.substr(0, slash + 1);
            filename = full_path.substr(slash + 1);
        } else {
            parent_dir = "sdmc:/";
            filename = full_path;
        }
    }

    InstalledListPanel::InstalledListPanel(s32 x, s32 y, s32 w, s32 h,
                                           std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav) {
        
        sd_card_icon = pu::ui::render::LoadImageFromFile("romfs:/sd_card.png");
        BuildStaticTextures();
    }

    // Split out of the constructor so a language change can rebuild exactly the
    // localized textures without touching the SD-card icon.
    void InstalledListPanel::BuildStaticTextures() {
        if (cover_placeholder_tex) { pu::ui::render::DeleteTexture(cover_placeholder_tex); cover_placeholder_tex = nullptr; }
        if (empty_state_tex)       { pu::ui::render::DeleteTexture(empty_state_tex);       empty_state_tex = nullptr; }
        if (label_platform_tex)    { pu::ui::render::DeleteTexture(label_platform_tex);    label_platform_tex = nullptr; }
        if (label_size_tex)        { pu::ui::render::DeleteTexture(label_size_tex);        label_size_tex = nullptr; }
        if (label_location_tex)    { pu::ui::render::DeleteTexture(label_location_tex);    label_location_tex = nullptr; }
        if (label_file_tex)        { pu::ui::render::DeleteTexture(label_file_tex);        label_file_tex = nullptr; }
        if (label_status_tex)      { pu::ui::render::DeleteTexture(label_status_tex);      label_status_tex = nullptr; }

        cover_placeholder_tex = pu::ui::render::RenderText("Ubuntu@30", romm::i18n::tr("cover.no_image"), pu::ui::Color(190, 180, 225, 255));
        empty_state_tex = pu::ui::render::RenderText("Ubuntu@30", romm::i18n::tr("installed.empty"), pu::ui::Color(130, 120, 165, 255));

        pu::ui::Color label_clr(190, 180, 225, 255);
        label_platform_tex = pu::ui::render::RenderText("Ubuntu@20", romm::i18n::tr("installed.label.platform"), label_clr);
        label_size_tex     = pu::ui::render::RenderText("Ubuntu@20", romm::i18n::tr("installed.label.size"), label_clr);
        label_location_tex = pu::ui::render::RenderText("Ubuntu@20", romm::i18n::tr("installed.label.location"), label_clr);
        label_file_tex     = pu::ui::render::RenderText("Ubuntu@20", romm::i18n::tr("installed.label.file"), label_clr);
        label_status_tex   = pu::ui::render::RenderText("Ubuntu@20", romm::i18n::tr("installed.label.status"), label_clr);
    }

    void InstalledListPanel::RefreshTranslations() {
        BuildStaticTextures();
        // Forces RebuildInfoStrip() to redo the size/status lines next frame.
        InvalidateSelectionVisuals();
    }

    void InstalledListPanel::ClearRowTextures() {
        for (auto& rt : row_texs) {
            if (rt.sel)   pu::ui::render::DeleteTexture(rt.sel);
            if (rt.unsel) pu::ui::render::DeleteTexture(rt.unsel);
        }
        row_texs.clear();
    }

    void InstalledListPanel::InvalidateSelectionVisuals() {
        ClearInfoTextures();
        cover_resolved = false;
        resolved_cover_source = "";
        resolved_is_local = false;
        resolved_is_url = false;
        current_generation_id++;
    }

    InstalledListPanel::~InstalledListPanel() {
        ClearInfoTextures();
        ClearRowTextures();
        if (sd_card_icon) {
            pu::ui::render::DeleteTexture(sd_card_icon);
            sd_card_icon = nullptr;
        }
        if (cover_placeholder_tex) {
            pu::ui::render::DeleteTexture(cover_placeholder_tex);
            cover_placeholder_tex = nullptr;
        }
        if (empty_state_tex) {
            pu::ui::render::DeleteTexture(empty_state_tex);
            empty_state_tex = nullptr;
        }
        if (label_platform_tex) { pu::ui::render::DeleteTexture(label_platform_tex); label_platform_tex = nullptr; }
        if (label_size_tex)     { pu::ui::render::DeleteTexture(label_size_tex);     label_size_tex = nullptr; }
        if (label_location_tex) { pu::ui::render::DeleteTexture(label_location_tex); label_location_tex = nullptr; }
        if (label_file_tex)     { pu::ui::render::DeleteTexture(label_file_tex);     label_file_tex = nullptr; }
        if (label_status_tex)   { pu::ui::render::DeleteTexture(label_status_tex);   label_status_tex = nullptr; }
    }

    void InstalledListPanel::ClearInfoTextures() {
        if (info_title_tex)    { pu::ui::render::DeleteTexture(info_title_tex);    info_title_tex = nullptr; }
        if (info_platform_tex) { pu::ui::render::DeleteTexture(info_platform_tex); info_platform_tex = nullptr; }
        if (info_size_tex)     { pu::ui::render::DeleteTexture(info_size_tex);     info_size_tex = nullptr; }
        if (info_location_tex) { pu::ui::render::DeleteTexture(info_location_tex); info_location_tex = nullptr; }
        if (info_file_tex)     { pu::ui::render::DeleteTexture(info_file_tex);     info_file_tex = nullptr; }
        if (info_status_tex)   { pu::ui::render::DeleteTexture(info_status_tex);   info_status_tex = nullptr; }
        info_cached_idx = SIZE_MAX;
    }

    static std::string FindLocalCover(const std::string& platform_slug, const std::string& filename) {
        std::string resolved_slug = romm::model::NormalizePlatformSlug(platform_slug);
        size_t dot = filename.find_last_of('.');
        std::string base_no_ext = (dot == std::string::npos) ? filename : filename.substr(0, dot);
        
        std::string base_dir = "sdmc:/switch/romm-nx/cache/covers/" + resolved_slug + "/";
        
        struct stat buffer;
        std::vector<std::string> exts = {".jpg", ".png", ".webp", ".jpeg"};
        for (const auto& ext : exts) {
            std::string path = base_dir + base_no_ext + ext;
            if (stat(path.c_str(), &buffer) == 0) {
                return path;
            }
        }
        return "";
    }

    static bool FileExists(const std::string& path) {
        struct stat buffer;
        return (stat(path.c_str(), &buffer) == 0);
    }

    void InstalledListPanel::RebuildInfoStrip() {
        if (games.empty() || selected_idx >= games.size()) {
            ClearInfoTextures();
            return;
        }
        if (selected_idx == info_cached_idx) return;

        ClearInfoTextures();
        const auto& g = games[selected_idx];

        pu::ui::Color title_clr(237, 229, 251, 255);
        pu::ui::Color text_clr(255, 255, 255, 255);

        std::string display_title = CleanDisplayTitle(g.title);
        info_title_tex = pu::ui::render::RenderText("Orbitron@30", display_title, title_clr, 680);
        
        // NOTE: this is intentionally its OWN table, not a call to the
        // shared PlatformDisplayName() above — that one returns the short
        // "PSP" (right for a tab label), while this info panel wants the
        // long "PLAYSTATION PORTABLE". They're two genuinely different
        // display conventions, not just duplicated code; only "3DS" was
        // actually missing here (fell through unchanged, unlike every other
        // platform which got its full name).
        std::string plat_display = platform_slug;
        std::transform(plat_display.begin(), plat_display.end(), plat_display.begin(), ::toupper);
        if (plat_display == "PSX") plat_display = "PLAYSTATION";
        else if (plat_display == "PS2") plat_display = "PLAYSTATION 2";
        else if (plat_display == "PSP") plat_display = "PLAYSTATION PORTABLE";
        else if (plat_display == "NDS") plat_display = "NINTENDO DS";
        else if (plat_display == "GB") plat_display = "GAME BOY";
        else if (plat_display == "GBC") plat_display = "GAME BOY COLOR";
        else if (plat_display == "GBA") plat_display = "GAME BOY ADVANCE";
        else if (plat_display == "3DS") plat_display = "NINTENDO 3DS";

        info_platform_tex = pu::ui::render::RenderText("Ubuntu@24", plat_display, text_clr);

        long long sz = g.size;
        struct stat sb;
        if (stat(g.install_path.c_str(), &sb) == 0 && sb.st_size > 0) sz = sb.st_size;
        std::string size_str = romm::i18n::tr("installed.size_unknown");
        if (sz > 0) {
            if (sz >= 1024LL * 1024LL * 1024LL) {
                std::ostringstream ss; ss << std::fixed << std::setprecision(2)
                    << (double)sz / (1024.0 * 1024.0 * 1024.0) << " GB";
                size_str = ss.str();
            } else {
                std::ostringstream ss; ss << std::fixed << std::setprecision(2)
                    << (double)sz / (1024.0 * 1024.0) << " MB";
                size_str = ss.str();
            }
        }
        info_size_tex = pu::ui::render::RenderText("Ubuntu@24", size_str, text_clr);

        std::string location_dir, filename_ext;
        SplitRomPath(g.install_path, location_dir, filename_ext);

        info_location_tex = pu::ui::render::RenderText("Ubuntu@24", location_dir, text_clr, 680);
        info_file_tex = pu::ui::render::RenderText("Ubuntu@24", filename_ext, text_clr, 680);
        // A game injected with "Nintendo Classics only" has no ROM on the card.
        // It still belongs in this list — it is installed, just somewhere else —
        // and uninstalling it here is what removes the injected title.
        info_status_tex = pu::ui::render::RenderText(
            "Ubuntu@24",
            romm::i18n::tr(g.switch_online_only ? "installed.status.switch_online_only"
                                                : "installed.status.installed"),
            text_clr);

        info_cached_idx = selected_idx;
    }

    void InstalledListPanel::SetGames(const std::vector<romm::model::InstalledIndexEntry>& entries,
                                      const std::string& norm_slug) {
        games = entries;
        platform_slug = norm_slug;
        ClearRowTextures();
        row_texs.resize(games.size());
        ResetSelection();
    }

    void InstalledListPanel::ResetSelection() {
        selected_idx = 0;
        scroll_offset = 0;
        InvalidateSelectionVisuals();
    }

    void InstalledListPanel::SetSelectedIdx(size_t idx) {
        if (idx < games.size()) {
            selected_idx = idx;
            int vis_rows = 10;
            if ((int)selected_idx < scroll_offset) {
                scroll_offset = (int)selected_idx;
            } else if ((int)selected_idx >= scroll_offset + vis_rows) {
                scroll_offset = (int)selected_idx - vis_rows + 1;
            }
            InvalidateSelectionVisuals();
        }
    }

    const romm::model::InstalledIndexEntry* InstalledListPanel::GetSelected() const {
        if (games.empty() || selected_idx >= games.size()) return nullptr;
        return &games[selected_idx];
    }

    static std::string TruncateTitle(const std::string& title, size_t max_chars = 32) {
        if (title.length() <= max_chars) return title;
        return title.substr(0, max_chars - 3) + "...";
    }

    void InstalledListPanel::OnRender(pu::ui::render::Renderer::Ref& drawer,
                                      const s32 x_coord, const s32 y_coord) {
        CoverCache::Instance().PollCompleted();

        if (games.empty()) {
            if (empty_state_tex) {
                s32 tw = pu::ui::render::GetTextureWidth(empty_state_tex);
                s32 th = pu::ui::render::GetTextureHeight(empty_state_tex);
                drawer->RenderTexture(empty_state_tex, x_coord + (600 - tw) / 2, y_coord + (h - th) / 2);
            }
            
            pu::ui::Color panel_bg(22, 24, 33, 255);
            drawer->RenderRoundedRectangleFill(panel_bg, x_coord + 625, y_coord, 1205, h, 8);
            
            pu::ui::Color ph_clr(30, 34, 43, 255);
            s32 ph_x = x_coord + 625 + 30;
            s32 ph_y = y_coord + 110;
            drawer->RenderRoundedRectangleFill(ph_clr, ph_x, ph_y, 380, 540, 6);
            auto plat_ph = GetPlaceholderCover(platform_slug);
            if (plat_ph) {
                DrawPlaceholderCover(drawer, plat_ph, ph_x, ph_y, 380, 540);
            } else if (cover_placeholder_tex) {
                s32 ptw = pu::ui::render::GetTextureWidth(cover_placeholder_tex);
                s32 pth = pu::ui::render::GetTextureHeight(cover_placeholder_tex);
                drawer->RenderTexture(cover_placeholder_tex, ph_x + (380 - ptw) / 2, ph_y + (540 - pth) / 2);
            }
            return;
        }

        pu::ui::Color list_bg(20, 22, 30, 255);
        drawer->RenderRoundedRectangleFill(list_bg, x_coord, y_coord, 600, h, 8);

        int vis_rows = 10;
        int total = (int)games.size();
        if ((int)selected_idx < scroll_offset) {
            scroll_offset = (int)selected_idx;
        } else if ((int)selected_idx >= scroll_offset + vis_rows) {
            scroll_offset = (int)selected_idx - vis_rows + 1;
        }

        int row_h = 70;
        for (int i = 0; i < vis_rows; ++i) {
            int idx = scroll_offset + i;
            if (idx >= total) break;

            const auto& g = games[idx];
            bool is_sel = (idx == (int)selected_idx);
            s32 row_y = y_coord + 25 + i * row_h;

            if (is_sel) {
                pu::ui::Color active_bg(72, 52, 138, 255);
                pu::ui::Color active_border(140, 100, 240, 255);
                drawer->RenderRoundedRectangleFill(active_border, x_coord + 12, row_y + 2, 560, row_h - 4, 8);
                drawer->RenderRoundedRectangleFill(active_bg,     x_coord + 15, row_y + 5, 554, row_h - 10, 6);
            } else {
                pu::ui::Color inactive_bg(30, 34, 43, 100);
                drawer->RenderRoundedRectangleFill(inactive_bg, x_coord + 15, row_y + 5, 554, row_h - 10, 6);
            }

            if (sd_card_icon) {
                pu::ui::render::TextureRenderOptions opts;
                opts.width = 30;
                opts.height = 30;
                drawer->RenderTexture(sd_card_icon, x_coord + 30, row_y + (row_h - 30) / 2, opts);
            }

            // Lazily render both title variants once per row and reuse them —
            // this used to be a TTF render + destroy per row, per frame.
            if (idx < (int)row_texs.size()) {
                auto& rt = row_texs[idx];
                if (!rt.built) {
                    // Never leave a row nameless. An empty title renders to a
                    // null texture, which left the row showing only the SD-card
                    // icon — and, because the "not built yet" test was the
                    // textures being null, retried the failed render every
                    // frame forever. Fall back through the other identifying
                    // fields, and mark the row built either way.
                    std::string source = g.title;
                    if (source.empty()) source = g.original_filename;
                    if (source.empty()) {
                        const size_t slash = g.install_path.find_last_of('/');
                        source = (slash == std::string::npos) ? g.install_path
                                                              : g.install_path.substr(slash + 1);
                    }
                    if (source.empty()) source = romm::i18n::tr("installed.unknown_title");

                    std::string disp_title = TruncateTitle(CleanDisplayTitle(source), 36);
                    rt.sel   = pu::ui::render::RenderText("Ubuntu@22", disp_title, pu::ui::Color(255, 255, 255, 255));
                    rt.unsel = pu::ui::render::RenderText("Ubuntu@22", disp_title, pu::ui::Color(190, 180, 225, 255));
                    rt.built = true;
                }
                auto title_tex = is_sel ? rt.sel : rt.unsel;
                if (title_tex) {
                    s32 th = pu::ui::render::GetTextureHeight(title_tex);
                    drawer->RenderTexture(title_tex, x_coord + 75, row_y + (row_h - th) / 2);
                }
            }
        }

        if (total > vis_rows) {
            s32 sb_x = x_coord + 600 - 16;
            s32 sb_y = y_coord + 25;
            s32 sb_h = h - 50;
            
            drawer->RenderRoundedRectangleFill(pu::ui::Color(45, 50, 62, 100), sb_x, sb_y, 6, sb_h, 3);
            
            float ratio = (float)vis_rows / total;
            s32 thumb_h = std::max((s32)(sb_h * ratio), 40);
            s32 scrollable_h = sb_h - thumb_h;
            s32 thumb_y = sb_y + (s32)(scrollable_h * ((float)scroll_offset / (total - vis_rows)));
            
            drawer->RenderRoundedRectangleFill(pu::ui::Color(140, 100, 240, 200), sb_x, thumb_y, 6, thumb_h, 3);
        }

        pu::ui::Color info_bg(22, 24, 33, 255);
        drawer->RenderRoundedRectangleFill(info_bg, x_coord + 625, y_coord, 1205, h, 8);

        const auto& g = games[selected_idx];
        CoverProfile profile = InstalledLayout::ProfileForSlug(platform_slug);

        if (!cover_resolved) {
            resolved_cover_source = "";
            resolved_is_local = false;
            resolved_is_url = false;

            if (!g.cover_path.empty()) {
                if ((g.cover_path.rfind("sdmc:/", 0) == 0 || g.cover_path.rfind("/", 0) == 0) && FileExists(g.cover_path)) {
                    resolved_cover_source = g.cover_path;
                    resolved_is_local = true;
                }
            }

            if (resolved_cover_source.empty() && g.rom_id > 0) {
                std::string platform_dir = "sdmc:/switch/romm-nx/cache/covers/" + platform_slug + "/big/";
                DIR* dir = opendir(platform_dir.c_str());
                if (dir) {
                    struct dirent* ent;
                    std::string prefix = std::to_string(g.rom_id) + "_";
                    while ((ent = readdir(dir)) != NULL) {
                        std::string fname = ent->d_name;
                        if (fname.rfind(prefix, 0) == 0) {
                            resolved_cover_source = platform_dir + fname;
                            resolved_is_local = true;
                            break;
                        }
                    }
                    closedir(dir);
                }
            }

            if (resolved_cover_source.empty() && g.rom_id > 0) {
                auto nav = nav_mgr.lock();
                if (nav) {
                    auto model = nav->GetModel();
                    if (model) {
                        // Searches hidden platforms too: an installed game keeps
                        // its cover whether or not its platform is currently
                        // shown in the browser.
                        if (const auto* game = model->FindGameByRomId(g.rom_id)) {
                            resolved_cover_source = game->cover_path_large;
                            if (resolved_cover_source.empty()) resolved_cover_source = game->cover_path;
                        }
                        if (resolved_cover_source.empty()) {
                            const auto* detail = model->GetCachedDetail(g.rom_id);
                            if (detail) {
                                resolved_cover_source = detail->path_cover_large;
                                if (resolved_cover_source.empty()) resolved_cover_source = detail->path_cover_small;
                            }
                        }
                    }
                }
                if (!resolved_cover_source.empty()) {
                    resolved_is_url = true;
                }
            }

            if (resolved_cover_source.empty()) {
                std::string fname = g.original_filename.empty() ? g.title : g.original_filename;
                std::string fallback_local = FindLocalCover(platform_slug, fname);
                if (!fallback_local.empty()) {
                    resolved_cover_source = fallback_local;
                    resolved_is_local = true;
                }
            }

            cover_resolved = true;
        }

        pu::sdl2::Texture tex = nullptr;
        CoverState cache_state = CoverState::Missing;
        if (!resolved_cover_source.empty()) {
            auto res = CoverCache::Instance().GetOrRequest(g.rom_id, platform_slug, resolved_cover_source, profile.type, "big", true);
            tex = res.texture;
            cache_state = res.state;
        }

        bool failed = (tex == nullptr) && (cache_state == CoverState::Missing || cache_state == CoverState::FailedPermanent || cache_state == CoverState::FailedTransient);
        if (failed && g.rom_id != failed_logged_rom_id) {
            std::cout << "[INSTALLED LOG] Cover load failed for: " << g.title << std::endl;
            failed_logged_rom_id = g.rom_id;
        }

        s32 actual_w = 380;
        s32 actual_h = 540;
        s32 actual_x = x_coord + 625 + 30;
        s32 actual_y = y_coord + 110;

        if (tex) {
            s32 tw = pu::ui::render::GetTextureWidth(tex);
            s32 th = pu::ui::render::GetTextureHeight(tex);
            float scale = std::min(380.0f / tw, 540.0f / th);
            actual_w = (s32)(tw * scale);
            actual_h = (s32)(th * scale);
            actual_x = x_coord + 625 + 30 + (380 - actual_w) / 2;
            actual_y = y_coord + 110 + (540 - actual_h) / 2;
        }

        pu::ui::Color border_clr(45, 50, 62, 255);
        drawer->RenderRoundedRectangleFill(border_clr, actual_x - 3, actual_y - 3, actual_w + 6, actual_h + 6, 8);

        pu::ui::Color ph_clr(30, 34, 43, 255);
        drawer->RenderRoundedRectangleFill(ph_clr, actual_x, actual_y, actual_w, actual_h, 6);

        if (tex) {
            pu::ui::render::TextureRenderOptions opts;
            opts.width  = actual_w;
            opts.height = actual_h;
            drawer->RenderTexture(tex, actual_x, actual_y, opts);
        } else {
            auto plat_ph = GetPlaceholderCover(platform_slug);
            if (plat_ph) {
                DrawPlaceholderCover(drawer, plat_ph, actual_x, actual_y, actual_w, actual_h);
            } else if (cover_placeholder_tex) {
                s32 ptw = pu::ui::render::GetTextureWidth(cover_placeholder_tex);
                s32 pth = pu::ui::render::GetTextureHeight(cover_placeholder_tex);
                drawer->RenderTexture(cover_placeholder_tex, actual_x + (actual_w - ptw) / 2, actual_y + (actual_h - pth) / 2);
            }
        }

        RebuildInfoStrip();
        s32 text_col_x = x_coord + 625 + 450;
        
        if (info_title_tex) {
            drawer->RenderTexture(info_title_tex, x_coord + 625 + 30, y_coord + 35);
        }

        s32 curr_y = y_coord + 110;

        auto render_pair = [&](pu::sdl2::Texture lbl, pu::sdl2::Texture val, int space) {
            if (lbl) {
                drawer->RenderTexture(lbl, text_col_x, curr_y);
                curr_y += 26;
            }
            if (val) {
                drawer->RenderTexture(val, text_col_x, curr_y);
            }
            curr_y += space;
        };

        render_pair(label_platform_tex, info_platform_tex, 80);
        render_pair(label_size_tex,     info_size_tex,     80);
        render_pair(label_location_tex, info_location_tex, 80);
        render_pair(label_file_tex,     info_file_tex,     80);
        render_pair(label_status_tex,   info_status_tex,   80);
    }

    bool InstalledListPanel::HandleInput(const u64 keys_down) {
        if (games.empty()) return false;
        size_t total = games.size();

        if (keys_down & HidNpadButton_Up || keys_down & HidNpadButton_StickLUp) {
            // InvalidateSelectionVisuals (not just ClearInfoTextures): the
            // cover-resolution state must reset with the selection, otherwise
            // the panel keeps showing the previous game's cover while scrolling.
            if (selected_idx > 0) { selected_idx--; InvalidateSelectionVisuals(); }
        } else if (keys_down & HidNpadButton_Down || keys_down & HidNpadButton_StickLDown) {
            if (selected_idx + 1 < total) { selected_idx++; InvalidateSelectionVisuals(); }
        } else if (keys_down & HidNpadButton_A) {
            return true;
        }
        return false;
    }

    // -----------------------------------------------------------------------
    // InstalledLayout
    // -----------------------------------------------------------------------

    InstalledLayout::InstalledLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {

        this->SetBackgroundColor(pu::ui::Color(16, 18, 22, 255));

        tab_bar = InstalledTabBar::New(40, 160, 1840, 60);
        this->Add(tab_bar);

        // Polished 1920x1080 panel mapping (Left margin 45, Top Y 230, Left Width 600, Gap 25, Right Width 1205, Height 750)
        list_panel = InstalledListPanel::New(45, 230, 600, 750, nav);
        this->Add(list_panel);

        hint_text = pu::ui::elm::TextBlock::New(0, 1080 - 65, romm::i18n::tr("hint.installed"));
        hint_text->SetFont("Ubuntu@26");
        hint_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        hint_text->SetHorizontalAlign(pu::ui::elm::HorizontalAlign::Center);
        this->Add(hint_text);

        auto modal = romm::ui::UninstallConfirmModal::New(nav);
        this->Add(modal);

        auto global_progress = GlobalProgressBar::New(40, 20, 460, 56, nav);
        this->Add(global_progress);

        tab_bar->SetFocused(false);
        list_panel->SetFocused(true);

        RebuildData();
    }

    void InstalledLayout::RebuildData() {
        std::string current_slug = "";
        if (tab_bar && tab_bar->TabCount() > 0) {
            current_slug = tab_bar->GetActiveSlug();
        }

        int saved_rom_id = 0;
        if (list_panel) {
            const auto* selected_game = list_panel->GetSelected();
            if (selected_game) {
                saved_rom_id = selected_game->rom_id;
            }
        }

        grouped.clear();
        tab_slugs.clear();

        auto& dl_mgr = romm::model::DownloadManager::Instance();
        dl_mgr.ReconcileInstalledIndex();
        auto index = dl_mgr.GetInstalledIndex();

        for (auto& [key, entry] : index) {
            std::string norm = romm::model::NormalizePlatformSlug(entry.platform_slug);
            grouped[norm].push_back(entry);
        }

        for (auto& [slug, list] : grouped) {
            std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
                return a.title < b.title;
            });
        }

        std::vector<std::string> slugs;
        for (auto& [slug, _] : grouped) slugs.push_back(slug);
        std::sort(slugs.begin(), slugs.end(), [](const std::string& a, const std::string& b) {
            int oa = PlatformSortOrder(a);
            int ob = PlatformSortOrder(b);
            return (oa != ob) ? oa < ob : a < b;
        });

        for (auto& s : slugs) {
            tab_slugs.push_back({s, (int)grouped[s].size()});
        }

        tab_bar->SetTabs(tab_slugs);

        size_t restore_idx = 0;
        if (!current_slug.empty()) {
            for (size_t i = 0; i < tab_slugs.size(); ++i) {
                if (tab_slugs[i].first == current_slug) {
                    restore_idx = i;
                    break;
                }
            }
        }

        if (restore_idx >= tab_slugs.size() && !tab_slugs.empty()) {
            restore_idx = tab_slugs.size() - 1;
        }

        if (!tab_slugs.empty()) {
            tab_bar->SetActiveTab(restore_idx);
            
            const std::string& slug = tab_slugs[restore_idx].first;
            auto it = grouped.find(slug);
            std::vector<romm::model::InstalledIndexEntry> filtered_games;
            if (it != grouped.end()) {
                filtered_games = it->second;
            }
            
            list_panel->SetGames(filtered_games, slug);
            
            size_t sel_idx = 0;
            if (saved_rom_id > 0) {
                for (size_t i = 0; i < filtered_games.size(); ++i) {
                    if (filtered_games[i].rom_id == saved_rom_id) {
                        sel_idx = i;
                        break;
                    }
                }
            }
            list_panel->SetSelectedIdx(sel_idx);
        } else {
            list_panel->SetGames({}, "");
        }
    }

    void InstalledLayout::SwitchToTab(size_t idx) {
        if (idx >= tab_slugs.size()) return;
        const std::string& slug = tab_slugs[idx].first;
        
        auto it = grouped.find(slug);
        if (it != grouped.end()) {
            list_panel->SetGames(it->second, slug);
        } else {
            list_panel->SetGames({}, slug);
        }
    }

    void InstalledLayout::OnSelectionUpdated() {}

    void InstalledLayout::RefreshTranslations() {
        if (hint_text) hint_text->SetText(romm::i18n::tr("hint.installed"));
        if (list_panel) list_panel->RefreshTranslations();
        // Tab labels are platform names (RomM/catalogue identities), so the tab
        // bar deliberately has nothing to re-translate.
    }

    void InstalledLayout::ForceRefresh() {
        RebuildData();
    }

    void InstalledLayout::HandleInput(const u64 keys_down, const u64 keys_up,
                                      const u64 keys_held,
                                      const pu::ui::TouchPoint) {
        bool tab_prev = (keys_down & HidNpadButton_L) || (keys_down & HidNpadButton_ZL);
        bool tab_next = (keys_down & HidNpadButton_R) || (keys_down & HidNpadButton_ZR);

        if (tab_prev && tab_bar->GetActiveTab() > 0) {
            tab_bar->SelectPrev();
            SwitchToTab(tab_bar->GetActiveTab());
            return;
        }
        if (tab_next && tab_bar->GetActiveTab() + 1 < tab_bar->TabCount()) {
            tab_bar->SelectNext();
            SwitchToTab(tab_bar->GetActiveTab());
            return;
        }

        if (keys_down & HidNpadButton_B) {
            auto nav = nav_mgr.lock();
            if (nav) {
                nav->SetCurrentScreen(romm::navigation::Screen::MainMenu);
                nav->GetApp()->LoadLayout(nav->GetMainMenuLayout());
            }
            return;
        }

        bool uninstall_triggered = list_panel->HandleInput(keys_down);
        if (uninstall_triggered) {
            const auto* entry = list_panel->GetSelected();
            if (entry) {
                auto nav = nav_mgr.lock();
                if (nav) {
                    romm::navigation::UninstallModalPayload p;
                    p.rom_id        = entry->rom_id;
                    p.platform_slug = entry->platform_slug;
                    p.title         = entry->title;
                    p.filename      = entry->original_filename.empty()
                                          ? entry->title : entry->original_filename;
                    p.cover_path    = entry->cover_path;
                    p.source_screen = romm::navigation::Screen::Installed;
                    nav->ShowUninstallModal(p);
                }
            }
        }
    }

} // namespace romm::ui
