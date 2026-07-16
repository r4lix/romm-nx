#pragma once

#include <map>
#include <memory>
#include <string>

struct HttpResult {
    bool completed = false;
    bool success = false;
    long statusCode = 0;
    std::string body;
    std::string error;
};

class HttpClient {
public:
    static void init();
    static void shutdown();

    static std::shared_ptr<HttpResult> getAsync(
        const std::string& url,
        const std::map<std::string, std::string>& headers);

    static std::shared_ptr<HttpResult> downloadFileAsync(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& outputPath);
};
