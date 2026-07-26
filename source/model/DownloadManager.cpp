#include "DownloadManager.hpp"
#include <switch.h>
#include "ConfigManager.hpp"
#include "RomPathManager.hpp"
#include "ScreenWakeManager.hpp"
#include "../navigation/HttpClient.hpp"
#include <iostream>
#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <algorithm>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <jpeglib.h>
#include <setjmp.h>
#include <dirent.h>
#include <fstream>
#include "JsonUtil.hpp"

namespace romm::model {

    static bool IsLogicalFileValid(const std::string& path);
    static Result DeleteLogicalFile(const std::string& stdio_path);
    static std::string StripDiscSuffix(const std::string& filename);
    static void WriteMultiDiscPlaylist(const DownloadTask& task);

    struct my_error_mgr {
        struct jpeg_error_mgr pub;
        jmp_buf setjmp_buffer;
    };
    typedef struct my_error_mgr * my_error_ptr;
    METHODDEF(void) my_error_exit(j_common_ptr cinfo) {
        my_error_ptr myerr = (my_error_ptr) cinfo->err;
        longjmp(myerr->setjmp_buffer, 1);
    }

    static bool SaveBaselineJpeg(SDL_Surface* surface, const std::string& path, int quality) {
        if (!surface) return false;
        
        SDL_Surface* rgb_surface = surface;
        bool free_surface = false;
        if (surface->format->format != SDL_PIXELFORMAT_RGB24) {
            rgb_surface = SDL_ConvertSurfaceFormat(surface, SDL_PIXELFORMAT_RGB24, 0);
            if (!rgb_surface) return false;
            free_surface = true;
        }

        FILE* outfile = fopen(path.c_str(), "wb");
        if (!outfile) {
            if (free_surface) SDL_FreeSurface(rgb_surface);
            return false;
        }

        struct jpeg_compress_struct cinfo;
        struct my_error_mgr jerr;

        cinfo.err = jpeg_std_error(&jerr.pub);
        jerr.pub.error_exit = my_error_exit;

        if (setjmp(jerr.setjmp_buffer)) {
            jpeg_destroy_compress(&cinfo);
            fclose(outfile);
            if (free_surface) SDL_FreeSurface(rgb_surface);
            return false;
        }

        jpeg_create_compress(&cinfo);
        jpeg_stdio_dest(&cinfo, outfile);

        cinfo.image_width = rgb_surface->w;
        cinfo.image_height = rgb_surface->h;
        cinfo.input_components = 3;
        cinfo.in_color_space = JCS_RGB;

        jpeg_set_defaults(&cinfo);
        jpeg_set_quality(&cinfo, quality, TRUE); // TRUE enforces Baseline JPEG (not progressive)
        jpeg_start_compress(&cinfo, TRUE);

        JSAMPROW row_pointer[1];
        int row_stride = rgb_surface->pitch;

        while (cinfo.next_scanline < cinfo.image_height) {
            row_pointer[0] = (JSAMPLE*)((Uint8*)rgb_surface->pixels + cinfo.next_scanline * row_stride);
            jpeg_write_scanlines(&cinfo, row_pointer, 1);
        }

        jpeg_finish_compress(&cinfo);
        fclose(outfile);
        jpeg_destroy_compress(&cinfo);

        if (free_surface) SDL_FreeSurface(rgb_surface);
        return true;
    }

    DownloadManager& DownloadManager::Instance() {
        static DownloadManager inst;
        return inst;
    }

    DownloadManager::~DownloadManager() {
        worker_running = false;
        CancelDownload();
        if (worker_thread.joinable()) {
            worker_thread.join();
        }
    }

    std::string DownloadManager::SanitizeFilename(const std::string& filename) {
        std::string safe = filename;
        const std::string invalid_chars = "\\/:*?\"<>|";
        for (char& c : safe) {
            if (invalid_chars.find(c) != std::string::npos || c < 32) {
                c = '_';
            }
        }
        return safe;
    }

    std::string DownloadManager::NormalizePath(const std::string& path) {
        std::string p = path;
        std::replace(p.begin(), p.end(), '\\', '/');
        while (p.find("../") != std::string::npos) {
            p.replace(p.find("../"), 3, "");
        }
        return p;
    }

    bool DownloadManager::IsInsideAllowedRoot(const std::string& path, const std::string& root) {
        return path.find(root) == 0;
    }

    std::string DownloadManager::ResolveGameInstallPath(const std::string& platform_slug, const std::string& filename) {
        auto& config = ConfigManager::Instance();
        std::string resolved_slug = NormalizePlatformSlug(platform_slug);
        std::string root = config.GetRomPath(resolved_slug);
        std::string final_path = NormalizePath(root + SanitizeFilename(filename));
        return final_path;
    }

    bool DownloadManager::IsGameInstalled(const std::string& platform_slug, const std::string& filename) {
        std::string final_path = ResolveGameInstallPath(platform_slug, filename);
        
        if (platform_slug == "ps2") {
            if (IsLogicalFileValid(final_path)) {
                return true;
            }
        }

        struct stat buffer;
        bool exists = (stat(final_path.c_str(), &buffer) == 0 && buffer.st_size > 0);
        return exists;
    }

