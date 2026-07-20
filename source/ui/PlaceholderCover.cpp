#include "PlaceholderCover.hpp"
#include "../model/DataModel.hpp"
#include <unordered_map>
#include <algorithm>

namespace romm::ui {

    pu::sdl2::Texture GetPlaceholderCover(const std::string& platform_slug) {
        // Misses are cached as nullptr too, so platforms without bundled art
        // don't re-probe RomFS every frame.
        static std::unordered_map<std::string, pu::sdl2::Texture> cache;

        const std::string norm = romm::model::NormalizePlatformSlug(platform_slug);
        if (norm.empty()) return nullptr;

        auto it = cache.find(norm);
        if (it != cache.end()) {
            return it->second;
        }

        pu::sdl2::Texture tex = pu::ui::render::LoadImageFromFile("romfs:/no-cover/" + norm + ".png");
        cache[norm] = tex;
        return tex;
    }

    void DrawPlaceholderCover(pu::ui::render::Renderer::Ref& drawer, pu::sdl2::Texture tex,
                              s32 box_x, s32 box_y, s32 box_w, s32 box_h) {
        if (!tex) return;
        s32 tw = pu::ui::render::GetTextureWidth(tex);
        s32 th = pu::ui::render::GetTextureHeight(tex);
        if (tw <= 0 || th <= 0) return;

        float scale = std::min((float)box_w / tw, (float)box_h / th);
        s32 draw_w = (s32)(tw * scale);
        s32 draw_h = (s32)(th * scale);

        pu::ui::render::TextureRenderOptions opts;
        opts.width = draw_w;
        opts.height = draw_h;
        drawer->RenderTexture(tex, box_x + (box_w - draw_w) / 2, box_y + (box_h - draw_h) / 2, opts);
    }

}
