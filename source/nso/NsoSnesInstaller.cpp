#include "NsoSnesInstaller.hpp"

#include "NsoImage.hpp"
#include "NsoJson.hpp"
#include "NsoLog.hpp"
#include "SfromWriter.hpp"
#include "SnesRom.hpp"

#include "../model/ConfigManager.hpp"
#include "../model/JsonUtil.hpp"
#include "../navigation/HttpClient.hpp"

#include <curl/curl.h>
#include <pthread.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <map>
#include <sys/stat.h>
#include <thread>
#include <utility>

namespace romm::nso {

    struct PipelineJob {
        NsoSnesInstaller* self;
        bool restore;
        NsoInstallRequest request;
    };

    namespace {

        bool PathExists(const std::string& path) {
            struct stat st {};
            return stat(path.c_str(), &st) == 0;
        }

        bool IsDirectory(const std::string& path) {
            struct stat st {};
            return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
        }

        long long FileSize(const std::string& path) {
            struct stat st {};
            if (stat(path.c_str(), &st) != 0) return -1;
            return (long long)st.st_size;
        }

        void EnsureDir(const std::string& path) {
            for (size_t i = 1; i < path.size(); ++i) {
                if (path[i] == '/') {
                    const std::string sub = path.substr(0, i);
                    if (!sub.empty() && sub.back() == ':') continue; // "sdmc:"
                    mkdir(sub.c_str(), 0777);
                }
            }
            mkdir(path.c_str(), 0777);
        }