    void DownloadManager::LoadInstalledIndex() {
        std::lock_guard<std::mutex> lock(index_mutex);
        installed_index.clear();
        
        std::ifstream file("sdmc:/switch/romm-nx/installed_index.json");
        if (!file.is_open()) return;
        
        std::string content((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        file.close();
        
        size_t pos = 0;
        while ((pos = content.find("{", pos)) != std::string::npos) {
            size_t end_pos = content.find("}", pos);
            if (end_pos == std::string::npos) break;
            std::string block = content.substr(pos, end_pos - pos + 1);
            
            std::string key;
            if (jsonExtractString(block, "key", key) && !key.empty()) {
                InstalledIndexEntry entry;
                jsonExtractString(block, "platform_slug", entry.platform_slug);
                jsonExtractString(block, "title", entry.title);
                jsonExtractString(block, "original_filename", entry.original_filename);
                jsonExtractString(block, "install_path", entry.install_path);
                jsonExtractString(block, "cover_path", entry.cover_path);
                jsonExtractInt(block, "rom_id", entry.rom_id);
                installed_index[key] = entry;
            }
            pos = end_pos + 1;
        }
        std::cout << "[INDEX] Loaded entries=" << installed_index.size() << std::endl;
    }

    // Minimal JSON string escaping. Game titles routinely contain double
    // quotes and backslashes ("Tom Clancy's ...", Windows-style paths), and
    // writing them raw produced a file the reader then mis-parsed — the entry's
    // title came back empty or truncated, which is why Installed rows could end
    // up showing nothing but an SD-card icon.
    static std::string JsonEscape(const std::string& s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (char c : s) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if ((unsigned char)c < 0x20) {
                        char buf[8];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    void DownloadManager::SaveInstalledIndex() {
        std::lock_guard<std::mutex> lock(index_mutex);
        std::string json_out = "[\n";
        bool first = true;
        for (const auto& pair : installed_index) {
            if (!first) json_out += ",\n";
            first = false;
            json_out += "  {\n";
            json_out += "    \"key\": \"" + JsonEscape(pair.first) + "\",\n";
            json_out += "    \"platform_slug\": \"" + JsonEscape(pair.second.platform_slug) + "\",\n";
            json_out += "    \"rom_id\": " + std::to_string(pair.second.rom_id) + ",\n";
            json_out += "    \"title\": \"" + JsonEscape(pair.second.title) + "\",\n";
            json_out += "    \"original_filename\": \"" + JsonEscape(pair.second.original_filename) + "\",\n";
            json_out += "    \"install_path\": \"" + JsonEscape(pair.second.install_path) + "\",\n";
            json_out += "    \"cover_path\": \"" + JsonEscape(pair.second.cover_path) + "\"\n";
            json_out += "  }";
        }
        json_out += "\n]\n";
        
        mkdir("sdmc:/switch/romm-nx", 0777);
        std::ofstream file("sdmc:/switch/romm-nx/installed_index.json");
        if (file.is_open()) {
            file << json_out;
            file.close();
        }
    }

    static std::string FindLocalCover(const std::string& platform_slug, const std::string& filename) {
        std::string resolved_slug = NormalizePlatformSlug(platform_slug);
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

    void DownloadManager::ReconcileInstalledIndex() {
        LoadInstalledIndex();
        
        int ps1_kept = 0, ps1_removed = 0, ps1_added_raw = 0;
        int psp_kept = 0, psp_removed = 0, psp_added_raw = 0;
        int nds_kept = 0, nds_removed = 0, nds_added_raw = 0;
        int gb_kept = 0, gb_removed = 0, gb_added_raw = 0;
        int gbc_kept = 0, gbc_removed = 0, gbc_added_raw = 0;
        int gba_kept = 0, gba_removed = 0, gba_added_raw = 0;
        int ps2_kept = 0, ps2_removed = 0, ps2_added_raw = 0;
        int n3ds_kept = 0, n3ds_removed = 0, n3ds_added_raw = 0;

        std::map<std::string, InstalledIndexEntry> normalized_index;

        {
            std::lock_guard<std::mutex> lock(index_mutex);
            for (auto& pair : installed_index) {
                auto entry = pair.second;
                bool exists = false;
                if (entry.platform_slug == "ps2") {
                    exists = IsLogicalFileValid(entry.install_path);
                } else {
                    struct stat buffer;
                    exists = (stat(entry.install_path.c_str(), &buffer) == 0 && buffer.st_size > 0);
                }
                
                if (exists) {
                    entry.platform_slug = NormalizePlatformSlug(entry.platform_slug);
                    if (entry.cover_path.empty()) {
                        std::string fname = entry.original_filename.empty() ? entry.title : entry.original_filename;
                        entry.cover_path = FindLocalCover(entry.platform_slug, fname);
                    }
                    
                    std::string fname = entry.original_filename;
                    if (fname.empty()) {
                        fname = entry.title;
                    }
                    std::string new_key = entry.platform_slug + "|" + fname;
                    normalized_index[new_key] = entry;
                    
                    if (entry.platform_slug == "psp") psp_kept++;
                    else if (entry.platform_slug == "nds") nds_kept++;
                    else if (entry.platform_slug == "gb") gb_kept++;
                    else if (entry.platform_slug == "gbc") gbc_kept++;
                    else if (entry.platform_slug == "gba") gba_kept++;
                    else if (entry.platform_slug == "ps2") ps2_kept++;
                    else if (entry.platform_slug == "3ds") n3ds_kept++;
                    else ps1_kept++;
                } else {
                    if (entry.platform_slug == "psp") psp_removed++;
                    else if (entry.platform_slug == "nds") nds_removed++;
                    else if (entry.platform_slug == "gb") gb_removed++;
                    else if (entry.platform_slug == "gbc") gbc_removed++;
                    else if (entry.platform_slug == "gba") gba_removed++;
                    else if (entry.platform_slug == "ps2") ps2_removed++;
                    else if (entry.platform_slug == "3ds") n3ds_removed++;
                    else ps1_removed++;
                }
            }
            installed_index = normalized_index;
        }
        
        // Scan ps1 for raw files
        std::string ps1_root = ConfigManager::Instance().GetRomPath("psx");
        DIR* ps1_dir = opendir(ps1_root.c_str());
        if (ps1_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(ps1_dir)) != NULL) {
                if (ent->d_type == DT_REG) {
                    files.push_back(ent->d_name);
                }
            }
            closedir(ps1_dir);
            
            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string ext = "";
                size_t dot = fname.find_last_of('.');
                if (dot != std::string::npos) ext = fname.substr(dot);
                
                if (ext == ".chd" || ext == ".pbp" || ext == ".iso" || ext == ".m3u" || ext == ".bin") {
                    if (ext == ".bin") {
                        std::string cue_name = fname.substr(0, dot) + ".cue";
                        if (std::find(files.begin(), files.end(), cue_name) != files.end()) {
                            continue;
                        }
                    }
                    
                    std::string key = "psx|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        InstalledIndexEntry raw;
                        raw.platform_slug = "psx";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = ps1_root + fname;
                        raw.cover_path = FindLocalCover("psx", fname);
                        installed_index[key] = raw;
                        ps1_added_raw++;
                    }
                }
            }
        }

        // Scan psp for raw files
        std::string psp_root = ConfigManager::Instance().GetRomPath("psp");
        DIR* psp_dir = opendir(psp_root.c_str());
        if (psp_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(psp_dir)) != NULL) {
                if (ent->d_type == DT_REG) {
                    files.push_back(ent->d_name);
                }
            }
            closedir(psp_dir);
            
            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string ext = "";
                size_t dot = fname.find_last_of('.');
                if (dot != std::string::npos) ext = fname.substr(dot);
                
                std::string ext_lower = ext;
                std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                
                if (ext_lower == ".iso" || ext_lower == ".cso" || ext_lower == ".pbp") {
                    std::string key = "psp|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        InstalledIndexEntry raw;
                        raw.platform_slug = "psp";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = psp_root + fname;
                        raw.cover_path = FindLocalCover("psp", fname);
                        installed_index[key] = raw;
                        psp_added_raw++;
                    }
                }
            }
        }

        // Scan nds for raw files
        std::string nds_root = ConfigManager::Instance().GetRomPath("nds");
        DIR* nds_dir = opendir(nds_root.c_str());
        if (nds_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(nds_dir)) != NULL) {
                if (ent->d_type == DT_REG) {
                    files.push_back(ent->d_name);
                }
            }
            closedir(nds_dir);
            
            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string ext = "";
                size_t dot = fname.find_last_of('.');
                if (dot != std::string::npos) ext = fname.substr(dot);
                
                std::string ext_lower = ext;
                std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                
                if (ext_lower == ".nds") {
                    std::string key = "nds|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        InstalledIndexEntry raw;
                        raw.platform_slug = "nds";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = nds_root + fname;
                        raw.cover_path = FindLocalCover("nds", fname);
                        installed_index[key] = raw;
                        nds_added_raw++;
                    }
                }
            }
        }
        // Scan gb for raw files
        std::string gb_root = ConfigManager::Instance().GetRomPath("gb");
        DIR* gb_dir = opendir(gb_root.c_str());
        if (gb_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(gb_dir)) != NULL) {
                if (ent->d_type == DT_REG) {
                    files.push_back(ent->d_name);
                }
            }
            closedir(gb_dir);
            
            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string ext = "";
                size_t dot = fname.find_last_of('.');
                if (dot != std::string::npos) ext = fname.substr(dot);
                
                std::string ext_lower = ext;
                std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                
                if (ext_lower == ".gb") {
                    std::string key = "gb|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        InstalledIndexEntry raw;
                        raw.platform_slug = "gb";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = gb_root + fname;
                        raw.cover_path = FindLocalCover("gb", fname);
                        installed_index[key] = raw;
                        gb_added_raw++;
                    }
                }
            }
        }

        // Scan gbc for raw files
        std::string gbc_root = ConfigManager::Instance().GetRomPath("gbc");
        DIR* gbc_dir = opendir(gbc_root.c_str());
        if (gbc_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(gbc_dir)) != NULL) {
                if (ent->d_type == DT_REG) {
                    files.push_back(ent->d_name);
                }
            }
            closedir(gbc_dir);
            
            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string ext = "";
                size_t dot = fname.find_last_of('.');
                if (dot != std::string::npos) ext = fname.substr(dot);
                
                std::string ext_lower = ext;
                std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                
                if (ext_lower == ".gbc") {
                    std::string key = "gbc|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        InstalledIndexEntry raw;
                        raw.platform_slug = "gbc";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = gbc_root + fname;
                        raw.cover_path = FindLocalCover("gbc", fname);
                        installed_index[key] = raw;
                        gbc_added_raw++;
                    }
                }
            }
        }

        // Scan gba for raw files
        std::string gba_root = ConfigManager::Instance().GetRomPath("gba");
        DIR* gba_dir = opendir(gba_root.c_str());
        if (gba_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(gba_dir)) != NULL) {
                if (ent->d_type == DT_REG) {
                    files.push_back(ent->d_name);
                }
            }
            closedir(gba_dir);
            
            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string ext = "";
                size_t dot = fname.find_last_of('.');
                if (dot != std::string::npos) ext = fname.substr(dot);
                
                std::string ext_lower = ext;
                std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                
                if (ext_lower == ".gba") {
                    std::string key = "gba|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        InstalledIndexEntry raw;
                        raw.platform_slug = "gba";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = gba_root + fname;
                        raw.cover_path = FindLocalCover("gba", fname);
                        installed_index[key] = raw;
                        gba_added_raw++;
                    }
                }
            }
        }

        // Scan ps2 for raw files
        std::string ps2_root = ConfigManager::Instance().GetRomPath("ps2");
        DIR* ps2_dir = opendir(ps2_root.c_str());
        if (ps2_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(ps2_dir)) != NULL) {
                if (ent->d_type == DT_REG || ent->d_type == DT_DIR || ent->d_type == DT_UNKNOWN) {
                    std::string fname = ent->d_name;
                    if (fname.find(".part") != std::string::npos) {
                        continue;
                    }
                    std::string ext = "";
                    size_t dot = fname.find_last_of('.');
                    if (dot != std::string::npos) ext = fname.substr(dot);
                    
                    std::string ext_lower = ext;
                    std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);
                    
                    if (ext_lower == ".iso" || ext_lower == ".chd") {
                        files.push_back(fname);
                    }
                }
            }
            closedir(ps2_dir);
            
            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string full_path = ps2_root + fname;
                if (IsLogicalFileValid(full_path)) {
                    std::string key = "ps2|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        s64 sz = 0;
                        FsFileSystem* fs = nullptr;
                        char fs_path[FS_MAX_PATH] = {0};
                        if (fsdevTranslatePath(full_path.c_str(), &fs, fs_path) == 0) {
                            FsFile f;
                            if (R_SUCCEEDED(fsFsOpenFile(fs, fs_path, FsOpenMode_Read, &f))) {
                                fsFileGetSize(&f, &sz);
                                fsFileClose(&f);
                            }
                        }

                        InstalledIndexEntry raw;
                        raw.platform_slug = "ps2";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = full_path;
                        raw.size = sz;
                        raw.cover_path = FindLocalCover("ps2", fname);
                        installed_index[key] = raw;
                        ps2_added_raw++;
                    }
                }
            }
        }

        // Scan 3ds for raw files
        std::string n3ds_root = ConfigManager::Instance().GetRomPath("3ds");
        DIR* n3ds_dir = opendir(n3ds_root.c_str());
        if (n3ds_dir) {
            struct dirent* ent;
            std::vector<std::string> files;
            while ((ent = readdir(n3ds_dir)) != NULL) {
                if (ent->d_type == DT_REG) {
                    files.push_back(ent->d_name);
                }
            }
            closedir(n3ds_dir);

            std::lock_guard<std::mutex> lock(index_mutex);
            for (const auto& fname : files) {
                std::string ext = "";
                size_t dot = fname.find_last_of('.');
                if (dot != std::string::npos) ext = fname.substr(dot);

                std::string ext_lower = ext;
                std::transform(ext_lower.begin(), ext_lower.end(), ext_lower.begin(), ::tolower);

                if (ext_lower == ".3ds" || ext_lower == ".cia" || ext_lower == ".cci") {
                    std::string key = "3ds|" + fname;
                    if (installed_index.find(key) == installed_index.end()) {
                        InstalledIndexEntry raw;
                        raw.platform_slug = "3ds";
                        raw.title = fname;
                        raw.original_filename = fname;
                        raw.install_path = n3ds_root + fname;
                        raw.cover_path = FindLocalCover("3ds", fname);
                        installed_index[key] = raw;
                        n3ds_added_raw++;
                    }
                }
            }
        }

        std::cout << "[INDEX] Reconciled kept=" << (ps1_kept + psp_kept + nds_kept + gb_kept + gbc_kept + gba_kept + ps2_kept + n3ds_kept)
                  << " removed=" << (ps1_removed + psp_removed + nds_removed + gb_removed + gbc_removed + gba_removed + ps2_removed + n3ds_removed)
                  << " added_raw=" << (ps1_added_raw + psp_added_raw + nds_added_raw + gb_added_raw + gbc_added_raw + gba_added_raw + ps2_added_raw + n3ds_added_raw) << std::endl;
        std::cout << "[INDEX] Reconcile platform=psp path=" << psp_root << " kept=" << psp_kept << " removed=" << psp_removed << " added_raw=" << psp_added_raw << std::endl;
        std::cout << "[INDEX] Reconcile platform=nds path=" << nds_root << " kept=" << nds_kept << " removed=" << nds_removed << " added_raw=" << nds_added_raw << std::endl;
        std::cout << "[INDEX] Reconcile platform=gb path=" << gb_root << " kept=" << gb_kept << " removed=" << gb_removed << " added_raw=" << gb_added_raw << std::endl;
        std::cout << "[INDEX] Reconcile platform=gbc path=" << gbc_root << " kept=" << gbc_kept << " removed=" << gbc_removed << " added_raw=" << gbc_added_raw << std::endl;
        std::cout << "[INDEX] Reconcile platform=gba path=" << gba_root << " kept=" << gba_kept << " removed=" << gba_removed << " added_raw=" << gba_added_raw << std::endl;
        std::cout << "[INDEX] Reconcile platform=ps2 path=" << ps2_root << " kept=" << ps2_kept << " removed=" << ps2_removed << " added_raw=" << ps2_added_raw << std::endl;
        std::cout << "[INDEX] Reconcile platform=3ds path=" << n3ds_root << " kept=" << n3ds_kept << " removed=" << n3ds_removed << " added_raw=" << n3ds_added_raw << std::endl;

        if (ps1_removed > 0 || ps1_added_raw > 0 || psp_removed > 0 || psp_added_raw > 0 || nds_removed > 0 || nds_added_raw > 0 ||
            gb_removed > 0 || gb_added_raw > 0 || gbc_removed > 0 || gbc_added_raw > 0 || gba_removed > 0 || gba_added_raw > 0 ||
            ps2_removed > 0 || ps2_added_raw > 0 || n3ds_removed > 0 || n3ds_added_raw > 0) {
            SaveInstalledIndex();
        }
    }

    std::map<std::string, InstalledIndexEntry> DownloadManager::GetInstalledIndex() const {
        // Safe copy
        return installed_index;
    }

    // ---------------------------------------------------------------------------
    // Install state cache
    // ---------------------------------------------------------------------------

    void DownloadManager::InvalidateInstallCache() {
        std::lock_guard<std::mutex> lock(install_cache_mutex);
        install_cache.clear();
    }

    void DownloadManager::RefreshInstallCache(const std::string& platform_slug, const std::string& filename) {
        bool exists = IsGameInstalled(platform_slug, filename);
        std::string key = platform_slug + "|" + filename;
        std::lock_guard<std::mutex> lock(install_cache_mutex);
        install_cache[key] = exists;
    }

    bool DownloadManager::GetCachedInstallState(const std::string& platform_slug, const std::string& filename) {
        std::string key = platform_slug + "|" + filename;
        {
            std::lock_guard<std::mutex> lock(install_cache_mutex);
            auto it = install_cache.find(key);
            if (it != install_cache.end()) {
                return it->second;
            }
        }
        // Cache miss — fall back to filesystem query and populate cache
        bool exists = IsGameInstalled(platform_slug, filename);
        std::lock_guard<std::mutex> lock(install_cache_mutex);
        install_cache[key] = exists;
        return exists;
    }

    void DownloadManager::UninstallGame(const std::string& platform_slug, const std::string& filename, const std::string& cover_path) {
        std::string safe_name = SanitizeFilename(filename);
        {
            std::lock_guard<std::mutex> lock(task_mutex);
            for (auto it = download_queue.begin(); it != download_queue.end(); ) {
                if (it->filename == safe_name) {
                    if (it->state == DownloadState::DownloadingGame || it->state == DownloadState::DownloadingCover || it->state == DownloadState::SyncingCover || it->state == DownloadState::Preparing) {
                        cancel_requested = true;
                        ++it;
                    } else {
                        it = download_queue.erase(it);
                    }
                } else {
                    ++it;
                }
            }
        }
        
        std::string resolved_slug = NormalizePlatformSlug(platform_slug);

        std::string final_path = ResolveGameInstallPath(resolved_slug, safe_name);
        std::string part_path = final_path + ".part";

        DeleteLogicalFile(final_path);
        DeleteLogicalFile(part_path);

        // Multi-disc sweep: a multi-disc game is identified either by its root .m3u
        // (current subfolder layout) or by a "(Disc N)" filename (older flat layout).
        // Either way, remove the whole set — the per-game subfolder, any flat sibling
        // discs/playlist at the root, and the merged-entry DuckStation cover — so it
        // uninstalls as one game instead of leaving orphans behind.
        std::string disc_base = StripDiscSuffix(safe_name);
        {
            std::string lower_name = safe_name;
            std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
            bool ends_m3u = lower_name.size() >= 4 && lower_name.compare(lower_name.size() - 4, 4, ".m3u") == 0;
            bool has_disc_marker = lower_name.find("(disc") != std::string::npos ||
                                   lower_name.find("(disk") != std::string::npos;
            if ((ends_m3u || has_disc_marker) && !disc_base.empty()) {
                std::string root = ConfigManager::Instance().GetRomPath(resolved_slug);

                // 1. Per-game subfolder (subfolder layout) — deletes the discs inside.
                std::string subdir = root + disc_base;
                struct stat sb;
                if (stat(subdir.c_str(), &sb) == 0) {
                    DeleteLogicalFile(subdir);
                    std::cout << "[UNINSTALL] Removed multi-disc subfolder: " << subdir << std::endl;
                }

                // 2. Any flat sibling discs / playlist at the root (old flat layout).
                DIR* dir = opendir(root.c_str());
                if (dir) {
                    struct dirent* ent;
                    std::vector<std::string> to_delete;
                    while ((ent = readdir(dir)) != NULL) {
                        std::string cand = ent->d_name;
                        if (cand == "." || cand == "..") continue;
                        if (StripDiscSuffix(cand) == disc_base) {
                            to_delete.push_back(root + cand);
                        }
                    }
                    closedir(dir);
                    for (const auto& p : to_delete) {
                        DeleteLogicalFile(p);
                        std::cout << "[UNINSTALL] Removed multi-disc sibling: " << p << std::endl;
                    }
                }

                // 3. DuckStation cover for the merged (m3u) entry.
                if (resolved_slug == "psx") {
                    std::string duck_m3u_cover = "sdmc:/switch/duckstation/covers/" + disc_base + ".jpg";
                    if (unlink(duck_m3u_cover.c_str()) == 0) {
                        std::cout << "[COVER] Deleted m3u cover: " << duck_m3u_cover << std::endl;
                    }
                }
            }
        }

        std::string base_no_ext = safe_name;
        size_t last_dot = base_no_ext.find_last_of('.');
        if (last_dot != std::string::npos) base_no_ext = base_no_ext.substr(0, last_dot);

        if (resolved_slug == "psx") {
            std::string duck_cover_path = "sdmc:/switch/duckstation/covers/" + base_no_ext + ".jpg";
            if (unlink(duck_cover_path.c_str()) == 0) {
                std::cout << "[COVER] Deleted cover: " << duck_cover_path << std::endl;
            } else {
                std::cout << "[COVER] Cover not found, skipping: " << duck_cover_path << std::endl;
            }
        }

        if (!cover_path.empty()) {
            std::string cache_path = "sdmc:/switch/romm-nx/cache/covers/" + resolved_slug + "/" + base_no_ext + ".jpg";
            if (unlink(cache_path.c_str()) == 0) {
                std::cout << "[COVER] Deleted cache cover: " << cache_path << std::endl;
            }
        }

        ReconcileInstalledIndex();
    }

    std::string DownloadManager::InstallIdentityFilename(const std::string& platform_slug,
                                                         const std::vector<RomFileEntry>& files,
                                                         const std::string& fallback) {
        std::string slug = NormalizePlatformSlug(platform_slug);
        if (slug == "psx" && files.size() > 1) {
            std::string base = StripDiscSuffix(SanitizeFilename(files.front().file_name));
            if (!base.empty()) return base + ".m3u";
        }
        if (!files.empty()) return files.front().file_name;
        return fallback;
    }

    void DownloadManager::EnqueueDownload(const GameDetail& detail, const std::string& platform_slug, const std::string& title) {
        std::lock_guard<std::mutex> lock(task_mutex);
        
        std::string resolved_slug = NormalizePlatformSlug(platform_slug);

        if (resolved_slug != "psx" && resolved_slug != "psp" && resolved_slug != "nds" &&
            resolved_slug != "gb" && resolved_slug != "gbc" && resolved_slug != "gba" &&
            resolved_slug != "ps2" && resolved_slug != "3ds") {
            std::cerr << "[Download] Downloads are not implemented for platform: " << resolved_slug << std::endl;
            return;
        }

        // Assemble the list of physical files. Single-disc games have one; a
        // multi-disc game (folder > discs on RomM) has one entry per disc, each
        // downloaded individually by its own file id.
        std::vector<RomFileEntry> src_files = detail.files;
        if (src_files.empty() && detail.file_id != 0 && !detail.file_name.empty()) {
            RomFileEntry rf;
            rf.id = detail.file_id;
            rf.file_name = detail.file_name;
            rf.file_size_bytes = detail.file_size_bytes;
            src_files.push_back(rf);
        }

        if (src_files.empty()) {
            std::cerr << "[Download] Missing download endpoint/file data." << std::endl;
            return;
        }

        for (const auto& t : download_queue) {
            if (t.rom_id == detail.rom_id) {
                return;
            }
        }

        // Per-platform extension whitelist, applied to every disc.
        auto ext_allowed = [&](const std::string& fname) -> bool {
            std::string ext;
            size_t dot = fname.find_last_of('.');
            if (dot != std::string::npos) ext = fname.substr(dot);
            std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
            if (resolved_slug == "psp") return ext == ".iso" || ext == ".cso" || ext == ".pbp";
            if (resolved_slug == "ps2") return ext == ".iso" || ext == ".chd";
            return true;
        };

        DownloadTask task;
        task.rom_id = detail.rom_id;
        task.title = title;
        task.state = DownloadState::Queued;
        task.downloaded_bytes = 0;
        task.error_message = "";
        task.platform_slug = resolved_slug;

        auto& config = ConfigManager::Instance();
        std::string root_dir = config.GetRomPath(resolved_slug);

        long long total = 0;
        for (const auto& sf : src_files) {
            if (!ext_allowed(sf.file_name)) {
                std::cerr << "[Download] Skipping file with disallowed extension for "
                          << resolved_slug << ": " << sf.file_name << std::endl;
                continue;
            }
            DownloadFile df;
            df.file_id = sf.id;
            df.original_filename = sf.file_name;
            df.filename = SanitizeFilename(sf.file_name);
            df.total_bytes = sf.file_size_bytes;
            df.final_path = ResolveGameInstallPath(resolved_slug, df.filename);
            df.part_path = df.final_path + ".part";

            if (!IsInsideAllowedRoot(df.final_path, root_dir)) {
                std::cerr << "[Download] Path validation failed for " << df.final_path << std::endl;
                return;
            }
            total += df.total_bytes;
            task.files.push_back(df);
        }

        if (task.files.empty()) {
            std::cerr << "[Download] No downloadable files after extension filtering." << std::endl;
            return;
        }

        // Multi-disc PS1: the discs live in a per-game subfolder and only the .m3u
        // sits at the platform root, so DuckStation lists a single game (with a
        // non-recursive PS1 search directory) instead of one entry per disc.
        bool psx_multidisc = (resolved_slug == "psx" && task.files.size() > 1);
        std::string disc_base; // subfolder / m3u / cover base name for a disc set
        if (psx_multidisc) {
            disc_base = StripDiscSuffix(task.files.front().filename);
            if (!disc_base.empty()) {
                std::string subdir = root_dir + disc_base + "/";
                for (auto& df : task.files) {
                    df.final_path = subdir + df.filename;
                    df.part_path = df.final_path + ".part";
                }
            } else {
                psx_multidisc = false; // couldn't derive a base; fall back to flat
            }
        }

        // Scalar fields mirror the first file for the cover/index/UI code.
        task.file_id = task.files.front().file_id;
        task.filename = task.files.front().filename;
        task.original_filename = task.files.front().original_filename;
        task.final_path = task.files.front().final_path;
        task.part_path = task.files.front().part_path;
        // Fall back to the rom-level total if per-file sizes weren't reported, so the
        // progress bar still has a denominator.
        task.total_bytes = (total > 0) ? total : detail.file_size_bytes;

        std::string raw_cover_url = detail.path_cover_large;
        if (raw_cover_url.empty()) raw_cover_url = detail.path_cover_small;
        if (!raw_cover_url.empty()) {
            if (raw_cover_url.find("http") == 0) task.cover_url = raw_cover_url;
            else task.cover_url = config.GetRommHost() + raw_cover_url;
        } else {
            task.cover_url = "";
        }
        task.cover_path_rel = raw_cover_url;

        // Cover cache is keyed by the disc-set base for multi-disc (matching the .m3u
        // identity that the Installed menu / reconcile look up), else by the file.
        std::string cover_base;
        if (psx_multidisc) {
            cover_base = disc_base;
        } else {
            cover_base = task.filename;
            size_t last_dot = cover_base.find_last_of('.');
            if (last_dot != std::string::npos) cover_base = cover_base.substr(0, last_dot);
        }
        task.cover_cache_path = "sdmc:/switch/romm-nx/cache/covers/" + resolved_slug + "/" + cover_base + ".jpg";

        // Refuse only if the whole game is already present; otherwise let the worker
        // fill in whichever discs (or the .m3u) are missing.
        bool all_present = true;
        for (const auto& df : task.files) {
            struct stat b;
            if (!(stat(df.final_path.c_str(), &b) == 0 && b.st_size > 0)) {
                all_present = false;
                break;
            }
        }
        if (all_present && psx_multidisc) {
            std::string m3u_path = root_dir + disc_base + ".m3u";
            struct stat b;
            if (!(stat(m3u_path.c_str(), &b) == 0 && b.st_size > 0)) {
                all_present = false; // discs there but playlist missing — regenerate it
            }
        }
        if (all_present) {
            std::cout << "[DOWNLOAD] Refused, already installed rom_id=" << task.rom_id << std::endl;
            return;
        }

        download_queue.push_back(task);
        
        if (!worker_running) {
            worker_running = true;
            if (worker_thread.joinable()) worker_thread.join();
            worker_thread = std::thread(&DownloadManager::DownloadWorker, this);
        }
        ScreenWakeManager::Instance().RequestUpdate();
    }

    void DownloadManager::RemoveFromQueue(int rom_id) {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (auto it = download_queue.begin(); it != download_queue.end(); ) {
            if (it->rom_id == rom_id) {
                if (it->state == DownloadState::DownloadingGame || it->state == DownloadState::DownloadingCover || it->state == DownloadState::SyncingCover || it->state == DownloadState::Preparing) {
                    cancel_requested = true;
                    ++it;
                } else {
                    it = download_queue.erase(it);
                }
            } else {
                ++it;
            }
        }
        ScreenWakeManager::Instance().RequestUpdate();
    }

    void DownloadManager::ClearCompleted() {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (auto it = download_queue.begin(); it != download_queue.end(); ) {
            if (it->state == DownloadState::Completed || it->state == DownloadState::Cancelled) {
                it = download_queue.erase(it);
            } else {
                ++it;
            }
        }
    }

    void DownloadManager::RetryFailed(int rom_id) {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (auto& t : download_queue) {
            if (t.rom_id == rom_id && (t.state == DownloadState::Failed || t.state == DownloadState::Cancelled)) {
                t.state = DownloadState::Queued;
                t.error_message = "";
                if (!worker_running) {
                    worker_running = true;
                    if (worker_thread.joinable()) worker_thread.join();
                    worker_thread = std::thread(&DownloadManager::DownloadWorker, this);
                }
            }
        }
        ScreenWakeManager::Instance().RequestUpdate();
    }

    void DownloadManager::CancelDownload() {
        cancel_requested = true;
        ScreenWakeManager::Instance().RequestUpdate();
    }

    DownloadTask DownloadManager::GetActiveDownloadSnapshot() {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (const auto& t : download_queue) {
            if (t.state == DownloadState::DownloadingGame || t.state == DownloadState::DownloadingCover || t.state == DownloadState::SyncingCover || t.state == DownloadState::Preparing) {
                return t;
            }
        }
        return DownloadTask();
    }

    bool DownloadManager::HasActiveDownload() {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (const auto& t : download_queue) {
            DownloadState s = t.state.load();
            if (s == DownloadState::DownloadingGame || s == DownloadState::Preparing || s == DownloadState::Queued) {
                return true;
            }
        }
        return false;
    }

    std::vector<DownloadTask> DownloadManager::GetQueueSnapshot() {
        std::lock_guard<std::mutex> lock(task_mutex);
        std::vector<DownloadTask> snap;
        for (const auto& t : download_queue) {
            snap.push_back(t);
        }
        return snap;
    }

    DownloadTask DownloadManager::GetTaskSnapshot(int rom_id) {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (const auto& t : download_queue) {
            if (t.rom_id == rom_id) return t;
        }
        return DownloadTask();
    }

    void DownloadManager::SetCurrentDownloadedBytes(long long bytes) {
        static auto last_time = std::chrono::high_resolution_clock::now();
        static long long last_bytes = 0;

        // curl reports dlnow relative to the current file; add the bytes already
        // finished by earlier discs so a multi-disc task's counter is cumulative.
        bytes += active_base_bytes.load();

        auto now = std::chrono::high_resolution_clock::now();
        auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_time).count();

        size_t current_speed = 0;
        if (dt > 250) { // Update speed every 250ms
            long long delta_bytes = bytes - last_bytes;
            if (delta_bytes < 0) delta_bytes = 0;
            current_speed = (size_t)((delta_bytes * 1000.0) / dt);
            last_time = now;
            last_bytes = bytes;
        }

        std::lock_guard<std::mutex> lock(task_mutex);
        for (auto& t : download_queue) {
            if (t.state == DownloadState::DownloadingGame || t.state == DownloadState::DownloadingCover || t.state == DownloadState::SyncingCover || t.state == DownloadState::Preparing) {
                t.downloaded_bytes.store(bytes);
                if (dt > 250) {
                    t.download_speed_bps.store(current_speed);
                }
                break;
            }
        }
    }

    struct DownloadWriter {
        bool is_big_file = false;
        FILE* file_ptr = nullptr;
        FsFile fs_file = {0};
        bool fs_file_open = false;
        Result last_write_error = 0;
        s64 write_offset = 0;
        bool crossed_4GiB_logged = false;

        // BigFile writes go straight to the FS sysmodule via fsFileWrite, which is
        // a synchronous IPC round-trip. curl hands us one callback per TLS record
        // (~16 KB), so without batching a 4+ GB ISO means hundreds of thousands of
        // individual IPC calls and throughput tanks. Coalesce into large chunks
        // before touching the filesystem.
        static constexpr size_t kFlushBufferSize = 4 * 1024 * 1024; // 4 MB
        std::vector<u8> flush_buffer;
        size_t flush_buffer_used = 0;
    };

    static bool FlushBigFileBuffer(DownloadWriter* writer) {
        if (writer->flush_buffer_used == 0) return true;

        s64 next_offset = writer->write_offset + (s64)writer->flush_buffer_used;
        if (!writer->crossed_4GiB_logged && next_offset > 0xFFFFFFFFLL && writer->write_offset <= 0xFFFFFFFFLL) {
            std::cout << "[BigFile] crossed_4GiB offset=" << next_offset << std::endl;
            writer->crossed_4GiB_logged = true;
        }

        Result rc = fsFileWrite(&(writer->fs_file), writer->write_offset, writer->flush_buffer.data(), writer->flush_buffer_used, FsWriteOption_None);
        if (R_FAILED(rc)) {
            writer->last_write_error = rc;
            std::cerr << "[BigFile] write failed at offset=" << writer->write_offset << " error=" << rc << std::endl;
            return false;
        }
        writer->write_offset += (s64)writer->flush_buffer_used;
        writer->flush_buffer_used = 0;
        return true;
    }

    static bool IsLogicalFileValid(const std::string& path) {
        FsFileSystem* fs = nullptr;
        char fs_path[FS_MAX_PATH] = {0};
        if (fsdevTranslatePath(path.c_str(), &fs, fs_path) != 0) {
            return false;
        }

        FsFile f;
        Result rc = fsFsOpenFile(fs, fs_path, FsOpenMode_Read, &f);
        if (R_SUCCEEDED(rc)) {
            s64 sz = 0;
            rc = fsFileGetSize(&f, &sz);
            fsFileClose(&f);
            if (R_SUCCEEDED(rc) && sz > 0) {
                return true;
            }
        }
        return false;
    }

    static Result DeleteLogicalFile(const std::string& stdio_path) {
        FsFileSystem* fs = nullptr;
        char fs_path[FS_MAX_PATH] = {0};
        if (fsdevTranslatePath(stdio_path.c_str(), &fs, fs_path) != 0) {
            unlink(stdio_path.c_str());
            return -1;
        }

        // 1. Try fsFsDeleteFile first
        Result rc = fsFsDeleteFile(fs, fs_path);
        if (R_SUCCEEDED(rc)) {
            return rc;
        }

        // 2. If it fails, check if it's a directory
        FsDirEntryType type;
        if (R_SUCCEEDED(fsFsGetEntryType(fs, fs_path, &type))) {
            if (type == FsDirEntryType_Dir) {
                // Confirm that it cannot be opened as a logical file
                FsFile f;
                Result open_rc = fsFsOpenFile(fs, fs_path, FsOpenMode_Read, &f);
                if (R_FAILED(open_rc)) {
                    rc = fsFsDeleteDirectoryRecursively(fs, fs_path);
                    return rc;
                } else {
                    fsFileClose(&f);
                }
            }
        }

        unlink(stdio_path.c_str());
        return rc;
    }



    static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        DownloadWriter* writer = static_cast<DownloadWriter*>(userp);
        size_t bytes_to_write = size * nmemb;
        if (bytes_to_write == 0) return 0;

        if (writer->is_big_file) {
            if (writer->flush_buffer.empty()) {
                writer->flush_buffer.resize(DownloadWriter::kFlushBufferSize);
            }

            const u8* src = static_cast<const u8*>(contents);
            size_t remaining = bytes_to_write;
            while (remaining > 0) {
                size_t space = DownloadWriter::kFlushBufferSize - writer->flush_buffer_used;
                size_t take = std::min(space, remaining);
                std::copy(src, src + take, writer->flush_buffer.begin() + writer->flush_buffer_used);
                writer->flush_buffer_used += take;
                src += take;
                remaining -= take;

                if (writer->flush_buffer_used == DownloadWriter::kFlushBufferSize) {
                    if (!FlushBigFileBuffer(writer)) {
                        return 0; // abort — last_write_error already set
                    }
                }
            }
            return bytes_to_write;
        } else {
            size_t written = fwrite(contents, size, nmemb, writer->file_ptr);
            return written;
        }
    }



    // Modern libcurl requires progress callback to be matching this signature
    static int xferinfo(void *p, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
        DownloadManager* mgr = static_cast<DownloadManager*>(p);
        mgr->SetCurrentDownloadedBytes(dlnow);
        if (mgr->IsCancelRequested()) {
            return 1; // abort transfer
        }
        return 0;
    }

    // "Final Fantasy VII (Europe) (Disc 1).chd" -> "Final Fantasy VII (Europe)".
    // Removes a trailing "(Disc N)"/"(Disk N)" marker and the extension; if no disc
    // marker is present, just strips the extension. Used for m3u/cover naming.
    static std::string StripDiscSuffix(const std::string& filename) {
        std::string lower = filename;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        size_t cut = std::string::npos;
        const char* markers[] = {" (disc", "(disc", " (disk", "(disk"};
        for (const char* m : markers) {
            size_t p = lower.find(m);
            if (p != std::string::npos && (cut == std::string::npos || p < cut)) cut = p;
        }
        std::string base;
        if (cut != std::string::npos) {
            base = filename.substr(0, cut);
        } else {
            base = filename;
            size_t dot = base.find_last_of('.');
            if (dot != std::string::npos) base = base.substr(0, dot);
        }
        while (!base.empty() && base.back() == ' ') base.pop_back();
        return base;
    }

    // Writes an .m3u playlist next to a multi-disc set's disc files, one basename
    // per line, so emulators load the discs as one game. Never clobbers an existing
    // (possibly user-edited) playlist.
    static void WriteMultiDiscPlaylist(const DownloadTask& task) {
        if (task.files.size() < 2) return;
        std::string base = StripDiscSuffix(task.files.front().filename);
        if (base.empty()) return;

        std::string root = ConfigManager::Instance().GetRomPath(task.platform_slug);
        std::string m3u_path = root + base + ".m3u";

        struct stat b;
        if (stat(m3u_path.c_str(), &b) == 0 && b.st_size > 0) {
            std::cout << "[M3U] Already exists, leaving as-is: " << m3u_path << std::endl;
            return;
        }

        std::ofstream out(m3u_path, std::ios::binary);
        if (!out.is_open()) {
            std::cerr << "[M3U] Failed to create playlist: " << m3u_path << std::endl;
            return;
        }
        // Discs live in a subfolder named after the disc-set base; reference them
        // relative to the .m3u (which sits at the platform root).
        for (const auto& f : task.files) {
            out << base << "/" << f.filename << "\n";
        }
        out.close();
        std::cout << "[M3U] Wrote playlist " << m3u_path << " discs=" << task.files.size() << std::endl;
    }

    DownloadManager::FileDownloadOutcome DownloadManager::DownloadFilePhysical(
            int rom_id, DownloadTask& task, const DownloadFile& df, long long base_bytes) {
        FileDownloadOutcome outcome;

        // Skip a file that's already present (resuming a partial multi-disc set).
        struct stat existing;
        if (stat(df.final_path.c_str(), &existing) == 0 && existing.st_size > 0) {
            std::cout << "[Download] File already exists: " << df.final_path << ". Skipping." << std::endl;
            outcome.success = true;
            outcome.final_size = existing.st_size;
            return outcome;
        }

        // Base offset so the shared progress counter reads cumulatively across discs.
        active_base_bytes.store(base_bytes);

        mkdir("sdmc:/roms", 0777);
        std::string platform_dir = ConfigManager::Instance().GetRomPath(task.platform_slug);
        RomPathManager::CreateFolderIfMissing(platform_dir);
        // Ensure the file's parent exists — this creates the per-game subfolder for
        // a multi-disc set (harmless no-op when the file lives at the platform root).
        size_t slash = df.final_path.find_last_of('/');
        if (slash != std::string::npos) {
            std::string parent = df.final_path.substr(0, slash);
            mkdir(parent.c_str(), 0777);
        }
        DeleteLogicalFile(df.part_path);

        bool use_big_file = false;
        if (task.platform_slug == "ps2" && (unsigned long long)df.total_bytes > 0xFFFFFFFFULL) {
            use_big_file = true;
            std::cout << "[BigFile] enabled platform=ps2" << std::endl;
            std::cout << "[BigFile] expected_size=" << df.total_bytes << std::endl;
            std::cout << "[BigFile] temp_path=" << df.part_path << std::endl;
        }

        DownloadWriter writer;
        writer.is_big_file = use_big_file;

        if (use_big_file) {
            FsFileSystem* fs = nullptr;
            char fs_path[FS_MAX_PATH] = {0};
            if (fsdevTranslatePath(df.part_path.c_str(), &fs, fs_path) != 0) {
                std::cerr << "[BigFile] Path translation failed: " << df.part_path << std::endl;
                outcome.error_message = "Path translation failed";
                return outcome;
            }

            Result rc = fsFsCreateFile(fs, fs_path, df.total_bytes, FsCreateOption_BigFile);
            std::cout << "[BigFile] create result=" << rc << std::endl;
            if (R_FAILED(rc)) {
                outcome.error_message = "Failed to create BigFile (error " + std::to_string(rc) + ")";
                return outcome;
            }

            rc = fsFsOpenFile(fs, fs_path, FsOpenMode_Write, &writer.fs_file);
            if (R_FAILED(rc)) {
                std::cerr << "[BigFile] Open file failed: " << rc << std::endl;
                outcome.error_message = "Failed to open BigFile";
                return outcome;
            }
            writer.fs_file_open = true;
            std::cout << "[BigFile] opened successfully" << std::endl;
            writer.write_offset = 0;
        } else {
            writer.file_ptr = fopen(df.part_path.c_str(), "wb");
            if (!writer.file_ptr) {
                std::cerr << "[Download] Could not open .part file for writing." << std::endl;
                outcome.error_message = "Could not create part file";
                return outcome;
            }
            static char file_buf[512 * 1024];
            setvbuf(writer.file_ptr, file_buf, _IOFBF, sizeof(file_buf));
        }

        CURL* curl = curl_easy_init();
        if (!curl) {
            if (use_big_file) {
                fsFileClose(&writer.fs_file);
            } else {
                fclose(writer.file_ptr);
            }
            outcome.error_message = "Curl init failed";
            return outcome;
        }

        char* encoded_name = curl_easy_escape(curl, df.original_filename.c_str(), 0);
        std::string safe_name_url = encoded_name ? encoded_name : df.original_filename;
        if (encoded_name) curl_free(encoded_name);

        auto& config = ConfigManager::Instance();
        std::string url = config.GetRommHost() + "/api/roms/" + std::to_string(df.file_id) + "/files/content/" + safe_name_url;

        SetTaskState(rom_id, DownloadState::DownloadingGame);
        ScreenWakeManager::Instance().RequestUpdate();
        std::cout << "[DOWNLOAD] Start platform=" << task.platform_slug << " title=" << task.title << " file=" << df.filename << std::endl;
        std::cout << "[Download] Starting game download to " << df.part_path << std::endl;

        struct curl_slist* headers = NULL;
        std::string auth = "Authorization: Bearer " + config.GetApiKey();
        headers = curl_slist_append(headers, auth.c_str());

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writer);
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
        curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        // 512 KB receive buffer, paired with the larger TCP window opened in
        // main() via SocketInitConfig — curl silently caps this at its build's
        // CURL_MAX_READ_SIZE if that's smaller, so it's a safe upper bound to ask for.
        curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 524288L);
        // NOTE: CURLOPT_TCP_KEEPALIVE deliberately omitted — the Switch's bsd
        // socket service does not reliably support SO_KEEPALIVE, and enabling
        // it can make curl fail the connection outright on real hardware.

        CURLcode res = curl_easy_perform(curl);

        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        bool download_success = (res == CURLE_OK && !cancel_requested);
        s64 final_logical_size = 0;

        if (download_success) {
            if (use_big_file) {
                // Flush any bytes still sitting in the coalescing buffer before
                // verifying the final size below.
                if (!FlushBigFileBuffer(&writer)) {
                    download_success = false;
                }

                if (download_success) {
                    // 1. verify write_offset == expected_size
                    if (writer.write_offset != df.total_bytes) {
                        std::cerr << "[Download] Size mismatch: write_offset=" << writer.write_offset
                                  << " expected=" << df.total_bytes << std::endl;
                        download_success = false;
                    } else {
                        // 2. fsFileFlush
                        fsFileFlush(&writer.fs_file);

                        // 3. fsFileGetSize and verify temporary logical size
                        s64 temp_size = 0;
                        Result sz_rc = fsFileGetSize(&writer.fs_file, &temp_size);
                        if (R_FAILED(sz_rc) || temp_size != df.total_bytes) {
                            std::cerr << "[Download] Temp size check failed: " << temp_size << " (rc=" << sz_rc << ")" << std::endl;
                            download_success = false;
                        }
                    }
                }

                // 4. fsFileClose
                fsFileClose(&writer.fs_file);
                writer.fs_file_open = false;
                std::cout << "[BigFile] final_written=" << writer.write_offset << std::endl;

                if (download_success) {
                    FsFileSystem* fs = nullptr;
                    char fs_part_path[FS_MAX_PATH] = {0};
                    char fs_final_path[FS_MAX_PATH] = {0};

                    if (fsdevTranslatePath(df.part_path.c_str(), &fs, fs_part_path) == 0 &&
                        fsdevTranslatePath(df.final_path.c_str(), &fs, fs_final_path) == 0) {

                        // 5. fsFsCommit
                        fsFsCommit(fs);

                        // 6. fsFsRenameFile
                        Result rename_rc = fsFsRenameFile(fs, fs_part_path, fs_final_path);
                        std::cout << "[BigFile] rename result=" << rename_rc << std::endl;

                        if (R_FAILED(rename_rc)) {
                            download_success = false;
                        } else {
                            // 7. fsFsCommit again
                            fsFsCommit(fs);

                            // 8. reopen the final logical path
                            FsFile final_file;
                            Result open_rc = fsFsOpenFile(fs, fs_final_path, FsOpenMode_Read, &final_file);
                            if (R_FAILED(open_rc)) {
                                std::cerr << "[Download] Reopen final file failed: " << open_rc << std::endl;
                                download_success = false;
                            } else {
                                // 9. verify its size again
                                Result sz_rc = fsFileGetSize(&final_file, &final_logical_size);
                                fsFileClose(&final_file);

                                if (R_FAILED(sz_rc) || final_logical_size != df.total_bytes) {
                                    std::cerr << "[Download] Final size verification failed: " << final_logical_size << std::endl;
                                    download_success = false;
                                } else {
                                    std::cout << "[BigFile] completed logical_size=" << final_logical_size << std::endl;
                                }
                            }
                        }
                    } else {
                        download_success = false;
                    }
                }
            } else {
                fclose(writer.file_ptr);

                struct stat check_buffer;
                // Accept the file if it downloaded fully. When the per-file size is
                // unknown (df.total_bytes <= 0) any non-empty file is accepted.
                if (stat(df.part_path.c_str(), &check_buffer) == 0 && check_buffer.st_size > 0 &&
                    (df.total_bytes <= 0 || check_buffer.st_size == df.total_bytes)) {
                    final_logical_size = check_buffer.st_size;
                    if (rename(df.part_path.c_str(), df.final_path.c_str()) != 0) {
                        download_success = false;
                    }
                } else {
                    download_success = false;
                }
            }
        } else {
            // Curl failed or cancelled
            if (use_big_file) {
                if (writer.fs_file_open) {
                    fsFileClose(&writer.fs_file);
                    writer.fs_file_open = false;
                }
            } else {
                if (writer.file_ptr) {
                    fclose(writer.file_ptr);
                }
            }
        }

        if (!download_success) {
            DeleteLogicalFile(df.part_path);
            outcome.cancelled = cancel_requested.load();
            if (outcome.cancelled) {
                outcome.error_message = "Cancelled";
            } else if (use_big_file && writer.last_write_error != 0) {
                outcome.error_message = "Write error " + std::to_string(writer.last_write_error);
            } else if (res != CURLE_OK) {
                outcome.error_message = curl_easy_strerror(res);
            } else {
                outcome.error_message = "Verification failed";
            }
            return outcome;
        }

        outcome.success = true;
        outcome.final_size = final_logical_size;
        return outcome;
    }

    void DownloadManager::SetTaskState(int rom_id, DownloadState state) {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (auto& t : download_queue) {
            if (t.rom_id == rom_id) {
                t.state = state;
                break;
            }
        }
    }

    void DownloadManager::FailTask(int rom_id, DownloadState state, const std::string& error_message) {
        std::lock_guard<std::mutex> lock(task_mutex);
        for (auto& t : download_queue) {
            if (t.rom_id == rom_id) {
                t.state = state;
                t.error_message = error_message;
                break;
            }
        }
    }

    void DownloadManager::DownloadWorker() {
        while (worker_running) {
            int active_rom_id = 0;
            bool found_task = false;
            {
                std::lock_guard<std::mutex> lock(task_mutex);
                for (auto& t : download_queue) {
                    if (t.state == DownloadState::Queued) {
                        t.state = DownloadState::Preparing;
                        active_rom_id = t.rom_id;
                        found_task = true;
                        cancel_requested = false;
                        break;
                    }
                }
            }
            ScreenWakeManager::Instance().RequestUpdate();

            if (!found_task) {
                // Re-check under task_mutex before exiting. EnqueueDownload
                // pushes tasks and reads worker_running under this same lock;
                // without this re-check, a task enqueued between the scan
                // above and the worker_running=false store below would never
                // be picked up (the enqueuer sees a "running" worker that is
                // actually about to exit and doesn't spawn a new one).
                {
                    std::lock_guard<std::mutex> lock(task_mutex);
                    bool any_queued = false;
                    for (const auto& t : download_queue) {
                        if (t.state == DownloadState::Queued) {
                            any_queued = true;
                            break;
                        }
                    }
                    if (any_queued) {
                        continue;
                    }
                    worker_running = false;
                }
                ScreenWakeManager::Instance().RequestUpdate();
                break;
            }

            // Snapshot the task by value and drive every subsequent state change
            // through SetTaskState/FailTask (rom_id lookups under task_mutex).
            // We deliberately never hold a DownloadTask* / iterator across the
            // network I/O below: std::deque::erase() on a non-front/back element
            // invalidates references to EVERY element in the deque, and
            // RemoveFromQueue/ClearCompleted/UninstallGame can erase other queue
            // entries concurrently while this download is in flight.
            const int rom_id = active_rom_id;
            DownloadTask current_task = GetTaskSnapshot(rom_id);

            struct stat buffer;

            // Fetch every physical file this task installs — one for a single-disc
            // game, several for a multi-disc set — using the same rom_id/task. The
            // shared progress counter accumulates across discs via active_base_bytes.
            long long cumulative_bytes = 0;
            bool task_failed = false;
            active_base_bytes.store(0);

            for (size_t fi = 0; fi < current_task.files.size(); ++fi) {
                const DownloadFile df = current_task.files[fi];

                // Mirror the current disc into the scalar fields the cover/index/UI
                // code still reasons about (one representative file at a time).
                current_task.file_id = df.file_id;
                current_task.filename = df.filename;
                current_task.original_filename = df.original_filename;
                current_task.final_path = df.final_path;
                current_task.part_path = df.part_path;

                FileDownloadOutcome oc = DownloadFilePhysical(rom_id, current_task, df, cumulative_bytes);

                if (oc.cancelled) {
                    task_failed = true;
                    break;
                }
                if (!oc.success) {
                    FailTask(rom_id, DownloadState::Failed, oc.error_message.empty() ? "Download failed" : oc.error_message);
                    std::cout << "[Download] Failed: " << oc.error_message << std::endl;
                    ScreenWakeManager::Instance().RequestUpdate();
                    task_failed = true;
                    break;
                }

                // Register this disc in the installed index. For a multi-disc PS1
                // set the individual discs live in a subfolder and are represented by
                // a single .m3u entry (registered below), so they aren't indexed here.
                bool is_psx_multidisc = (current_task.platform_slug == "psx" && current_task.files.size() > 1);
                if (!is_psx_multidisc) {
                    std::string key = current_task.platform_slug + "|" + current_task.filename;
                    InstalledIndexEntry entry;
                    entry.platform_slug = current_task.platform_slug;
                    entry.rom_id = current_task.rom_id;
                    // Prefer the real game title. This used to record a
                    // *filename* in the index's title field, which is what the
                    // Installed screen displays — so rows showed a stripped
                    // filename at best, and nothing at all when it was empty.
                    entry.title = !current_task.title.empty()   ? current_task.title
                                : !current_task.original_filename.empty() ? current_task.original_filename
                                                                          : current_task.filename;
                    entry.original_filename = current_task.original_filename;
                    entry.install_path = current_task.final_path;
                    entry.cover_path = current_task.cover_cache_path;
                    entry.size = oc.final_size;

                    {
                        std::lock_guard<std::mutex> lock(index_mutex);
                        installed_index[key] = entry;
                    }
                    SaveInstalledIndex();

                    std::cout << "[INSTALL] Registered key=" << key << " title=" << entry.title << " path=" << entry.install_path << std::endl;
                }

                cumulative_bytes += df.total_bytes;
            }
            active_base_bytes.store(0);

            if (task_failed) {
                if (cancel_requested) {
                    InvalidateInstallCache();
                    SetTaskState(rom_id, DownloadState::Cancelled);
                }
                ScreenWakeManager::Instance().RequestUpdate();
                continue;
            }

            // Multi-disc PS1: write the root .m3u playlist so DuckStation treats the
            // discs (in their subfolder) as a single game with disc-swapping and a
            // shared memory card, register that .m3u as the one installed entry, and
            // point the DuckStation cover at it.
            if (current_task.files.size() > 1 && current_task.platform_slug == "psx") {
                WriteMultiDiscPlaylist(current_task);
                std::string m3u_base = StripDiscSuffix(current_task.files.front().filename);
                if (!m3u_base.empty()) {
                    std::string m3u_name = m3u_base + ".m3u";
                    std::string root = ConfigManager::Instance().GetRomPath(current_task.platform_slug);
                    std::string m3u_path = root + m3u_name;

                    struct stat mb;
                    long long m3u_size = (stat(m3u_path.c_str(), &mb) == 0) ? (long long)mb.st_size : 0;

                    std::string key = current_task.platform_slug + "|" + m3u_name;
                    InstalledIndexEntry entry;
                    entry.platform_slug = current_task.platform_slug;
                    entry.rom_id = current_task.rom_id;
                    entry.title = current_task.title.empty() ? m3u_base : current_task.title;
                    entry.original_filename = m3u_name;
                    entry.install_path = m3u_path;
                    entry.cover_path = current_task.cover_cache_path;
                    entry.size = m3u_size;
                    {
                        std::lock_guard<std::mutex> lock(index_mutex);
                        installed_index[key] = entry;
                    }
                    SaveInstalledIndex();
                    std::cout << "[INSTALL] Registered multi-disc key=" << key << " path=" << m3u_path << std::endl;

                    // DuckStation cover matches the merged (m3u) entry name.
                    current_task.filename = m3u_name;
                }
            }

            if (!current_task.cover_url.empty() && !cancel_requested) {
                bool skip_cover = false;
                if (stat(current_task.cover_cache_path.c_str(), &buffer) == 0 && buffer.st_size > 0) {
                    skip_cover = true;
                    std::cout << "[Download] Cover already cached at " << current_task.cover_cache_path << std::endl;
                }

                if (!skip_cover) {
                    mkdir("sdmc:/switch", 0777);
                    mkdir("sdmc:/switch/romm-nx", 0777);
                    mkdir("sdmc:/switch/romm-nx/cache", 0777);
                    mkdir("sdmc:/switch/romm-nx/cache/covers", 0777);
                    std::string cover_platform_dir = "sdmc:/switch/romm-nx/cache/covers/" + current_task.platform_slug;
                    mkdir(cover_platform_dir.c_str(), 0777);

                    std::string cover_tmp_path = current_task.cover_cache_path + ".tmp";
                    unlink(cover_tmp_path.c_str());

                    SetTaskState(rom_id, DownloadState::DownloadingCover);
                    std::cout << "[Download] Downloading cover from " << current_task.cover_url << " to " << cover_tmp_path << std::endl;

                    FILE* fc = fopen(cover_tmp_path.c_str(), "wb");
                    if (fc) {
                        CURL* curl = curl_easy_init();
                        if (curl) {
                            auto& config = ConfigManager::Instance();
                            struct curl_slist* headers = NULL;
                            std::string auth = "Authorization: Bearer " + config.GetApiKey();
                            headers = curl_slist_append(headers, auth.c_str());

                            DownloadWriter cover_writer;
                            cover_writer.is_big_file = false;
                            cover_writer.file_ptr = fc;

                            curl_easy_setopt(curl, CURLOPT_URL, current_task.cover_url.c_str());
                            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
                            curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
                            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
                            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &cover_writer);
                            curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
                            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

                            CURLcode res = curl_easy_perform(curl);
                            fclose(fc);
                            curl_slist_free_all(headers);
                            curl_easy_cleanup(curl);

                            if (res == CURLE_OK && !cancel_requested) {
                                if (rename(cover_tmp_path.c_str(), current_task.cover_cache_path.c_str()) == 0) {
                                    std::cout << "[Download] Cover downloaded successfully." << std::endl;
                                } else {
                                    std::cerr << "[Download] Cover rename failed." << std::endl;
                                }
                            } else {
                                std::cerr << "[Download] Cover download failed: " << (cancel_requested ? "Cancelled" : curl_easy_strerror(res)) << std::endl;
                                unlink(cover_tmp_path.c_str());
                            }
                        } else {
                            fclose(fc);
                            unlink(cover_tmp_path.c_str());
                        }
                    } else {
                        std::cerr << "[Download] Could not open cover tmp file for writing." << std::endl;
                    }
                }

                if (!cancel_requested) {
                    struct stat ds_stat;
                    if (stat("sdmc:/switch/duckstation", &ds_stat) == 0 && S_ISDIR(ds_stat.st_mode)) {
                        SetTaskState(rom_id, DownloadState::SyncingCover);
                        mkdir("sdmc:/switch/duckstation/covers", 0777);
                        
                        std::string base_no_ext = current_task.filename;
                        size_t last_dot = base_no_ext.find_last_of('.');
                        if (last_dot != std::string::npos) {
                            base_no_ext = base_no_ext.substr(0, last_dot);
                        }
                        
                        std::string duck_cover_path = "sdmc:/switch/duckstation/covers/" + base_no_ext + ".jpg";
                        
                        bool do_copy = true;
                        struct stat exist_cover_stat;
                        if (stat(duck_cover_path.c_str(), &exist_cover_stat) == 0 && exist_cover_stat.st_size > 0) {
                            do_copy = false;
                            std::cout << "[Download] DuckStation cover already exists: " << duck_cover_path << std::endl;
                        }
                        
                        if (do_copy) {
                            if (stat(current_task.cover_cache_path.c_str(), &buffer) == 0 && buffer.st_size > 0) {
                                std::cout << "[Download] Normalizing cover for DuckStation: " << duck_cover_path << std::endl;
                                
                                FILE* src = fopen(current_task.cover_cache_path.c_str(), "rb");
                                std::string format = "UNKNOWN";
                                if (src) {
                                    unsigned char magic[12];
                                    size_t read_bytes = fread(magic, 1, 12, src);
                                    if (read_bytes >= 4) {
                                        if (magic[0] == 0xFF && magic[1] == 0xD8 && magic[2] == 0xFF) {
                                            format = "JPEG";
                                        } else if (magic[0] == 0x89 && magic[1] == 0x50 && magic[2] == 0x4E && magic[3] == 0x47) {
                                            format = "PNG";
                                        } else if (read_bytes >= 12 && magic[0] == 'R' && magic[1] == 'I' && magic[2] == 'F' && magic[3] == 'F' &&
                                                   magic[8] == 'W' && magic[9] == 'E' && magic[10] == 'B' && magic[11] == 'P') {
                                            format = "WEBP";
                                        }
                                    }
                                    fclose(src);
                                }

                                SDL_Surface* orig = IMG_Load(current_task.cover_cache_path.c_str());
                                if (!orig) {
                                    std::cerr << "[Download] Failed to load cover for normalization." << std::endl;
                                } else {
                                    SDL_Surface* canvas = SDL_CreateRGBSurfaceWithFormat(0, 500, 500, 24, SDL_PIXELFORMAT_RGB24);
                                    if (canvas) {
                                        SDL_FillRect(canvas, NULL, SDL_MapRGB(canvas->format, 0, 0, 0));

                                        float scale = std::min(500.0f / orig->w, 500.0f / orig->h);
                                        int new_w = (int)(orig->w * scale);
                                        int new_h = (int)(orig->h * scale);

                                        SDL_Rect dst_rect;
                                        dst_rect.x = (500 - new_w) / 2;
                                        dst_rect.y = (500 - new_h) / 2;
                                        dst_rect.w = new_w;
                                        dst_rect.h = new_h;

                                        SDL_BlitScaled(orig, NULL, canvas, &dst_rect);

                                        std::string duck_tmp_path = duck_cover_path + ".tmp";
                                        unlink(duck_tmp_path.c_str());

                                        bool save_res = SaveBaselineJpeg(canvas, duck_tmp_path, 85);
                                        if (save_res) {
                                            if (rename(duck_tmp_path.c_str(), duck_cover_path.c_str()) != 0) {
                                                std::cerr << "[Download] Failed to rename normalized cover." << std::endl;
                                            }
                                        }
                                        SDL_FreeSurface(canvas);
                                    }
                                    SDL_FreeSurface(orig);
                                }
                            }
                        }
                    }
                }
            }

            if (cancel_requested) {
                InvalidateInstallCache(); // ensure UI re-checks state after cancel
                SetTaskState(rom_id, DownloadState::Cancelled);
                ScreenWakeManager::Instance().RequestUpdate();
                continue;
            }

            SetTaskState(rom_id, DownloadState::Completed);
            InvalidateInstallCache(); // UI should see newly installed game immediately
            std::cout << "[Download] Successfully completed task." << std::endl;
            ScreenWakeManager::Instance().RequestUpdate();
        }
    }
}
