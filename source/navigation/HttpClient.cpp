#include "HttpClient.hpp"

#include <curl/curl.h>
#include <pthread.h>

#include <condition_variable>
#include <functional>
#include <iostream>
#include <mutex>
#include <queue>
#include <vector>

namespace {

size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
    const size_t total = size * nmemb;
    auto* buffer = static_cast<std::string*>(userp);
    buffer->append(static_cast<char*>(contents), total);
    return total;
}

size_t writeToFile(void* contents, size_t size, size_t nmemb, void* userp) {
    return fwrite(contents, size, nmemb, static_cast<FILE*>(userp));
}

// Persistent per-thread curl handle. The app issues many small HTTPS requests
// (covers, ROM lists, details); creating a fresh handle per request forced a
// full TCP+TLS handshake every time, which dominates per-request latency on
// the Switch's CPU. Reusing one handle per worker thread keeps connections
// alive (curl's connection cache is per-handle), so subsequent requests to the
// same host skip the handshake entirely. curl_easy_reset() clears options but
// deliberately preserves live connections.
thread_local CURL* tl_curl = nullptr;

CURL* acquireCurl() {
    if (tl_curl) {
        curl_easy_reset(tl_curl);
    } else {
        tl_curl = curl_easy_init();
    }
    return tl_curl;
}

void releaseThreadCurl() {
    if (tl_curl) {
        curl_easy_cleanup(tl_curl);
        tl_curl = nullptr;
    }
}

struct curl_slist* buildHeaderList(const std::map<std::string, std::string>& headers) {
    struct curl_slist* headerList = nullptr;
    headerList = curl_slist_append(headerList, "User-Agent: romm-nx/3.0.0");
    headerList = curl_slist_append(headerList, "Expect:");
    for (const auto& entry : headers) {
        const std::string line = entry.first + ": " + entry.second;
        headerList = curl_slist_append(headerList, line.c_str());
    }
    return headerList;
}

void performRequest(
    bool isPost,
    std::string url,
    std::map<std::string, std::string> headers,
    std::string body,
    std::shared_ptr<HttpResult> result) {

    CURL* curl = acquireCurl();
    if (!curl) {
        result->success = false;
        result->error = "curl init failed";
        result->completed = true;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result->body);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* headerList = buildHeaderList(headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    if (isPost) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    } else {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    }

    const CURLcode code = curl_easy_perform(curl);
    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->statusCode);
        result->success = result->statusCode >= 200 && result->statusCode < 300;
        if (!result->success && result->body.empty()) {
            result->error = "HTTP " + std::to_string(result->statusCode);
        }
    } else {
        result->success = false;
        result->error = curl_easy_strerror(code);
    }

    if (headerList) {
        curl_slist_free_all(headerList);
    }
    result->completed = true;
}

void performDownload(
    std::string url,
    std::map<std::string, std::string> headers,
    std::string outputPath,
    std::shared_ptr<HttpResult> result) {

    CURL* curl = acquireCurl();
    if (!curl) {
        result->success = false;
        result->error = "curl init failed";
        result->completed = true;
        return;
    }

    FILE* file = fopen(outputPath.c_str(), "wb");
    if (!file) {
        result->success = false;
        result->error = "failed to open output file";
        result->completed = true;
        return;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    struct curl_slist* headerList = buildHeaderList(headers);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);

    const CURLcode code = curl_easy_perform(curl);
    fclose(file);

    if (code == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result->statusCode);
        result->success = result->statusCode >= 200 && result->statusCode < 300;
        if (!result->success) {
            result->error = "HTTP " + std::to_string(result->statusCode);
            remove(outputPath.c_str());
        }
    } else {
        result->success = false;
        result->error = curl_easy_strerror(code);
        remove(outputPath.c_str());
    }

    if (headerList) {
        curl_slist_free_all(headerList);
    }
    result->completed = true;
}

class TaskQueue {
public:
    // Every async HTTP call in the app (cover downloads, ROM list/detail
    // fetches, etc.) funnels through this one queue. A single worker meant
    // they all ran strictly one at a time — a grid full of covers had to wait
    // for each one's full connect+handshake+transfer cycle before the next
    // could even start. Multiple workers let curl (thread-safe across
    // separate easy handles, per its documented model — see HttpClient::init's
    // curl_global_init call) actually run several transfers concurrently.
    static constexpr int kWorkerCount = 4;

    TaskQueue() {
        for (int i = 0; i < kWorkerCount; ++i) {
            spawnWorker();
        }
    }

    ~TaskQueue() {
        shutdown();
    }

    void enqueue(std::function<void()> task) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            tasks_.push(std::move(task));
        }
        condition_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) {
                return;
            }
            stopped_ = true;
        }
        condition_.notify_all();
        for (pthread_t thread : workers_) {
            pthread_join(thread, nullptr);
        }
        workers_.clear();
    }

private:
    void spawnWorker() {
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 0x100000);
        const int rc = pthread_create(&thread, &attr, workerEntry, this);
        pthread_attr_destroy(&attr);
        if (rc == 0) {
            workers_.push_back(thread);
        }
    }

    static void* workerEntry(void* arg) {
        static_cast<TaskQueue*>(arg)->workerLoop();
        return nullptr;
    }

    void workerLoop() {
        while (true) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [this] { return stopped_ || !tasks_.empty(); });
                if (stopped_ && tasks_.empty()) {
                    break;
                }
                task = std::move(tasks_.front());
                tasks_.pop();
            }
            task();
        }
        // Free this worker's persistent curl handle before the thread exits.
        releaseThreadCurl();
    }

    std::queue<std::function<void()>> tasks_;
    std::mutex mutex_;
    std::condition_variable condition_;
    bool stopped_ = false;
    std::vector<pthread_t> workers_;
};

TaskQueue* queue() {
    static TaskQueue instance;
    return &instance;
}

} // namespace

void HttpClient::init() {
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

void HttpClient::shutdown() {
    queue()->shutdown();
    // getSync runs on the caller's thread (normally the main thread), so this
    // thread may also own a persistent handle — free it before global cleanup.
    releaseThreadCurl();
    curl_global_cleanup();
}

std::shared_ptr<HttpResult> HttpClient::getAsync(
    const std::string& url,
    const std::map<std::string, std::string>& headers) {

    auto result = std::make_shared<HttpResult>();
    queue()->enqueue([=]() {
        performRequest(false, url, headers, "", result);
    });
    return result;
}

HttpResult HttpClient::getSync(
    const std::string& url,
    const std::map<std::string, std::string>& headers) {

    auto result = std::make_shared<HttpResult>();
    performRequest(false, url, headers, "", result);
    return *result;
}

std::shared_ptr<HttpResult> HttpClient::downloadFileAsync(
    const std::string& url,
    const std::map<std::string, std::string>& headers,
    const std::string& outputPath) {

    auto result = std::make_shared<HttpResult>();
    queue()->enqueue([=]() {
        performDownload(url, headers, outputPath, result);
    });
    return result;
}

void HttpClient::runAsync(std::function<void()> task) {
    queue()->enqueue(std::move(task));
}
