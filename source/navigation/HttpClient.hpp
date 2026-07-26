#pragma once

#include <map>
#include <memory>
#include <string>
#include <functional>

struct HttpResult {
    bool completed = false;
    bool success = false;
    long statusCode = 0;
    std::string body;
    std::string error;
};

// Lane a queued task runs in. The worker pool is shared by everything the app
// does over the network, so without lanes a single FIFO lets bulk work starve
// the things the user is actually waiting on: browsing a large library queues
// hundreds of cover downloads, and the ROM-detail fetch issued when they press
// A lands behind every one of them.
//
//   High   — the user is blocked on this right now (ROM detail / description).
//   Normal — user-initiated but not blocking a visible screen (platform and
//            ROM list fetches, update checks).
//   Low    — speculative bulk work (cover downloads and their decodes).
enum class HttpPriority {
    High = 0,
    Normal = 1,
    Low = 2
};

class HttpClient {
public:
    static void init();
    static void shutdown();

    static std::shared_ptr<HttpResult> getAsync(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        HttpPriority priority = HttpPriority::Normal);

    static HttpResult getSync(
        const std::string& url,
        const std::map<std::string, std::string>& headers);

    static std::shared_ptr<HttpResult> downloadFileAsync(
        const std::string& url,
        const std::map<std::string, std::string>& headers,
        const std::string& outputPath,
        HttpPriority priority = HttpPriority::Normal);

    static void runAsync(std::function<void()> task, HttpPriority priority = HttpPriority::Normal);
};
