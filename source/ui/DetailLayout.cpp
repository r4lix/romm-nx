#include "DetailLayout.hpp"
#include "PlaceholderCover.hpp"
#include "../model/DataModel.hpp"
#include "../model/DownloadManager.hpp"
#include "../navigation/NavigationManager.hpp"
#include "CoverProfile.hpp"
#include "CoverCache.hpp"
#include "UninstallConfirmModal.hpp"
#include "../model/RommApi.hpp"
#include "../model/ConfigManager.hpp"
#include <sys/stat.h>
#include "GlobalProgressBar.hpp"

namespace romm::ui {

    namespace {
        constexpr s32 TAB_Y_OFFSET = 170;
        constexpr s32 TAB_HEIGHT = 60;
        constexpr s32 TABS_TO_SECTION_GAP = 20; // 12-16px at 720p scaled by 1.5 -> ~20px
        constexpr s32 SECTION_TITLE_HEIGHT = 35; // height of Orbitron@37
        constexpr s32 SECTION_TITLE_TO_CONTENT_GAP = 15; // 8-12px at 720p scaled by 1.5 -> ~15px
    }


    static std::vector<std::string> WordWrapLinesPixel(const std::string& font_name, const std::string& text, s32 max_width_px) {
        std::vector<std::string> lines;
        std::string current_line;
        std::string word;
        
        for (char c : text) {
            if (c == ' ' || c == '\n') {
                std::string test_line = current_line;
                if (!test_line.empty()) {
                    test_line += " ";
                }
                test_line += word;
                
                s32 test_w = pu::ui::render::GetTextWidth(font_name, test_line);
                if (test_w > max_width_px && !current_line.empty()) {
                    lines.push_back(current_line);
                    current_line = word;
                } else {
                    current_line = test_line;
                }
                word.clear();
                
                if (c == '\n') {
                    lines.push_back(current_line);
                    current_line.clear();
                }
            } else {
                word += c;
            }
        }
        if (!word.empty()) {
            std::string test_line = current_line;
            if (!test_line.empty()) {
                test_line += " ";
            }
            test_line += word;
            s32 test_w = pu::ui::render::GetTextWidth(font_name, test_line);
            if (test_w > max_width_px && !current_line.empty()) {
                lines.push_back(current_line);
                lines.push_back(word);
            } else {
                current_line = test_line;
                lines.push_back(current_line);
            }
        } else if (!current_line.empty()) {
            lines.push_back(current_line);
        }
        return lines;
    }

    // --- DetailCard Implementation ---

