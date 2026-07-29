#include "I18n.hpp"
#include "../model/ConfigManager.hpp"
#include "../model/JsonUtil.hpp"

#include <switch.h>

#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <set>

namespace romm::i18n {

    namespace {

        // tr() is called from the render thread and, for download error
        // strings, from DownloadManager's worker. One mutex over the whole
        // state is plenty: lookups are map reads and the language changes at
        // most once per user interaction.
        std::mutex g_mutex;

        std::map<std::string, std::string> g_active;   // language in effect
        std::map<std::string, std::string> g_fallback; // always English
        std::set<std::string> g_logged_missing;        // warn once per key

        Language g_language = Language::English;
        std::string g_setting = kSettingAuto;
        uint64_t g_generation = 1;
        bool g_fallback_loaded = false;

        const char* LanguageName(Language lang) {
            return (lang == Language::French) ? "French" : "English";
        }

        const char* LanguageCode(Language lang) {
            return (lang == Language::French) ? kSettingFrench : kSettingEnglish;
        }

        bool ReadFile(const std::string& path, std::string& out) {
            FILE* f = std::fopen(path.c_str(), "rb");
            if (!f) {
                return false;
            }
            out.clear();
            char buf[1024];
            while (size_t read_bytes = std::fread(buf, 1, sizeof(buf), f)) {
                out.append(buf, read_bytes);
            }
            std::fclose(f);
            return true;
        }

        // A missing or malformed dictionary leaves `out` empty rather than
        // aborting: English then answers every lookup, which is exactly the
        // documented degraded mode.
        bool LoadDictionary(Language lang, std::map<std::string, std::string>& out) {
            out.clear();
            const std::string path = std::string("romfs:/lang/") + LanguageCode(lang) + ".json";

            std::string content;
            if (!ReadFile(path, content)) {
                std::cerr << "[i18n] Language file not found: " << path << std::endl;
                return false;
            }
            if (!romm::model::jsonParseFlatStringMap(content, out)) {
                std::cerr << "[i18n] Malformed language file: " << path << std::endl;
                out.clear();
                return false;
            }
            std::cout << "[i18n] Loaded " << out.size() << " strings from " << path << std::endl;
            return true;
        }

        // Caller holds g_mutex.
        void ApplyLanguageLocked(Language lang) {
            if (!g_fallback_loaded) {
                LoadDictionary(Language::English, g_fallback);
                g_fallback_loaded = true;
            }

            if (lang == Language::English) {
                g_active.clear(); // every lookup falls through to g_fallback
            } else {
                LoadDictionary(lang, g_active);
            }

            g_language = lang;
            g_generation++;
            g_logged_missing.clear();
        }

        Language ResolveSetting(const std::string& setting) {
            if (setting == kSettingEnglish) return Language::English;
            if (setting == kSettingFrench) return Language::French;
            return DetectSystemLanguage();
        }

        std::string NormalizeSetting(const std::string& setting) {
            if (setting == kSettingAuto || setting == kSettingEnglish || setting == kSettingFrench) {
                return setting;
            }
            std::cerr << "[i18n] Unknown language setting \"" << setting
                      << "\", falling back to \"auto\"" << std::endl;
            return kSettingAuto;
        }

    } // namespace

    Language DetectSystemLanguage() {
        // setInitialize/setExit rather than holding the service open: this runs
        // twice at most (startup, and any switch back to "System default").
        if (R_FAILED(setInitialize())) {
            std::cerr << "[i18n] setInitialize failed, defaulting to English" << std::endl;
            return Language::English;
        }

        u64 language_code = 0;
        Result rc = setGetSystemLanguage(&language_code);
        if (R_FAILED(rc)) {
            std::cerr << "[i18n] setGetSystemLanguage failed (" << rc
                      << "), defaulting to English" << std::endl;
            setExit();
            return Language::English;
        }

        SetLanguage sys_language = SetLanguage_ENUS;
        rc = setMakeLanguage(language_code, &sys_language);
        setExit();

        if (R_FAILED(rc)) {
            std::cerr << "[i18n] setMakeLanguage failed (" << rc
                      << "), defaulting to English" << std::endl;
            return Language::English;
        }

        // Both French variants map to the single French dictionary; every other
        // system language is unsupported and gets English.
        if (sys_language == SetLanguage_FR || sys_language == SetLanguage_FRCA) {
            std::cout << "[i18n] System language detected: French" << std::endl;
            return Language::French;
        }

        std::cout << "[i18n] System language " << (int)sys_language
                  << " is not supported, using English" << std::endl;
        return Language::English;
    }

