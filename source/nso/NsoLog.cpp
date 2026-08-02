#include "NsoLog.hpp"

#include <switch.h>

#include <cstdio>
#include <ctime>
#include <iostream>
#include <sys/stat.h>

namespace romm::nso {

    namespace {
        // Beyond this the log is rotated to <name>.1 so a user who runs the
        // pipeline repeatedly doesn't quietly accumulate megabytes on the SD.
        constexpr long kMaxLogBytes = 512 * 1024;

        void EnsureParentDirs(const std::string& path) {
            for (size_t i = 1; i < path.size(); ++i) {
                if (path[i] == '/') {
                    const std::string sub = path.substr(0, i);
                    // "sdmc:" itself is not a directory we can create.
                    if (sub.size() >= 5 && sub.compare(sub.size() - 1, 1, ":") == 0) continue;
                    mkdir(sub.c_str(), 0777);
                }
            }
        }

        std::string Timestamp() {
            const std::time_t now = std::time(nullptr);
            std::tm* tm_info = std::localtime(&now);
            char buf[32];
            if (tm_info && std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", tm_info) > 0) {
                return std::string(buf);
            }
            return "--------- --:--:--";
        }
    }

    NsoLog& NsoLog::Instance() {
        static NsoLog inst;
        return inst;
    }

    void NsoLog::WriteRaw(const std::string& line) {
        std::lock_guard<std::mutex> lock(mutex);
        EnsureParentDirs(kLogPath);
        FILE* f = std::fopen(kLogPath, "ab");
        if (!f) return;
        std::fwrite(line.data(), 1, line.size(), f);
        std::fputc('\n', f);
        std::fclose(f);

        // fclose alone does NOT put the bytes on the card: the FS service holds
        // them until the device is committed, so a crash loses whatever was
        // still cached. That is exactly the case this log exists for, and it
        // cost a debugging round trip — the first crash report arrived as a
        // 60-byte file that stopped mid-banner while the real failure was
        // somewhere else entirely. Commit every line; ~40 lines per run is a
        // price worth paying to know where a hang or crash actually happened.
        fsdevCommitDevice("sdmc");
    }

    void NsoLog::BeginSession(const std::string& what) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            EnsureParentDirs(kLogPath);
            struct stat st {};
            if (stat(kLogPath, &st) == 0 && st.st_size > kMaxLogBytes) {
                const std::string rotated = std::string(kLogPath) + ".1";
                std::remove(rotated.c_str()); // sdmc: rename() will not overwrite
                std::rename(kLogPath, rotated.c_str());
            }
        }
        WriteRaw("");
        WriteRaw("========================================================");
        WriteRaw("[" + Timestamp() + "] BEGIN " + what);
        WriteRaw("========================================================");
        std::cout << "[NSO-SNES] BEGIN " << what << std::endl;
    }

    void NsoLog::EndSession(bool success, const std::string& summary) {
        WriteRaw("[" + Timestamp() + "] END " + std::string(success ? "OK" : "FAILED") +
                 (summary.empty() ? "" : (" - " + summary)));
        std::cout << "[NSO-SNES] END " << (success ? "OK" : "FAILED") << " " << summary << std::endl;
    }

    void NsoLog::Line(const std::string& message) {
        WriteRaw("[" + Timestamp() + "] " + message);
        std::cout << "[NSO-SNES] " << message << std::endl;
    }

    void NsoLog::KV(const std::string& key, const std::string& value) {
        Line("    " + key + " = " + value);
    }

    void NsoLog::Step(int index, const std::string& name) {
        Line("--- STEP " + std::to_string(index) + ": " + name + " ---");
    }

    void NsoLog::Fail(const std::string& step, const std::string& detail) {
        Line("!!! FAILURE in step '" + step + "': " + detail);
    }

}