    DetailCard::DetailCard(s32 x, s32 y, s32 w, s32 h, std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Element(), x(x), y(y), w(w), h(h), nav_mgr(nav) {
        title_tex = nullptr;
        title_scroll_x = 0.0f;
        title_wait_timer = 0.0f;
        title_scroll_state = TitleScrollState::WaitStart;
        InitTextures();
    }

    DetailCard::~DetailCard() {
        ClearTextures();
    }

    static uint64_t Fnv1aHash(const std::string& str) {
        uint64_t hash = 14695981039346656037ULL;
        for (char c : str) {
            hash ^= static_cast<uint8_t>(c);
            hash *= 1099511628211ULL;
        }
        return hash;
    }

    static std::string HashString(const std::string& str) {
        uint64_t hash_val = Fnv1aHash(str);
        char buf[32];
        sprintf(buf, "%016llx", (unsigned long long)hash_val);
        return std::string(buf);
    }

    void DetailCard::SetContext(const DetailGameContext& context) {
        ctx = context;

        current_cover_rom_id = -1;
        is_active = true;
        logged_complete_gen_val = 0;
        logged_apply_gen_val = 0;
        last_known_cache_state = CoverState::Unknown;

        // Try to pre-populate cover_tex with a cached grid texture immediately to avoid blanking
        std::string norm_slug = romm::model::NormalizePlatformSlug(ctx.platform_slug);
        
        // Fixed cover art viewport container (stable per platform aspect ratio)
        s32 cover_x = x + 40;
        s32 cover_y = y + 40;
        s32 cover_w = 430;
        s32 cover_h = 560;

        if (norm_slug == "nds") {
            actual_cover_w = 340;
            actual_cover_h = 308;
        } else if (norm_slug == "gb" || norm_slug == "gbc" || norm_slug == "gba" || ctx.is_ps1) {
            actual_cover_w = 340;
            actual_cover_h = 340;
        } else if (norm_slug == "psp") {
            actual_cover_w = 340;
            actual_cover_h = 560;
        } else {
            actual_cover_w = cover_w;
            actual_cover_h = cover_h;
        }
        actual_cover_x = cover_x + (cover_w - actual_cover_w) / 2;
        actual_cover_y = cover_y + (cover_h - actual_cover_h) / 2;

        // Populate cover profile for this platform
        auto nav = nav_mgr.lock();
        if (nav && nav->GetModel()) {
            size_t plat_idx = ctx.platform_idx;
            const auto& platforms = nav->GetModel()->GetPlatforms();
            if (plat_idx < platforms.size()) {
                currentCoverProfile = GetCoverProfile(platforms.at(plat_idx));
                cached_plat_idx = plat_idx;
            }
        }

        // Check if there is already a loaded texture in CoverCache for this ROM
        pu::sdl2::Texture initial_tex = nullptr;
        CoverCacheKey initial_key = {};
        DetailCoverState initial_cover_state = DetailCoverState::Placeholder;

        if (!ctx.cover_path_large.empty()) {
            auto large_res = CoverCache::Instance().GetOrRequest(ctx.rom_id, norm_slug, ctx.cover_path_large, currentCoverProfile.type, "big", false);
            if (large_res.state == CoverState::Ready && large_res.texture != nullptr) {
                initial_tex = large_res.texture;
                initial_key = large_res.key;
                initial_cover_state = DetailCoverState::UsingLarge;
            }
        }
        if (!initial_tex && !ctx.cover_path.empty()) {
            auto small_res = CoverCache::Instance().GetOrRequest(ctx.rom_id, norm_slug, ctx.cover_path, currentCoverProfile.type, "small", false);
            if (small_res.state == CoverState::Ready && small_res.texture != nullptr) {
                initial_tex = small_res.texture;
                initial_key = small_res.key;
                initial_cover_state = DetailCoverState::UsingSmallFallback;
            }
        }

        if (initial_tex) {
            cover_tex = initial_tex;
            displayed_key = initial_key;
            cover_state = initial_cover_state;
            display_state = DetailDisplayState::CoverFallback;
            std::cout << "[DETAIL] Reused cached grid texture immediately for ROM ID: " << ctx.rom_id << std::endl;
        } else {
            cover_tex = nullptr;
            displayed_key = {};
            cover_state = DetailCoverState::Placeholder;
            display_state = DetailDisplayState::LoadingPlaceholder;
        }

        request_state = DetailRequestState::Idle;

        title_scroll_x = 0.0f;
        title_wait_timer = 2.0f;
        title_scroll_state = TitleScrollState::WaitStart;
        last_title_scroll_update_time = std::chrono::steady_clock::time_point();
        cached_title = "";
        if (title_tex) {
            pu::ui::render::DeleteTexture(title_tex);
            title_tex = nullptr;
        }

        int req_w = 0, req_h = 0;
        GetVariantDimensions("small", req_w, req_h);

        expected_identity.generation = ctx.generation;
        expected_identity.cache_key.platform_slug = norm_slug;
        expected_identity.cache_key.rom_id = ctx.rom_id;
        expected_identity.cache_key.cover_source = ctx.cover_path;
        expected_identity.cache_key.variant = "small";
        expected_identity.cache_key.requested_width = req_w;
        expected_identity.cache_key.requested_height = req_h;

        std::cout << "[DETAIL_IMAGE_SOURCE_CHANGED]"
                  << " rom_id=" << ctx.rom_id
                  << " old_variant=none"
                  << " new_variant=small"
                  << " source_hash=" << HashString(ctx.cover_path) << std::endl;

        std::cout << "[DETAIL_IMAGE_REQUEST]"
                  << " rom_id=" << expected_identity.cache_key.rom_id
                  << " variant=" << expected_identity.cache_key.variant
                  << " size=" << expected_identity.cache_key.requested_width << "x" << expected_identity.cache_key.requested_height
                  << " source_hash=" << HashString(expected_identity.cache_key.cover_source) << std::endl;

        ResolveDetailImageState(true);
        ForceRefresh();
    }

    void DetailCard::OnLeave() {
        is_active = false;
        expected_identity.generation = 0;
        displayed_key = {};
        cover_tex = nullptr;
        cover_state = DetailCoverState::None;
        display_state = DetailDisplayState::Placeholder;
        request_state = DetailRequestState::Idle;
        current_cover_rom_id = -1;
    }

    FullscreenKeys DetailCard::GetFullscreenKeys() {
        FullscreenKeys keys;
        auto nav = nav_mgr.lock();
        if (!nav) return keys;
        
        auto model = nav->GetModel();
        if (!model) return keys;
        
        int rom_id = ctx.rom_id;
        const auto* detail = model->GetCachedDetail(rom_id);
        std::string norm_slug = romm::model::NormalizePlatformSlug(ctx.platform_slug);
        
        bool is_psp = (norm_slug == "psp");
        bool is_nds = (norm_slug == "nds" || norm_slug == "nintendo-ds" || norm_slug == "nintendo_ds" || norm_slug == "Nintendo DS");
        bool is_gb = (norm_slug == "gb" || norm_slug == "game-boy" || norm_slug == "gameboy" || norm_slug == "nintendo-game-boy");
        bool is_gbc = (norm_slug == "gbc" || norm_slug == "game-boy-color" || norm_slug == "gameboy-color" || norm_slug == "nintendo-game-boy-color");
        bool is_gba = (norm_slug == "gba" || norm_slug == "game-boy-advance" || norm_slug == "gameboy-advance" || norm_slug == "nintendo-game-boy-advance");
        bool is_gameboy_family = (is_gb || is_gbc || is_gba);
        bool is_ps1 = ctx.is_ps1;
        
        int big_w = 360, big_h = 480;
        if (is_psp) { big_w = 340; big_h = 560; }
        else if (is_nds) { big_w = 340; big_h = 308; }
        else if (is_gameboy_family || is_ps1) { big_w = 340; big_h = 340; }
        
        int small_w = 180, small_h = 270;
        if (is_nds || is_gameboy_family) { small_w = 380; small_h = 344; }
        else if (is_psp) { small_w = 247; small_h = 378; }
        else if (is_ps1) { small_w = 240; small_h = 240; }
        
        // 1. Miximage Key
        if (detail && !detail->miximage_v2_url.empty()) {
            keys.miximage_key.rom_id = rom_id;
            keys.miximage_key.platform_slug = norm_slug;
            keys.miximage_key.cover_source = detail->miximage_v2_url;
            keys.miximage_key.variant = "miximage_v2";
            keys.miximage_key.requested_width = big_w;
            keys.miximage_key.requested_height = big_h;
        }
        
        // 2. Large Cover Key
        std::string large_source = "";
        if (detail && !detail->path_cover_large.empty()) {
            large_source = detail->path_cover_large;
        } else if (!ctx.cover_path_large.empty()) {
            large_source = ctx.cover_path_large;
        }
        if (!large_source.empty()) {
            keys.large_key.rom_id = rom_id;
            keys.large_key.platform_slug = norm_slug;
            keys.large_key.cover_source = large_source;
            keys.large_key.variant = "big";
            keys.large_key.requested_width = big_w;
            keys.large_key.requested_height = big_h;
        }
        
        // 3. Small Cover Key
        if (!ctx.cover_path.empty()) {
            keys.small_key.rom_id = rom_id;
            keys.small_key.platform_slug = norm_slug;
            keys.small_key.cover_source = ctx.cover_path;
            keys.small_key.variant = "small";
            keys.small_key.requested_width = small_w;
            keys.small_key.requested_height = small_h;
        }
        
        return keys;
    }

    void DetailLayout::SetContext(const DetailGameContext& context) {
        static uint64_t global_generation = 0;
        global_generation++;

        ctx = context;
        ctx.generation = global_generation;
        ctx.cover_variant = "small";

        if (card) card->SetContext(ctx);
    }

    void DetailLayout::OnLeave() {
        ctx.generation = 0;
        if (card) card->OnLeave();
    }

    void DetailCard::InitTextures() {
        ClearTextures();
        pu::ui::Color text_color(237, 229, 251, 255); // #EDE5FB
        
        tex_btn_download = pu::ui::render::RenderText("Orbitron@30", "DOWNLOAD", text_color);
        tex_btn_preparing = pu::ui::render::RenderText("Orbitron@30", "PREPARING...", text_color);
        tex_btn_downloaded = pu::ui::render::RenderText("Orbitron@30", "DOWNLOADED", text_color);
        tex_btn_failed = pu::ui::render::RenderText("Orbitron@30", "RETRY", text_color);
        tex_btn_unsupported = pu::ui::render::RenderText("Orbitron@30", "UNSUPPORTED", text_color);
        
        tex_btn_uninstall = pu::ui::render::RenderText("Orbitron@30", "UNINSTALL", text_color);
        tex_btn_confirm_uninstall = pu::ui::render::RenderText("Orbitron@30", "CONFIRM? (A=Yes, B=No)", text_color);
        tex_btn_add_to_queue = pu::ui::render::RenderText("Orbitron@30", "ADD TO QUEUE", text_color);
        tex_btn_remove_from_queue = pu::ui::render::RenderText("Orbitron@30", "REMOVE FROM QUEUE", text_color);

        details_tex = pu::ui::render::RenderText("Orbitron@30", "DETAILS", text_color);
        save_data_tex = pu::ui::render::RenderText("Orbitron@30", "SAVE DATA", text_color);
        mods_tex = pu::ui::render::RenderText("Orbitron@30", "MODS", text_color);
        cheats_tex = pu::ui::render::RenderText("Orbitron@30", "CHEATS", text_color);

        cover_placeholder_tex = pu::ui::render::RenderText("Ubuntu@30", "NO IMAGE", text_color);
        loading_tex = pu::ui::render::RenderText("Ubuntu@30", "LOADING...", text_color);
    }

    void DetailCard::ClearTextures() {
        if (tex_btn_download) { pu::ui::render::DeleteTexture(tex_btn_download); tex_btn_download = nullptr; }
        if (tex_btn_preparing) { pu::ui::render::DeleteTexture(tex_btn_preparing); tex_btn_preparing = nullptr; }
        if (tex_btn_downloaded) { pu::ui::render::DeleteTexture(tex_btn_downloaded); tex_btn_downloaded = nullptr; }
        if (tex_btn_failed) { pu::ui::render::DeleteTexture(tex_btn_failed); tex_btn_failed = nullptr; }
        if (tex_btn_unsupported) { pu::ui::render::DeleteTexture(tex_btn_unsupported); tex_btn_unsupported = nullptr; }
        
        if (tex_btn_uninstall) { pu::ui::render::DeleteTexture(tex_btn_uninstall); tex_btn_uninstall = nullptr; }
        if (tex_btn_confirm_uninstall) { pu::ui::render::DeleteTexture(tex_btn_confirm_uninstall); tex_btn_confirm_uninstall = nullptr; }
        if (tex_btn_add_to_queue) { pu::ui::render::DeleteTexture(tex_btn_add_to_queue); tex_btn_add_to_queue = nullptr; }
        if (tex_btn_remove_from_queue) { pu::ui::render::DeleteTexture(tex_btn_remove_from_queue); tex_btn_remove_from_queue = nullptr; }
        
        if (dynamic_download_tex) { pu::ui::render::DeleteTexture(dynamic_download_tex); dynamic_download_tex = nullptr; }

        if (details_tex) { pu::ui::render::DeleteTexture(details_tex); details_tex = nullptr; }
        if (save_data_tex) { pu::ui::render::DeleteTexture(save_data_tex); save_data_tex = nullptr; }
        if (mods_tex) { pu::ui::render::DeleteTexture(mods_tex); mods_tex = nullptr; }
        if (cheats_tex) { pu::ui::render::DeleteTexture(cheats_tex); cheats_tex = nullptr; }
        if (cover_placeholder_tex) { pu::ui::render::DeleteTexture(cover_placeholder_tex); cover_placeholder_tex = nullptr; }
        if (loading_tex) { pu::ui::render::DeleteTexture(loading_tex); loading_tex = nullptr; }
        if (title_tex) { pu::ui::render::DeleteTexture(title_tex); title_tex = nullptr; }
    }

    void DetailCard::GetVariantDimensions(const std::string& variant, int& w, int& h) {
        std::string norm_slug = romm::model::NormalizePlatformSlug(ctx.platform_slug);
        bool is_psp = (norm_slug == "psp");
        bool is_nds = (norm_slug == "nds" || norm_slug == "nintendo-ds" || norm_slug == "nintendo_ds" || norm_slug == "Nintendo DS");
        bool is_gb = (norm_slug == "gb" || norm_slug == "game-boy" || norm_slug == "gameboy" || norm_slug == "nintendo-game-boy");
        bool is_gbc = (norm_slug == "gbc" || norm_slug == "game-boy-color" || norm_slug == "gameboy-color" || norm_slug == "nintendo-game-boy-color");
        bool is_gba = (norm_slug == "gba" || norm_slug == "game-boy-advance" || norm_slug == "gameboy-advance" || norm_slug == "nintendo-game-boy-advance");
        bool is_gameboy_family = (is_gb || is_gbc || is_gba);
        bool is_ps1 = ctx.is_ps1;

        if (variant == "miximage_v2" || variant == "big") {
            if (is_psp) { w = 340; h = 560; }
            else if (is_nds) { w = 340; h = 308; }
            else if (is_gameboy_family || is_ps1) { w = 340; h = 340; }
            else { w = 360; h = 480; }
        } else {
            // "small"
            if (is_nds || is_gameboy_family) { w = 380; h = 344; }
            else if (is_psp) { w = 247; h = 378; }
            else if (is_ps1) { w = 240; h = 240; }
            else { w = 180; h = 270; }
        }
    }

    void DetailCard::ResolveDetailImageState(bool allow_download) {
        if (!is_active) {
            return;
        }

        auto nav = nav_mgr.lock();
        if (!nav) return;
        auto model = nav->GetModel();
        if (!model) return;

        int rom_id = ctx.rom_id;
        if (rom_id <= 0) return;

        // Ensure we only process if the identity matches the active context
        if (rom_id != expected_identity.cache_key.rom_id ||
            ctx.generation != expected_identity.generation ||
            romm::model::NormalizePlatformSlug(ctx.platform_slug) != expected_identity.cache_key.platform_slug) {
            return;
        }

        // 1. Centralized target resolution
        std::string target_source = ctx.cover_path;
        std::string target_variant = "small";

        romm::model::DetailLoadState state = model->GetDetailState(rom_id);
        if (state == romm::model::DetailLoadState::Loaded) {
            const auto* detail = model->GetCachedDetail(rom_id);
            if (detail) {
                std::string miximage_url = detail->miximage_v2_url;
                std::string large_url = detail->path_cover_large;

                // Query CoverCache for permanent failure state
                CoverCacheKey mix_key = expected_identity.cache_key;
                mix_key.cover_source = miximage_url;
                mix_key.variant = "miximage_v2";
                int mix_w = 0, mix_h = 0;
                GetVariantDimensions("miximage_v2", mix_w, mix_h);
                mix_key.requested_width = mix_w;
                mix_key.requested_height = mix_h;

                auto mix_res = CoverCache::Instance().GetOrRequest(rom_id, ctx.platform_slug, miximage_url, currentCoverProfile.type, "miximage_v2", false);

                if (!miximage_url.empty() && mix_res.state != CoverState::FailedPermanent) {
                    target_source = miximage_url;
                    target_variant = "miximage_v2";
                } else if (!large_url.empty()) {
                    CoverCacheKey large_key = mix_key;
                    large_key.cover_source = large_url;
                    large_key.variant = "big";
                    int l_w = 0, l_h = 0;
                    GetVariantDimensions("big", l_w, l_h);
                    large_key.requested_width = l_w;
                    large_key.requested_height = l_h;

                    auto large_res = CoverCache::Instance().GetOrRequest(rom_id, ctx.platform_slug, large_url, currentCoverProfile.type, "big", false);
                    if (large_res.state != CoverState::FailedPermanent) {
                        target_source = large_url;
                        target_variant = "big";
                    }
                }
            }
        }

        // Set dimensions for expected target key
        int target_w = 0, target_h = 0;
        GetVariantDimensions(target_variant, target_w, target_h);

        bool target_changed = (expected_identity.cache_key.cover_source != target_source ||
                               expected_identity.cache_key.variant != target_variant);

        if (target_changed) {
            expected_identity.cache_key.rom_id = rom_id;
            expected_identity.cache_key.platform_slug = romm::model::NormalizePlatformSlug(ctx.platform_slug);
            expected_identity.cache_key.cover_source = target_source;
            expected_identity.cache_key.variant = target_variant;
            expected_identity.cache_key.requested_width = target_w;
            expected_identity.cache_key.requested_height = target_h;

            std::cout << "[DETAIL_IMAGE_SOURCE_CHANGED] rom_id=" << rom_id
                      << " target=" << target_variant << std::endl;
        }

        // 2. Query CoverCache (allows download only if allow_download is true)
        auto result = CoverCache::Instance().GetOrRequest(
            expected_identity.cache_key.rom_id,
            expected_identity.cache_key.platform_slug,
            expected_identity.cache_key.cover_source,
            currentCoverProfile.type,
            expected_identity.cache_key.variant,
            allow_download
        );

        // 3. Process Cache State to drive Request State and Display State
        if (result.state == CoverState::Ready && result.texture != nullptr) {
            // Atomic texture swap!
            cover_tex = result.texture;
            displayed_key = result.key;
            request_state = DetailRequestState::Idle;
            
            if (result.key.variant == "miximage_v2") {
                display_state = DetailDisplayState::MiximageReady;
                cover_state = DetailCoverState::UsingMiximage;
            } else {
                display_state = DetailDisplayState::CoverFallback;
                if (result.key.variant == "big") {
                    cover_state = DetailCoverState::UsingLarge;
                } else {
                    cover_state = DetailCoverState::UsingSmallFallback;
                }
            }
        } else if (result.state == CoverState::FailedPermanent) {
            request_state = DetailRequestState::FailedPermanent;
            if (cover_tex == nullptr) {
                display_state = DetailDisplayState::Placeholder;
                cover_state = DetailCoverState::Failed;
            }
            // Trigger immediate fallback search if miximage failed permanently
            if (expected_identity.cache_key.variant == "miximage_v2" || expected_identity.cache_key.variant == "big") {
                ResolveDetailImageState(allow_download);
            }
        } else if (result.state == CoverState::FailedTransient) {
            request_state = DetailRequestState::FailedTransient;
            if (cover_tex == nullptr) {
                display_state = DetailDisplayState::Placeholder;
                cover_state = DetailCoverState::Failed;
            }
        } else if (result.state == CoverState::Loading || result.state == CoverState::Missing) {
            request_state = (expected_identity.cache_key.variant == "miximage_v2") ? DetailRequestState::RequestingMiximage : DetailRequestState::RequestingCover;
            if (cover_tex == nullptr) {
                display_state = DetailDisplayState::LoadingPlaceholder;
                cover_state = DetailCoverState::Placeholder;
            }
        }
    }

    void DetailCard::OnRender(pu::ui::render::Renderer::Ref &drawer, const s32 x_coord, const s32 y_coord) {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        pu::ui::Color fill_color(30, 34, 43, 255);   // Web Charcoal Grey (#1E222B)
        pu::ui::Color border_color(45, 50, 62, 255); // Slate Border Grey (#2D323E)

        // Draw card border outline and background via layered rounded rectangles
        drawer->RenderRoundedRectangleFill(border_color, x_coord, y_coord, w, h, 20);
        drawer->RenderRoundedRectangleFill(fill_color, x_coord + 4, y_coord + 4, w - 8, h - 8, 16);

        CoverCache::Instance().PollCompleted();

        // 1. Check if expected key state changed in CoverCache
        CoverState current_cache_state = CoverState::Unknown;
        if (expected_identity.cache_key.rom_id > 0) {
            auto cache_res = CoverCache::Instance().GetOrRequest(
                expected_identity.cache_key.rom_id,
                expected_identity.cache_key.platform_slug,
                expected_identity.cache_key.cover_source,
                currentCoverProfile.type,
                expected_identity.cache_key.variant,
                false // do not allow download/filesystem check
            );
            current_cache_state = cache_res.state;
        }

        if (current_cache_state != last_known_cache_state || cover_tex == nullptr) {
            ResolveDetailImageState(true);
            last_known_cache_state = current_cache_state;
        }

        // Draw the viewport container (STABLE geometry)
        bool cover_focused = (nav->GetDetailFocus() == romm::navigation::DetailFocus::Cover);
        pu::ui::Color highlight_color(230, 199, 167, 255);
        pu::ui::Color viewport_border = cover_focused ? highlight_color : pu::ui::Color(45, 50, 62, 100);
        pu::ui::Color viewport_bg(16, 18, 22, 255); // Web Dark Slate (#101216)
        s32 thickness = cover_focused ? 4 : 2;

        drawer->RenderRoundedRectangleFill(viewport_border, actual_cover_x - thickness, actual_cover_y - thickness, actual_cover_w + thickness * 2, actual_cover_h + thickness * 2, 12);
        drawer->RenderRoundedRectangleFill(viewport_bg, actual_cover_x, actual_cover_y, actual_cover_w, actual_cover_h, 10);

        // Draw texture inside viewport with contain-style rendering
        if (cover_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(cover_tex);
            s32 th = pu::ui::render::GetTextureHeight(cover_tex);
            float scale = std::min((float)actual_cover_w / tw, (float)actual_cover_h / th);
            s32 draw_w = (s32)(tw * scale);
            s32 draw_h = (s32)(th * scale);
            s32 draw_x = actual_cover_x + (actual_cover_w - draw_w) / 2;
            s32 draw_y = actual_cover_y + (actual_cover_h - draw_h) / 2;
            pu::ui::render::TextureRenderOptions opts;
            opts.width  = draw_w;
            opts.height = draw_h;
            drawer->RenderTexture(cover_tex, draw_x, draw_y, opts);
        } else {
            // No cover available (loading, missing, or failed): prefer the
            // bundled per-platform "NO COVER" art, fall back to the old text.
            auto ph_tex = GetPlaceholderCover(expected_identity.cache_key.platform_slug);
            if (ph_tex) {
                DrawPlaceholderCover(drawer, ph_tex, actual_cover_x, actual_cover_y, actual_cover_w, actual_cover_h);
            } else if (display_state == DetailDisplayState::Placeholder) {
                s32 ph_w = pu::ui::render::GetTextureWidth(cover_placeholder_tex);
                s32 ph_h = pu::ui::render::GetTextureHeight(cover_placeholder_tex);
                drawer->RenderTexture(cover_placeholder_tex, actual_cover_x + (actual_cover_w - ph_w) / 2, actual_cover_y + (actual_cover_h - ph_h) / 2);
            } else {
                s32 ph_w = pu::ui::render::GetTextureWidth(loading_tex);
                s32 ph_h = pu::ui::render::GetTextureHeight(loading_tex);
                drawer->RenderTexture(loading_tex, actual_cover_x + (actual_cover_w - ph_w) / 2, actual_cover_y + (actual_cover_h - ph_h) / 2);
            }
        }

        // Action Button: fixed below the outer container frame
        s32 cover_x = x_coord + 40;
        s32 cover_y = y_coord + 40;
        s32 cover_w = 430;
        s32 cover_h = 560;

        s32 btn_y = cover_y + cover_h + 30; // 190 + 560 + 30 = 780
        s32 btn_h = 80;
        s32 btn_w = cover_w; // 430
        s32 btn_x = cover_x; // 190
        
        bool actions_focused = (nav->GetDetailFocus() == romm::navigation::DetailFocus::Actions);
        
        pu::ui::Color btn_bg(16, 18, 22, 255); // Web Dark Slate (#101216)
        pu::ui::Color btn_border;
        s32 btn_border_w = 0;

        if (actions_focused) {
            btn_border = pu::ui::Color(230, 199, 167, 255); // Cream (#E6C7A7)
            btn_border_w = 4;
        } else {
            btn_border = pu::ui::Color(230, 199, 167, 100); // Dimmed cream
            btn_border_w = 2;
        }

        drawer->RenderRoundedRectangleFill(btn_border, btn_x, btn_y, btn_w, btn_h, 8);
        drawer->RenderRoundedRectangleFill(btn_bg, btn_x + btn_border_w, btn_y + btn_border_w, btn_w - (btn_border_w * 2), btn_h - (btn_border_w * 2), 6);

        pu::sdl2::Texture active_btn_tex = tex_btn_download;
        bool draw_progress_bar = false;
        float progress_pct = 0.0f;
        std::string new_dynamic_text = "";

        int rom_id = ctx.rom_id;
        auto model = nav->GetModel();
        
        if (rom_id > 0 && model) {
            auto& dl_mgr = romm::model::DownloadManager::Instance();
            auto task_snap = dl_mgr.GetTaskSnapshot(rom_id);
            auto active_snap = dl_mgr.GetActiveDownloadSnapshot();
            std::string platform_slug = ctx.platform_slug;

            if (rom_id != checked_rom_id) {
                checked_rom_id = rom_id;
                file_exists_checked = false;
                final_file_exists = false;
                part_file_exists = false;
                const auto* detail_pre = model->GetCachedDetail(rom_id);
                if (detail_pre && !detail_pre->file_name.empty()) {
                    dl_mgr.RefreshInstallCache(platform_slug, detail_pre->file_name);
                }
            }

            {
                const auto* detail = model->GetCachedDetail(rom_id);
                if (detail && !detail->file_name.empty()) {
                    final_file_exists = dl_mgr.GetCachedInstallState(platform_slug, detail->file_name);
                } else {
                    final_file_exists = false;
                }
            }

            if (final_file_exists) {
                current_action_state = DownloadActionState::Uninstall;
            } else if (task_snap.rom_id == rom_id && (task_snap.state == romm::model::DownloadState::DownloadingGame || task_snap.state == romm::model::DownloadState::DownloadingCover || task_snap.state == romm::model::DownloadState::SyncingCover || task_snap.state == romm::model::DownloadState::Preparing)) {
                current_action_state = DownloadActionState::Downloading;
            } else if (task_snap.rom_id == rom_id && task_snap.state == romm::model::DownloadState::Queued) {
                current_action_state = DownloadActionState::Queued;
            } else if (task_snap.rom_id == rom_id && (task_snap.state == romm::model::DownloadState::Failed || task_snap.state == romm::model::DownloadState::Cancelled)) {
                current_action_state = DownloadActionState::Failed;
            } else if (active_snap.rom_id != 0) {
                current_action_state = DownloadActionState::AddToQueue;
            } else {
                current_action_state = DownloadActionState::Download;
            }

            bool is_ps1 = ctx.is_ps1;

            if (current_action_state == DownloadActionState::Uninstall) {
                active_btn_tex = tex_btn_uninstall;
            } else if (current_action_state == DownloadActionState::Downloading) {
                if (task_snap.state == romm::model::DownloadState::DownloadingGame) {
                    draw_progress_bar = true;
                    long long down = task_snap.downloaded_bytes;
                    long long total = task_snap.total_bytes;
                    if (total > 0) {
                        progress_pct = (float)down / total;
                        int pct_int = (int)(progress_pct * 100);
                        new_dynamic_text = "DOWNLOADING " + std::to_string(pct_int) + "%";
                    } else {
                        new_dynamic_text = "DOWNLOADING...";
                    }
                } else if (task_snap.state == romm::model::DownloadState::DownloadingCover) {
                    new_dynamic_text = "DOWNLOADING COVER...";
                } else if (task_snap.state == romm::model::DownloadState::SyncingCover) {
                    new_dynamic_text = "SYNCING COVER...";
                } else {
                    active_btn_tex = tex_btn_preparing;
                }
            } else if (current_action_state == DownloadActionState::Queued) {
                auto queue = dl_mgr.GetQueueSnapshot();
                int q_pos = 0;
                for (const auto& t : queue) {
                    if (t.state == romm::model::DownloadState::Queued) q_pos++;
                    if (t.rom_id == rom_id) break;
                }
                new_dynamic_text = "QUEUED #" + std::to_string(q_pos);
            } else if (current_action_state == DownloadActionState::Failed) {
                active_btn_tex = tex_btn_failed;
            } else if (current_action_state == DownloadActionState::AddToQueue) {
                if (!is_ps1 && platform_slug != "psp" && platform_slug != "nds" && platform_slug != "gb" && platform_slug != "gbc" && platform_slug != "gba" && platform_slug != "ps2") active_btn_tex = tex_btn_unsupported;
                else active_btn_tex = tex_btn_add_to_queue;
            } else {
                if (!is_ps1 && platform_slug != "psp" && platform_slug != "nds" && platform_slug != "gb" && platform_slug != "gbc" && platform_slug != "gba" && platform_slug != "ps2") active_btn_tex = tex_btn_unsupported;
                else active_btn_tex = tex_btn_download;
            }
            
            if (!new_dynamic_text.empty()) {
                if (current_dynamic_text != new_dynamic_text || dynamic_download_tex == nullptr) {
                    if (dynamic_download_tex) {
                        pu::ui::render::DeleteTexture(dynamic_download_tex);
                    }
                    pu::ui::Color text_color(237, 229, 251, 255);
                    dynamic_download_tex = pu::ui::render::RenderText("Orbitron@30", new_dynamic_text, text_color);
                    current_dynamic_text = new_dynamic_text;
                }
                active_btn_tex = dynamic_download_tex;
            }
        }

        if (active_btn_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(active_btn_tex);
            s32 th = pu::ui::render::GetTextureHeight(active_btn_tex);
            drawer->RenderTexture(active_btn_tex, btn_x + (btn_w - tw) / 2, btn_y + (btn_h - th) / 2);
        }

        // Draw Progress Bar if needed
        if (draw_progress_bar) {
            s32 pb_h = 14;
            s32 pb_w = btn_w;
            s32 pb_x = btn_x;
            s32 pb_y = btn_y + btn_h + 10;
            
            pu::ui::Color pb_bg(16, 18, 22, 255);
            pu::ui::Color pb_fill(85, 63, 152, 255); // Violet accent
            
            drawer->RenderRoundedRectangleFill(pb_bg, pb_x, pb_y, pb_w, pb_h, 6);
            if (progress_pct > 0.0f) {
                s32 fill_w = (s32)(pb_w * progress_pct);
                if (fill_w > 0) {
                    drawer->RenderRoundedRectangleFill(pb_fill, pb_x, pb_y, fill_w, pb_h, 6);
                }
            }
        }

        // Tabs Row (4 tabs)
        s32 tab_x_start = x_coord + 510;
        s32 tab_y = y_coord + TAB_Y_OFFSET;
        s32 tab_w = 240;
        s32 tab_h = TAB_HEIGHT;
        s32 tab_spacing = 20;

        bool tabs_focused = (nav->GetDetailFocus() == romm::navigation::DetailFocus::Tabs);
        size_t selected_tab_idx = nav->GetSelectedDetailTabIdx();

        for (size_t i = 0; i < 4; ++i) {
            s32 tx = tab_x_start + i * (tab_w + tab_spacing);
            bool is_active_tab = (i == selected_tab_idx);

            pu::ui::Color t_bg;
            pu::ui::Color t_border;
            s32 t_border_w = 0;

            if (is_active_tab) {
                t_bg = pu::ui::Color(85, 63, 152, 255); // Violet highlight capsule (#553F98)
                if (tabs_focused) {
                    t_border = pu::ui::Color(230, 199, 167, 255); // Cream border
                    t_border_w = 3;
                }
            } else {
                t_bg = pu::ui::Color(16, 18, 22, 255); // Web Dark Slate (#101216)
                t_border = pu::ui::Color(45, 50, 62, 255); // Slate Border Grey (#2D323E)
                t_border_w = 2;
            }

            if (t_border_w > 0) {
                drawer->RenderRoundedRectangleFill(t_border, tx, tab_y, tab_w, tab_h, 8);
                drawer->RenderRoundedRectangleFill(t_bg, tx + t_border_w, tab_y + t_border_w, tab_w - (t_border_w * 2), tab_h - (t_border_w * 2), 6);
            } else {
                drawer->RenderRoundedRectangleFill(t_bg, tx, tab_y, tab_w, tab_h, 8);
            }

            pu::sdl2::Texture tab_tex;
            switch(i) {
                case 0: tab_tex = details_tex; break;
                case 1: tab_tex = save_data_tex; break;
                case 2: tab_tex = mods_tex; break;
                case 3: tab_tex = cheats_tex; break;
                default: tab_tex = nullptr; break;
            }

            if (tab_tex) {
                s32 tw = pu::ui::render::GetTextureWidth(tab_tex);
                s32 th = pu::ui::render::GetTextureHeight(tab_tex);
                drawer->RenderTexture(tab_tex, tx + (tab_w - tw) / 2, tab_y + (tab_h - th) / 2);
            }
        }

        // Render Game Title with Marquee Scrolling
        s32 max_title_w = 1060;
        
        // Update delta time
        auto marquee_now = std::chrono::steady_clock::now();
        float marquee_dt = 0.0f;
        if (last_title_scroll_update_time.time_since_epoch().count() > 0) {
            marquee_dt = std::chrono::duration<float>(marquee_now - last_title_scroll_update_time).count();
        }
        last_title_scroll_update_time = marquee_now;
        if (marquee_dt > 0.1f) marquee_dt = 0.1f;

        // Check if title changed or needs rendering
        if (cached_title != ctx.title) {
            cached_title = ctx.title;
            if (title_tex) {
                pu::ui::render::DeleteTexture(title_tex);
                title_tex = nullptr;
            }
            if (!ctx.title.empty()) {
                pu::ui::Color text_color(237, 229, 251, 255);
                title_tex = pu::ui::render::RenderText("Orbitron@45", ctx.title, text_color);
            }
            title_scroll_x = 0.0f;
            title_wait_timer = 2.0f;
            title_scroll_state = TitleScrollState::WaitStart;

            std::cout << "[DETAIL_TITLE_RENDER]"
                      << " rom_id=" << ctx.rom_id
                      << " title_present=" << (ctx.title.empty() ? "false" : "true")
                      << " title_len=" << ctx.title.length() << std::endl;
        }

        s32 title_w = 0;
        s32 title_h = 0;
        if (title_tex) {
            title_w = pu::ui::render::GetTextureWidth(title_tex);
            title_h = pu::ui::render::GetTextureHeight(title_tex);
        }

        if (title_w > max_title_w) {
            float max_scroll = (float)(title_w - max_title_w);
            float scroll_speed = 80.0f;

            if (title_scroll_state == TitleScrollState::WaitStart) {
                title_wait_timer -= marquee_dt;
                if (title_wait_timer <= 0.0f) {
                    title_scroll_state = TitleScrollState::Scrolling;
                }
            } else if (title_scroll_state == TitleScrollState::Scrolling) {
                title_scroll_x += scroll_speed * marquee_dt;
                if (title_scroll_x >= max_scroll) {
                    title_scroll_x = max_scroll;
                    title_scroll_state = TitleScrollState::WaitEnd;
                    title_wait_timer = 2.0f;
                }
            } else if (title_scroll_state == TitleScrollState::WaitEnd) {
                title_wait_timer -= marquee_dt;
                if (title_wait_timer <= 0.0f) {
                    title_scroll_x = 0.0f;
                    title_scroll_state = TitleScrollState::WaitStart;
                    title_wait_timer = 2.0f;
                }
            }
        } else {
            title_scroll_x = 0.0f;
        }

        if (title_tex) {
            s32 draw_x = x_coord + 510;
            s32 draw_y = y_coord + 50;

            if (title_w > max_title_w) {
                SDL_Renderer* renderer = pu::ui::render::GetMainRenderer();
                SDL_Rect old_clip;
                SDL_bool has_clip = SDL_RenderIsClipEnabled(renderer);
                if (has_clip) {
                    SDL_RenderGetClipRect(renderer, &old_clip);
                }

                SDL_Rect clip_rect = {
                    (int)(x_coord + 510),
                    (int)(y_coord + 40),
                    (int)max_title_w,
                    (int)80
                };
                SDL_RenderSetClipRect(renderer, &clip_rect);

                pu::ui::render::TextureRenderOptions opts;
                opts.width = title_w;
                opts.height = title_h;
                drawer->RenderTexture(title_tex, draw_x - (s32)title_scroll_x, draw_y, opts);

                if (has_clip) {
                    SDL_RenderSetClipRect(renderer, &old_clip);
                } else {
                    SDL_RenderSetClipRect(renderer, nullptr);
                }
            } else {
                pu::ui::render::TextureRenderOptions opts;
                opts.width = title_w;
                opts.height = title_h;
                drawer->RenderTexture(title_tex, draw_x, draw_y, opts);
            }
        }
    }

    void DetailCard::OnInput(const u64 keys_down, const u64 keys_up, const u64 keys_held, const pu::ui::TouchPoint touch_pos) {
        // NavigationManager handles inputs, we don't need to do much here, 
        // except we could use this if we wanted isolated input.
    }

    // --- DetailLayout Implementation ---

    DetailLayout::DetailLayout(std::shared_ptr<romm::navigation::NavigationManager> nav)
        : Layout::Layout(), nav_mgr(nav) {

        // Main background: Web Dark Slate (#101216)
        this->SetBackgroundColor(pu::ui::Color(16, 18, 22, 255));

        // Create the card border element first so it renders behind the text
        card = DetailCard::New(150, 150, 1620, 780, nav);
        this->Add(card);

        // Title (Orbitron Black, Very light text, moved right to x=660)
        game_title_text = pu::ui::elm::TextBlock::New(660, 200, "Game Title");
        game_title_text->SetFont("Orbitron@45");
        game_title_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        this->Add(game_title_text);

        // Platform Name (Orbitron, Brand Violet, moved right to x=660) - Kept empty and hidden
        platform_text = pu::ui::elm::TextBlock::New(660, 280, "");
        platform_text->SetFont("Orbitron@37");
        platform_text->SetColor(pu::ui::Color(85, 63, 152, 255));
        this->Add(platform_text);

        // Meta (Ubuntu, Light Lavender, moved down to x=660, y=280)
        meta_text = pu::ui::elm::TextBlock::New(660, 280, "Metadata Info Line");
        meta_text->SetFont("Ubuntu@30");
        meta_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        this->Add(meta_text);

        const s32 card_y = 150;
        const s32 tabs_bottom = card_y + TAB_Y_OFFSET + TAB_HEIGHT; // 380
        const s32 section_title_y = tabs_bottom + TABS_TO_SECTION_GAP; // 400
        const s32 section_content_y = section_title_y + SECTION_TITLE_HEIGHT + SECTION_TITLE_TO_CONTENT_GAP; // 450

        // Description Section Header (Orbitron, Cream Accent)
        desc_title_text = pu::ui::elm::TextBlock::New(660, section_title_y, "DESCRIPTION");
        desc_title_text->SetFont("Orbitron@37");
        desc_title_text->SetColor(pu::ui::Color(230, 199, 167, 255));
        this->Add(desc_title_text);

        // Description Content (Ubuntu, Very light text)
        desc_text = pu::ui::elm::TextBlock::New(660, section_content_y, "No description available.");
        desc_text->SetFont("Ubuntu@30");
        desc_text->SetColor(pu::ui::Color(237, 229, 251, 255));
        this->Add(desc_text);

        // Trailer Link block (Ubuntu, Light Lavender, moved down to x=660, y=780)
        trailer_title_text = pu::ui::elm::TextBlock::New(660, 780, "");
        trailer_title_text->SetFont("Ubuntu@30");
        trailer_title_text->SetColor(pu::ui::Color(85, 63, 152, 255));
        this->Add(trailer_title_text);

        auto modal = romm::ui::UninstallConfirmModal::New(nav);
        this->Add(modal);

        // Download status text
        download_status_text = pu::ui::elm::TextBlock::New(660, 800, "");
        download_status_text->SetFont("Ubuntu@24");
        download_status_text->SetColor(pu::ui::Color(230, 199, 167, 255));
        this->Add(download_status_text);

        // Controller Hints (Ubuntu, Light Lavender)
        hint_text = pu::ui::elm::TextBlock::New(660, 850, "B Return to Library");
        hint_text->SetFont("Ubuntu@30");
        hint_text->SetColor(pu::ui::Color(190, 180, 225, 255));
        this->Add(hint_text);
    }

    void DetailLayout::OnSelectionUpdated() {
        auto nav = nav_mgr.lock();
        if (!nav) return;

        auto model = nav->GetModel();
        if (!model) return;

        if (ctx.rom_id <= 0) return;

        // Set persistent static header texts to empty (rendered custom or removed)
        game_title_text->SetText("");
        platform_text->SetText("");

        size_t tab_idx = nav->GetSelectedDetailTabIdx();
        int rom_id = ctx.rom_id;
        romm::model::DetailLoadState state = model->GetDetailState(rom_id);
        const auto* detail = model->GetCachedDetail(rom_id);

        if (card) {
            card->ResolveDetailImageState(true);
        }

        if (tab_idx == 0) { // DETAILS
            if (state == romm::model::DetailLoadState::Loading || state == romm::model::DetailLoadState::NotLoaded) {
                meta_text->SetText("Loading details...");
                desc_title_text->SetText("DESCRIPTION");
                desc_text->SetText("");
            } else if (state == romm::model::DetailLoadState::Failed) {
                meta_text->SetText("Failed to load details from RomM.");
                desc_title_text->SetText("DESCRIPTION");
                desc_text->SetText("");
            } else if (detail) {
                std::string meta = "";
                if (!detail->developer.empty()) meta += "Developer: " + detail->developer + "  |  ";
                if (!detail->publisher.empty() && detail->publisher != detail->developer) meta += "Publisher: " + detail->publisher + "  |  ";
                
                std::string size_str = "Size: Unknown";
                if (detail->file_size_bytes > 0) {
                    double size_mb = (double)detail->file_size_bytes / (1024.0 * 1024.0);
                    if (size_mb >= 1024.0) {
                        char buf[64]; std::sprintf(buf, "%.2f GB", size_mb / 1024.0); size_str = buf;
                    } else if (size_mb >= 1.0) {
                        char buf[64]; std::sprintf(buf, "%.2f MB", size_mb); size_str = buf;
                    } else {
                        double size_kb = (double)detail->file_size_bytes / 1024.0;
                        if (size_kb >= 1.0) {
                            char buf[64]; std::sprintf(buf, "%.2f KB", size_kb); size_str = buf;
                        } else {
                            char buf[64]; std::sprintf(buf, "%lld B", detail->file_size_bytes); size_str = buf;
                        }
                    }
                }
                meta += size_str;

                meta_text->SetText(meta);
                desc_title_text->SetText("DESCRIPTION");
                
                std::string desc = detail->description;
                if (desc.empty()) desc = "No description available.";
                
                std::cout << "[DETAIL] Description source rom=" << detail->rom_id << std::endl;
                
                descriptionLines = WordWrapLinesPixel("Ubuntu@30", desc, 1020);

                const s32 card_y = 150;
                const s32 tabs_bottom = card_y + TAB_Y_OFFSET + TAB_HEIGHT; // 380
                const s32 section_title_y = tabs_bottom + TABS_TO_SECTION_GAP; // 400
                const s32 section_content_y = section_title_y + SECTION_TITLE_HEIGHT + SECTION_TITLE_TO_CONTENT_GAP; // 450
                const s32 description_bottom = 750;
                const s32 description_height = description_bottom - section_content_y; // 300
                const s32 font_height = 42;
                const size_t max_visible = description_height / font_height; // 7

                if (descriptionLines.size() > max_visible) {
                    maxDescriptionScrollOffset = descriptionLines.size() - max_visible;
                } else {
                    maxDescriptionScrollOffset = 0;
                }
                descriptionScrollOffset = 0;

                std::string visible_desc;
                for (size_t i = 0; i < (size_t)max_visible && i < descriptionLines.size(); ++i) {
                    visible_desc += descriptionLines[i];
                    if (i < (size_t)max_visible - 1 && i + 1 < descriptionLines.size()) {
                        visible_desc += "\n";
                    }
                }
                desc_text->SetText(visible_desc);

                UpdateFooterHints();
            }
            trailer_title_text->SetText("");
        }
        else if (tab_idx == 1) { // SAVE DATA
            meta_text->SetText("");
            desc_title_text->SetText("SAVE DATA");
            desc_text->SetText("Coming later");
            trailer_title_text->SetText("");
            UpdateFooterHints();
        }
        else if (tab_idx == 2) { // MODS
            meta_text->SetText("");
            desc_title_text->SetText("");
            desc_text->SetText("Coming later");
            trailer_title_text->SetText("");
            UpdateFooterHints();
        }
        else if (tab_idx == 3) { // CHEATS
            meta_text->SetText("");
            desc_title_text->SetText("");
            desc_text->SetText("Coming later");
            trailer_title_text->SetText("");
            UpdateFooterHints();
        }
        UpdateDownloadStatus();
    }

    void DetailLayout::UpdateDownloadStatus() {
        // Remove text-based status updates, as the visual queue system handles it via the button and global bar.
        // We leave the function empty to satisfy any NavigationManager calls, or optionally show a small note.
        if (download_status_text) {
            download_status_text->SetText("");
        }
    }

    void DetailLayout::ScrollDescription(int direction) {
        if (descriptionLines.empty() || maxDescriptionScrollOffset <= 0) return;
        
        const s32 card_y = 150;
        const s32 tabs_bottom = card_y + TAB_Y_OFFSET + TAB_HEIGHT; // 380
        const s32 section_title_y = tabs_bottom + TABS_TO_SECTION_GAP; // 400
        const s32 section_content_y = section_title_y + SECTION_TITLE_HEIGHT + SECTION_TITLE_TO_CONTENT_GAP; // 450
        const s32 description_bottom = 750;
        const s32 description_height = description_bottom - section_content_y; // 300
        const s32 font_height = 42;
        const size_t max_visible = description_height / font_height; // 7
        if (direction > 0) {
            if ((size_t)descriptionScrollOffset + max_visible < descriptionLines.size()) {
                descriptionScrollOffset++;
            }
        } else if (direction < 0) {
            if (descriptionScrollOffset > 0) {
                descriptionScrollOffset--;
            }
        }
        
        std::string visible_desc;
        for (size_t i = 0; i < max_visible && (descriptionScrollOffset + i) < descriptionLines.size(); ++i) {
            visible_desc += descriptionLines[descriptionScrollOffset + i];
            if (i < max_visible - 1 && (descriptionScrollOffset + i + 1) < descriptionLines.size()) {
                visible_desc += "\n";
            }
        }
        desc_text->SetText(visible_desc);
    }

    void DetailLayout::UpdateFooterHints() {
        if (!hint_text) return;
        
        auto nav = nav_mgr.lock();
        if (!nav) return;
        
        bool has_image = false;
        if (card && card->GetCoverTexture() != nullptr &&
            card->GetCoverState() != DetailCoverState::Placeholder &&
            card->GetCoverState() != DetailCoverState::Failed) {
            has_image = true;
        }
        
        auto focus = nav->GetDetailFocus();
        std::string base_hints;
        
        if (focus == romm::navigation::DetailFocus::Cover) {
            base_hints = "A Enlarge   |   Right Focus Panel   |   B Back";
        } else {
            std::string prefix = "";
            if (has_image) {
                prefix = "Left Focus Image   |   ";
            }
            
            std::string desc_scroll = "";
            if (maxDescriptionScrollOffset > 0) {
                desc_scroll = "   |   R-Stick Scroll Desc";
            }
            
            base_hints = prefix + "A Select / Focus   |   B Back / Library" + desc_scroll;
        }
        
        hint_text->SetText(base_hints);
    }

    // --- FullscreenImageElement Implementation ---

    FullscreenImageElement::FullscreenImageElement(s32 x, s32 y, s32 w, s32 h) : Element() {
        this->x = x;
        this->y = y;
        this->w = w;
        this->h = h;
        this->current_mode = FullscreenMode::MixImage;
    }

    void FullscreenImageElement::SetKeys(const FullscreenKeys& new_keys) {
        keys = new_keys;
        if (keys.miximage_key.rom_id > 0 && !keys.miximage_key.cover_source.empty()) {
            current_mode = FullscreenMode::MixImage;
        } else {
            current_mode = FullscreenMode::LargeCover;
        }
    }

    void FullscreenImageElement::CycleMode(int direction) {
        int cur = (int)current_mode;
        int next = cur;
        for (int i = 0; i < 2; ++i) {
            cur = (cur + direction + 2) % 2;
            bool valid = false;
            if (cur == 0 && keys.miximage_key.rom_id > 0 && !keys.miximage_key.cover_source.empty()) valid = true;
            if (cur == 1 && keys.large_key.rom_id > 0 && !keys.large_key.cover_source.empty()) valid = true;
            
            if (valid) {
                next = cur;
                break;
            }
        }
        current_mode = (FullscreenMode)next;
        std::cout << "[FULLSCREEN] Mode cycled to: " << (int)current_mode << std::endl;
    }

    void FullscreenImageElement::OnRender(pu::ui::render::Renderer::Ref& drawer, const s32 x_coord, const s32 y_coord) {
        // Run central texture cache polling to complete background loading on the main thread
        CoverCache::Instance().PollCompleted();

        pu::sdl2::Texture selected_tex = nullptr;
        CoverCacheKey query_key;
        bool has_key = false;

        if (current_mode == FullscreenMode::MixImage && keys.miximage_key.rom_id > 0 && !keys.miximage_key.cover_source.empty()) {
            query_key = keys.miximage_key;
            has_key = true;
        } else if (current_mode == FullscreenMode::LargeCover && keys.large_key.rom_id > 0 && !keys.large_key.cover_source.empty()) {
            query_key = keys.large_key;
            has_key = true;
        }

        if (has_key) {
            auto res = CoverCache::Instance().GetOrRequest(
                query_key.rom_id,
                query_key.platform_slug,
                query_key.cover_source,
                CoverProfileType::DefaultPortrait,
                query_key.variant,
                true // allow download when explicitly requested
            );
            if (res.state == CoverState::Ready && res.texture) {
                selected_tex = res.texture;
            }
        }

        // Fallback scan: if the selected image is not ready, find the best ready texture to display
        pu::sdl2::Texture display_tex = selected_tex;

        if (!display_tex) {
            // 1. Try MixImage if ready
            if (keys.miximage_key.rom_id > 0 && !keys.miximage_key.cover_source.empty()) {
                auto res = CoverCache::Instance().GetOrRequest(
                    keys.miximage_key.rom_id,
                    keys.miximage_key.platform_slug,
                    keys.miximage_key.cover_source,
                    CoverProfileType::DefaultPortrait,
                    keys.miximage_key.variant,
                    false
                );
                if (res.state == CoverState::Ready && res.texture) {
                    display_tex = res.texture;
                }
            }
            // 2. Try LargeCover if ready
            if (!display_tex && keys.large_key.rom_id > 0 && !keys.large_key.cover_source.empty()) {
                auto res = CoverCache::Instance().GetOrRequest(
                    keys.large_key.rom_id,
                    keys.large_key.platform_slug,
                    keys.large_key.cover_source,
                    CoverProfileType::DefaultPortrait,
                    keys.large_key.variant,
                    false
                );
                if (res.state == CoverState::Ready && res.texture) {
                    display_tex = res.texture;
                }
            }
            // 3. Last resort: the small cover — this is what the Detail page itself
            // is already showing, so if we have nothing else, this beats a black
            // screen. Allow a download here too since it's our final fallback.
            if (!display_tex && keys.small_key.rom_id > 0 && !keys.small_key.cover_source.empty()) {
                auto res = CoverCache::Instance().GetOrRequest(
                    keys.small_key.rom_id,
                    keys.small_key.platform_slug,
                    keys.small_key.cover_source,
                    CoverProfileType::DefaultPortrait,
                    keys.small_key.variant,
                    true
                );
                if (res.state == CoverState::Ready && res.texture) {
                    display_tex = res.texture;
                }
            }
        }

        // Render target texture preserve aspect ratio
        if (display_tex) {
            s32 tw = pu::ui::render::GetTextureWidth(display_tex);
            s32 th = pu::ui::render::GetTextureHeight(display_tex);
            float scale = std::min(1920.0f / tw, 1080.0f / th);
            s32 draw_w = (s32)(tw * scale);
            s32 draw_h = (s32)(th * scale);
            s32 draw_x = (1920 - draw_w) / 2;
            s32 draw_y = (1080 - draw_h) / 2;
            pu::ui::render::TextureRenderOptions opts;
            opts.width = draw_w;
            opts.height = draw_h;
            drawer->RenderTexture(display_tex, draw_x, draw_y, opts);
        }
    }

    // --- FullscreenImageLayout Implementation ---

    FullscreenImageLayout::FullscreenImageLayout() : Layout() {
        this->SetBackgroundColor(pu::ui::Color(0, 0, 0, 255));
        img_el = FullscreenImageElement::New(0, 0, 1920, 1080);
        this->Add(img_el);
    }
}
