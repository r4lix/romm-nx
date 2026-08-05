#include "NsoSnesInstaller.hpp"

#include "GbRom.hpp"
#include "GbaRom.hpp"
#include "N64Rom.hpp"
#include "NesRom.hpp"
#include "NsoImage.hpp"
#include "NsoJson.hpp"
#include "NsoLog.hpp"
#include "NsoGbDb.hpp"
#include "NsoGbaDb.hpp"
#include "NsoN64Db.hpp"
#include "NsoN64MetaPack.hpp"
#include "NsoNesDb.hpp"
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
        NsoJobKind kind;
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

        // Same, for a binary payload. The NES path needs the whole ROM in
        // memory to parse its header and hash it; SNES gets this via
        // SfromWriter instead.
        bool ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) return false;
            std::fseek(f, 0, SEEK_END);
            const long size = std::ftell(f);
            std::fseek(f, 0, SEEK_SET);
            if (size <= 0) { std::fclose(f); return false; }
            out.resize((size_t)size);
            const size_t read = std::fread(out.data(), 1, out.size(), f);
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

        std::map<std::string, InjectedEntry> LoadInjectedIndex(const std::string& index_path) {
            std::map<std::string, InjectedEntry> out;
            std::string text;
            if (!ReadFileText(index_path, text)) return out;
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

        void SaveInjectedIndex(const std::string& root, const std::string& index_path,
                               const std::map<std::string, InjectedEntry>& index) {
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
            EnsureDir(root);
            WriteFileAtomic(index_path, text, error);
        }

        // ------------------------------------------------------------------
        // Platform profile
        // ------------------------------------------------------------------
        //
        // Everything that differs between the Switch Online apps, in one place.
        // The pipeline below reads this instead of hardcoding SNES: the staging
        // and backup order, the rollback and the validation are identical for
        // both platforms, and duplicating 1600 lines to change six strings and
        // four function calls would guarantee the two copies drifted.
        struct NsoProfile {
            NsoPlatform platform = NsoPlatform::Snes;
            const char* name = "SNES Online";
            const char* rom_kind = "SNES ROM";

            std::string root;
            std::string staging;
            std::string backups;
            std::string index;

            // Asset file suffixes, in the order they are staged and installed.
            // SNES has four (the zeroed .sfromsig included), NES three.
            std::vector<std::string> asset_suffixes;

            // Whether the ROM goes through a container step (.sfrom) or is
            // installed byte for byte (.nes).
            bool container_rom = true;
        };

        NsoProfile MakeProfile(NsoPlatform platform) {
            NsoProfile p;
            p.platform = platform;
            if (platform == NsoPlatform::N64) {
                p.name = "Nintendo 64 Online";
                p.rom_kind = "Nintendo 64 ROM";
                p.root = "sdmc:/switch/romm-nx/nso-n64";
                p.container_rom = false;
                // ".bnz" is the file that gets written — a zlib stream — while
                // the database entry names ".bin". Both are listed so removal
                // finds whichever is there, including a raw one left by another
                // tool. ".dtz" is the MetaPack, without which the title installs
                // perfectly and crashes the moment it is launched.
                p.asset_suffixes = {".bnz", ".bin", ".dtz", ".png", "-details.png"};
            } else if (platform == NsoPlatform::Gba) {
                p.name = "Game Boy Advance Online";
                p.rom_kind = "Game Boy Advance ROM";
                p.root = "sdmc:/switch/romm-nx/nso-gba";
                p.container_rom = false;
                p.asset_suffixes = {".gba", ".png", "-details.png"};
            } else if (platform == NsoPlatform::GameBoy) {
                p.name = "Game Boy Online";
                p.rom_kind = "Game Boy ROM";
                p.root = "sdmc:/switch/romm-nx/nso-gb";
                p.container_rom = false;
                // Both extensions: which one a title uses depends on the ROM's
                // own Color flag, not on the platform, so the removal path has
                // to look for either.
                p.asset_suffixes = {".gb", ".gbc", ".png", "-details.png"};
            } else if (platform == NsoPlatform::Nes) {
                p.name = "NES Online";
                p.rom_kind = "NES ROM";
                p.root = "sdmc:/switch/romm-nx/nso-nes";
                p.container_rom = false;
                // ".png" must not be tested before "00.png" would be: both are
                // suffixes of the details file name, and the removal path
                // matches on the full "<code><suffix>" so order is safe either
                // way — but the install order is what the log shows, so keep
                // ROM, cover, details.
                p.asset_suffixes = {".nes", ".png", "00.png"};
            } else {
                p.root = "sdmc:/switch/romm-nx/nso-snes";
                p.asset_suffixes = {".sfrom", ".sfromsig", ".png", "-details.png"};
            }
            p.staging = p.root + "/staging";
            p.backups = p.root + "/backups";
            p.index = p.root + "/injected.txt";
            return p;
        }

        // The key-guide labels a platform's info screen shows, and the ONE place
        // that mapping is allowed to live.
        //
        // Install writes these keys into every strings.lng and removal erases
        // them, so the two disagreeing is not cosmetic: whatever removal does
        // not know about stays on the console forever. They did disagree —
        // removal assumed SNES for everything except NES, which left N64's
        // eleven controller keys (cunit_*, stick_*, z_r_) behind on every
        // uninstall, in every language. Route both sides through here.
        const std::vector<NsoGuideKey>& GuideKeysFor(NsoPlatform platform) {
            switch (platform) {
                case NsoPlatform::N64:     return N64GuideKeys();
                case NsoPlatform::Gba:     return GbaGuideKeys();
                case NsoPlatform::GameBoy: return GbGuideKeys();
                case NsoPlatform::Nes:     return NesGuideKeys();
                default:                   return SnesGuideKeys();
            }
        }

        // Every guide key any platform can write, deduplicated.
        //
        // Removal uses this rather than GuideKeysFor(). Install has to be exact
        // — it is writing the keys — but removal only has to be exhaustive, and
        // erasing a key that was never there costs nothing (EraseMembers simply
        // does not find it). That makes "removal erases everything install could
        // have written" true by construction rather than by two lists agreeing,
        // and it also cleans up after older builds whose key set differed.
        const std::vector<NsoGuideKey>& AllGuideKeys() {
            static const std::vector<NsoGuideKey> keys = [] {
                std::vector<NsoGuideKey> all;
                const std::vector<NsoGuideKey>* sets[] = {
                    &SnesGuideKeys(), &NesGuideKeys(), &GbGuideKeys(),
                    &GbaGuideKeys(), &N64GuideKeys()
                };
                for (const auto* set : sets) {
                    for (const auto& key : *set) {
                        bool seen = false;
                        for (const auto& have : all) {
                            if (std::strcmp(have.suffix, key.suffix) == 0) { seen = true; break; }
                        }
                        if (!seen) all.push_back(key);
                    }
                }
                return all;
            }();
            return keys;
        }

        NsoSnesInstall DetectFor(NsoPlatform platform) {
            switch (platform) {
                case NsoPlatform::Nes:     return DetectNsoNes();
                case NsoPlatform::GameBoy: return DetectNsoGb();
                case NsoPlatform::Gba:     return DetectNsoGba();
                case NsoPlatform::N64:     return DetectNsoN64();
                default:                   return DetectNsoSnes();
            }
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

    bool IsNsoPlatformUnstable(NsoPlatform platform) {
        return platform == NsoPlatform::N64;
    }

    bool PlatformSupportsInjection(const std::string& canonical_platform_id) {
        NsoPlatform ignored;
        return NsoPlatformForId(canonical_platform_id, ignored);
    }

    bool NsoPlatformForId(const std::string& canonical_platform_id, NsoPlatform& out) {
        if (canonical_platform_id == "snes") {
            out = NsoPlatform::Snes;
            return true;
        }
        if (canonical_platform_id == "nes") {
            out = NsoPlatform::Nes;
            return true;
        }
        // Both RomM platforms feed the one Game Boy app. Whether a title ends up
        // as DMG or CGB is decided from the cartridge header at install time,
        // not from which shelf the library filed it on.
        if (canonical_platform_id == "gb" || canonical_platform_id == "gbc") {
            out = NsoPlatform::GameBoy;
            return true;
        }
        if (canonical_platform_id == "gba") {
            out = NsoPlatform::Gba;
            return true;
        }
        if (canonical_platform_id == "n64") {
            out = NsoPlatform::N64;
            return true;
        }
        return false;
    }

    const char* NsoPlatformName(NsoPlatform platform) {
        switch (platform) {
            case NsoPlatform::Nes:     return "NES Online";
            case NsoPlatform::GameBoy: return "Game Boy Online";
            case NsoPlatform::Gba:     return "Game Boy Advance Online";
            case NsoPlatform::N64:     return "Nintendo 64 Online";
            default:                   return "SNES Online";
        }
    }

    NsoSnesInstaller& NsoSnesInstaller::Instance() {
        static NsoSnesInstaller inst;
        return inst;
    }

    const char* NsoSnesInstaller::LogPath() const {
        return NsoLog::Instance().Path();
    }

    void NsoSnesInstaller::RefreshDetection(NsoPlatform platform) {
        // Detection runs outside the lock: it walks /atmosphere and parses a
        // database, which is far too long to hold a mutex the UI polls.
        NsoSnesInstall found = DetectFor(platform);
        std::lock_guard<std::mutex> lock(mutex);
        detection[PlatformSlot(platform)] = found;
    }

    NsoSnesInstall NsoSnesInstaller::GetDetection(NsoPlatform platform) const {
        std::lock_guard<std::mutex> lock(mutex);
        return detection[PlatformSlot(platform)];
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

    std::string NsoSnesInstaller::LatestBackupPath(NsoPlatform platform) const {
        const std::string backups = MakeProfile(platform).backups;
        if (!IsDirectory(backups)) return std::string();
        DIR* dir = opendir(backups.c_str());
        if (!dir) return std::string();
        std::string newest;
        while (dirent* entry = readdir(dir)) {
            const std::string name = entry->d_name;
            if (name == "." || name == "..") continue;
            const std::string full = backups + "/" + name;
            if (!IsDirectory(full)) continue;
            if (!PathExists(full + "/manifest.txt")) continue;
            // Names are YYYYMMDD-HHMMSS, so lexical order is chronological.
            if (name > newest) newest = name;
        }
        closedir(dir);
        return newest.empty() ? std::string() : (backups + "/" + newest);
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

    // Replaces a running step's detail without ending it — the bulk removal's
    // "12 / 28 - <title>" counter, which has to move while the step is still
    // in flight.
    void NsoSnesInstaller::UpdateStep(size_t index, const std::string& detail) {
        std::lock_guard<std::mutex> lock(mutex);
        if (index >= steps.size()) return;
        steps[index].detail = detail;
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
        switch (job->kind) {
            case NsoJobKind::Install:      job->self->RunInstall(job->request); break;
            case NsoJobKind::Restore:      job->self->RunRestore(job->request.platform); break;
            case NsoJobKind::UninstallAll: job->self->RunUninstallAll(job->request.platform); break;
        }
        job->self->busy.store(false);
        delete job;
        return nullptr;
    }

    bool NsoSnesInstaller::SpawnWorker(NsoJobKind kind, const NsoInstallRequest& request) {
        PipelineJob* job = new PipelineJob{this, kind, request};

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


    NsoInstallOutcome NsoSnesInstaller::UninstallSync(int rom_id, const std::string& fallback_title,
                                                      NsoPlatform platform) {
        NsoInstallOutcome outcome;
        auto& log = NsoLog::Instance();
        const NsoProfile profile = MakeProfile(platform);

        auto index = LoadInjectedIndex(profile.index);
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

        log.BeginSession("Remove \"" + title + "\" (" + code + ") from " + profile.name);
        if (matched_by_title) {
            log.Line("matched by title: this index entry was written before rom_id was recorded");
        }

        std::string error;
        const bool ok = RemoveInjectedEntry(platform, hash, code, title, error);
        log.EndSession(ok, ok ? title + " removed" : error);
        busy.store(false);
        outcome.success = ok;
        outcome.error = ok ? "" : error;
        outcome.code = code;
        return outcome;
    }

    // The removal itself, shared by the single-game uninstall above and the
    // bulk "remove all" below. Assumes the caller holds `busy` and has a log
    // session open — it must not touch either, or the bulk path (which holds
    // busy for the whole run) would deadlock against itself.
    bool NsoSnesInstaller::RemoveInjectedEntry(NsoPlatform platform,
                                               const std::string& hash, const std::string& code,
                                               const std::string& title, std::string& out_error) {
        auto& log = NsoLog::Instance();
        const NsoProfile profile = MakeProfile(platform);
        auto index = LoadInjectedIndex(profile.index);

        auto finish = [&](bool ok, const std::string& why) {
            if (!ok) out_error = why;
            return ok;
        };

        NsoSnesInstall install = DetectFor(platform);
        if (!install.found) return finish(false, std::string("no ") + profile.name + " LayeredFS found");

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
            if (!UnpatchStringsFile(original, code, AllGuideKeys(), patched, changed, error)) {
                return finish(false, error);
            }
            if (changed) new_strings.emplace_back(file, patched);
        }

        const std::string title_dir = install.titles_dir + "/" + code;
        const std::string backup_dir = profile.backups + "/" + NowStamp(true);
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
        for (size_t i = 0; i < profile.asset_suffixes.size(); ++i) {
            if (!back_up(title_dir + "/" + code + profile.asset_suffixes[i], "asset_" + std::to_string(i))) {
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
        SaveInjectedIndex(profile.root, profile.index, index);
        RefreshDetection(platform);
        return true;
    }

    void NsoSnesInstaller::StartInstall(const NsoInstallRequest& request) {
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true)) return;
        // Cleared here rather than on the worker so the screen can never draw a
        // previous run's step list in the frames before the thread starts.
        ResetSteps({});
        if (!SpawnWorker(NsoJobKind::Install, request)) {
            ResetSteps({"Start pipeline"});
            FailStep(0, NsoErrorKind::FileWrite, "could not start the worker thread");
            busy.store(false);
        }
    }

    size_t NsoSnesInstaller::InjectedGameCount(NsoPlatform platform) const {
        size_t count = 0;
        for (const auto& entry : LoadInjectedIndex(MakeProfile(platform).index)) {
            if (!entry.second.code.empty()) count++;
        }
        return count;
    }

    void NsoSnesInstaller::StartUninstallAll(NsoPlatform platform) {
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true)) return;
        ResetSteps({});
        NsoInstallRequest job_request;
        job_request.platform = platform;
        if (!SpawnWorker(NsoJobKind::UninstallAll, job_request)) {
            ResetSteps({"Start pipeline"});
            FailStep(0, NsoErrorKind::FileWrite, "could not start the worker thread");
            busy.store(false);
        }
    }

    // Removes every game romm-nx injected, one at a time, each with its own
    // backup — the same per-game path the single uninstall takes, so a bulk
    // clean-up is exactly N ordinary removals and never a special case that
    // rewrites the database wholesale.
    //
    // Fixed step list rather than one step per game: with 28 injected titles a
    // per-game list runs off the bottom of the progress panel, so the running
    // count lives in step 1's detail instead.
    void NsoSnesInstaller::RunUninstallAll(NsoPlatform platform) {
        auto& log = NsoLog::Instance();
        const NsoProfile profile = MakeProfile(platform);
        log.BeginSession(std::string("Remove every romm-nx-injected game from ") + profile.name);

        ResetSteps({"Read injected index", "Remove entries", "Verify database"});

        struct Target { std::string hash, code, title; };
        std::vector<Target> targets;
        BeginStep(0);
        for (const auto& entry : LoadInjectedIndex(profile.index)) {
            if (entry.second.code.empty()) continue;
            targets.push_back({entry.first, entry.second.code, entry.second.title});
        }
        FinishStep(0, std::to_string(targets.size()) + " injected games");
        log.KV("targets", std::to_string(targets.size()));

        if (targets.empty()) {
            SkipStep(1, "nothing to remove");
            SkipStep(2, "nothing to remove");
            SetSummary("No romm-nx-injected games to remove.");
            {
                std::lock_guard<std::mutex> lock(mutex);
                state = NsoPipelineState::Success;
            }
            log.EndSession(true, "nothing to remove");
            return;
        }

        size_t removed = 0;
        std::string last_error;
        BeginStep(1);
        for (size_t i = 0; i < targets.size(); ++i) {
            UpdateStep(1, std::to_string(i + 1) + " / " + std::to_string(targets.size())
                          + " - " + targets[i].title);
            std::string error;
            if (RemoveInjectedEntry(platform, targets[i].hash, targets[i].code, targets[i].title, error)) {
                removed++;
                log.Line("removed " + targets[i].code + " (" + targets[i].title + ")");
            } else {
                last_error = error;
                // Each removal is independently backed up and written
                // atomically, so one failure does not poison the rest — keep
                // going and report the tally, rather than stopping half done.
                log.Line("FAILED " + targets[i].code + " (" + targets[i].title + "): " + error);
            }
        }

        const size_t failed = targets.size() - removed;
        const std::string tally = std::to_string(removed) + " removed, " + std::to_string(failed) + " failed";
        if (failed > 0) {
            FailStep(1, NsoErrorKind::FileWrite, tally + " (last: " + last_error + ")");
        } else {
            FinishStep(1, tally);
        }

        BeginStep(2);
        RefreshDetection(platform);
        const auto detection = GetDetection(platform);
        FinishStep(2, std::to_string(detection.entry_count) + " entries, "
                      + std::to_string(detection.injected_asset_dirs) + " asset folders left");

        if (failed > 0) {
            SetSummary("Removed " + std::to_string(removed) + " of " + std::to_string(targets.size())
                       + " injected games; " + std::to_string(failed) + " could not be removed.");
            log.EndSession(false, tally);
        } else {
            {
                std::lock_guard<std::mutex> lock(mutex);
                state = NsoPipelineState::Success;
            }
            SetSummary("Removed all " + std::to_string(removed) + " romm-nx-injected games from " + profile.name + ".");
            log.EndSession(true, tally);
        }
    }

    void NsoSnesInstaller::StartRestore(NsoPlatform platform) {
        bool expected = false;
        if (!busy.compare_exchange_strong(expected, true)) return;
        ResetSteps({});
        NsoInstallRequest job_request;
        job_request.platform = platform;
        if (!SpawnWorker(NsoJobKind::Restore, job_request)) {
            ResetSteps({"Start pipeline"});
            FailStep(0, NsoErrorKind::FileWrite, "could not start the worker thread");
            busy.store(false);
        }
    }

    void NsoSnesInstaller::RunInstall(NsoInstallRequest request) {
        auto& log = NsoLog::Instance();
        auto& config = romm::model::ConfigManager::Instance();
        const NsoProfile profile = MakeProfile(request.platform);
        const bool is_nes = (request.platform == NsoPlatform::Nes);
        const bool is_gb = (request.platform == NsoPlatform::GameBoy);
        const bool is_gba = (request.platform == NsoPlatform::Gba);
        const bool is_n64 = (request.platform == NsoPlatform::N64);
        // Set from the cartridge header once the ROM is analysed. It decides the
        // code prefix, the ROM extension and the entry's "platform" field, so it
        // cannot be known before step 5. The whole parse is kept, not just the
        // mode: step 8 builds the code from the global checksum in it.
        GbMode gb_mode = GbMode::Dmg;
        GbRomInfo gb_info;
        GbaRomInfo gba_info;
        N64RomInfo n64_info;

        ResetSteps({
            std::string("Detect ") + profile.name + " installation",   // 0
            "Prepare staging area",              // 1
            "Fetch RomM metadata",               // 2
            "Download ROM",                      // 3
            "Download cover",                    // 4
            std::string("Analyse ") + profile.rom_kind,                // 5
            profile.container_rom ? "Generate .sfrom" : "Prepare ROM file", // 6
            "Convert cover images",              // 7
            "Build database entry",              // 8
            "Validate staged database",          // 9
            "Back up active files",              // 10
            "Install title assets",              // 11
            "Patch localized strings",           // 12
            "Replace database",                  // 13
            "Verify installation"                // 14
        });

        log.BeginSession(std::string(profile.name) + " injection for \"" + request.title + "\" (rom_id " +
                         std::to_string(request.rom_id) + ")");

        // --- 0. Detection -------------------------------------------------
        BeginStep(0);
        NsoSnesInstall install = DetectFor(request.platform);
        {
            std::lock_guard<std::mutex> lock(mutex);
            detection[PlatformSlot(request.platform)] = install;
        }
        if (!install.found) {
            FailStep(0, NsoErrorKind::NotDetected,
                     install.error.empty() ? (std::string("no ") + profile.name + " LayeredFS found")
                                           : install.error);
            log.EndSession(false, "detection failed");
            return;
        }
        log.KV("nso.title_id", install.title_id);
        log.KV("nso.content_root", install.content_root);
        log.KV("nso.database", install.database_path);
        // Only SNES has a signature check; logging "absent" for the others
        // would read as a problem in a log written to diagnose problems.
        if (request.platform == NsoPlatform::Snes) {
            log.KV("nso.exefs_mod", install.has_exefs_mod ? "present" : "absent");
        }
        log.KV("nso.entries_before", std::to_string(install.entry_count));
        for (size_t i = 0; i < install.strings_files.size(); ++i) {
            log.KV("nso.strings[" + install.strings_languages[i] + "]", install.strings_files[i]);
        }
        FinishStep(0, install.title_id + " - " + std::to_string(install.entry_count) + " entries");

        // --- 1. Staging ---------------------------------------------------
        BeginStep(1);
        log.Line("clearing staging directory");
        RemoveDirectoryTree(profile.staging);
        log.Line("creating staging directory");
        EnsureDir(profile.staging);
        if (!IsDirectory(profile.staging)) {
            FailStep(1, ClassifyWriteFailure(), "cannot create " + profile.staging);
            log.EndSession(false, "staging unavailable");
            return;
        }
        log.KV("staging.dir", profile.staging);
        // Created whether or not it is used: an empty folder on the card is how
        // a user discovers that dropping a community .dtz there is an option.
        if (is_n64) EnsureDir(kN64MetaPackDir);
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
        const std::string staged_rom = profile.staging + "/source.rom";
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
        const std::string staged_cover = profile.staging + "/source_cover.img";
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
                     std::string("no cover art available; ") + profile.name +
                     " requires a cover image for every entry");
            log.EndSession(false, "no cover");
            return;
        }

        // --- 5/6. ROM analysis and container -------------------------------
        //
        // The two platforms diverge here and nowhere else in the pipeline. SNES
        // wraps the ROM in a .sfrom plus a zeroed .sfromsig; NES installs the
        // .nes byte for byte, header included, and has no signature file at all.
        // `rom_sha256` is what both paths feed to the code allocator and the
        // injected-games index.
        std::string rom_sha256;
        std::string rom_internal_title; // SNES cartridge header title, if any
        std::string staged_rom_file;   // what lands as <CODE>.sfrom / <CODE>.nes
        std::string staged_sfromsig;   // SNES only; empty for NES
        std::string staged_metapack;   // N64 only: the <CODE>.dtz the app boots from
        bool metapack_from_user = false; // a community pack, rather than generated
        bool n64_metapack_pal = false;   // what the generated pack ended up with,
        size_t n64_metapack_idle = 0;    // for the success page

        if (is_n64) {
            // The only platform whose ROM is neither copied nor wrapped but
            // rewritten: byte-order converted if needed, then zlib-compressed,
            // streamed rather than buffered because these run to 64 MiB.
            BeginStep(5);
            log.Line("reading the Nintendo 64 ROM header");
            n64_info = InspectN64RomFile(staged_rom);
            if (!n64_info.valid) {
                const NsoErrorKind kind = n64_info.unsupported ? NsoErrorKind::UnsupportedRom
                                                               : NsoErrorKind::InvalidRom;
                FailStep(5, kind,
                         "Invalid Nintendo 64 ROM." + std::string(1, char(10)) + n64_info.error +
                         std::string(1, char(10)) + "No Nintendo 64 Online files were modified.");
                log.EndSession(false, "ROM rejected");
                return;
            }
            log.KV("rom.format", N64FormatName(n64_info.source_format));
            log.KV("rom.name", n64_info.internal_name);
            log.KV("rom.cartridge_id", n64_info.cartridge_id);
            log.KV("rom.country", std::string(1, n64_info.country_code) + " (" +
                                   N64CountryName(n64_info.country_code) + ")");
            log.KV("rom.crc1", N64Crc1Hex(n64_info.crc1));
            for (const auto& warning : n64_info.warnings) log.Line("warning: " + warning);
            // Not a fault in the dump, and not something the app minds — but the
            // MetaPack an N64 title needs to boot is published per ROM, and
            // almost every one that exists is for the US build.
            if (!IsN64UsRegion(n64_info.country_code)) {
                log.Line("warning: this is a " + std::string(N64CountryName(n64_info.country_code)) +
                         " dump; MetaPacks are published mostly for US ROMs");
            }
            {
                std::string detail = std::string(N64FormatName(n64_info.source_format)) + ", " +
                                     HumanBytes((long long)n64_info.file_size);
                if (!n64_info.internal_name.empty()) detail += ", " + n64_info.internal_name;
                detail += ", " + std::string(N64CountryName(n64_info.country_code));
                if (!IsN64UsRegion(n64_info.country_code)) detail += " - few MetaPacks exist";
                FinishStep(5, detail);
            }

            BeginStep(6);
            UpdateStep(6, "converting and compressing");
            staged_rom_file = profile.staging + "/title.bnz";
            std::string convert_error;
            if (!ConvertN64RomToBnz(staged_rom, staged_rom_file, n64_info.source_format,
                                    rom_sha256, convert_error)) {
                FailStep(6, ClassifyWriteFailure(), convert_error);
                log.EndSession(false, "rom conversion failed");
                return;
            }
            rom_internal_title = n64_info.internal_name;
            log.KV("rom.sha256", rom_sha256);

            // --- the MetaPack ------------------------------------------------
            // The one file that decides whether any of the above matters. A
            // community pack is always preferred: it carries a real idle
            // address, the cartridge's actual save type and any per-game ROM
            // patches, none of which can be derived from the dump.
            UpdateStep(6, "preparing the MetaPack");
            staged_metapack = profile.staging + "/title.dtz";
            std::string matched_by;
            const std::string user_pack =
                FindUserN64MetaPack(request.rom_filename, request.title, n64_info.crc1, matched_by);

            if (!user_pack.empty()) {
                std::string pack_bytes;
                std::vector<std::string> members;
                std::string pack_error;
                if (!ReadFileText(user_pack, pack_bytes)) {
                    pack_error = "could not read it";
                } else if (!VerifyN64MetaPack(pack_bytes, members, pack_error)) {
                    // Leave pack_error as VerifyN64MetaPack set it.
                } else {
                    std::string copy_error;
                    if (CopyFile(user_pack, staged_metapack, copy_error)) {
                        metapack_from_user = true;
                        log.KV("metapack.source", user_pack + " (matched by " + matched_by + ")");
                        log.KV("metapack.members", std::to_string(members.size()));
                    } else {
                        pack_error = copy_error;
                    }
                }
                if (!metapack_from_user) {
                    // Not fatal: a generated pack still goes in below, and the
                    // install is no worse off than before MetaPacks existed.
                    log.Line("warning: ignoring " + user_pack + " - " + pack_error);
                }
            } else {
                log.Line("no community MetaPack in " + std::string(kN64MetaPackDir) +
                         " for this game (looked for " + N64Crc1Hex(n64_info.crc1) + ".dtz)");
            }

            if (!metapack_from_user) {
                N64MetaPackOptions options;
                // 50 Hz builds need this or they get NTSC video timing. It is
                // the one field that genuinely differs between a US and a
                // European pack.
                options.pal = IsN64PalRegion(n64_info.country_code);

                // Only loops that cannot be left except by an interrupt, so an
                // entry here is derived rather than guessed. Often empty, which
                // is the correct answer when nothing qualifies.
                for (const N64IdleLoop& loop :
                         ScanN64BootIdle(staged_rom, n64_info.source_format, n64_info.entry_point)) {
                    options.idle.push_back({loop.addr, loop.inst});
                }

                std::string pack_error;
                if (!WriteN64MetaPack(staged_metapack, options, pack_error)) {
                    FailStep(6, ClassifyWriteFailure(), pack_error);
                    log.EndSession(false, "metapack generation failed");
                    return;
                }
                n64_metapack_pal = options.pal;
                n64_metapack_idle = options.idle.size();
                std::string detail = "generated";
                detail += options.pal ? ", PAL" : ", NTSC";
                detail += ", " + std::to_string(options.idle.size()) + " idle entr" +
                          (options.idle.size() == 1 ? "y" : "ies");
                detail += ", " + options.backup_type + " save";
                log.KV("metapack.source", detail);
                for (const auto& entry : options.idle) {
                    log.KV("metapack.idle", N64Crc1Hex(entry.jmp_addr) + " / " + N64Crc1Hex(entry.jmp_inst));
                }
            }
            log.KV("metapack.bytes", HumanBytes(FileSize(staged_metapack)));

            {
                const long long packed = FileSize(staged_rom_file);
                const long long source = (long long)n64_info.file_size;
                const int percent = source > 0 ? (int)((packed * 100) / source) : 0;
                log.KV("rom.bnz", HumanBytes(packed) + " (" + std::to_string(percent) + "% of the ROM)");
                FinishStep(6, HumanBytes(packed) + ", zlib " + std::to_string(percent) + "%" +
                              (n64_info.converted ? ", byte order converted" : "") +
                              (metapack_from_user ? ", community MetaPack" : ", generated MetaPack"));
            }
        } else if (is_gba) {
            BeginStep(5);
            log.Line("reading and analysing the Game Boy Advance ROM image");
            std::vector<uint8_t> rom_bytes;
            if (!ReadFileBytes(staged_rom, rom_bytes)) {
                FailStep(5, NsoErrorKind::InvalidRom, "could not read the downloaded ROM");
                log.EndSession(false, "ROM unreadable");
                return;
            }
            gba_info = AnalyzeGbaRom(rom_bytes);
            if (!gba_info.valid) {
                const NsoErrorKind kind = gba_info.unsupported ? NsoErrorKind::UnsupportedRom
                                                               : NsoErrorKind::InvalidRom;
                FailStep(5, kind,
                         "Invalid Game Boy Advance ROM." + std::string(1, char(10)) + gba_info.error +
                         std::string(1, char(10)) + "No Game Boy Advance Online files were modified.");
                log.EndSession(false, "ROM rejected");
                return;
            }
            rom_sha256 = gba_info.sha256;
            rom_internal_title = gba_info.internal_title;
            log.KV("rom.title", gba_info.internal_title);
            log.KV("rom.game_code", gba_info.game_code);
            log.KV("rom.maker", gba_info.maker_code);
            log.KV("rom.sha256", gba_info.sha256);
            for (const auto& warning : gba_info.warnings) log.Line("warning: " + warning);
            {
                std::string detail = gba_info.game_code + ", " + HumanBytes((long long)gba_info.file_size);
                if (!gba_info.warnings.empty()) {
                    detail += " - " + gba_info.warnings.front();
                    if (gba_info.warnings.size() > 1) {
                        detail += " (+" + std::to_string(gba_info.warnings.size() - 1) + " more, see log)";
                    }
                }
                FinishStep(5, detail);
            }

            BeginStep(6);
            staged_rom_file = profile.staging + "/title.gba";
            std::string copy_error;
            if (!CopyFile(staged_rom, staged_rom_file, copy_error)) {
                FailStep(6, ClassifyWriteFailure(), copy_error);
                log.EndSession(false, "ROM staging failed");
                return;
            }
            FinishStep(6, HumanBytes(FileSize(staged_rom_file)));
        } else if (is_gb) {
            BeginStep(5);
            log.Line("reading and analysing the Game Boy ROM image");
            std::vector<uint8_t> rom_bytes;
            if (!ReadFileBytes(staged_rom, rom_bytes)) {
                FailStep(5, NsoErrorKind::InvalidRom, "could not read the downloaded ROM");
                log.EndSession(false, "ROM unreadable");
                return;
            }
            const GbRomInfo gb = AnalyzeGbRom(rom_bytes);
            if (!gb.valid) {
                const NsoErrorKind kind = gb.unsupported ? NsoErrorKind::UnsupportedRom
                                                         : NsoErrorKind::InvalidRom;
                FailStep(5, kind,
                         "Invalid Game Boy ROM.\n" + gb.error +
                         "\nNo Game Boy Online files were modified.");
                log.EndSession(false, "ROM rejected");
                return;
            }
            gb_mode = gb.mode;
            gb_info = gb;
            rom_sha256 = gb.sha256;
            rom_internal_title = gb.internal_title;
            log.KV("rom.mode", gb_mode == GbMode::Cgb ? "CGB (Game Boy Color)" : "DMG (Game Boy)");
            log.KV("rom.title", gb.internal_title);
            log.KV("rom.cartridge", GbCartridgeTypeName(gb.cartridge_type).empty()
                                        ? "unknown"
                                        : GbCartridgeTypeName(gb.cartridge_type));
            log.KV("rom.global_checksum", GbChecksumHex(gb.global_checksum));
            log.KV("rom.sha256", gb.sha256);
            for (const auto& warning : gb.warnings) log.Line("warning: " + warning);
            {
                std::string detail = (gb_mode == GbMode::Cgb ? "Game Boy Color, " : "Game Boy, ") +
                                     HumanBytes((long long)gb.file_size);
                if (!gb.warnings.empty()) {
                    detail += " - " + gb.warnings.front();
                    if (gb.warnings.size() > 1) {
                        detail += " (+" + std::to_string(gb.warnings.size() - 1) + " more, see log)";
                    }
                }
                FinishStep(5, detail);
            }

            BeginStep(6);
            staged_rom_file = profile.staging + "/title" + GbRomSuffix(gb_mode);
            std::string copy_error;
            if (!CopyFile(staged_rom, staged_rom_file, copy_error)) {
                FailStep(6, ClassifyWriteFailure(), copy_error);
                log.EndSession(false, "ROM staging failed");
                return;
            }
            FinishStep(6, HumanBytes(FileSize(staged_rom_file)) + ", " +
                          (gb_mode == GbMode::Cgb ? ".gbc" : ".gb"));
        } else if (is_nes) {
            BeginStep(5);
            log.Line("reading and analysing the NES ROM image");
            std::vector<uint8_t> rom_bytes;
            if (!ReadFileBytes(staged_rom, rom_bytes)) {
                FailStep(5, NsoErrorKind::InvalidRom, "could not read the downloaded ROM");
                log.EndSession(false, "ROM unreadable");
                return;
            }
            const NesRomInfo nes = AnalyzeNesRom(rom_bytes);
            if (!nes.valid) {
                const NsoErrorKind kind = nes.unsupported ? NsoErrorKind::UnsupportedRom
                                                          : NsoErrorKind::InvalidRom;
                FailStep(5, kind,
                         (nes.unsupported ? "Unsupported NES image.\n"
                                          : "Invalid NES ROM.\n") +
                         nes.error + "\nNo NES Online files were modified.");
                log.EndSession(false, "ROM rejected");
                return;
            }
            rom_sha256 = nes.sha256;
            log.KV("rom.container", NesContainerName(nes.container));
            log.KV("rom.mapper", std::to_string(nes.mapper) +
                                 (NesMapperName(nes.mapper).empty() ? "" : " (" + NesMapperName(nes.mapper) + ")"));
            log.KV("rom.prg", HumanBytes((long long)nes.prg_size));
            log.KV("rom.chr", nes.chr_ram ? "CHR RAM" : HumanBytes((long long)nes.chr_size));
            log.KV("rom.sha256", nes.sha256);
            for (const auto& warning : nes.warnings) log.Line("warning: " + warning);
            {
                std::string detail = NesContainerName(nes.container) + ", mapper " +
                                     std::to_string(nes.mapper) + ", " + HumanBytes((long long)nes.file_size);
                if (!nes.warnings.empty()) {
                    detail += " - " + nes.warnings.front();
                    if (nes.warnings.size() > 1) {
                        detail += " (+" + std::to_string(nes.warnings.size() - 1) + " more, see log)";
                    }
                }
                FinishStep(5, detail);
            }

            // "Generation" for NES is a copy into staging under the name the
            // installer expects. Deliberately still a step of its own: it is
            // where a full SD card shows up, and staging the file means the
            // install phase copies from a verified local source rather than
            // from the download directory.
            BeginStep(6);
            staged_rom_file = profile.staging + "/title.nes";
            std::string copy_error;
            if (!CopyFile(staged_rom, staged_rom_file, copy_error)) {
                FailStep(6, ClassifyWriteFailure(), copy_error);
                log.EndSession(false, "ROM staging failed");
                return;
            }
            FinishStep(6, HumanBytes(FileSize(staged_rom_file)) + ", iNES header kept");
        } else {
        BeginStep(5);
        const std::string staged_sfrom = profile.staging + "/title.sfrom";
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
        staged_sfromsig = profile.staging + "/title.sfromsig";
        std::string sig_error;
        if (!WriteSfromSig(staged_sfromsig, sig_error)) {
            FailStep(6, ClassifyWriteFailure(), sig_error);
            log.EndSession(false, "sfromsig write failed");
            return;
        }
        rom_sha256 = conversion.rom.sha256;
        rom_internal_title = conversion.rom.internal_title;
        staged_rom_file = staged_sfrom;
        FinishStep(6, HumanBytes((long long)conversion.totalBytes) + ", footer " + conversion.footerHex);
        } // end SNES container branch

        // --- 7. Images -----------------------------------------------------
        BeginStep(7);
        const std::string staged_cover_png = profile.staging + "/title.png";
        const std::string staged_details_png = profile.staging + "/title-details.png";
        // SNES fits the art into a fixed 512x374 box; NES fixes the height at
        // 512 and lets the width follow the source aspect. Both then render the
        // same 400x300 details screen.
        log.Line(profile.container_rom ? "decoding cover and encoding the 512x374 PNG"
                                       : "decoding cover and encoding the 512-tall PNG");
        // N64 covers are 512x374, the same box SNES uses; measured on CaVE's
        // own output for an injected title.
        NsoImageResult cover_result =
            (is_gb || is_gba) ? ConvertCoverGb(staged_cover, staged_cover_png)  :
            is_nes            ? ConvertCoverNes(staged_cover, staged_cover_png) :
                                ConvertCover(staged_cover, staged_cover_png);
        if (!cover_result.success) {
            FailStep(7, NsoErrorKind::ImageConversion, cover_result.error);
            log.EndSession(false, "cover conversion failed");
            return;
        }
        // Game Boy's details screen is 1069x802, not the 400x300 the SNES and
        // NES apps use.
        log.Line(is_gb ? "encoding the 1069x802 details PNG" : "encoding the 400x300 details PNG");
        NsoImageResult details_result = is_gb ? ConvertDetailsGb(staged_cover, staged_details_png)
                                              : ConvertDetails(staged_cover, staged_details_png);
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
                       " -> " + std::to_string(cover_result.output_width) + "x" +
                       std::to_string(cover_result.output_height) + " + " +
                       std::to_string(details_result.output_width) + "x" +
                       std::to_string(details_result.output_height));

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

        auto injected = LoadInjectedIndex(profile.index);
        std::string preferred_code;
        {
            auto it = injected.find(rom_sha256);
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
        std::string code;
        if (is_n64) {
            code = AllocateN64GameCode(db, rom_sha256, preferred_code);
        } else if (is_gba) {
            // The cartridge's own four-character game code, as CaVE names them.
            code = AllocateGbaGameCode(db, gba_info, preferred_code);
        } else if (is_gb) {
            // Derived, not allocated: a Game Boy code is the cartridge's own
            // global checksum in hex, so it comes straight from the step-5
            // parse. Re-reading the file here instead would risk falling back to
            // a zeroed header — and D-0000_e — if the read failed.
            code = AllocateGbGameCode(db, gb_info, preferred_code);
        } else if (is_nes) {
            code = AllocateNesGameCode(db, rom_sha256, preferred_code);
        } else {
            code = AllocateGameCode(db, rom_sha256, preferred_code);
        }
        if (code.empty()) {
            FailStep(8, NsoErrorKind::DuplicateEntry,
                     is_n64 ? "no free N-#### code left in the database" :
                     is_gba ? "no free code left near this ROM's game code in the database" :
                     is_gb  ? "no free code left near this ROM's checksum in the database" :
                     is_nes ? "no free CLV-P-NZ__E code left in the database"
                            : "no free S-#### code left in the database");
            log.EndSession(false, "code allocation failed");
            return;
        }

        NsoTitleMeta meta = is_n64 ? N64DefaultTitleMeta() :
                            is_gba ? GbaDefaultTitleMeta() :
                            is_gb  ? GbDefaultTitleMeta()  :
                            is_nes ? NesDefaultTitleMeta() : NsoTitleMeta();
        meta.code = code;
        // SNES can fall back to the cartridge header's internal title; an iNES
        // header carries no title at all, so NES has only what RomM supplied.
        // SNES and Game Boy both carry a title in the cartridge header; an iNES
        // header does not.
        meta.title = !request.title.empty() ? request.title
                     : (!rom_internal_title.empty() ? rom_internal_title : std::string("Unknown"));
        meta.sort_title = MakeSortKey(meta.title);
        meta.publisher = publisher.empty() ? "Unknown" : publisher;
        meta.sort_publisher = MakeSortKey(meta.publisher);
        meta.copyright = publisher.empty() ? "" : ("\xC2\xA9 " + publisher);
        meta.release_date = release_date.empty() ? TodayIso() : release_date;
        meta.lcla6_release_date = TodayIso();
        meta.players_count = players_count;
        meta.simultaneous = players_count > 1;
        if (!is_nes && !is_gb && !is_gba && !is_n64) {
            meta.save_count = 1;
            meta.volume = 100;
        }
        // NES keeps NesDefaultTitleMeta()'s save_count 0 / volume 80, which is
        // what CaVE writes for a ROM outside the stock catalogue.

        const std::string entry_json =
            is_n64 ? BuildN64TitleEntryJson(meta, NsoN64Extras{})       :
            is_gba ? BuildGbaTitleEntryJson(meta, NsoGbaExtras{})       :
            is_gb  ? BuildGbTitleEntryJson(meta, gb_mode, NsoGbExtras{}) :
            is_nes ? BuildNesTitleEntryJson(meta, NsoNesExtras{})        :
                     BuildTitleEntryJson(meta);
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
        const std::string staged_db = profile.staging + "/lclassics.titlesdb";
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
        const bool staged_db_ok =
            is_n64 ? VerifySerializedN64Db(reread, db.codes, code, verify_error)         :
            is_gba ? VerifySerializedGbaDb(reread, db.codes, code, verify_error)         :
            is_gb  ? VerifySerializedGbDb(reread, db.codes, code, gb_mode, verify_error) :
            is_nes ? VerifySerializedNesDb(reread, db.codes, code, verify_error)         :
                     VerifySerializedDb(reread, db.codes, code, verify_error);
        if (!staged_db_ok) {
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
            if (!PatchStringsFile(original, code, wrapped_description,
                                  GuideKeysFor(request.platform),
                                  patched, changed, strings_error)) {
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
        const std::string backup_dir = profile.backups + "/" + NowStamp(true);
        EnsureDir(backup_dir);
        if (!IsDirectory(backup_dir)) {
            FailStep(10, ClassifyWriteFailure(), "cannot create " + backup_dir);
            log.EndSession(false, "backup directory unavailable");
            return;
        }

        const std::string title_dir = install.titles_dir + "/" + code;
        const bool title_dir_existed = IsDirectory(title_dir);

        struct Artefact { std::string staged; std::string target; };
        std::vector<Artefact> artefacts;
        if (is_n64) {
            // The compressed ROM lands as "<CODE>.bnz" while the entry above
            // names "<CODE>.bin" — CaVE's arrangement, and what the app reads.
            // The MetaPack sits beside them under the title's own code; nothing
            // in the database entry points at it, so the name is the only thing
            // tying it to the game.
            artefacts = {
                {staged_rom_file,    title_dir + "/" + code + ".bnz"},
                {staged_metapack,    title_dir + "/" + code + kN64MetaPackSuffix},
                {staged_cover_png,   title_dir + "/" + code + ".png"},
                {staged_details_png, title_dir + "/" + code + "-details.png"}
            };
        } else if (is_gba) {
            artefacts = {
                {staged_rom_file,    title_dir + "/" + code + ".gba"},
                {staged_cover_png,   title_dir + "/" + code + ".png"},
                {staged_details_png, title_dir + "/" + code + "-details.png"}
            };
        } else if (is_gb) {
            artefacts = {
                {staged_rom_file,    title_dir + "/" + code + GbRomSuffix(gb_mode)},
                {staged_cover_png,   title_dir + "/" + code + ".png"},
                {staged_details_png, title_dir + "/" + code + "-details.png"}
            };
        } else if (is_nes) {
            // Three files, and the details screen is "<CODE>00.png" here rather
            // than SNES's "<CODE>-details.png".
            artefacts = {
                {staged_rom_file,    title_dir + "/" + code + ".nes"},
                {staged_cover_png,   title_dir + "/" + code + ".png"},
                {staged_details_png, title_dir + "/" + code + "00.png"}
            };
        } else {
            artefacts = {
                {staged_rom_file,    title_dir + "/" + code + ".sfrom"},
                {staged_sfromsig,    title_dir + "/" + code + ".sfromsig"},
                {staged_cover_png,   title_dir + "/" + code + ".png"},
                {staged_details_png, title_dir + "/" + code + "-details.png"}
            };
        }

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
            manifest += "sha256=" + rom_sha256 + "\n";
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
            const bool final_db_ok =
                is_n64 ? VerifySerializedN64Db(final_db.text, db.codes, code, final_error)         :
                is_gba ? VerifySerializedGbaDb(final_db.text, db.codes, code, final_error)         :
                is_gb  ? VerifySerializedGbDb(final_db.text, db.codes, code, gb_mode, final_error) :
                is_nes ? VerifySerializedNesDb(final_db.text, db.codes, code, final_error)         :
                         VerifySerializedDb(final_db.text, db.codes, code, final_error);
            if (!final_db_ok) {
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
            auto index = LoadInjectedIndex(profile.index);
            index[rom_sha256] = InjectedEntry{code, meta.title, request.rom_id};
            SaveInjectedIndex(profile.root, profile.index, index);
        }

        RemoveDirectoryTree(profile.staging);

        {
            std::lock_guard<std::mutex> lock(mutex);
            state = NsoPipelineState::Success;
            summary = meta.title + " installed as " + code + ".\nReboot or relaunch " +
                      profile.name + " to see it.";
            if (request.platform == NsoPlatform::N64) {
                // Everything above can succeed and the game still not start:
                // the N64 app boots each title from its MetaPack (.dtz). Saying
                // which one went in is the difference between "romm-nx is
                // broken" and "this game needs a community pack".
                if (metapack_from_user) {
                    summary += "\n\nInstalled with the community MetaPack found in " +
                               std::string(kN64MetaPackDir) + ".";
                } else {
                    summary += "\n\nromm-nx generated a MetaPack for this title (";
                    summary += n64_metapack_pal ? "PAL timing" : "NTSC timing";
                    summary += ", " + std::to_string(n64_metapack_idle) + " idle entr" +
                               (n64_metapack_idle == 1 ? "y" : "ies") + ").";
                    if (n64_metapack_idle == 0) {
                        // The one field that has been seen to decide whether a
                        // title runs. Nothing in this ROM's boot segment
                        // qualified, so say so rather than let a black screen
                        // look unexplained.
                        summary += "\nNo idle loop could be derived from this ROM. If the game does "
                                   "not start, a community .dtz for it in " +
                                   std::string(kN64MetaPackDir) + " is the fix - name it after the "
                                   "ROM, the title, or its CRC1 " + N64Crc1Hex(n64_info.crc1) + ".";
                    }
                }
            }
        }
        log.EndSession(true, meta.title + " -> " + code);
        RefreshDetection(request.platform);
    }

    void NsoSnesInstaller::RunRestore(NsoPlatform platform) {
        auto& log = NsoLog::Instance();

        ResetSteps({
            "Locate latest backup",
            "Read backup manifest",
            "Remove files created by the installation",
            "Restore backed-up files",
            "Validate restored database"
        });

        const NsoProfile profile = MakeProfile(platform);
        log.BeginSession(std::string(profile.name) + " backup restore");

        BeginStep(0);
        const std::string backup_dir = LatestBackupPath(platform);
        if (backup_dir.empty()) {
            FailStep(0, NsoErrorKind::NotDetected, "no backup found under " + profile.backups);
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
            auto index = LoadInjectedIndex(profile.index);
            index.erase(sha256);
            SaveInjectedIndex(profile.root, profile.index, index);
        }

        {
            std::lock_guard<std::mutex> lock(mutex);
            state = NsoPipelineState::Success;
            summary = std::string("Restored the ") + profile.name + " backup from " + backup_dir + ".";
        }
        log.EndSession(true, "restored " + backup_dir);
        RefreshDetection(platform);
    }

}
