#pragma once

#include "http_client.hpp"
#include "json_util.hpp"

#include <map>
#include <memory>
#include <string>

struct RommApi {
    static std::string normalizeHost(const std::string& host);
    static std::map<std::string, std::string> authHeaders();

    static std::shared_ptr<HttpResult> fetchPlatforms();
    static std::shared_ptr<HttpResult> fetchRoms(int platformId, int limit = 50, int offset = 0);
    static std::shared_ptr<HttpResult> fetchRomDetail(int romId);

    static std::string buildContentUrl(int romId, const std::string& fileName);
    static std::string defaultDownloadPath(int platformId, const std::string& fileName);
};
