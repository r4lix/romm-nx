#include "FileBrowserLayout.hpp"
#include "../navigation/NavigationManager.hpp"
#include "../model/ConfigManager.hpp"
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <sys/statvfs.h>
#include <switch.h>
#include <arpa/inet.h>

#include "MainMenuLayout.hpp"

namespace romm::ui {

    static std::string FormatSize(long long bytes);

    std::string GetSafeFont(const std::string& requested, const std::string& fallback) {
        if (pu::ui::render::GetTextHeight(requested, "a") > 0) {
            return requested;
        }
        std::cout << "[FILE_BROWSER] missing_font requested=" << requested << " fallback=" << fallback << std::endl;
        return fallback;
    }

    FileBrowserPane::FileBrowserPane(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav), cancel_scan(false),
          last_stats_time(std::chrono::steady_clock::now() - std::chrono::seconds(10)), current_mount_idx(0) {
        
        std::cout << "[FILE_BROWSER] init_start" << std::endl;
        
        loading_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), "Loading...", pu::ui::Color(190, 180, 225, 255));
        empty_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), "Folder is empty.", pu::ui::Color(190, 180, 225, 255));
        options_title_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), "File Options", pu::ui::Color(255, 255, 255, 255));
        confirm_title_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), "Delete Selected files?", pu::ui::Color(255, 255, 255, 255));

        mkdir("sdmc:/switch", 0777);
        mkdir("sdmc:/switch/romm-nx", 0777);

        RefreshStats();
        BuildLocationsList();
        
        // Log opening default path
        std::cout << "[FILE_BROWSER] open path=sdmc:/" << std::endl;
        LoadDirectoryAsync("sdmc:/");

        std::cout << "[FILE_BROWSER] init_done" << std::endl;
    }

    FileBrowserPane::~FileBrowserPane() {
        CancelPendingScan();
        ClearTextures();
    }

    void FileBrowserPane::ClearTextures() {
        for (auto& loc : locations) {
            if (loc.text_tex_selected) pu::ui::render::DeleteTexture(loc.text_tex_selected);
            if (loc.text_tex_unselected) pu::ui::render::DeleteTexture(loc.text_tex_unselected);
        }
        locations.clear();

        std::lock_guard<std::mutex> lock(data_mutex);
        for (auto& item : loaded_items) {
            if (item.text_tex_selected) pu::ui::render::DeleteTexture(item.text_tex_selected);
            if (item.text_tex_unselected) pu::ui::render::DeleteTexture(item.text_tex_unselected);
            if (item.details_tex_selected) pu::ui::render::DeleteTexture(item.details_tex_selected);
            if (item.details_tex_unselected) pu::ui::render::DeleteTexture(item.details_tex_unselected);
        }
        loaded_items.clear();

        for (auto& item : retired_items) {
            if (item.text_tex_selected) pu::ui::render::DeleteTexture(item.text_tex_selected);
            if (item.text_tex_unselected) pu::ui::render::DeleteTexture(item.text_tex_unselected);
            if (item.details_tex_selected) pu::ui::render::DeleteTexture(item.details_tex_selected);
            if (item.details_tex_unselected) pu::ui::render::DeleteTexture(item.details_tex_unselected);
        }
        retired_items.clear();

        net1_cache.Clear();
        net2_cache.Clear();
        sd_cache.Clear();
        sys_cache.Clear();
        pos_cache.Clear();
        error_cache.Clear();
        for (auto& [key, tex] : button_text_cache) {
            if (tex) pu::ui::render::DeleteTexture(tex);
        }
        button_text_cache.clear();

        for (auto& tex : options_texs_selected) pu::ui::render::DeleteTexture(tex);
        for (auto& tex : options_texs_unselected) pu::ui::render::DeleteTexture(tex);
        options_texs_selected.clear();
        options_texs_unselected.clear();

        for (auto& tex : confirm_texs_selected) pu::ui::render::DeleteTexture(tex);
        for (auto& tex : confirm_texs_unselected) pu::ui::render::DeleteTexture(tex);
        confirm_texs_selected.clear();
        confirm_texs_unselected.clear();

        for (auto& tex : properties_texs) pu::ui::render::DeleteTexture(tex);
        properties_texs.clear();

        if (loading_tex) { pu::ui::render::DeleteTexture(loading_tex); loading_tex = nullptr; }
        if (empty_tex) { pu::ui::render::DeleteTexture(empty_tex); empty_tex = nullptr; }
        if (options_title_tex) { pu::ui::render::DeleteTexture(options_title_tex); options_title_tex = nullptr; }
        if (confirm_title_tex) { pu::ui::render::DeleteTexture(confirm_title_tex); confirm_title_tex = nullptr; }
        if (mount_val_tex_selected) { pu::ui::render::DeleteTexture(mount_val_tex_selected); mount_val_tex_selected = nullptr; }
        if (mount_val_tex_unselected) { pu::ui::render::DeleteTexture(mount_val_tex_unselected); mount_val_tex_unselected = nullptr; }

        detail_texs.Clear();
    }

    bool FileBrowserPane::DirectoryExists(const std::string& path) {
        struct stat st;
        if (stat(path.c_str(), &st) == 0) {
            return S_ISDIR(st.st_mode);
        }
        return false;
    }

    std::string FileBrowserPane::NormalizePath(const std::string& path) {
        std::string clean = path;
        for (auto& c : clean) {
            if (c == '\\') c = '/';
        }

        std::string prefix = "";
        if (clean.rfind("sdmc:/", 0) == 0) {
            prefix = "sdmc:/";
            clean = clean.substr(6);
        }

        std::vector<std::string> segments;
        std::stringstream ss(clean);
        std::string seg;
        while (std::getline(ss, seg, '/')) {
            if (seg.empty() || seg == ".") continue;
            if (seg == "..") {
                if (!segments.empty()) {
                    segments.pop_back();
                }
            } else {
                segments.push_back(seg);
            }
        }

        std::string result = prefix;
        for (size_t i = 0; i < segments.size(); ++i) {
            result += segments[i];
            if (i + 1 < segments.size()) {
                result += "/";
            }
        }

        if (!result.empty() && result.back() != '/' && (path.back() == '/' || segments.empty())) {
            result += "/";
        }

        return result;
    }

    bool FileBrowserPane::IsWritablePath(const std::string& path) {
        std::string normalized = NormalizePath(path);

        // Block path traversal tricks completely
        if (normalized.find("..") != std::string::npos) {
            return false;
        }

        // Only SD card paths are ever writable
        if (normalized.rfind("sdmc:/", 0) != 0) {
            return false;
        }

        auto& config = romm::model::ConfigManager::Instance();
        if (config.FileBrowserWriteAnywhere()) {
            return true;
        }

        // Default policy: writes only inside the ROMs tree — sdmc:/roms/ plus
        // any custom per-platform ROM path configured in Settings.
        if (normalized == "sdmc:/roms" || normalized.rfind("sdmc:/roms/", 0) == 0) {
            return true;
        }
        static const char* kRomSlugs[] = {"psx", "psp", "nds", "gb", "gbc", "gba", "ps2"};
        for (const char* slug : kRomSlugs) {
            std::string root = config.GetRomPath(slug);
            if (root.empty()) continue;
            if (root.back() == '/') {
                if (normalized == root.substr(0, root.size() - 1) || normalized.rfind(root, 0) == 0) {
                    return true;
                }
            } else if (normalized == root || normalized.rfind(root + "/", 0) == 0) {
                return true;
            }
        }
        return false;
    }

    bool FileBrowserPane::IsValidName(const std::string& name) {
        if (name.empty()) return false;
        if (name == "." || name == "..") return false;

        std::string forbidden = "/\\:*?\"<>|";
        for (char c : name) {
            if (forbidden.find(c) != std::string::npos) {
                return false;
            }
        }

        if (name.find("..") != std::string::npos) {
            return false;
        }

        return true;
    }

    void FileBrowserPane::BuildLocationsList() {
        for (auto& loc : locations) {
            if (loc.text_tex_selected) pu::ui::render::DeleteTexture(loc.text_tex_selected);
            if (loc.text_tex_unselected) pu::ui::render::DeleteTexture(loc.text_tex_unselected);
        }
        locations.clear();

        struct LocDef {
            std::string name;
            std::string path;
        };

        std::vector<LocDef> defs = {
            {"SD Card Root", "sdmc:/"},
            {"romm-nx Folder", "sdmc:/switch/romm-nx/"},
            {"ROMs Folder", "sdmc:/roms/"}
        };

        auto& config = romm::model::ConfigManager::Instance();
        std::string psp_path = config.GetRomPath("psp");
        if (!psp_path.empty() && psp_path != "sdmc:/roms/psp/") {
            defs.push_back({"PSP ROMs Path", psp_path});
        }

        pu::ui::Color selected_clr(237, 229, 251, 255);
        pu::ui::Color unselected_clr(190, 180, 225, 255);
        pu::ui::Color missing_clr(231, 76, 60, 255);

        for (const auto& d : defs) {
            LocationEntry loc;
            loc.name = d.name;
            loc.path = d.path;
            loc.exists = DirectoryExists(d.path);

            std::string display_name = d.name;
            if (!loc.exists) {
                display_name += " (missing)";
            }

            pu::ui::Color text_color = loc.exists ? unselected_clr : missing_clr;
            pu::ui::Color sel_text_color = loc.exists ? selected_clr : missing_clr;

            loc.text_tex_selected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), display_name, sel_text_color, 410);
            loc.text_tex_unselected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), display_name, text_color, 410);

        locations.push_back(loc);
        }
    }

    void FileBrowserPane::LoadDirectoryAsync(const std::string& path) {
        CancelPendingScan();
        
        std::cout << "[FILE_BROWSER] scan_start path=" << path << std::endl;

        {
            std::lock_guard<std::mutex> lock(data_mutex);
            scan_generation_id++;
            current_scan_path = path;
            is_loading = true;
            load_failed = false;
            error_msg = "";
        }

        uint32_t active_gen = scan_generation_id;

        scan_thread = std::thread([this, path, active_gen]() {
            auto start_time = std::chrono::high_resolution_clock::now();

            DIR* dir = opendir(path.c_str());
            if (!dir) {
                std::lock_guard<std::mutex> d_lock(data_mutex);
                if (!cancel_scan && active_gen == scan_generation_id && current_scan_path == path) {
                    is_loading = false;
                    load_failed = true;
                    error_msg = "Could not open directory.";
                    needs_layout_update = true;
                    std::cout << "[FILE_BROWSER] error path=" << path << " message=Could not open directory." << std::endl;
                } else {
                    std::cout << "[FILE_BROWSER] async_ignore path=" << path << " reason=stale_scan" << std::endl;
                }
                return;
            }

            std::vector<FileEntry> entries;
            
            // Add Parent ".." directory link if we are not at root
            if (path != "sdmc:/" && path != "sdmc:") {
                FileEntry parent;
                parent.name = "..";
                
                // Deduce parent path
                std::string parent_dir = path;
                if (parent_dir.back() == '/') parent_dir.pop_back();
                size_t slash = parent_dir.find_last_of('/');
                if (slash != std::string::npos && slash >= 5) {
                    parent.path = parent_dir.substr(0, slash + 1);
                } else {
                    parent.path = "sdmc:/";
                }
                parent.is_dir = true;
                parent.item_count = -1;
                entries.push_back(parent);
            }

            struct dirent* ent;
            while (!cancel_scan && (ent = readdir(dir)) != nullptr) {
                std::string name = ent->d_name;
                if (name == "." || name == "..") continue;

                FileEntry entry;
                entry.name = name;
                entry.path = path;
                if (entry.path.back() != '/') entry.path += "/";
                entry.path += name;

                struct stat st;
                if (stat(entry.path.c_str(), &st) == 0) {
                    entry.is_dir = S_ISDIR(st.st_mode);
                    entry.size = st.st_size;
                    entry.mtime = st.st_mtime;

                    if (entry.is_dir) {
                        entry.item_count = -1;
                    }
                }
                entries.push_back(entry);
            }
            closedir(dir);

            if (cancel_scan) {
                return;
            }

            // Sort entries: folders first (keeping ".." as first), then files alphabetically
            std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
                if (a.name == "..") return true;
                if (b.name == "..") return false;
                
                if (a.is_dir != b.is_dir) {
                    return a.is_dir;
                }

                std::string a_lower = a.name;
                std::string b_lower = b.name;
                std::transform(a_lower.begin(), a_lower.end(), a_lower.begin(), ::tolower);
                std::transform(b_lower.begin(), b_lower.end(), b_lower.begin(), ::tolower);
                return a_lower < b_lower;
            });

            auto end_time = std::chrono::high_resolution_clock::now();
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count();

            int folder_count = 0;
            int file_count = 0;
            for (const auto& e : entries) {
                if (e.name == "..") continue;
                if (e.is_dir) folder_count++;
                else file_count++;
            }

            {
                std::lock_guard<std::mutex> d_lock(data_mutex);
                if (!cancel_scan && active_gen == scan_generation_id && current_scan_path == path) {
                    // The displaced entries own GPU textures that may only be
                    // freed on the render thread — park them for OnRender to
                    // clean up (previously they were dropped here and leaked).
                    for (auto& old : loaded_items) {
                        retired_items.push_back(std::move(old));
                    }
                    loaded_items = std::move(entries);
                    is_loading = false;
                    needs_layout_update = true;
                    std::cout << "[FILE_BROWSER] scan_done path=" << path 
                              << " folders=" << folder_count 
                              << " files=" << file_count 
                              << " ms=" << elapsed_ms << std::endl;
                } else {
                    std::cout << "[FILE_BROWSER] async_ignore path=" << path << " reason=stale_scan" << std::endl;
                }
            }
        });
    }

    // Render the four text textures for one row. Called lazily from OnRender
    // for visible rows only (with a per-frame budget) — the old code
    // pre-rendered every entry in the directory in a single frame, which froze
    // the UI for seconds in large folders.
    void FileBrowserPane::CreateItemTextures(FileEntry& item) {
        pu::ui::Color selected_clr(230, 199, 167, 255);
        pu::ui::Color unselected_clr(255, 255, 255, 255);
        pu::ui::Color detail_selected_clr(210, 190, 170, 255);
        pu::ui::Color detail_unselected_clr(120, 125, 135, 255);

        // Limit text width to 1000px for full-width list layout
        item.text_tex_selected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), item.name, selected_clr, 1000);
        item.text_tex_unselected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), item.name, unselected_clr, 1000);

        std::string details_str = "";
        if (item.name == "..") {
            details_str = "";
        } else if (item.is_dir) {
            details_str = "Folder";
        } else {
            std::string date_str = "";
            if (item.mtime > 0) {
                struct tm* timeinfo = std::localtime(&item.mtime);
                char buf[32];
                std::strftime(buf, sizeof(buf), "%d/%m/%Y", timeinfo);
                date_str = buf;
            }
            details_str = FormatSize(item.size);
            if (!date_str.empty()) {
                details_str += "\n" + date_str;
            }
        }

        // Right-aligned details card (max 400px width)
        item.details_tex_selected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@20", "Ubuntu@18"), details_str, detail_selected_clr, 400);
        item.details_tex_unselected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@20", "Ubuntu@18"), details_str, detail_unselected_clr, 400);
    }

    void FileBrowserPane::RebuildFileTextures() {
        std::lock_guard<std::mutex> lock(data_mutex);

        // Restore selection if we just navigated back to parent folder
        if (!target_select_name.empty()) {
            bool found = false;
            for (size_t idx = 0; idx < loaded_items.size(); ++idx) {
                if (loaded_items[idx].name == target_select_name) {
                    selected_file_idx = idx;
                    if (loaded_items.size() <= 8) {
                        file_scroll_offset = 0;
                    } else {
                        file_scroll_offset = (int)selected_file_idx - 4; // Center selection in list (max 8 visible)
                        if (file_scroll_offset < 0) file_scroll_offset = 0;
                        if (file_scroll_offset + 8 > (int)loaded_items.size()) {
                            file_scroll_offset = (int)loaded_items.size() - 8;
                        }
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                selected_file_idx = 0;
                file_scroll_offset = 0;
            }
            target_select_name = "";
        }

        needs_layout_update = false;
    }

    static std::string FormatSize(long long bytes) {
        if (bytes <= 0) return "0 B";
        if (bytes >= 1024LL * 1024LL * 1024LL) {
            double gb = (double)bytes / (1024.0 * 1024.0 * 1024.0);
            char buf[32];
            std::sprintf(buf, "%.1f GB", gb);
            return buf;
        } else if (bytes >= 1024LL * 1024LL) {
            double mb = (double)bytes / (1024.0 * 1024.0);
            char buf[32];
            std::sprintf(buf, "%.1f MB", mb);
            return buf;
        } else if (bytes >= 1024LL) {
            double kb = (double)bytes / 1024.0;
            char buf[32];
            std::sprintf(buf, "%.1f KB", kb);
            return buf;
        } else {
            return std::to_string(bytes) + " B";
        }
    }

    static std::string GetPathBasename(const std::string& path) {
        std::string p = path;
        if (p.empty()) return "";
        if (p.back() == '/') p.pop_back();
        size_t slash = p.find_last_of('/');
        if (slash != std::string::npos) {
            return p.substr(slash + 1);
        }
        return p;
    }

    void FileBrowserPane::RebuildDetailTextures() {
        detail_texs.Clear();

        std::string path = "";
        std::string name = "";
        std::string type = "";
        std::string size = "";
        std::string time_str = "";

        if (active_focus == FileBrowserFocus::Locations) {
            if (selected_location_idx < locations.size()) {
                const auto& loc = locations[selected_location_idx];
                path = loc.path;
                name = loc.name;
                type = "Quick Location";
                size = loc.exists ? "Directory exists" : "Directory missing";
            }
        } else {
            std::lock_guard<std::mutex> lock(data_mutex);
            if (selected_file_idx < loaded_items.size()) {
                const auto& item = loaded_items[selected_file_idx];
                path = item.path;
                name = item.name;
                
                if (item.name == "..") {
                    type = "Navigation helper";
                    size = "Parent directory link";
                } else if (item.is_dir) {
                    type = "Folder";
                    if (item.item_count >= 0) {
                        size = std::to_string(item.item_count) + " items";
                    }
                } else {
                    type = "File";
                    size = FormatSize(item.size);
                    size_t last_dot = item.name.find_last_of('.');
                    if (last_dot != std::string::npos) {
                        type += " (" + item.name.substr(last_dot) + ")";
                    }
                }

                if (item.mtime > 0) {
                    struct tm* timeinfo = std::localtime(&item.mtime);
                    char buf[64];
                    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", timeinfo);
                    time_str = buf;
                }
            }
        }

        if (name.empty()) return;

        pu::ui::Color text_color(190, 180, 225, 255);
        pu::ui::Color title_color(237, 229, 251, 255);

        detail_texs.cached_path = path;
        detail_texs.name_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), name, title_color, 370);
        detail_texs.type_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@20", "Ubuntu@18"), "Type: " + type, text_color, 370);
        detail_texs.path_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@18", "Ubuntu@20"), "Path: " + path, text_color, 370);
        if (!size.empty()) {
            detail_texs.size_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@20", "Ubuntu@18"), "Size: " + size, text_color, 370);
        }
        if (!time_str.empty()) {
            detail_texs.time_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@20", "Ubuntu@18"), "Modified: " + time_str, text_color, 370);
        }
    }

    void FileBrowserPane::RebuildOptionsTextures() {
        for (auto& tex : options_texs_selected) pu::ui::render::DeleteTexture(tex);
        for (auto& tex : options_texs_unselected) pu::ui::render::DeleteTexture(tex);
        options_texs_selected.clear();
        options_texs_unselected.clear();
        options_menu_items.clear();

        if (mount_val_tex_selected) { pu::ui::render::DeleteTexture(mount_val_tex_selected); mount_val_tex_selected = nullptr; }
        if (mount_val_tex_unselected) { pu::ui::render::DeleteTexture(mount_val_tex_unselected); mount_val_tex_unselected = nullptr; }

        options_menu_items.push_back("Mount");
        options_menu_items.push_back("Open");
        options_menu_items.push_back("Properties");
        options_menu_items.push_back("Refresh");

        bool can_write = IsWritablePath(current_path);
        if (can_write) {
            options_menu_items.push_back("Create Folder");
            bool sel_is_mutable = false;
            {
                std::lock_guard<std::mutex> lock(data_mutex);
                sel_is_mutable = !loaded_items.empty() && selected_file_idx < loaded_items.size() &&
                                 loaded_items[selected_file_idx].name != "..";
            }
            if (sel_is_mutable) {
                options_menu_items.push_back("Rename");
                options_menu_items.push_back("Delete");
            }
        }

        pu::ui::Color selected_clr(230, 199, 167, 255);
        pu::ui::Color unselected_clr(255, 255, 255, 255);

        // Pre-render mount name textures
        std::string mount_names[] = { "microSD card", "romm-nx", "ROMs" };
        std::string active_mount_name = mount_names[current_mount_idx % 3];
        mount_val_tex_selected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), active_mount_name, selected_clr);
        mount_val_tex_unselected = pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), active_mount_name, unselected_clr);

        for (const auto& opt : options_menu_items) {
            options_texs_selected.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), opt, selected_clr, 370));
            options_texs_unselected.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), opt, unselected_clr, 370));
        }

        if (selected_option_idx >= options_menu_items.size()) {
            selected_option_idx = 0;
        }
    }

    void FileBrowserPane::RebuildConfirmTextures() {
        for (auto& tex : confirm_texs_selected) pu::ui::render::DeleteTexture(tex);
        for (auto& tex : confirm_texs_unselected) pu::ui::render::DeleteTexture(tex);
        confirm_texs_selected.clear();
        confirm_texs_unselected.clear();

        confirm_menu_items = { "No", "Yes" };

        pu::ui::Color selected_clr(230, 199, 167, 255);
        pu::ui::Color unselected_clr(255, 255, 255, 255);

        for (const auto& item : confirm_menu_items) {
            confirm_texs_selected.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), item, selected_clr, 370));
            confirm_texs_unselected.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), item, unselected_clr, 370));
        }

        selected_confirm_idx = 0; // Default to "No"
    }

    void FileBrowserPane::RefreshStats() {
        cached_net_info.status_line1 = "Disconnected";
        cached_net_info.status_line2 = "0.0.0.0";
        
        NifmInternetConnectionType type = (NifmInternetConnectionType)0;
        u32 wifi = 0;
        NifmInternetConnectionStatus status = (NifmInternetConnectionStatus)0;
        if (R_SUCCEEDED(nifmGetInternetConnectionStatus(&type, &wifi, &status))) {
            if (status == NifmInternetConnectionStatus_Connected) {
                if (type == NifmInternetConnectionType_WiFi) {
                    int pct = 0;
                    if (wifi >= 3) pct = 100;
                    else if (wifi == 2) pct = 66;
                    else if (wifi == 1) pct = 33;
                    cached_net_info.status_line1 = "Wi-Fi " + std::to_string(pct) + "%";
                } else if (type == NifmInternetConnectionType_Ethernet) {
                    cached_net_info.status_line1 = "Ethernet";
                } else {
                    cached_net_info.status_line1 = "Connected";
                }
                
                u32 ip = 0;
                if (R_SUCCEEDED(nifmGetCurrentIpAddress(&ip))) {
                    u8* bytes = (u8*)&ip;
                    char ip_buf[32];
                    std::sprintf(ip_buf, "%d.%d.%d.%d", bytes[0], bytes[1], bytes[2], bytes[3]);
                    cached_net_info.status_line2 = ip_buf;
                }
            }
        }
        
        cached_store_info.sd_free = 0.0;
        cached_store_info.sd_total = 0.0;
        cached_store_info.sys_free = 26.4; // Fallback NAND
        cached_store_info.sys_total = 32.0;
        
        struct statvfs vfs_sd;
        if (statvfs("sdmc:/", &vfs_sd) == 0) {
            double free_bytes = (double)vfs_sd.f_bfree * vfs_sd.f_frsize;
            double total_bytes = (double)vfs_sd.f_blocks * vfs_sd.f_frsize;
            cached_store_info.sd_free = free_bytes / (1024.0 * 1024.0 * 1024.0);
            cached_store_info.sd_total = total_bytes / (1024.0 * 1024.0 * 1024.0);
        }
        
        struct statvfs vfs_sys;
        if (statvfs("nand:/", &vfs_sys) == 0) {
            double free_bytes = (double)vfs_sys.f_bfree * vfs_sys.f_frsize;
            double total_bytes = (double)vfs_sys.f_blocks * vfs_sys.f_frsize;
            cached_store_info.sys_free = free_bytes / (1024.0 * 1024.0 * 1024.0);
            cached_store_info.sys_total = total_bytes / (1024.0 * 1024.0 * 1024.0);
        }
    }

    // Re-render only when the string changes; returns the cached texture.
    static pu::sdl2::Texture UpdateCachedText(CachedText& cache, const std::string& str, const std::string& font, pu::ui::Color color) {
        if (!cache.tex || cache.str != str) {
            if (cache.tex) {
                pu::ui::render::DeleteTexture(cache.tex);
                cache.tex = nullptr;
            }
            cache.str = str;
            if (!str.empty()) {
                cache.tex = pu::ui::render::RenderText(font, str, color);
            }
        }
        return cache.tex;
    }

    void FileBrowserPane::DrawSwitchButton(pu::ui::render::Renderer::Ref &drawer, const std::string& letter, const std::string& label, s32 &x_pos, s32 y_pos) {
        pu::ui::Color btn_bg(50, 55, 65, 255);
        pu::ui::Color btn_text(255, 255, 255, 255);

        s32 box_w = 40;
        s32 box_h = 40;
        drawer->RenderRoundedRectangleFill(btn_bg, x_pos, y_pos - 6, box_w, box_h, 8);

        // Footer glyphs come from a tiny fixed set — cache them instead of
        // doing a TTF render + texture destroy for each, every frame.
        auto& letter_tex = button_text_cache["K|" + letter];
        if (!letter_tex) {
            letter_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@20", "Ubuntu@24"), letter, btn_text);
        }
        if (letter_tex) {
            s32 lw = pu::ui::render::GetTextureWidth(letter_tex);
            s32 lh = pu::ui::render::GetTextureHeight(letter_tex);
            drawer->RenderTexture(letter_tex, x_pos + (box_w - lw) / 2, y_pos - 6 + (box_h - lh) / 2);
        }

        x_pos += box_w + 12;
        auto& label_tex = button_text_cache["L|" + label];
        if (!label_tex) {
            label_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), label, pu::ui::Color(200, 205, 215, 255));
        }
        if (label_tex) {
            s32 label_h = pu::ui::render::GetTextureHeight(label_tex);
            drawer->RenderTexture(label_tex, x_pos, y_pos - 6 + (box_h - label_h) / 2);
            x_pos += pu::ui::render::GetTextureWidth(label_tex) + 30;
        }
    }

    void FileBrowserPane::DrawRectangleBorder(pu::ui::render::Renderer::Ref &drawer, const pu::ui::Color clr, const s32 x_pos, const s32 y_pos, const s32 width, const s32 height, const s32 border_width) {
        for (int i = 0; i < border_width; ++i) {
            drawer->RenderRectangle(clr, x_pos + i, y_pos + i, width - i * 2, height - i * 2);
        }
    }

    void FileBrowserPane::DrawFolderIcon(pu::ui::render::Renderer::Ref &drawer, s32 x_coord, s32 y_coord, pu::ui::Color color) {
        drawer->RenderRoundedRectangleFill(color, x_coord, y_coord + 6, 32, 22, 4);
        drawer->RenderRoundedRectangleFill(color, x_coord, y_coord, 14, 8, 2);
    }

    void FileBrowserPane::DrawFileIcon(pu::ui::render::Renderer::Ref &drawer, s32 x_coord, s32 y_coord, pu::ui::Color color) {
        drawer->RenderRoundedRectangleFill(color, x_coord + 4, y_coord, 24, 30, 3);
    }

    void FileBrowserPane::OnSelectionUpdated() {
        RebuildDetailTextures();
        RebuildOptionsTextures();
        RebuildConfirmTextures();
    }

    void FileBrowserPane::ForceRefresh() {
        BuildLocationsList();
        LoadDirectoryAsync(current_path);
        OnSelectionUpdated();
    }

    void FileBrowserPane::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        bool logged_render = false;
        bool do_layout_update = false;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            // Free textures of entries displaced by the scan thread — texture
            // deletion must happen here on the render thread.
            for (auto& item : retired_items) {
                if (item.text_tex_selected) pu::ui::render::DeleteTexture(item.text_tex_selected);
                if (item.text_tex_unselected) pu::ui::render::DeleteTexture(item.text_tex_unselected);
                if (item.details_tex_selected) pu::ui::render::DeleteTexture(item.details_tex_selected);
                if (item.details_tex_unselected) pu::ui::render::DeleteTexture(item.details_tex_unselected);
            }
            retired_items.clear();
            do_layout_update = needs_layout_update;
        }
        if (do_layout_update) {
            std::cout << "[FILE_BROWSER] render_start" << std::endl;
            RebuildFileTextures();
            OnSelectionUpdated();
            logged_render = true;
        }

        // Cache system/network stats updates (every 3 seconds)
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats_time).count() >= 3) {
            RefreshStats();
            last_stats_time = now;
        }

        bool is_dimmed = (active_focus == FileBrowserFocus::OptionsMenu || active_focus == FileBrowserFocus::DeleteConfirm || active_focus == FileBrowserFocus::PropertiesModal);
        pu::ui::Color separator_clr(40, 45, 55, 255);
        pu::ui::Color cream_accent(230, 199, 167, 255);
        pu::ui::Color progress_fill(85, 63, 152, 255);

        // 1. Draw Top Separator Line
        drawer->RenderRectangleFill(separator_clr, 60, 135, 1800, 1);

        // 2. Draw Top-Right System Info (textures cached; strings change at
        // most every 3s when RefreshStats runs)
        auto wifi_tex = UpdateCachedText(net1_cache, cached_net_info.status_line1, GetSafeFont("Ubuntu@20", "Ubuntu@24"), pu::ui::Color(190, 180, 225, 255));
        if (wifi_tex) {
            drawer->RenderTexture(wifi_tex, 1300 - pu::ui::render::GetTextureWidth(wifi_tex), 50);
        }
        auto ip_tex = UpdateCachedText(net2_cache, cached_net_info.status_line2, GetSafeFont("Ubuntu@20", "Ubuntu@24"), pu::ui::Color(190, 180, 225, 255));
        if (ip_tex) {
            drawer->RenderTexture(ip_tex, 1300 - pu::ui::render::GetTextureWidth(ip_tex), 80);
        }

        // microSD space
        char sd_buf[64];
        std::sprintf(sd_buf, "microSD %.1f GB", cached_store_info.sd_free);
        auto sd_tex = UpdateCachedText(sd_cache, sd_buf, GetSafeFont("Ubuntu@20", "Ubuntu@24"), pu::ui::Color(190, 180, 225, 255));
        if (sd_tex) {
            drawer->RenderTexture(sd_tex, 1350, 60);
        }
        pu::ui::Color bar_bg(30, 32, 36, 255);
        drawer->RenderRectangleFill(bar_bg, 1350, 95, 220, 8);
        double sd_used = cached_store_info.sd_total - cached_store_info.sd_free;
        double sd_pct = (cached_store_info.sd_total > 0.0) ? (sd_used / cached_store_info.sd_total) : 0.0;
        s32 sd_fill_w = (s32)(220 * sd_pct);
        if (sd_fill_w > 220) sd_fill_w = 220;
        if (sd_fill_w < 0) sd_fill_w = 0;
        drawer->RenderRectangleFill(progress_fill, 1350, 95, sd_fill_w, 8);

        // NAND (System) space
        char sys_buf[64];
        std::sprintf(sys_buf, "System %.1f GB", cached_store_info.sys_free);
        auto sys_tex = UpdateCachedText(sys_cache, sys_buf, GetSafeFont("Ubuntu@20", "Ubuntu@24"), pu::ui::Color(190, 180, 225, 255));
        if (sys_tex) {
            drawer->RenderTexture(sys_tex, 1620, 60);
        }
        drawer->RenderRectangleFill(bar_bg, 1620, 95, 220, 8);
        double sys_used = cached_store_info.sys_total - cached_store_info.sys_free;
        double sys_pct = (cached_store_info.sys_total > 0.0) ? (sys_used / cached_store_info.sys_total) : 0.0;
        s32 sys_fill_w = (s32)(220 * sys_pct);
        if (sys_fill_w > 220) sys_fill_w = 220;
        if (sys_fill_w < 0) sys_fill_w = 0;
        drawer->RenderRectangleFill(progress_fill, 1620, 95, sys_fill_w, 8);

        // 3. Draw Directory Listing (Full Width)
        s32 file_h = 92;
        size_t max_visible = 8;

        // Render straight from loaded_items under the lock (the scan thread
        // only holds it for brief moments) — the previous full-vector copy
        // per frame churned hundreds of string allocations at 60fps. Rows
        // lazily render their text textures here, a few per frame, instead of
        // pre-rendering the entire directory in one hitch.
        size_t item_count_snapshot = 0;
        {
        std::lock_guard<std::mutex> lock(data_mutex);
        item_count_snapshot = loaded_items.size();

        if (is_loading) {
            s32 tw = pu::ui::render::GetTextureWidth(loading_tex);
            s32 th = pu::ui::render::GetTextureHeight(loading_tex);
            drawer->RenderTexture(loading_tex, x_coord + (1800 - tw) / 2, y_coord + (760 - th) / 2);
        } else if (load_failed) {
            pu::ui::Color red_clr(231, 76, 60, 255);
            auto err_tex = UpdateCachedText(error_cache, error_msg.empty() ? "Failed to read path." : error_msg, GetSafeFont("Ubuntu@30", "Ubuntu@24"), red_clr);
            if (err_tex) {
                s32 tw = pu::ui::render::GetTextureWidth(err_tex);
                s32 th = pu::ui::render::GetTextureHeight(err_tex);
                drawer->RenderTexture(err_tex, x_coord + (1800 - tw) / 2, y_coord + (760 - th) / 2);
            }
        } else if (loaded_items.empty()) {
            s32 tw = pu::ui::render::GetTextureWidth(empty_tex);
            s32 th = pu::ui::render::GetTextureHeight(empty_tex);
            drawer->RenderTexture(empty_tex, x_coord + (1800 - tw) / 2, y_coord + (760 - th) / 2);
        } else {
            // Adjust scroll offset
            if (selected_file_idx < (size_t)file_scroll_offset) {
                file_scroll_offset = selected_file_idx;
            } else if (selected_file_idx >= (size_t)file_scroll_offset + max_visible) {
                file_scroll_offset = (int)(selected_file_idx - max_visible + 1);
            }

            int lazy_texture_budget = 4; // rows whose text we render this frame

            for (size_t i = 0; i < max_visible; ++i) {
                size_t idx = file_scroll_offset + i;
                if (idx >= loaded_items.size()) break;

                s32 iy = y_coord + (s32)i * file_h;
                bool is_selected = (active_focus == FileBrowserFocus::Files && idx == selected_file_idx);

                if (is_selected && !is_dimmed) {
                    DrawRectangleBorder(drawer, cream_accent, x_coord, iy, 1800, file_h - 10, 2);
                }

                auto& item = loaded_items[idx];
                if (!item.text_tex_unselected && lazy_texture_budget > 0) {
                    CreateItemTextures(item);
                    lazy_texture_budget--;
                }

                // Draw vector icons
                s32 icon_x = x_coord + 20;
                s32 icon_y = iy + 20;
                pu::ui::Color icon_color = is_dimmed ? pu::ui::Color(100, 110, 120, 255) : (is_selected ? cream_accent : pu::ui::Color(255, 255, 255, 255));
                
                if (item.name == "..") {
                    DrawFolderIcon(drawer, icon_x, icon_y, icon_color);
                } else if (item.is_dir) {
                    DrawFolderIcon(drawer, icon_x, icon_y, icon_color);
                } else {
                    if (!is_dimmed && !is_selected) {
                        size_t last_dot = item.name.find_last_of('.');
                        if (last_dot != std::string::npos) {
                            std::string ext = item.name.substr(last_dot);
                            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
                            if (ext == ".iso" || ext == ".cso" || ext == ".pbp" || ext == ".bin" || ext == ".chd" || ext == ".nds" || ext == ".zip" || ext == ".7z") {
                                icon_color = cream_accent;
                            }
                        }
                    }
                    DrawFileIcon(drawer, icon_x, icon_y, icon_color);
                }

                auto title_tex = is_selected ? item.text_tex_selected : item.text_tex_unselected;
                if (title_tex) {
                    s32 th = pu::ui::render::GetTextureHeight(title_tex);
                    drawer->RenderTexture(title_tex, x_coord + 80, iy + (file_h - 10 - th) / 2);
                }

                auto details_tex = is_selected ? item.details_tex_selected : item.details_tex_unselected;
                if (details_tex) {
                    s32 details_w = pu::ui::render::GetTextureWidth(details_tex);
                    s32 details_h = pu::ui::render::GetTextureHeight(details_tex);
                    drawer->RenderTexture(details_tex, x_coord + 1800 - 30 - details_w, iy + (file_h - 10 - details_h) / 2);
                }

                drawer->RenderRectangleFill(separator_clr, x_coord, iy + file_h - 1, 1800, 1);
            }
        }
        } // release data_mutex

        // Draw dimming overlay if options panel or dialog is open
        if (is_dimmed) {
            drawer->RenderRectangleFill(pu::ui::Color(5, 6, 8, 180), 60, 150, 1320, 780);
        }

        // 4. Draw Right Options Menu
        s32 det_x = 1380;
        s32 det_y = 150;
        s32 det_w = 540;
        s32 det_h = 780;

        if (active_focus == FileBrowserFocus::OptionsMenu) {
            pu::ui::Color panel_bg(15, 17, 21, 255);
            drawer->RenderRectangleFill(panel_bg, det_x, det_y, det_w, det_h);
            drawer->RenderRectangleFill(separator_clr, det_x, det_y, 1, det_h);

            if (options_title_tex) {
                drawer->RenderTexture(options_title_tex, 1420, 180);
            }

            s32 opt_y_start = 250;
            s32 opt_h = 75;
            for (size_t i = 0; i < options_menu_items.size(); ++i) {
                s32 oy = opt_y_start + (s32)i * opt_h;
                bool is_opt_selected = (i == selected_option_idx);

                if (is_opt_selected) {
                    DrawRectangleBorder(drawer, cream_accent, 1420, oy, 460, opt_h - 10, 2);
                }

                if (i < options_texs_selected.size()) {
                    auto tex = is_opt_selected ? options_texs_selected[i] : options_texs_unselected[i];
                    if (tex) {
                        drawer->RenderTexture(tex, 1440, oy + 12);
                    }
                }

                if (options_menu_items[i] == "Mount") {
                    auto val_tex = is_opt_selected ? mount_val_tex_selected : mount_val_tex_unselected;
                    if (val_tex) {
                        s32 val_w = pu::ui::render::GetTextureWidth(val_tex);
                        drawer->RenderTexture(val_tex, 1840 - val_w, oy + 12);
                    }
                }
            }
        }
        else if (active_focus == FileBrowserFocus::DeleteConfirm) {
            // Draw centered Delete Confirmation Modal (720x280)
            pu::ui::Color modal_bg(15, 17, 21, 255);
            drawer->RenderRectangleFill(modal_bg, 600, 400, 720, 280);
            DrawRectangleBorder(drawer, separator_clr, 600, 400, 720, 280, 2);

            if (confirm_title_tex) {
                s32 prompt_w = pu::ui::render::GetTextureWidth(confirm_title_tex);
                drawer->RenderTexture(confirm_title_tex, 600 + (720 - prompt_w) / 2, 450);
            }

            for (size_t i = 0; i < 2; ++i) {
                s32 bx = (i == 0) ? 660 : 1010;
                s32 by = 560;
                bool is_btn_selected = (selected_confirm_idx == i);

                if (is_btn_selected) {
                    DrawRectangleBorder(drawer, cream_accent, bx, by, 250, 70, 2);
                } else {
                    DrawRectangleBorder(drawer, separator_clr, bx, by, 250, 70, 1);
                }

                auto btn_tex = is_btn_selected ? confirm_texs_selected[i] : confirm_texs_unselected[i];
                if (btn_tex) {
                    s32 btn_w = pu::ui::render::GetTextureWidth(btn_tex);
                    s32 btn_h = pu::ui::render::GetTextureHeight(btn_tex);
                    drawer->RenderTexture(btn_tex, bx + (250 - btn_w) / 2, by + (70 - btn_h) / 2);
                }
            }
        }
        else if (active_focus == FileBrowserFocus::PropertiesModal) {
            // Draw centered Properties Modal (800x480)
            pu::ui::Color modal_bg(15, 17, 21, 255);
            drawer->RenderRectangleFill(modal_bg, 560, 300, 800, 480);
            DrawRectangleBorder(drawer, separator_clr, 560, 300, 800, 480, 2);

            s32 text_y = 340;
            if (!properties_texs.empty()) {
                auto title_tex = properties_texs[0];
                if (title_tex) {
                    drawer->RenderTexture(title_tex, 600, text_y);
                }
                text_y += 55;
                
                drawer->RenderRectangleFill(separator_clr, 600, text_y, 720, 1);
                text_y += 25;

                for (size_t i = 1; i < properties_texs.size() - 1; ++i) {
                    auto tex = properties_texs[i];
                    if (tex) {
                        drawer->RenderTexture(tex, 600, text_y);
                    }
                    text_y += 42;
                }

                // Draw OK button centered at the bottom of the modal: X = 835, Y = 680
                s32 bx = 835;
                s32 by = 680;
                DrawRectangleBorder(drawer, cream_accent, bx, by, 250, 60, 2);

                auto ok_tex = properties_texs.back();
                if (ok_tex) {
                    s32 ok_w = pu::ui::render::GetTextureWidth(ok_tex);
                    s32 ok_h = pu::ui::render::GetTextureHeight(ok_tex);
                    drawer->RenderTexture(ok_tex, bx + (250 - ok_w) / 2, by + (60 - ok_h) / 2);
                }
            }
        }

        // 5. Draw Footer controls and Separator Line
        drawer->RenderRectangleFill(separator_clr, 60, 930, 1800, 1);

        char pos_buf[64];
        std::sprintf(pos_buf, "%zu / %zu", item_count_snapshot == 0 ? (size_t)0 : (selected_file_idx + 1), item_count_snapshot);
        auto pos_tex = UpdateCachedText(pos_cache, pos_buf, GetSafeFont("Ubuntu@24", "Ubuntu@20"), pu::ui::Color(120, 125, 135, 255));
        if (pos_tex) {
            drawer->RenderTexture(pos_tex, 60, 965);
        }

        struct FooterBtn {
            std::string key;
            std::string label;
        };
        std::vector<FooterBtn> btns;
        if (active_focus == FileBrowserFocus::OptionsMenu) {
            btns = { {"B", "Close"}, {"A", "OK"} };
        } else if (active_focus == FileBrowserFocus::DeleteConfirm) {
            btns = { {"B", "Cancel"}, {"A", "Confirm"} };
        } else {
            btns = { {"X", "Options"}, {"B", "Back"}, {"A", "Open"} };
        }

        s32 total_w = 0;
        for (const auto& btn : btns) {
            auto& label_tex = button_text_cache["L|" + btn.label];
            if (!label_tex) {
                label_tex = pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), btn.label, pu::ui::Color(200, 205, 215, 255));
            }
            total_w += 40 + 12 + (label_tex ? pu::ui::render::GetTextureWidth(label_tex) : 0) + 30;
        }
        s32 start_x = 1860 - total_w;
        for (const auto& btn : btns) {
            DrawSwitchButton(drawer, btn.key, btn.label, start_x, 965);
        }

        if (logged_render) {
            std::cout << "[FILE_BROWSER] render_done" << std::endl;
        }
    }

    void FileBrowserPane::HandleInput(const u64 keys_down, const u64 keys_held) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        // Snapshot just the item count and the selected entry's data fields
        // under a short lock — input handling only ever needs those, and the
        // previous full-vector copy churned every string in the directory on
        // every input frame. (Texture pointers deliberately not copied; the
        // snapshot owns nothing.)
        size_t item_count = 0;
        FileEntry sel_item;
        bool has_sel = false;
        {
            std::lock_guard<std::mutex> lock(data_mutex);
            item_count = loaded_items.size();
            if (selected_file_idx < item_count) {
                const auto& src = loaded_items[selected_file_idx];
                sel_item.name = src.name;
                sel_item.path = src.path;
                sel_item.is_dir = src.is_dir;
                sel_item.size = src.size;
                sel_item.mtime = src.mtime;
                has_sel = true;
            }
        }

        // 1. Held repeat scroll timer logic
        bool up_pressed = (keys_down & HidNpadButton_Up) || (keys_down & HidNpadButton_StickLUp);
        bool down_pressed = (keys_down & HidNpadButton_Down) || (keys_down & HidNpadButton_StickLDown);
        bool up_held = (keys_held & HidNpadButton_Up) || (keys_held & HidNpadButton_StickLUp);
        bool down_held = (keys_held & HidNpadButton_Down) || (keys_held & HidNpadButton_StickLDown);
        
        auto now = std::chrono::steady_clock::now();
        bool do_up = false;
        bool do_down = false;
        
        if (up_pressed) {
            do_up = true;
            last_scroll_time = now;
            is_repeating = false;
        } else if (down_pressed) {
            do_down = true;
            last_scroll_time = now;
            is_repeating = false;
        } else if (up_held || down_held) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_scroll_time).count();
            long long delay = is_repeating ? 80 : 350;
            if (elapsed >= delay) {
                if (up_held) do_up = true;
                if (down_held) do_down = true;
                last_scroll_time = now;
                is_repeating = true;
            }
        }

        if (active_focus == FileBrowserFocus::Locations) {
            active_focus = FileBrowserFocus::Files;
        }

        if (active_focus == FileBrowserFocus::Files) {
            if (do_up) {
                if (selected_file_idx > 0) {
                    selected_file_idx--;
                    OnSelectionUpdated();
                }
            }
            else if (do_down) {
                if (item_count > 0 && selected_file_idx < item_count - 1) {
                    selected_file_idx++;
                    OnSelectionUpdated();
                }
            }

            if (keys_down & HidNpadButton_A) {
                if (has_sel && sel_item.is_dir) {
                    current_path = NormalizePath(sel_item.path);
                    if (current_path.back() != '/') current_path += "/";

                    std::cout << "[FILE_BROWSER] open path=" << current_path << std::endl;
                    LoadDirectoryAsync(current_path);

                    selected_file_idx = 0;
                    file_scroll_offset = 0;
                    OnSelectionUpdated();
                }
            }
            if (keys_down & HidNpadButton_B) {
                // Go parent
                if (current_path != "sdmc:/" && current_path != "sdmc:") {
                    std::string parent_dir = current_path;
                    if (parent_dir.back() == '/') parent_dir.pop_back();
                    size_t slash = parent_dir.find_last_of('/');
                    
                    // Save parent basename to automatically select it when loaded
                    target_select_name = GetPathBasename(parent_dir);

                    if (slash != std::string::npos && slash >= 5) {
                        current_path = parent_dir.substr(0, slash + 1);
                    } else {
                        current_path = "sdmc:/";
                    }
                    std::cout << "[FILE_BROWSER] open path=" << current_path << " target_select=" << target_select_name << std::endl;
                    LoadDirectoryAsync(current_path);
                    
                    selected_file_idx = 0;
                    file_scroll_offset = 0;
                    OnSelectionUpdated();
                } else {
                    // Return to Main Menu
                    nav->SetCurrentScreen(romm::navigation::Screen::MainMenu);
                    nav->GetApp()->LoadLayout(nav->GetMainMenuLayout());
                    std::cout << "[NAV] Returning to Main Menu" << std::endl;
                }
            }
            if (keys_down & HidNpadButton_X) {
                // Open contextual Options panel
                active_focus = FileBrowserFocus::OptionsMenu;
                selected_option_idx = 0;
                OnSelectionUpdated();
            }
        }
        else if (active_focus == FileBrowserFocus::OptionsMenu) {
            if (do_up) {
                if (selected_option_idx > 0) {
                    selected_option_idx--;
                    OnSelectionUpdated();
                }
            }
            else if (do_down) {
                if (selected_option_idx < options_menu_items.size() - 1) {
                    selected_option_idx++;
                    OnSelectionUpdated();
                }
            }
            if (keys_down & HidNpadButton_B) {
                // Close contextual Options panel
                active_focus = FileBrowserFocus::Files;
                OnSelectionUpdated();
            }
            if (keys_down & HidNpadButton_A) {
                std::string option = options_menu_items[selected_option_idx];
                
                if (option == "Mount") {
                    current_mount_idx = (current_mount_idx + 1) % 3;
                    std::string mount_paths[] = { "sdmc:/", "sdmc:/switch/romm-nx/", "sdmc:/roms/" };
                    current_path = mount_paths[current_mount_idx];
                    
                    std::cout << "[FILE_BROWSER] mount_cycle idx=" << current_mount_idx << " path=" << current_path << std::endl;
                    LoadDirectoryAsync(current_path);
                    
                    selected_file_idx = 0;
                    file_scroll_offset = 0;
                    OnSelectionUpdated();
                }
                else if (option == "Open") {
                    active_focus = FileBrowserFocus::Files;
                    HandleInput(HidNpadButton_A, 0); // Trigger standard Open behavior
                }
                else if (option == "Properties") {
                    if (has_sel) {
                        pu::ui::Color text_clr(255, 255, 255, 255);
                        pu::ui::Color title_clr(230, 199, 167, 255); // Cream Accent

                        for (auto& tex : properties_texs) {
                            if (tex) pu::ui::render::DeleteTexture(tex);
                        }
                        properties_texs.clear();

                        properties_texs.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@30", "Ubuntu@24"), "Item Properties", title_clr));
                        properties_texs.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), "Name: " + sel_item.name, text_clr, 720));
                        properties_texs.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), "Type: " + (sel_item.is_dir ? std::string("Folder") : std::string("File")), text_clr, 720));
                        if (!sel_item.is_dir) {
                            properties_texs.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), "Size: " + FormatSize(sel_item.size), text_clr, 720));
                        }
                        properties_texs.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), "Path: " + sel_item.path, text_clr, 720));
                        properties_texs.push_back(pu::ui::render::RenderText(GetSafeFont("Ubuntu@24", "Ubuntu@20"), "OK", title_clr));

                        active_focus = FileBrowserFocus::PropertiesModal;
                        OnSelectionUpdated();
                    }
                }
                else if (option == "Refresh") {
                    ForceRefresh();
                    active_focus = FileBrowserFocus::Files;
                    OnSelectionUpdated();
                }
                else if (option == "Create Folder") {
                    if (IsWritablePath(current_path)) {
                        std::string new_folder_name = nav->ShowKeyboard("Create Folder", "Enter folder name:", "");
                        if (!new_folder_name.empty()) {
                            if (IsValidName(new_folder_name)) {
                                std::string target = current_path;
                                if (target.back() != '/') target += "/";
                                target += new_folder_name;
                                target = NormalizePath(target);
                                
                                if (IsWritablePath(target)) {
                                    int rc = mkdir(target.c_str(), 0777);
                                    if (rc == 0) {
                                        std::cout << "[FILE_BROWSER] create_folder path=" << target << std::endl;
                                        ForceRefresh();
                                        active_focus = FileBrowserFocus::Files;
                                    } else {
                                        nav->ShowKeyboard("Error", "Could not create directory. Already exists?", "");
                                    }
                                } else {
                                    std::cout << "[FILE_BROWSER] blocked_write path=" << target << " reason=write_not_allowed" << std::endl;
                                    nav->ShowKeyboard("Error", "Writes are limited to the ROMs folder. See Settings > File browser writes.", "");
                                }
                            } else {
                                nav->ShowKeyboard("Error", "Invalid name. Avoid symbols / \\ : * ? \" < > | ..", "");
                            }
                        }
                    } else {
                        std::cout << "[FILE_BROWSER] blocked_write path=" << current_path << " reason=write_not_allowed" << std::endl;
                    }
                }
                else if (option == "Rename") {
                    if (has_sel) {
                        const auto& item = sel_item;
                        if (IsWritablePath(item.path)) {
                            std::string new_name = nav->ShowKeyboard("Rename Item", "Enter new name:", item.name);
                            if (!new_name.empty() && new_name != item.name) {
                                if (IsValidName(new_name)) {
                                    std::string target = current_path;
                                    if (target.back() != '/') target += "/";
                                    target += new_name;
                                    target = NormalizePath(target);

                                    if (IsWritablePath(target)) {
                                        struct stat buf;
                                        if (stat(target.c_str(), &buf) == 0) {
                                            nav->ShowKeyboard("Error", "Target path already exists. Overwrite blocked.", "");
                                        } else {
                                            int rc = rename(item.path.c_str(), target.c_str());
                                            if (rc == 0) {
                                                std::cout << "[FILE_BROWSER] rename from=" << item.path << " to=" << target << std::endl;
                                                ForceRefresh();
                                                active_focus = FileBrowserFocus::Files;
                                            } else {
                                                nav->ShowKeyboard("Error", "Could not rename item.", "");
                                            }
                                        }
                                    } else {
                                        std::cout << "[FILE_BROWSER] blocked_write path=" << target << " reason=write_not_allowed" << std::endl;
                                        nav->ShowKeyboard("Error", "Writes are limited to the ROMs folder. See Settings > File browser writes.", "");
                                    }
                                } else {
                                    nav->ShowKeyboard("Error", "Invalid name. Avoid symbols / \\ : * ? \" < > | ..", "");
                                }
                            }
                        } else {
                            std::cout << "[FILE_BROWSER] blocked_write path=" << item.path << " reason=write_not_allowed" << std::endl;
                        }
                    }
                }
                else if (option == "Delete") {
                    // Open Delete Confirm submenu
                    active_focus = FileBrowserFocus::DeleteConfirm;
                    OnSelectionUpdated();
                }
            }
        }
        else if (active_focus == FileBrowserFocus::DeleteConfirm) {
            bool go_left = (keys_down & HidNpadButton_Left) || (keys_down & HidNpadButton_StickLLeft);
            bool go_right = (keys_down & HidNpadButton_Right) || (keys_down & HidNpadButton_StickLRight);

            if (go_left) {
                if (selected_confirm_idx != 0) {
                    selected_confirm_idx = 0;
                    OnSelectionUpdated();
                }
            }
            if (go_right) {
                if (selected_confirm_idx != 1) {
                    selected_confirm_idx = 1;
                    OnSelectionUpdated();
                }
            }

            if (keys_down & HidNpadButton_B) {
                // Cancel deletion
                active_focus = FileBrowserFocus::OptionsMenu;
                OnSelectionUpdated();
            }
            if (keys_down & HidNpadButton_A) {
                if (selected_confirm_idx == 0) {
                    // "No" (Cancel)
                    active_focus = FileBrowserFocus::OptionsMenu;
                    OnSelectionUpdated();
                } else {
                    // "Yes" (Confirm Delete)
                    if (has_sel) {
                        const auto& item = sel_item;
                        if (IsWritablePath(item.path) && item.name != "..") {
                            int rc = -1;
                            if (item.is_dir) {
                                // Folder: check if empty dynamically on delete
                                int count = 0;
                                DIR* subdir = opendir(item.path.c_str());
                                if (subdir) {
                                    struct dirent* sub_ent;
                                    while ((sub_ent = readdir(subdir)) != nullptr) {
                                        std::string sub_name = sub_ent->d_name;
                                        if (sub_name != "." && sub_name != "..") {
                                            count++;
                                        }
                                    }
                                    closedir(subdir);
                                }
                                if (count == 0) {
                                    rc = rmdir(item.path.c_str());
                                } else {
                                    nav->ShowKeyboard("Error", "Folder is not empty. Recursive delete is disabled.", "");
                                    active_focus = FileBrowserFocus::OptionsMenu;
                                    OnSelectionUpdated();
                                    return;
                                }
                            } else {
                                // File
                                rc = unlink(item.path.c_str());
                            }

                            if (rc == 0) {
                                std::cout << "[FILE_BROWSER] delete path=" << item.path << std::endl;
                                ForceRefresh();
                                active_focus = FileBrowserFocus::Files;
                            } else {
                                nav->ShowKeyboard("Error", "Could not delete item.", "");
                                active_focus = FileBrowserFocus::OptionsMenu;
                                OnSelectionUpdated();
                            }
                        } else {
                            std::cout << "[FILE_BROWSER] blocked_write path=" << item.path << " reason=write_not_allowed" << std::endl;
                            active_focus = FileBrowserFocus::OptionsMenu;
                            OnSelectionUpdated();
                        }
                    }
                }
            }
        }
        else if (active_focus == FileBrowserFocus::PropertiesModal) {
            if (keys_down & HidNpadButton_A || keys_down & HidNpadButton_B) {
                active_focus = FileBrowserFocus::OptionsMenu;
                for (auto& tex : properties_texs) {
                    if (tex) pu::ui::render::DeleteTexture(tex);
                }
                properties_texs.clear();
                OnSelectionUpdated();
            }
        }
    }


    // --- FileBrowserLayout Implementation ---

    FileBrowserLayout::FileBrowserLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {
        
        std::cout << "[FILE_BROWSER] init_start" << std::endl;
        
        this->SetBackgroundColor(pu::ui::Color(10, 11, 14, 255));

        header_text = pu::ui::elm::TextBlock::New(60, 55, "FileBrowser (beta)");
        header_text->SetFont(GetSafeFont("Ubuntu@30", "Ubuntu@24"));
        header_text->SetColor(pu::ui::Color(255, 255, 255, 255));
        this->Add(header_text);

        path_text = pu::ui::elm::TextBlock::New(420, 55, "/");
        path_text->SetFont(GetSafeFont("Ubuntu@24", "Ubuntu@20"));
        path_text->SetColor(pu::ui::Color(120, 125, 135, 255));
        this->Add(path_text);

        pane = FileBrowserPane::New(60, 150, 1800, 890, nav);
        this->Add(pane);
        
        std::cout << "[FILE_BROWSER] init_done" << std::endl;
    }

    void FileBrowserLayout::OnSelectionUpdated() {
        if (pane) pane->OnSelectionUpdated();
    }

    void FileBrowserLayout::ForceRefresh() {
        if (pane) pane->ForceRefresh();
    }

    void FileBrowserLayout::CancelPendingScan() {
        if (pane) pane->CancelPendingScan();
    }

    void FileBrowserPane::CancelPendingScan() {
        cancel_scan = true;
        if (scan_thread.joinable()) {
            scan_thread.join();
        }
        cancel_scan = false;
    }

    void FileBrowserLayout::HandleInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) {
        if (pane) {
            pane->HandleInput(keys_down, keys_held);

            // Format current path relative to "/" (e.g. sdmc:/switch -> /switch)
            std::string raw_path = pane->GetCurrentPath();
            std::string display_path = "/";
            if (raw_path.rfind("sdmc:/", 0) == 0) {
                display_path = raw_path.substr(5);
            }
            if (display_path.empty()) display_path = "/";
            path_text->SetText(display_path);
        }
    }

}
