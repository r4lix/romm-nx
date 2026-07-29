#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Centralized localization for romm-nx's OWN interface strings.
//
// Scope rule, deliberately narrow: nothing that comes back from the RomM
// server ever passes through here. Game titles, platform names, descriptions,
// genres, developers, publishers, regions, file names, sizes, URLs and every
// other API value are rendered exactly as received. Where such a value has to
// appear inside a sentence, the sentence is a template and the value is
// substituted by format() — see the {title}/{platform} placeholders in the
// dictionaries.
namespace romm::i18n {

    enum class Language {
        English,
        French
    };

    // Accepted values of the "language" key in config.json.
    inline constexpr const char* kSettingAuto = "auto";
    inline constexpr const char* kSettingEnglish = "en";
    inline constexpr const char* kSettingFrench = "fr";

    // Loads romfs:/lang/en.json (the canonical fallback dictionary) and, when
    // the active language isn't English, romfs:/lang/<code>.json on top of it.
    //
    // Must run after RomFS is mounted — Plutonium does that in
    // Renderer::Initialize(), i.e. before Application::OnLoad(). Safe to call
    // more than once; later calls just re-read the config setting.
    void Initialize();

    // "auto", "en" or "fr". An unknown value logs a warning and is treated as
    // "auto" rather than failing. Reloads dictionaries and bumps Generation()
    // if the effective language actually changed.
    void SetLanguageSetting(const std::string& setting);
    const std::string& GetLanguageSetting();

    // The language actually in effect (never "auto" — that's already resolved).
    Language GetLanguage();
    std::string GetLanguageCode();

    // Switch system language folded onto a supported one: French and Canadian
    // French give French, everything else (and any detection failure) English.
    Language DetectSystemLanguage();

    // Incremented every time the active language changes. Elements that
    // pre-render text into textures compare against this to know their cache is
    // stale without diffing every string.
    uint64_t Generation();

    // Active dictionary, then English, then the key itself. Never returns an
    // empty string for a non-empty key, and never fails: a missing key is a
    // logged warning, not an error.
    std::string tr(const std::string& key);

    using Args = std::vector<std::pair<std::string, std::string>>;

    // tr() followed by {name} placeholder substitution. Values are inserted
    // verbatim and are never themselves looked up — this is the only supported
    // way to put RomM data inside a localized sentence.
    std::string format(const std::string& key, const Args& args);

}
