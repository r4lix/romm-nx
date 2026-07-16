#pragma once

#include <string>

class Config {
public:
    static Config& instance();

    void load();
    void save();

    const std::string& hostUrl() const { return hostUrl_; }
    const std::string& accessToken() const { return accessToken_; }

    void setHostUrl(const std::string& url) { hostUrl_ = url; }
    void setAccessToken(const std::string& token) { accessToken_ = token; }

    std::string configPath() const;

private:
    Config();

    std::string hostUrl_ = "http://localhost:8080";
    std::string accessToken_;
};