        bool ReadFileText(const std::string& path, std::string& out) {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) return false;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size < 0) { std::fclose(f); return false; }
            out.resize((size_t)size);
            const size_t read = size > 0 ? std::fread(&out[0], 1, out.size(), f) : 0;
            std::fclose(f);
            return read == out.size();
        }

        // Writes through a .tmp sibling and swaps it in. The Switch's sdmc:
        // driver refuses rename() onto an existing name, so the original is
        // removed first — the window is a few microseconds and the previous
        // contents are already sitting in the backup directory by then.
        bool WriteFileAtomic(const std::string& path, const std::string& data, std::string& error) {
            const std::string tmp = path + ".tmp";
            std::remove(tmp.c_str());
            FILE* f = std::fopen(tmp.c_str(), "wb");
            if (!f) {
                error = "cannot create " + tmp;
                return false;
            }
            const bool wrote = data.empty() || std::fwrite(data.data(), 1, data.size(), f) == data.size();
            const bool closed = std::fclose(f) == 0;
            if (!wrote || !closed) {
                std::remove(tmp.c_str());
                error = "write failed for " + tmp + " (SD card full or read-only?)";
                return false;
            }
            std::remove(path.c_str());
            if (std::rename(tmp.c_str(), path.c_str()) != 0) {
                std::remove(tmp.c_str());
                error = "cannot move " + tmp + " into place";
                return false;
            }
            return true;
        }

        bool CopyFile(const std::string& src, const std::string& dst, std::string& error) {
            FILE* in = std::fopen(src.c_str(), "rb");
            if (!in) {
                error = "cannot read " + src;
                return false;
            }
            const std::string tmp = dst + ".tmp";
            std::remove(tmp.c_str());
            FILE* out = std::fopen(tmp.c_str(), "wb");
            if (!out) {
                std::fclose(in);
                error = "cannot create " + tmp;
                return false;
            }

            static thread_local std::vector<char> buffer;
            if (buffer.size() != 256 * 1024) buffer.resize(256 * 1024);

            bool ok = true;
            while (true) {
                const size_t got = std::fread(buffer.data(), 1, buffer.size(), in);
                if (got == 0) break;
                if (std::fwrite(buffer.data(), 1, got, out) != got) { ok = false; break; }
            }
            if (std::ferror(in)) ok = false;
            std::fclose(in);
            if (std::fclose(out) != 0) ok = false;

            if (!ok) {
                std::remove(tmp.c_str());
                error = "copy failed: " + src + " -> " + dst;
                return false;
            }

            std::remove(dst.c_str());
            if (std::rename(tmp.c_str(), dst.c_str()) != 0) {
                std::remove(tmp.c_str());
                error = "cannot move " + tmp + " into place";
                return false;
            }
            return true;
        }

        // Enumerate fully, close the handle, and only then delete. Removing
        // entries while the same DIR is still being walked is undefined and,
        // on the Switch's FAT driver, a good way to lose the iterator.
        void RemoveDirectoryTree(const std::string& path) {
            std::vector<std::string> names;
            if (DIR* dir = opendir(path.c_str())) {
                while (dirent* entry = readdir(dir)) {
                    const std::string name = entry->d_name;
                    if (name == "." || name == "..") continue;
                    names.push_back(name);
                }
                closedir(dir);
            }
            for (const std::string& name : names) {
                const std::string child = path + "/" + name;
                if (IsDirectory(child)) RemoveDirectoryTree(child);
                else std::remove(child.c_str());
            }
            rmdir(path.c_str());
        }

        std::string NowStamp(bool compact) {
            const std::time_t now = std::time(nullptr);
            std::tm* tm_info = std::localtime(&now);
            char buf[32];
            if (tm_info && std::strftime(buf, sizeof(buf),
                                         compact ? "%Y%m%d-%H%M%S" : "%Y-%m-%d %H:%M:%S", tm_info) > 0) {
                return buf;
            }
            return compact ? "00000000-000000" : "0000-00-00 00:00:00";
        }

        std::string TodayIso() {
            const std::time_t now = std::time(nullptr);
            std::tm* tm_info = std::localtime(&now);
            char buf[16];
            if (tm_info && std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm_info) > 0) return buf;
            return "1970-01-01";
        }

        // RomM reports first_release_date in MILLIseconds. Taken as seconds it
        // produced "28719-05-10" for a 1996 game, which the NSO info screen
        // rendered as "Released in: ----". Anything past ~year 5138 can only be
        // a millisecond value, so scale it; a real seconds value is untouched.
        std::string UnixToIsoDate(long long value) {
            if (value <= 0) return std::string();
            long long seconds = value;
            if (seconds > 100000000000LL) seconds /= 1000;
            const std::time_t t = (std::time_t)seconds;
            std::tm* tm_info = std::gmtime(&t);
            char buf[16];
            if (tm_info && std::strftime(buf, sizeof(buf), "%Y-%m-%d", tm_info) > 0) return buf;
            return std::string();
        }

        // A failed write is classified from errno rather than from a free-space
        // pre-check: statvfs on the SD devoptab is one more untested syscall in
        // a feature that already has enough of them, and "the write failed" is
        // the fact that actually matters.
        NsoErrorKind ClassifyWriteFailure() {
            return (errno == ENOSPC) ? NsoErrorKind::InsufficientSpace : NsoErrorKind::FileWrite;
        }

        std::string UrlEncode(const std::string& value) {
            CURL* curl = curl_easy_init();
            if (!curl) return value;
            char* escaped = curl_easy_escape(curl, value.c_str(), 0);
            std::string out = escaped ? escaped : value;
            if (escaped) curl_free(escaped);
            curl_easy_cleanup(curl);
            return out;
        }

        std::string HumanBytes(long long bytes) {
            char buf[48];
            if (bytes < 1024) std::snprintf(buf, sizeof(buf), "%lld B", bytes);
            else if (bytes < 1024 * 1024) std::snprintf(buf, sizeof(buf), "%.1f KB", (double)bytes / 1024.0);
            else std::snprintf(buf, sizeof(buf), "%.2f MB", (double)bytes / (1024.0 * 1024.0));
            return buf;
        }

        // Strips the query string so a signed/timestamped cover URL never puts
        // a token in the log.
        std::string SafeUrlForLog(const std::string& url) {
            const size_t q = url.find('?');
            return (q == std::string::npos) ? url : url.substr(0, q);
        }

        // Word-wraps a description the way the NSO info screen needs it: it does
        // no wrapping of its own, and CaVE's own placeholder text documents a
        // ~50 column budget.
        std::string WrapDescription(const std::string& text, size_t width) {
            std::string out;
            size_t line_len = 0;
            size_t i = 0;
            while (i < text.size()) {
                if (text[i] == '\n') {
                    out.push_back('\n');
                    line_len = 0;
                    ++i;
                    continue;
                }
                size_t word_end = i;
                while (word_end < text.size() && text[word_end] != ' ' && text[word_end] != '\n') ++word_end;
                const size_t word_len = word_end - i;
                if (line_len > 0 && line_len + 1 + word_len > width) {
                    out.push_back('\n');
                    line_len = 0;
                } else if (line_len > 0) {
                    out.push_back(' ');
                    ++line_len;
                }
                out.append(text, i, word_len);
                line_len += word_len;
                i = word_end;
                while (i < text.size() && text[i] == ' ') ++i;
            }
            return out;
        }

        struct BackupRecord {
            char kind; // 'R' restore-from-backup, 'C' created file, 'D' created directory
            std::string backup_name;
            std::string target;
        };

        // --- injected.txt -------------------------------------------------
        // romm-nx's own side index, never read by the emulator: maps a ROM hash
        // to the code it was given so a reinstall reuses the same slot instead
        // of leaking a second entry. Tab-separated so a title containing any
        // JSON-significant character needs no escaping.
        struct InjectedEntry {
            std::string code;
            std::string title;
            // Lets uninstall find the entry without re-hashing the ROM — which
            // would not match anyway for a dump that carried a copier header,
            // since the recorded hash is of the normalized image.
            int rom_id = 0;
        };

        std::map<std::string, InjectedEntry> LoadInjectedIndex() {
            std::map<std::string, InjectedEntry> out;
            std::string text;
            if (!ReadFileText(kInjectedIndex, text)) return out;
            size_t pos = 0;
            while (pos < text.size()) {
                size_t eol = text.find('\n', pos);
                if (eol == std::string::npos) eol = text.size();
                std::string line = text.substr(pos, eol - pos);
                pos = eol + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();

                const size_t tab = line.find('\t');
                if (tab == std::string::npos) continue;
                const size_t tab2 = line.find('\t', tab + 1);

                const std::string hash = line.substr(0, tab);
                InjectedEntry entry;
                if (tab2 == std::string::npos) {
                    entry.code = line.substr(tab + 1);
                } else {
                    entry.code = line.substr(tab + 1, tab2 - tab - 1);
                    const size_t tab3 = line.find('	', tab2 + 1);
                    if (tab3 == std::string::npos) {
                        entry.title = line.substr(tab2 + 1);   // written by an earlier build
                    } else {
                        entry.title = line.substr(tab2 + 1, tab3 - tab2 - 1);
                        entry.rom_id = (int)std::strtol(line.substr(tab3 + 1).c_str(), nullptr, 10);
                    }
                }
                if (!hash.empty() && !entry.code.empty()) out[hash] = entry;
            }
            return out;
        }

        void SaveInjectedIndex(const std::map<std::string, InjectedEntry>& index) {
            std::string text;
            for (const auto& entry : index) {
                // Four fields. rom_id was added to InjectedEntry and to the
                // reader but not here, so every line was written with three and
                // read back with rom_id 0 — which made UninstallSync's rom_id
                // lookup miss every entry and silently report "never injected",
                // leaving uninstalled games sitting in the Switch Online app.
                text += entry.first + "\t" + entry.second.code + "\t" + entry.second.title
                      + "\t" + std::to_string(entry.second.rom_id) + "\n";
            }
            std::string error;
            EnsureDir(kNsoRoot);
            WriteFileAtomic(kInjectedIndex, text, error);
        }

    } // namespace

    bool IsRetryableFailure(NsoErrorKind kind) {
        switch (kind) {
            case NsoErrorKind::SourceDownload: // network blip
            case NsoErrorKind::CoverDecode:    // cover fetch failed
            case NsoErrorKind::FileWrite:      // SD hiccup
                return true;
            default:
                // Everything else is deterministic: an unsupported mapping, a
                // malformed database, a failed validation or a full card gives
                // the same answer next time, and InsufficientSpace gets worse.
                return false;
        }
    }

    bool PlatformSupportsInjection(const std::string& canonical_platform_id) {
        return canonical_platform_id == "snes";
    }

    NsoSnesInstaller& NsoSnesInstaller::Instance() {
        static NsoSnesInstaller inst;
        return inst;
    }

    const char* NsoSnesInstaller::LogPath() const {
        return NsoLog::Instance().Path();
    }

    void NsoSnesInstaller::RefreshDetection() {
        NsoSnesInstall found = DetectNsoSnes();
        std::lock_guard<std::mutex> lock(mutex);
        detection = found;
    }

    NsoSnesInstall NsoSnesInstaller::GetDetection() const {
        std::lock_guard<std::mutex> lock(mutex);
        return detection;
    }

    NsoPipelineState NsoSnesInstaller::GetState() const {
        std::lock_guard<std::mutex> lock(mutex);
        return state;
    }

    NsoErrorKind NsoSnesInstaller::GetErrorKind() const {
        std::lock_guard<std::mutex> lock(mutex);
        return error_kind;
    }

    std::string NsoSnesInstaller::GetError() const {
        std::lock_guard<std::mutex> lock(mutex);
        return error_message;
    }

    std::string NsoSnesInstaller::GetSummary() const {
        std::lock_guard<std::mutex> lock(mutex);
        return summary;
    }

    std::vector<NsoStep> NsoSnesInstaller::GetSteps() const {
        std::lock_guard<std::mutex> lock(mutex);
        return steps;
    }

    std::string NsoSnesInstaller::LatestBackupPath() const {
        if (!IsDirectory(kBackupsDir)) return std::string();
        DIR* dir = opendir(kBackupsDir);
        if (!dir) return std::string();
        std::string newest;
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = std::string(kBackupsDir) + "/" + name;
            if (!IsDirectory(full)) continue;
            if (!PathExists(full + "/manifest.txt")) continue;
            // Names are YYYYMMDD-HHMMSS, so lexical order is chronological.
            if (name > newest) newest = name;
        }
        closedir(dir);
        return newest.empty() ? std::string() : (std::string(kBackupsDir) + "/" + newest);
    }

    void NsoSnesInstaller::ResetSteps(const std::vector<std::string>& names) {
        std::lock_guard<std::mutex> lock(mutex);
        steps.clear();
        steps.reserve(names.size());
        for (const auto& name : names) {
            NsoStep step;
            step.name = name;
            steps.push_back(step);
        }
        state = NsoPipelineState::Running;
        error_kind = NsoErrorKind::None;
        error_message.clear();
        summary.clear();
        install_code.clear();
    }

    void NsoSnesInstaller::BeginStep(size_t index) {
        std::string name;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (index >= steps.size()) return;
            steps[index].status = NsoStepStatus::Running;
            name = steps[index].name;
        }
        NsoLog::Instance().Step((int)index + 1, name);
    }

    void NsoSnesInstaller::FinishStep(size_t index, const std::string& detail) {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= steps.size()) return;
        steps[index].status = NsoStepStatus::Done;
        steps[index].detail = detail;
    }

    void NsoSnesInstaller::SkipStep(size_t index, const std::string& detail) {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= steps.size()) return;
        steps[index].status = NsoStepStatus::Skipped;
        steps[index].detail = detail;
    }

    void NsoSnesInstaller::FailStep(size_t index, NsoErrorKind kind, const std::string& detail) {
        std::string step_name;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (index < steps.size()) {
                steps[index].status = NsoStepStatus::Failed;
                steps[index].detail = detail;
                step_name = steps[index].name;
            }
            state = NsoPipelineState::Failed;
            error_kind = kind;
            error_message = detail;
        }
        NsoLog::Instance().Fail(step_name, detail);
    }

    void NsoSnesInstaller::SetSummary(const std::string& text) {
        std::lock_guard<std::mutex> lock(mutex);
        summary = text;
    }

    void* NsoSnesInstaller::ThreadEntry(void* arg) {
        PipelineJob* job = static_cast<PipelineJob*>(arg);
        if (job->restore) {
            job->self->RunRestore();
        } else {
            job->self->RunInstall(job->request);
        }
        job->self->busy.store(false);
        delete job;
        return nullptr;
    }

    bool NsoSnesInstaller::SpawnWorker(bool restore, const NsoInstallRequest& request) {
        PipelineJob* job = new PipelineJob{this, restore, request};

        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 0x100000); // 1 MB, same as HttpClient's pool
        pthread_t thread;
        const int rc = pthread_create(&thread, &attr, &NsoSnesInstaller::ThreadEntry, job);
        pthread_attr_destroy(&attr);

        if (rc != 0) {
            delete job;
            return false;
        }
        pthread_detach(thread);
        return true;
    }

    NsoInstallOutcome NsoSnesInstaller::InstallSync(const NsoInstallRequest& request) {
        // Wait out a manual install rather than dropping this one on the floor.
        bool expected = false;
        while (!busy.compare_exchange_strong(expected, true)) {
            expected = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
        ResetSteps({});
        RunInstall(request);
        busy.store(false);

        NsoInstallOutcome outcome;
        {
            std::lock_guard<std::mutex> lock(mutex);
            outcome.success = (state == NsoPipelineState::Success);
            outcome.error_kind = error_kind;
            outcome.error = error_message;
            outcome.code = install_code;
        }
        return outcome;
    }


    NsoInstallOutcome NsoSnesInstaller::UninstallSync(int rom_id, const std::string& fallback_title) {
        NsoInstallOutcome outcome;
        auto& log = NsoLog::Instance();

        auto index = LoadInjectedIndex();
        std::string hash, code, title;
        for (const auto& entry : index) {
            if (entry.second.rom_id != rom_id || entry.second.code.empty()) continue;
            hash = entry.first; code = entry.second.code; title = entry.second.title;
            break;
        }
        // Nothing matched by rom_id: the entry may predate the index carrying
        // one (those read back as 0). Match those, and only those, by title, so
        // a game injected by an earlier build is still removable.
        bool matched_by_title = false;
        if (code.empty() && !fallback_title.empty()) {
            for (const auto& entry : index) {
                if (entry.second.rom_id != 0 || entry.second.code.empty()) continue;
                if (entry.second.title != fallback_title) continue;
                hash = entry.first; code = entry.second.code; title = entry.second.title;
                matched_by_title = true;
                break;
            }
        }
        if (code.empty()) {
            outcome.success = true; // never injected by romm-nx
            return outcome;
        }

        bool expected = false;
        while (!busy.compare_exchange_strong(expected, true)) {
            expected = false;
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }

        log.BeginSession("Remove \"" + title + "\" (" + code + ") from SNES Online");
        if (matched_by_title) {
            log.Line("matched by title: this index entry was written before rom_id was recorded");
        }
        auto finish = [&](bool ok, const std::string& why) {
            log.EndSession(ok, why);
            busy.store(false);
            outcome.success = ok;
            outcome.error = ok ? "" : why;
            outcome.code = code;
            return outcome;
        };

        NsoSnesInstall install = DetectNsoSnes();
        if (!install.found) return finish(false, "no SNES Online LayeredFS found");

        TitlesDb db;
        if (!LoadTitlesDb(install.database_path, db)) return finish(false, db.error);

        std::string new_db;
        bool found_entry = false;
        std::string error;
        if (!RemoveTitleEntry(db, code, new_db, found_entry, error)) return finish(false, error);

        // Strings first (in memory), so nothing is written until every edit is
        // known to be valid.
        std::vector<std::pair<std::string, std::string>> new_strings;
        for (const std::string& file : install.strings_files) {
            std::string original;
            if (!ReadFileText(file, original)) continue;
            std::string patched;
            bool changed = false;
            if (!UnpatchStringsFile(original, code, patched, changed, error)) return finish(false, error);
            if (changed) new_strings.emplace_back(file, patched);
        }

        const std::string title_dir = install.titles_dir + "/" + code;
        const std::string backup_dir = std::string(kBackupsDir) + "/" + NowStamp(true);
        EnsureDir(backup_dir);
        if (!IsDirectory(backup_dir)) return finish(false, "cannot create " + backup_dir);

        // Back up everything about to be removed, using the same manifest the
        // restore action already understands.
        std::vector<BackupRecord> records;
        std::string copy_error;
        auto back_up = [&](const std::string& target, const std::string& name) {
            if (!PathExists(target)) return true;
            if (!CopyFile(target, backup_dir + "/" + name, copy_error)) return false;
            records.push_back({'R', name, target});
            return true;
        };
        if (!back_up(install.database_path, "lclassics.titlesdb")) return finish(false, copy_error);
        for (size_t i = 0; i < new_strings.size(); ++i) {
            if (!back_up(new_strings[i].first, "strings_" + std::to_string(i) + ".lng")) return finish(false, copy_error);
        }
        const char* kAssetSuffix[4] = {".sfrom", ".sfromsig", ".png", "-details.png"};
        for (int i = 0; i < 4; ++i) {
            if (!back_up(title_dir + "/" + code + kAssetSuffix[i], "asset_" + std::to_string(i))) {
                return finish(false, copy_error);
            }
        }

        {
            std::string manifest;
            manifest += "version=1\n";
            manifest += "timestamp=" + NowStamp(false) + "\n";
            manifest += "title_id=" + install.title_id + "\n";
            manifest += "code=" + code + "\n";
            manifest += "title=" + title + "\n";
            manifest += "sha256=" + hash + "\n";
            manifest += "database=" + install.database_path + "\n";
            for (const auto& r : records) {
                manifest += std::string(1, r.kind) + "\t" + r.backup_name + "\t" + r.target + "\n";
            }
            std::string manifest_error;
            if (!WriteFileAtomic(backup_dir + "/manifest.txt", manifest, manifest_error)) {
                return finish(false, manifest_error);
            }
        }
        log.KV("backup.dir", backup_dir);

        for (const auto& entry : new_strings) {
            std::string write_error;
            if (!WriteFileAtomic(entry.first, entry.second, write_error)) return finish(false, write_error);
            log.Line("unpatched " + entry.first);
        }
        if (found_entry) {
            std::string write_error;
            if (!WriteFileAtomic(install.database_path, new_db, write_error)) return finish(false, write_error);
            log.Line("removed " + code + " from " + install.database_path);
        }
        RemoveDirectoryTree(title_dir);
        log.Line("removed " + title_dir);

        index.erase(hash);
        SaveInjectedIndex(index);
        RefreshDetection();
        return finish(true, title + " removed");
    }

    void NsoSnesInstaller::StartInstall(const NsoInstallRequest& request) {
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true)) return;
        // Cleared here rather than on the worker so the screen can never draw a
        // previous run's step list in the frames before the thread starts.
        ResetSteps({});
        if (!SpawnWorker(false, request)) {
            ResetSteps({"Start pipeline"});
            FailStep(0, NsoErrorKind::FileWrite, "could not start the worker thread");
            busy.store(false);
        }
    }

    void NsoSnesInstaller::StartRestore() {
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true)) return;
        ResetSteps({});
        if (!SpawnWorker(true, NsoInstallRequest{})) {
            ResetSteps({"Start pipeline"});
            FailStep(0, NsoErrorKind::FileWrite, "could not start the worker thread");
            busy.store(false);
        }
    }

    void NsoSnesInstaller::RunInstall(NsoInstallRequest request) {
        auto& log = NsoLog::Instance();
        auto& config = romm::model::ConfigManager::Instance();

        ResetSteps({
            "Detect SNES Online installation",   // 0
            "Prepare staging area",              // 1
            "Fetch RomM metadata",               // 2
            "Download ROM",                      // 3
            "Download cover",                    // 4
            "Analyse SNES ROM",                  // 5
            "Generate .sfrom",                   // 6
            "Convert cover images",              // 7
            "Build database entry",              // 8
            "Validate staged database",          // 9
            "Back up active files",              // 10
            "Install title assets",              // 11
            "Patch localized strings",           // 12
            "Replace database",                  // 13
            "Verify installation"                // 14
        });

        log.BeginSession("SNES Online injection test for \"" + request.title + "\" (rom_id " +
                         std::to_string(request.rom_id) + ")");

        // --- 0. Detection -------------------------------------------------
        BeginStep(0);
        NsoSnesInstall install = DetectNsoSnes();
        {
            std::lock_guard<std::mutex> lock(mutex);
            detection = install;
        }
        if (!install.found) {
            FailStep(0, NsoErrorKind::NotDetected,
                     install.error.empty() ? "no SNES Online LayeredFS found" : install.error);
            log.EndSession(false, "detection failed");
            return;
        }
        log.KV("nso.title_id", install.title_id);
        log.KV("nso.content_root", install.content_root);
        log.KV("nso.database", install.database_path);
        log.KV("nso.exefs_mod", install.has_exefs_mod ? "present" : "absent");
        log.KV("nso.entries_before", std::to_string(install.entry_count));
        for (size_t i = 0; i < install.strings_files.size(); ++i) {
            log.KV("nso.strings[" + install.strings_languages[i] + "]", install.strings_files[i]);
        }
        FinishStep(0, install.title_id + " - " + std::to_string(install.entry_count) + " entries");

        // --- 1. Staging ---------------------------------------------------
        BeginStep(1);
        log.Line("clearing staging directory");
        RemoveDirectoryTree(kStagingDir);
        log.Line("creating staging directory");
        EnsureDir(kStagingDir);
        if (!IsDirectory(kStagingDir)) {
            FailStep(1, ClassifyWriteFailure(), std::string("cannot create ") + kStagingDir);
            log.EndSession(false, "staging unavailable");
            return;
        }
        log.KV("staging.dir", kStagingDir);
        FinishStep(1, "ready");

        // --- 2. Metadata --------------------------------------------------
        BeginStep(2);
        std::string publisher;
        std::string description;
        std::string release_date;
        int players_count = 1;
        {
            const std::string url = config.GetRommHost() + "/api/roms/" + std::to_string(request.rom_id);
            const std::map<std::string, std::string> headers = {
                {"Authorization", "Bearer " + config.GetApiKey()},
                {"Content-Type", "application/json"}
            };
            // getAsync, not getSync: this keeps curl and the TLS handshake on
            // HttpClient's own pool threads, so nothing in this pipeline ever
            // runs a network stack on the worker's stack. It also means the
            // per-thread curl handle getSync would create here is never
            // leaked — this thread is detached and would never release it.
            log.Line("requesting ROM metadata");
            auto pending = HttpClient::getAsync(url, headers, HttpPriority::High);
            while (!pending->completed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            const HttpResult& res = *pending;
            if (res.success) {
                romm::model::jsonExtractString(res.body, "summary", description);

                std::vector<std::string> companies;
                if (romm::model::jsonExtractStringArray(res.body, "companies", companies) && !companies.empty()) {
                    publisher = companies.front();
                }

                long long first_release = 0;
                if (romm::model::jsonExtractLongLong(res.body, "first_release_date", first_release)) {
                    release_date = UnixToIsoDate(first_release);
                }

                int parsed_players = 0;
                if (romm::model::jsonExtractInt(res.body, "player_count", parsed_players) && parsed_players > 0) {
                    players_count = parsed_players;
                } else {
                    std::string players_text;
                    if (romm::model::jsonExtractString(res.body, "player_count", players_text)) {
                        const long parsed = std::strtol(players_text.c_str(), nullptr, 10);
                        if (parsed > 0) players_count = (int)parsed;
                    }
                }

                // The content endpoint is addressed by *file* id, not ROM id,
                // and the caller (the picker) only knows the ROM. Resolve the
                // first entry of the "files" array here. Multi-file ROMs are
                // out of scope: a SNES title is always a single .sfc/.smc.
                if (request.file_id == 0 || request.rom_filename.empty()) {
                    const size_t files_key = res.body.find("\"files\"");
                    const size_t array_start = (files_key == std::string::npos)
                                                   ? std::string::npos
                                                   : res.body.find('[', files_key);
                    const size_t object_start = (array_start == std::string::npos)
                                                    ? std::string::npos
                                                    : res.body.find('{', array_start);
                    if (object_start != std::string::npos) {
                        int depth = 0;
                        size_t object_end = std::string::npos;
                        bool in_string = false;
                        for (size_t i = object_start; i < res.body.size(); ++i) {
                            const char c = res.body[i];
                            if (c == '"' && (i == 0 || res.body[i - 1] != '\\')) { in_string = !in_string; continue; }
                            if (in_string) continue;
                            if (c == '{') ++depth;
                            else if (c == '}' && --depth == 0) { object_end = i; break; }
                        }
                        if (object_end != std::string::npos) {
                            const std::string first_file = res.body.substr(object_start, object_end - object_start + 1);
                            int parsed_id = 0;
                            if (request.file_id == 0 && romm::model::jsonExtractInt(first_file, "id", parsed_id)) {
                                request.file_id = parsed_id;
                            }
                            if (request.rom_filename.empty()) {
                                romm::model::jsonExtractString(first_file, "file_name", request.rom_filename);
                            }
                        }
                    }
                }
                if (request.rom_filename.empty()) {
                    romm::model::jsonExtractString(res.body, "fs_name", request.rom_filename);
                }
                if (request.file_id == 0) {
                    romm::model::jsonExtractInt(res.body, "file_id", request.file_id);
                }
                if (request.cover_url.empty()) {
                    // Same precedence as the normal download path: the HD cover
                    // when there is one, the thumbnail otherwise.
                    std::string cover;
                    if (!romm::model::jsonExtractString(res.body, "path_cover_large", cover) || cover.empty()) {
                        romm::model::jsonExtractString(res.body, "path_cover_small", cover);
                    }
                    if (!cover.empty()) {
                        const size_t q = cover.find('?');
                        if (q != std::string::npos) cover = cover.substr(0, q);
                        request.cover_url = (cover.rfind("http", 0) == 0) ? cover : (config.GetRommHost() + cover);
                    }
                }
                FinishStep(2, "publisher \"" + (publisher.empty() ? std::string("unknown") : publisher) +
                               "\", " + std::to_string(players_count) + " player(s)");
            } else {
                // Metadata is a nice-to-have; the pipeline still produces a
                // working entry from defaults.
                SkipStep(2, "unavailable (HTTP " + std::to_string(res.statusCode) + "), using defaults");
            }
        }
        if (players_count < 1) players_count = 1;
        if (players_count > 8) players_count = 8;
        log.KV("meta.publisher", publisher.empty() ? "(none)" : publisher);
        log.KV("meta.release_date", release_date.empty() ? "(none)" : release_date);
        log.KV("meta.players_count", std::to_string(players_count));
        log.KV("meta.description_chars", std::to_string(description.size()));

        // --- 3. ROM download ----------------------------------------------
        BeginStep(3);
        const std::string staged_rom = std::string(kStagingDir) + "/source.rom";
        if (!request.local_rom_path.empty() && FileSize(request.local_rom_path) > 0) {
            // The download flow already fetched this ROM. Copy it into staging
            // rather than pulling it down a second time — which is also what
            // makes a retry cheap.
            log.KV("rom.source", "already on SD: " + request.local_rom_path);
            std::string copy_error;
            if (!CopyFile(request.local_rom_path, staged_rom, copy_error)) {
                FailStep(3, ClassifyWriteFailure(), copy_error);
                log.EndSession(false, "staging the local ROM failed");
                return;
            }
            FinishStep(3, HumanBytes(FileSize(staged_rom)) + " (already downloaded)");
        } else if (request.rom_filename.empty() || request.file_id == 0) {
            FailStep(3, NsoErrorKind::SourceDownload, "RomM did not report a downloadable file for this ROM");
            log.EndSession(false, "no file to download");
            return;
        } else {
            const std::string url = config.GetRommHost() + "/api/roms/" + std::to_string(request.file_id) +
                                    "/files/content/" + UrlEncode(request.rom_filename);
            log.KV("download.rom_url", SafeUrlForLog(url));
            const std::map<std::string, std::string> headers = {
                {"Authorization", "Bearer " + config.GetApiKey()}
            };
            auto result = HttpClient::downloadFileAsync(url, headers, staged_rom, HttpPriority::Normal);
            while (!result->completed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
                const long long got = FileSize(staged_rom);
                if (got > 0) {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (steps.size() > 3) steps[3].detail = HumanBytes(got);
                }
            }
            if (!result->success) {
                FailStep(3, NsoErrorKind::SourceDownload,
                         result->error.empty() ? ("HTTP " + std::to_string(result->statusCode)) : result->error);
                log.EndSession(false, "ROM download failed");
                return;
            }
            FinishStep(3, HumanBytes(FileSize(staged_rom)));
        }
        log.KV("rom.staged_bytes", std::to_string(FileSize(staged_rom)));

        // --- 4. Cover download --------------------------------------------
        BeginStep(4);
        const std::string staged_cover = std::string(kStagingDir) + "/source_cover.img";
        bool have_cover = false;
        if (request.cover_url.empty()) {
            SkipStep(4, "RomM has no cover for this ROM");
        } else {
            log.KV("download.cover_url", SafeUrlForLog(request.cover_url));
            const std::map<std::string, std::string> headers = {
                {"Authorization", "Bearer " + config.GetApiKey()}
            };
            auto result = HttpClient::downloadFileAsync(request.cover_url, headers, staged_cover, HttpPriority::Normal);
            while (!result->completed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(150));
            }
            if (result->success && FileSize(staged_cover) > 0) {
                have_cover = true;
                FinishStep(4, HumanBytes(FileSize(staged_cover)));
            } else {
                SkipStep(4, result->error.empty() ? "download failed" : result->error);
            }
        }
        if (!have_cover) {
            FailStep(4, NsoErrorKind::CoverDecode,
                     "no cover art available; SNES Online requires a cover image for every entry");
            log.EndSession(false, "no cover");
            return;
        }

        // --- 5/6. ROM analysis and .sfrom generation -----------------------
        BeginStep(5);
        const std::string staged_sfrom = std::string(kStagingDir) + "/title.sfrom";
        log.Line("reading and analysing the ROM image");
        SfromConversionResult conversion = ConvertToSfrom(staged_rom, staged_sfrom);
        if (!conversion.rom.valid) {
            const NsoErrorKind kind = conversion.rom.unsupported ? NsoErrorKind::UnsupportedRom
                                                                 : NsoErrorKind::InvalidRom;
            const std::string message = conversion.rom.unsupported
                ? ("SFROM conversion unsupported for this ROM type.\n" + conversion.rom.error +
                   "\nNo SNES Online files were modified.")
                : ("Invalid SNES ROM.\n" + conversion.rom.error + "\nNo SNES Online files were modified.");
            FailStep(5, kind, message);
            log.EndSession(false, "ROM rejected");
            return;
        }
        {
            std::string detail = conversion.mapping + ", " + HumanBytes((long long)conversion.rom.rom_size) +
                                 (conversion.rom.had_copier_header ? ", copier header stripped" : "");
            if (!conversion.rom.warnings.empty()) {
                // Converted anyway, but the user should know why a game might
                // misbehave once it boots.
                detail += " - " + conversion.rom.warnings.front();
                if (conversion.rom.warnings.size() > 1) {
                    detail += " (+" + std::to_string(conversion.rom.warnings.size() - 1) + " more, see log)";
                }
            }
            FinishStep(5, detail);
        }

        BeginStep(6);
        if (!conversion.success) {
            FailStep(6, NsoErrorKind::SfromGeneration, conversion.error);
            log.EndSession(false, "sfrom generation failed");
            return;
        }
        const std::string staged_sfromsig = std::string(kStagingDir) + "/title.sfromsig";
        std::string sig_error;
        if (!WriteSfromSig(staged_sfromsig, sig_error)) {
            FailStep(6, ClassifyWriteFailure(), sig_error);
            log.EndSession(false, "sfromsig write failed");
            return;
        }
        FinishStep(6, HumanBytes((long long)conversion.totalBytes) + ", footer " + conversion.footerHex);

        // --- 7. Images -----------------------------------------------------
        BeginStep(7);
        const std::string staged_cover_png = std::string(kStagingDir) + "/title.png";
        const std::string staged_details_png = std::string(kStagingDir) + "/title-details.png";
        log.Line("decoding cover and encoding the 512x374 PNG");
        NsoImageResult cover_result = ConvertCover(staged_cover, staged_cover_png);
        if (!cover_result.success) {
            FailStep(7, NsoErrorKind::ImageConversion, cover_result.error);
            log.EndSession(false, "cover conversion failed");
            return;
        }
        log.Line("encoding the 400x300 details PNG");
        NsoImageResult details_result = ConvertDetails(staged_cover, staged_details_png);
        if (!details_result.success) {
            FailStep(7, NsoErrorKind::ImageConversion, details_result.error);
            log.EndSession(false, "details conversion failed");
            return;
        }
        log.KV("image.source", std::to_string(cover_result.source_width) + "x" +
                               std::to_string(cover_result.source_height));
        log.KV("image.cover_out", std::to_string(cover_result.output_width) + "x" +
                                  std::to_string(cover_result.output_height) + ", " +
                                  std::to_string(cover_result.output_bytes) + " bytes");
        log.KV("image.details_out", std::to_string(details_result.output_width) + "x" +
                                    std::to_string(details_result.output_height) + ", " +
                                    std::to_string(details_result.output_bytes) + " bytes");
        FinishStep(7, std::to_string(cover_result.source_width) + "x" + std::to_string(cover_result.source_height) +
                       " -> 512x374 + 400x300");

        // --- 8. Database entry ---------------------------------------------
        BeginStep(8);
        if (!install.database_exists) {
            // App and mod are there, LayeredFS isn't yet. Create the database
            // rather than refusing — this is exactly the "empty database, user
            // installs what they want" setup.
            std::string create_error;
            if (!CreateEmptyDatabase(install, create_error)) {
                FailStep(8, ClassifyWriteFailure(), create_error);
                log.EndSession(false, "could not create the database");
                return;
            }
            log.KV("db.created", install.database_path);
        }
        log.Line("parsing " + install.database_path);
        TitlesDb db;
        if (!LoadTitlesDb(install.database_path, db)) {
            FailStep(8, NsoErrorKind::MalformedDatabase, db.error);
            log.EndSession(false, "database unreadable");
            return;
        }

        auto injected = LoadInjectedIndex();
        std::string preferred_code;
        {
            auto it = injected.find(conversion.rom.sha256);
            if (it != injected.end()) {
                preferred_code = it->second.code;
                // The remembered slot is only reusable if it is still free, or
                // still holds the same game. If something else has taken it
                // since (a CaVE session, a restored backup from another
                // console), overwriting it would silently delete that entry.
                if (HasCode(db, preferred_code)) {
                    std::string current_title;
                    GetEntryField(db, preferred_code, "title", current_title);
                    if (!it->second.title.empty() && current_title != it->second.title) {
                        log.Line("remembered code " + preferred_code + " now holds \"" + current_title +
                                 "\"; allocating a fresh code instead");
                        preferred_code.clear();
                    }
                }
            }
        }
        const bool is_reinstall = !preferred_code.empty() && HasCode(db, preferred_code);
        const std::string code = AllocateGameCode(db, conversion.rom.sha256, preferred_code);
        if (code.empty()) {
            FailStep(8, NsoErrorKind::DuplicateEntry, "no free S-#### code left in the database");
            log.EndSession(false, "code allocation failed");
            return;
        }

        NsoTitleMeta meta;
        meta.code = code;
        meta.title = request.title.empty() ? conversion.rom.internal_title : request.title;
        meta.sort_title = MakeSortKey(meta.title);
        meta.publisher = publisher.empty() ? "Unknown" : publisher;
        meta.sort_publisher = MakeSortKey(meta.publisher);
        meta.copyright = publisher.empty() ? "" : ("\xC2\xA9 " + publisher);
        meta.release_date = release_date.empty() ? TodayIso() : release_date;
        meta.lcla6_release_date = TodayIso();
        meta.players_count = players_count;
        meta.simultaneous = players_count > 1;
        meta.save_count = 1;
        meta.volume = 100;

        const std::string entry_json = BuildTitleEntryJson(meta);
        log.KV("db.entries_before", std::to_string(db.codes.size()));
        log.KV("db.generated_code", code + (is_reinstall ? " (reusing the slot from an earlier run)" : " (new)"));
        log.KV("db.entry_json", entry_json);

        std::string new_db_text;
        std::string db_error;
        if (!UpsertTitleEntry(db, code, entry_json, new_db_text, db_error)) {
            FailStep(8, NsoErrorKind::MalformedDatabase, db_error);
            log.EndSession(false, "database edit failed");
            return;
        }
        FinishStep(8, code);

        // --- 9. Validate staged database ------------------------------------
        BeginStep(9);
        const std::string staged_db = std::string(kStagingDir) + "/lclassics.titlesdb";
        std::string write_error;
        if (!WriteFileAtomic(staged_db, new_db_text, write_error)) {
            FailStep(9, ClassifyWriteFailure(), write_error);
            log.EndSession(false, "staged database write failed");
            return;
        }
        std::string reread;
        if (!ReadFileText(staged_db, reread) || reread != new_db_text) {
            FailStep(9, NsoErrorKind::Validation, "staged database did not read back byte-identical");
            log.EndSession(false, "staged database mismatch");
            return;
        }
        std::string verify_error;
        if (!VerifySerializedDb(reread, db.codes, code, verify_error)) {
            FailStep(9, NsoErrorKind::Validation, verify_error);
            log.EndSession(false, "staged database validation failed");
            return;
        }
        size_t entries_after = 0;
        {
            TitlesDb staged_parsed;
            if (!LoadTitlesDb(staged_db, staged_parsed)) {
                FailStep(9, NsoErrorKind::Validation, staged_parsed.error);
                log.EndSession(false, "staged database re-parse failed");
                return;
            }
            entries_after = staged_parsed.codes.size();
            log.KV("db.entries_after", std::to_string(entries_after));
        }

        // Patch every localized string table present in the LayeredFS. The
        // emulator reads the title's description and its key-guide labels from
        // strings.lng, not from the database, so an entry without them shows an
        // empty info screen.
        const std::string wrapped_description = description.empty()
            ? ("Injected by romm-nx (experimental).\n\n" + meta.title)
            : WrapDescription(description, 50);
        std::vector<std::pair<std::string, std::string>> staged_strings; // target path -> new content
        for (size_t i = 0; i < install.strings_files.size(); ++i) {
            std::string original;
            if (!ReadFileText(install.strings_files[i], original)) {
                log.Line("strings: cannot read " + install.strings_files[i] + " (skipped)");
                continue;
            }
            log.Line("patching " + install.strings_files[i] + " (" + std::to_string(original.size()) + " bytes)");
            std::string patched;
            bool changed = false;
            std::string strings_error;
            if (!PatchStringsFile(original, code, wrapped_description, patched, changed, strings_error)) {
                FailStep(9, NsoErrorKind::MalformedDatabase,
                         install.strings_languages[i] + "/strings.lng: " + strings_error);
                log.EndSession(false, "strings patch failed");
                return;
            }
            if (changed) staged_strings.emplace_back(install.strings_files[i], patched);
        }
        FinishStep(9, std::to_string(db.codes.size()) + " -> " + std::to_string(entries_after) +
                       " entries, " + std::to_string(staged_strings.size()) + " string table(s) staged");

        // --- 10. Backup ------------------------------------------------------
        BeginStep(10);
        const std::string backup_dir = std::string(kBackupsDir) + "/" + NowStamp(true);
        EnsureDir(backup_dir);
        if (!IsDirectory(backup_dir)) {
            FailStep(10, ClassifyWriteFailure(), "cannot create " + backup_dir);
            log.EndSession(false, "backup directory unavailable");
            return;
        }

        const std::string title_dir = install.titles_dir + "/" + code;
        const bool title_dir_existed = IsDirectory(title_dir);

        struct Artefact { std::string staged; std::string target; };
        const std::vector<Artefact> artefacts = {
            {staged_sfrom,        title_dir + "/" + code + ".sfrom"},
            {staged_sfromsig,     title_dir + "/" + code + ".sfromsig"},
            {staged_cover_png,    title_dir + "/" + code + ".png"},
            {staged_details_png,  title_dir + "/" + code + "-details.png"}
        };

        std::vector<BackupRecord> records;
        std::string backup_error;

        auto back_up = [&](const std::string& target, const std::string& backup_name) -> bool {
            if (!CopyFile(target, backup_dir + "/" + backup_name, backup_error)) return false;
            records.push_back({'R', backup_name, target});
            return true;
        };

        if (!back_up(install.database_path, "lclassics.titlesdb")) {
            FailStep(10, ClassifyWriteFailure(), backup_error);
            log.EndSession(false, "database backup failed");
            return;
        }
        for (size_t i = 0; i < staged_strings.size(); ++i) {
            const std::string name = "strings_" + std::to_string(i) + ".lng";
            if (!back_up(staged_strings[i].first, name)) {
                FailStep(10, ClassifyWriteFailure(), backup_error);
                log.EndSession(false, "strings backup failed");
                return;
            }
        }
        if (!title_dir_existed) {
            records.push_back({'D', "", title_dir});
        }
        for (size_t i = 0; i < artefacts.size(); ++i) {
            if (PathExists(artefacts[i].target)) {
                const std::string name = "asset_" + std::to_string(i);
                if (!back_up(artefacts[i].target, name)) {
                    FailStep(10, ClassifyWriteFailure(), backup_error);
                    log.EndSession(false, "asset backup failed");
                    return;
                }
            } else {
                records.push_back({'C', "", artefacts[i].target});
            }
        }

        {
            std::string manifest;
            manifest += "version=1\n";
            manifest += "timestamp=" + NowStamp(false) + "\n";
            manifest += "title_id=" + install.title_id + "\n";
            manifest += "code=" + code + "\n";
            manifest += "title=" + meta.title + "\n";
            manifest += "sha256=" + conversion.rom.sha256 + "\n";
            manifest += "database=" + install.database_path + "\n";
            for (const auto& record : records) {
                manifest.push_back(record.kind);
                manifest.push_back('\t');
                manifest += record.backup_name;
                manifest.push_back('\t');
                manifest += record.target;
                manifest.push_back('\n');
            }
            std::string manifest_error;
            if (!WriteFileAtomic(backup_dir + "/manifest.txt", manifest, manifest_error)) {
                FailStep(10, ClassifyWriteFailure(), manifest_error);
                log.EndSession(false, "manifest write failed");
                return;
            }
        }
        log.KV("backup.dir", backup_dir);
        log.KV("backup.records", std::to_string(records.size()));
        FinishStep(10, backup_dir);

        // Anything that fails from here on has to put the console back exactly
        // as it was found.
        auto rollback = [&](const std::string& reason) {
            log.Line("Rolling back: " + reason);
            for (const auto& record : records) {
                if (record.kind == 'C') std::remove(record.target.c_str());
            }
            for (const auto& record : records) {
                if (record.kind == 'D') rmdir(record.target.c_str());
            }
            std::string restore_error;
            for (const auto& record : records) {
                if (record.kind != 'R') continue;
                if (!CopyFile(backup_dir + "/" + record.backup_name, record.target, restore_error)) {
                    log.Line("ROLLBACK FAILURE: could not restore " + record.target + " (" + restore_error + ")");
                }
            }
            TitlesDb check;
            if (LoadTitlesDb(install.database_path, check)) {
                log.Line("Rollback complete; active database is valid with " +
                         std::to_string(check.codes.size()) + " entries.");
            } else {
                log.Line("ROLLBACK FAILURE: active database is invalid after restore (" + check.error + ")");
            }
        };

        // --- 11. Install assets ---------------------------------------------
        BeginStep(11);
        EnsureDir(title_dir);
        if (!IsDirectory(title_dir)) {
            FailStep(11, ClassifyWriteFailure(), "cannot create " + title_dir);
            rollback("title directory could not be created");
            log.EndSession(false, "install failed");
            return;
        }
        for (const auto& artefact : artefacts) {
            const long long staged_size = FileSize(artefact.staged);
            if (staged_size <= 0) {
                FailStep(11, NsoErrorKind::Validation, "staged file missing: " + artefact.staged);
                rollback("a staged artefact was missing");
                log.EndSession(false, "install failed");
                return;
            }
            std::string copy_error;
            if (!CopyFile(artefact.staged, artefact.target, copy_error)) {
                FailStep(11, ClassifyWriteFailure(), copy_error);
                rollback(copy_error);
                log.EndSession(false, "install failed");
                return;
            }
            const long long installed_size = FileSize(artefact.target);
            if (installed_size != staged_size) {
                FailStep(11, NsoErrorKind::Validation,
                         "size mismatch after copying " + artefact.target + " (" +
                         std::to_string(installed_size) + " != " + std::to_string(staged_size) + ")");
                rollback("installed artefact size mismatch");
                log.EndSession(false, "install failed");
                return;
            }
            log.Line("installed " + artefact.target + " (" + std::to_string(installed_size) + " bytes)");
        }
        FinishStep(11, std::to_string(artefacts.size()) + " files in " + title_dir);

        // --- 12. Strings ------------------------------------------------------
        BeginStep(12);
        if (staged_strings.empty()) {
            SkipStep(12, "no localized string table needed changes");
        } else {
            for (const auto& entry : staged_strings) {
                std::string strings_error;
                if (!WriteFileAtomic(entry.first, entry.second, strings_error)) {
                    FailStep(12, ClassifyWriteFailure(), strings_error);
                    rollback(strings_error);
                    log.EndSession(false, "strings write failed");
                    return;
                }
                log.Line("patched " + entry.first + " (" + std::to_string(entry.second.size()) + " bytes)");
            }
            FinishStep(12, std::to_string(staged_strings.size()) + " file(s)");
        }

        // --- 13. Database (always last) ---------------------------------------
        BeginStep(13);
        {
            std::string db_write_error;
            if (!WriteFileAtomic(install.database_path, new_db_text, db_write_error)) {
                FailStep(13, ClassifyWriteFailure(), db_write_error);
                rollback(db_write_error);
                log.EndSession(false, "database replace failed");
                return;
            }
            log.Line("replaced " + install.database_path + " (" + std::to_string(new_db_text.size()) + " bytes)");
        }
        FinishStep(13, HumanBytes((long long)new_db_text.size()));

        // --- 14. Verify --------------------------------------------------------
        BeginStep(14);
        {
            TitlesDb final_db;
            if (!LoadTitlesDb(install.database_path, final_db)) {
                FailStep(14, NsoErrorKind::Validation, final_db.error);
                rollback("installed database failed to parse");
                log.EndSession(false, "verification failed");
                return;
            }
            std::string final_error;
            if (!VerifySerializedDb(final_db.text, db.codes, code, final_error)) {
                FailStep(14, NsoErrorKind::Validation, final_error);
                rollback(final_error);
                log.EndSession(false, "verification failed");
                return;
            }
            for (const auto& artefact : artefacts) {
                if (FileSize(artefact.target) <= 0) {
                    FailStep(14, NsoErrorKind::Validation, "missing after install: " + artefact.target);
                    rollback("an installed artefact vanished");
                    log.EndSession(false, "verification failed");
                    return;
                }
            }
            log.KV("verify.entries", std::to_string(final_db.codes.size()));
            FinishStep(14, std::to_string(final_db.codes.size()) + " entries, all assets present");
        }

        // Record the hash -> code mapping so a reinstall reuses this slot.
        {
            auto index = LoadInjectedIndex();
            index[conversion.rom.sha256] = InjectedEntry{code, meta.title, request.rom_id};
            SaveInjectedIndex(index);
        }

        RemoveDirectoryTree(kStagingDir);

        {
            std::lock_guard<std::mutex> lock(mutex);
            state = NsoPipelineState::Success;
            summary = meta.title + " installed as " + code + ".\nReboot or relaunch SNES Online to see it.";
        }
        log.EndSession(true, meta.title + " -> " + code);
        RefreshDetection();
    }

    void NsoSnesInstaller::RunRestore() {
        auto& log = NsoLog::Instance();

        ResetSteps({
            "Locate latest backup",
            "Read backup manifest",
            "Remove files created by the installation",
            "Restore backed-up files",
            "Validate restored database"
        });

        log.BeginSession("SNES Online backup restore");

        BeginStep(0);
        const std::string backup_dir = LatestBackupPath();
        if (backup_dir.empty()) {
            FailStep(0, NsoErrorKind::NotDetected, "no backup found under " + std::string(kBackupsDir));
            log.EndSession(false, "no backup");
            return;
        }
        log.KV("restore.backup_dir", backup_dir);
        FinishStep(0, backup_dir);

        BeginStep(1);
        std::string manifest;
        if (!ReadFileText(backup_dir + "/manifest.txt", manifest)) {
            FailStep(1, NsoErrorKind::Rollback, "cannot read " + backup_dir + "/manifest.txt");
            log.EndSession(false, "manifest unreadable");
            return;
        }

        std::vector<BackupRecord> records;
        std::string database_path;
        std::string code;
        std::string sha256;
        {
            size_t pos = 0;
            while (pos < manifest.size()) {
                size_t eol = manifest.find('\n', pos);
                if (eol == std::string::npos) eol = manifest.size();
                std::string line = manifest.substr(pos, eol - pos);
                pos = eol + 1;
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;

                if (line.rfind("database=", 0) == 0) { database_path = line.substr(9); continue; }
                if (line.rfind("code=", 0) == 0) { code = line.substr(5); continue; }
                if (line.rfind("sha256=", 0) == 0) { sha256 = line.substr(7); continue; }
                if (line.size() < 3 || line[1] != '\t') continue;

                BackupRecord record;
                record.kind = line[0];
                const size_t tab2 = line.find('\t', 2);
                if (tab2 == std::string::npos) continue;
                record.backup_name = line.substr(2, tab2 - 2);
                record.target = line.substr(tab2 + 1);
                if (record.kind == 'R' || record.kind == 'C' || record.kind == 'D') {
                    records.push_back(record);
                }
            }
        }
        if (records.empty() || database_path.empty()) {
            FailStep(1, NsoErrorKind::Rollback, "backup manifest is empty or malformed");
            log.EndSession(false, "manifest malformed");
            return;
        }
        FinishStep(1, std::to_string(records.size()) + " records, code " + code);

        BeginStep(2);
        size_t removed = 0;
        for (const auto& record : records) {
            if (record.kind != 'C') continue;
            if (std::remove(record.target.c_str()) == 0) {
                ++removed;
                log.Line("removed " + record.target);
            }
        }
        for (const auto& record : records) {
            if (record.kind != 'D') continue;
            if (rmdir(record.target.c_str()) == 0) log.Line("removed directory " + record.target);
        }
        FinishStep(2, std::to_string(removed) + " file(s) removed");

        BeginStep(3);
        size_t restored = 0;
        std::string restore_error;
        bool restore_failed = false;
        for (const auto& record : records) {
            if (record.kind != 'R') continue;
            const std::string source = backup_dir + "/" + record.backup_name;
            if (!CopyFile(source, record.target, restore_error)) {
                log.Line("RESTORE FAILURE: " + record.target + " (" + restore_error + ")");
                restore_failed = true;
                continue;
            }
            ++restored;
            log.Line("restored " + record.target);
        }
        if (restore_failed) {
            FailStep(3, NsoErrorKind::Rollback, restore_error);
            log.EndSession(false, "restore incomplete");
            return;
        }
        FinishStep(3, std::to_string(restored) + " file(s) restored");

        BeginStep(4);
        TitlesDb db;
        if (!LoadTitlesDb(database_path, db)) {
            FailStep(4, NsoErrorKind::Validation, db.error);
            log.EndSession(false, "restored database invalid");
            return;
        }
        if (!code.empty() && HasCode(db, code)) {
            log.Line("note: " + code + " is still present in the restored database "
                     "(the backup was taken during a reinstall of that entry).");
        }
        FinishStep(4, std::to_string(db.codes.size()) + " entries");

        if (!sha256.empty()) {
            auto index = LoadInjectedIndex();
            index.erase(sha256);
            SaveInjectedIndex(index);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            state = NsoPipelineState::Success;
            summary = "Restored the SNES Online backup from " + backup_dir + ".";
        }
        log.EndSession(true, "restored " + backup_dir);
        RefreshDetection();
    }

}
