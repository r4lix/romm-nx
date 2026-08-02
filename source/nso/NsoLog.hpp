#pragma once

#include <mutex>
#include <string>

// Dedicated log file for the experimental SNES Online injection pipeline.
//
// Everything the pipeline learns or writes ends up here: detected LayeredFS
// paths, ROM analysis, generated SFROM header values, image dimensions, every
// file copied or replaced, backup location and the exact failing step. It is
// deliberately separate from stdout so a user can hand the file over after a
// hardware test without capturing a serial/nxlink session.
//
// Never log the RomM host credentials: URLs are logged with their query string
// stripped and the Authorization header is never passed through here.
namespace romm::nso {

    class NsoLog {
    public:
        static NsoLog& Instance();

        static constexpr const char* kLogPath = "sdmc:/switch/romm-nx/logs/nso-snes-injection.log";

        // Starts a new block in the log with a banner line. Rotates the file
        // when it has grown past ~512 KB so repeated runs can't fill the SD.
        void BeginSession(const std::string& what);
        void EndSession(bool success, const std::string& summary);

        void Line(const std::string& message);
        void KV(const std::string& key, const std::string& value);
        void Step(int index, const std::string& name);
        void Fail(const std::string& step, const std::string& detail);

        const char* Path() const { return kLogPath; }

    private:
        NsoLog() = default;
        void WriteRaw(const std::string& line);

        std::mutex mutex;
    };

}
