#include "RommApi.hpp"
#include "ConfigManager.hpp"
#include "JsonUtil.hpp"
#include <iostream>
#include <sstream>
#include <chrono>

namespace romm::model {

    std::shared_ptr<PlatformFetchResult> RommApi::fetchPlatformsAsync() {
        auto& config = ConfigManager::Instance();
        if (!config.IsValid()) {
            std::cerr << "[api] API call blocked: Configuration is invalid" << std::endl;
            return nullptr;
        }

        std::string url = config.GetRommHost() + "/api/platforms";
        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + config.GetApiKey()},
            {"Accept", "application/json"}
        };

        std::cout << "[api] Fetching platforms asynchronously from: " << url << std::endl;

        auto result = std::make_shared<PlatformFetchResult>();
        HttpClient::runAsync([=]() {
            HttpResult http_res = HttpClient::getSync(url, headers);
            result->statusCode = http_res.statusCode;
            result->success = http_res.success;
            if (http_res.success) {
                auto parse_start = std::chrono::high_resolution_clock::now();
                std::vector<romm::model::PlatformEntry> raw_plats;
                if (romm::model::jsonParsePlatformList(http_res.body, raw_plats)) {
                    for (const auto& entry : raw_plats) {
                        romm::model::Platform p;
                        p.name = entry.name;
                        p.id = std::to_string(entry.id);
                        p.slug = entry.slug;
                        result->platforms.push_back(p);
                    }
                } else {
                    result->success = false;
                }
                auto parse_end = std::chrono::high_resolution_clock::now();
                auto parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(parse_end - parse_start).count();
                std::cout << "[PERF] JSON parse: " << parse_ms << " ms async" << std::endl;
            }
            result->completed = true;
        }, HttpPriority::High);

        return result;
    }

    std::shared_ptr<RomFetchResult> RommApi::fetchRomsAsync(int platformId, int requestId) {
        auto& config = ConfigManager::Instance();
        if (!config.IsValid()) {
            std::cerr << "[api] API call blocked: Configuration is invalid" << std::endl;
            return nullptr;
        }

        std::ostringstream url;
        url << config.GetRommHost() << "/api/roms?platform_ids=" << platformId << "&limit=10000";
        std::string url_str = url.str();

        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + config.GetApiKey()},
            {"Accept", "application/json"}
        };

        std::cout << "[api] Fetching ROMs asynchronously from: " << url_str << std::endl;

        auto result = std::make_shared<RomFetchResult>();
        result->platform_id = platformId;
        result->request_id = requestId;
        HttpClient::runAsync([=]() {
            auto fetch_start = std::chrono::high_resolution_clock::now();
            HttpResult http_res = HttpClient::getSync(url_str, headers);
            auto fetch_end = std::chrono::high_resolution_clock::now();
            auto fetch_ms = std::chrono::duration_cast<std::chrono::milliseconds>(fetch_end - fetch_start).count();
            std::cout << "[PERF] ROM API fetch time: " << fetch_ms << " ms" << std::endl;

            result->statusCode = http_res.statusCode;
            result->success = http_res.success;
            if (http_res.success) {
                auto parse_start = std::chrono::high_resolution_clock::now();
                std::vector<romm::model::RomEntry> raw_roms;
                if (romm::model::jsonParseRomItems(http_res.body, raw_roms)) {
                    for (const auto& entry : raw_roms) {
                        romm::model::Game g;
                        g.title = entry.name;
                        g.id = entry.id;
                        g.fs_name = entry.fs_name;
                        g.fs_size_bytes = entry.fs_size_bytes;
                        g.cover_path = entry.path_cover_small;
                        g.cover_path_large = entry.path_cover_large;
                        result->games.push_back(g);
                    }
                } else {
                    result->success = false;
                }
                auto parse_end = std::chrono::high_resolution_clock::now();
                auto parse_ms = std::chrono::duration_cast<std::chrono::milliseconds>(parse_end - parse_start).count();
                std::cout << "[PERF] ROM parse time: " << parse_ms << " ms" << std::endl;
            }
            result->completed = true;
        }, HttpPriority::High);

        return result;
    }

    std::shared_ptr<RomDetailFetchResult> RommApi::fetchRomDetailAsync(int romId, uint64_t generation, const std::string& platform_slug) {
        auto& config = ConfigManager::Instance();
        if (!config.IsValid()) {
            std::cerr << "[api] API call blocked: Configuration is invalid" << std::endl;
            return nullptr;
        }

        std::ostringstream url;
        url << config.GetRommHost() << "/api/roms/" << romId;
        std::string url_str = url.str();

        std::map<std::string, std::string> headers = {
            {"Authorization", "Bearer " + config.GetApiKey()},
            {"Accept", "application/json"}
        };

        std::cout << "[api] Fetching ROM detail asynchronously from: " << url_str << std::endl;

        auto result = std::make_shared<RomDetailFetchResult>();
        result->rom_id = romId;
        result->generation = generation;
        result->platform_slug = platform_slug;
        
        // High lane: the user pressed A and is staring at an empty description
        // panel until this lands. Without the priority it queues behind every
        // cover download the grid piled up while they were browsing.
        HttpClient::runAsync([=]() {
            HttpResult http_res = HttpClient::getSync(url_str, headers);
            result->statusCode = http_res.statusCode;
            result->success = http_res.success;

            if (http_res.success) {
                GameDetail detail;
                detail.rom_id = romId;
                
                romm::model::jsonExtractString(http_res.body, "summary", detail.description);

                // fs_size_bytes can exceed 2GB, so it needs the 64-bit extractor.
                // jsonExtractLongLong never throws (it returns false if the field is
                // missing, null, or non-numeric), unlike a raw std::stoll on unvalidated
                // input — important since this app is built with -fno-exceptions.
                long long file_size = 0;
                if (romm::model::jsonExtractLongLong(http_res.body, "fs_size_bytes", file_size)) {
                    detail.file_size_bytes = file_size;
                }

                romm::model::jsonExtractString(http_res.body, "fs_name", detail.file_name);
                romm::model::jsonExtractString(http_res.body, "path_cover_large", detail.path_cover_large);
                romm::model::jsonExtractString(http_res.body, "path_cover_small", detail.path_cover_small);
                romm::model::jsonExtractString(http_res.body, "miximage_v2_url", detail.miximage_v2_url);
                
                // Strip ?ts= query parameters
                auto ql = detail.path_cover_large.find('?');
                if (ql != std::string::npos) detail.path_cover_large = detail.path_cover_large.substr(0, ql);
                
                auto qs = detail.path_cover_small.find('?');
                if (qs != std::string::npos) detail.path_cover_small = detail.path_cover_small.substr(0, qs);

                romm::model::jsonExtractStringArray(http_res.body, "genres", detail.genres);
                romm::model::jsonExtractStringArray(http_res.body, "collections", detail.collections);
                
                std::vector<std::string> companies;
                if (romm::model::jsonExtractStringArray(http_res.body, "companies", companies) && !companies.empty()) {
                    detail.developer = companies.front();
                }
                
                std::vector<std::string> franchises;
                if (romm::model::jsonExtractStringArray(http_res.body, "franchises", franchises) && !franchises.empty()) {
                    detail.franchise = franchises.front();
                }

                std::vector<std::string> age_ratings;
                if (romm::model::jsonExtractStringArray(http_res.body, "age_ratings", age_ratings) && !age_ratings.empty()) {
                    detail.age_rating = age_ratings.front();
                }

                romm::model::jsonExtractString(http_res.body, "player_count", detail.players);
                
                // For release date, it's an integer timestamp, we can extract and convert or just skip for now.
                // Or maybe just let it be empty since it's an int and we need it as a string.

                // Parse "files" array length and first file_id
                size_t files_pos = http_res.body.find("\"files\"");
                if (files_pos != std::string::npos) {
                    size_t array_start = http_res.body.find('[', files_pos);
                    size_t array_end = http_res.body.find(']', files_pos);
                    // Find matching bracket
                    if (array_start != std::string::npos) {
                        int depth = 0;
                        for (size_t i = array_start; i < http_res.body.size(); ++i) {
                            if (http_res.body[i] == '[') depth++;
                            else if (http_res.body[i] == ']') {
                                depth--;
                                if (depth == 0) {
                                    array_end = i;
                                    break;
                                }
                            }
                        }
                    }
                    if (array_start != std::string::npos && array_end != std::string::npos && array_end > array_start) {
                        std::string files_json = http_res.body.substr(array_start, array_end - array_start + 1);

                        // Walk each top-level object in the files array and pull out
                        // its id, file_name and per-file size. String-aware brace
                        // matching so a '{' inside a filename can't throw off the scan.
                        auto matchBrace = [](const std::string& s, size_t start) -> size_t {
                            int depth = 0;
                            bool in_str = false;
                            for (size_t i = start; i < s.size(); ++i) {
                                char c = s[i];
                                if (c == '"' && (i == 0 || s[i - 1] != '\\')) { in_str = !in_str; continue; }
                                if (in_str) continue;
                                if (c == '{') depth++;
                                else if (c == '}') { depth--; if (depth == 0) return i; }
                            }
                            return std::string::npos;
                        };

                        size_t obj_pos = 0;
                        while ((obj_pos = files_json.find('{', obj_pos)) != std::string::npos) {
                            size_t obj_end = matchBrace(files_json, obj_pos);
                            if (obj_end == std::string::npos) break;
                            std::string obj = files_json.substr(obj_pos, obj_end - obj_pos + 1);

                            romm::model::RomFileEntry rf;
                            romm::model::jsonExtractInt(obj, "id", rf.id);
                            romm::model::jsonExtractString(obj, "file_name", rf.file_name);
                            long long fsz = 0;
                            if (romm::model::jsonExtractLongLong(obj, "file_size_bytes", fsz)) {
                                rf.file_size_bytes = fsz;
                            }
                            if (rf.id != 0 && !rf.file_name.empty()) {
                                detail.files.push_back(rf);
                            }
                            obj_pos = obj_end + 1;
                        }

                        detail.files_count = (int)detail.files.size();
                        if (!detail.files.empty()) {
                            detail.file_id = detail.files.front().id;
                        }
                    }
                }

                result->detail = detail;
            }

            result->completed = true;
        }, HttpPriority::High);

        return result;
    }

}