    void Initialize() {
        const std::string setting =
            NormalizeSetting(romm::model::ConfigManager::Instance().GetLanguage());
        const Language lang = ResolveSetting(setting);

        std::lock_guard<std::mutex> lock(g_mutex);
        g_setting = setting;
        ApplyLanguageLocked(lang);
        std::cout << "[i18n] Initialized: setting=" << g_setting
                  << " active=" << LanguageName(g_language) << std::endl;
    }

    void SetLanguageSetting(const std::string& setting) {
        const std::string normalized = NormalizeSetting(setting);
        // Resolved outside the lock: DetectSystemLanguage() talks to the `set`
        // service and has no business holding the dictionary mutex.
        const Language lang = ResolveSetting(normalized);

        std::lock_guard<std::mutex> lock(g_mutex);
        const bool language_changed = (lang != g_language);
        g_setting = normalized;
        if (!language_changed) {
            return; // e.g. "auto" -> "en" on an English console
        }
        ApplyLanguageLocked(lang);
        std::cout << "[i18n] Language changed: setting=" << g_setting
                  << " active=" << LanguageName(g_language) << std::endl;
    }

    const std::string& GetLanguageSetting() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_setting;
    }

    Language GetLanguage() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_language;
    }

    std::string GetLanguageCode() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return LanguageCode(g_language);
    }

    uint64_t Generation() {
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_generation;
    }

    std::string tr(const std::string& key) {
        if (key.empty()) {
            return key;
        }

        std::lock_guard<std::mutex> lock(g_mutex);

        auto it = g_active.find(key);
        if (it != g_active.end()) {
            return it->second;
        }

        auto fb = g_fallback.find(key);
        if (fb != g_fallback.end()) {
            if (g_language != Language::English && g_logged_missing.insert(key).second) {
                std::cerr << "[i18n] Missing " << LanguageName(g_language)
                          << " translation: " << key << std::endl;
            }
            return fb->second;
        }

        // Nothing anywhere. Returning the key keeps the UI readable and
        // debuggable instead of drawing an empty row.
        if (g_logged_missing.insert(key).second) {
            if (g_language != Language::English) {
                std::cerr << "[i18n] Missing " << LanguageName(g_language)
                          << " translation: " << key << std::endl;
            }
            std::cerr << "[i18n] Missing fallback translation: " << key << std::endl;
        }
        return key;
    }

    std::string format(const std::string& key, const Args& args) {
        const std::string tmpl = tr(key);
        if (args.empty()) {
            return tmpl;
        }

        // Single left-to-right pass so a substituted RomM value that happens to
        // contain "{something}" can never be re-substituted.
        std::string out;
        out.reserve(tmpl.size() + 32);

        size_t pos = 0;
        while (pos < tmpl.size()) {
            const size_t open = tmpl.find('{', pos);
            if (open == std::string::npos) {
                out.append(tmpl, pos, std::string::npos);
                break;
            }
            const size_t close = tmpl.find('}', open + 1);
            if (close == std::string::npos) {
                out.append(tmpl, pos, std::string::npos);
                break;
            }

            out.append(tmpl, pos, open - pos);
            const std::string name = tmpl.substr(open + 1, close - open - 1);

            bool replaced = false;
            for (const auto& arg : args) {
                if (arg.first == name) {
                    out += arg.second;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) {
                // Leave the placeholder visible rather than silently dropping
                // it — a blank where a game title should be is far harder to
                // spot than "{title}".
                out.append(tmpl, open, close - open + 1);
            }
            pos = close + 1;
        }

        return out;
    }

}
