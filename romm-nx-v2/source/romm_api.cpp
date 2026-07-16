#include "romm_api.hpp"

#include "config.hpp"
#include "http_client.hpp"

#include <sys/stat.h>

#include <cstdio>
#include <sstream>

namespace {

std::string urlEncode(const std::string& value) {
    std::ostringstream encoded;
    for (unsigned char ch : value) {
        if ((ch >= 'A' && ch <= 'Z') ||
            (ch >= 'a' && ch <= 'z') ||
            (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            encoded << ch;
        } else {
            char buffer[4];
            std::snprintf(buffer, sizeof(buffer), "%%%02X", ch);
            encoded << buffer;
        }
    }
    return encoded.str();
}

void ensureDirectory(const std::string& path) {
    struct stat st {};
    if (stat(path.c_str(), &st) == 0) {
        return;
    }
    mkdir(path.c_str(), 0777);
}

} // namespace

std::string RommApi::normalizeHost(const std::string& host) {
    if (host.empty()) {
        return host;
    }
    if (host.back() == '/') {
        return host.substr(0, host.size() - 1);
    }
    return host;
}

std::map<std::string, std::string> RommApi::authHeaders() {
    return {
        {"Authorization", "Bearer " + Config::instance().accessToken()},
        {"Accept", "application/json"},
    };
}

std::shared_ptr<HttpResult> RommApi::fetchPlatforms() {
    const std::string url = normalizeHost(Config::instance().hostUrl()) + "/api/platforms";
    return HttpClient::getAsync(url, authHeaders());
}

std::shared_ptr<HttpResult> RommApi::fetchRoms(int platformId, int limit, int offset) {
    std::ostringstream url;
    url << normalizeHost(Config::instance().hostUrl())
        << "/api/roms?platform_ids=" << platformId
        << "&limit=" << limit
        << "&offset=" << offset;
    return HttpClient::getAsync(url.str(), authHeaders());
}

std::shared_ptr<HttpResult> RommApi::fetchRomDetail(int romId) {
    const std::string url = normalizeHost(Config::instance().hostUrl()) + "/api/roms/" + std::to_string(romId);
    return HttpClient::getAsync(url, authHeaders());
}

std::string RommApi::buildContentUrl(int romId, const std::string& fileName) {
    return normalizeHost(Config::instance().hostUrl()) +
           "/api/roms/" + std::to_string(romId) +
           "/content/" + urlEncode(fileName);
}

std::string RommApi::defaultDownloadPath(int platformId, const std::string& fileName) {
    ensureDirectory("sdmc:/romm-nx");
    ensureDirectory("sdmc:/romm-nx/downloads");

    std::ostringstream path;
    path << "sdmc:/romm-nx/downloads/p" << platformId << "_" << fileName;
    return path.str();
}
