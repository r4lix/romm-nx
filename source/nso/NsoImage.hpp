#pragma once

#include <cstddef>
#include <string>

// Cover / details-screen image conversion for the SNES Online injection test.
//
// The real CaVE-generated LayeredFS on this console stores both images as plain
// PNG — `/titles/<code>/<code>.png` at 512x374 (8-bit RGBA, non-interlaced) and
// `/titles/<code>/<code>-details.png` at 400x300 (8-bit RGB). There is no XTX
// texture and no zlib-wrapped `.xtx.z` anywhere in the SNES title: that layout
// belongs to other Switch Online apps, not this one. See docs/nso-snes-format.md.
//
// Decoding goes through SDL2_image (already used for covers elsewhere in
// romm-nx) and encoding through libpng directly, so the colour type and
// interlace mode written match the reference files exactly.
namespace romm::nso {

    struct NsoImageResult {
        bool success = false;
        std::string error;
        int source_width = 0;
        int source_height = 0;
        int output_width = 0;
        int output_height = 0;
        size_t output_bytes = 0;
    };

    // Required output geometry, taken from the installed reference titles.
    constexpr int kCoverWidth = 512;
    constexpr int kCoverHeight = 374;
    constexpr int kDetailsWidth = 400;
    constexpr int kDetailsHeight = 300;

    // Fits the source art inside the target box without distortion and pads the
    // remainder with opaque black. Cropping to fill was rejected: RomM serves
    // portrait box art for a good share of SNES titles, and a centre-crop of a
    // portrait cover into a 512x374 landscape frame throws away the title text.
    NsoImageResult ConvertCover(const std::string& sourcePath, const std::string& outputPath);
    NsoImageResult ConvertDetails(const std::string& sourcePath, const std::string& outputPath);

}
