#pragma once

#include <pu/Plutonium>
#include <string>

namespace romm::ui {

    // Bundled per-platform "NO COVER" placeholder art, shown wherever a real
    // cover isn't available yet (still loading, missing, or failed).
    // Files live at romfs:/no-cover/<normalized-slug>.png; returns nullptr for
    // platforms without bundled art. Lazy-loads each file once and caches the
    // texture for the app's lifetime. Render thread only.
    pu::sdl2::Texture GetPlaceholderCover(const std::string& platform_slug);

    // Contain-scale a placeholder into a target box and draw it centered.
    // No-op when tex is null.
    void DrawPlaceholderCover(pu::ui::render::Renderer::Ref& drawer, pu::sdl2::Texture tex,
                              s32 box_x, s32 box_y, s32 box_w, s32 box_h);

}
