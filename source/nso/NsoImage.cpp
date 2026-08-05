#include "NsoImage.hpp"

#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <png.h>

#include <csetjmp>
#include <cstdio>
#include <sys/stat.h>
#include <vector>

namespace romm::nso {

    namespace {

        struct PngErrorState {
            jmp_buf jump;
        };

        void PngErrorFn(png_structp png, png_const_charp) {
            auto* state = static_cast<PngErrorState*>(png_get_error_ptr(png));
            if (state) longjmp(state->jump, 1);
        }

        void PngWarnFn(png_structp, png_const_charp) {
            // libpng warns about things like an iCCP profile; nothing to do.
        }

        // Writes `surface` (which must already be in the requested layout) as a
        // non-interlaced 8-bit PNG. with_alpha selects colour type 6 (RGBA) or
        // 2 (RGB), matching the two reference file kinds.
        bool WritePng(SDL_Surface* surface, const std::string& path, bool with_alpha, std::string& error) {
            FILE* fp = std::fopen(path.c_str(), "wb");
            if (!fp) {
                error = "cannot create " + path;
                return false;
            }

            PngErrorState state;
            png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, &state, PngErrorFn, PngWarnFn);
            if (!png) {
                std::fclose(fp);
                error = "png_create_write_struct failed";
                return false;
            }
            png_infop info = png_create_info_struct(png);
            if (!info) {
                png_destroy_write_struct(&png, nullptr);
                std::fclose(fp);
                error = "png_create_info_struct failed";
                return false;
            }

            // Built before setjmp deliberately: a non-volatile local modified
            // between setjmp and longjmp has an indeterminate value on the
            // error path, and this one owns a heap allocation.
            std::vector<png_bytep> rows((size_t)surface->h);
            for (int y = 0; y < surface->h; ++y) {
                rows[(size_t)y] = static_cast<png_bytep>(surface->pixels) + (size_t)y * (size_t)surface->pitch;
            }

            if (setjmp(state.jump)) {
                png_destroy_write_struct(&png, &info);
                std::fclose(fp);
                std::remove(path.c_str());
                error = "libpng aborted while writing " + path;
                return false;
            }

            png_init_io(png, fp);
            png_set_IHDR(png, info, (png_uint_32)surface->w, (png_uint_32)surface->h, 8,
                         with_alpha ? PNG_COLOR_TYPE_RGB_ALPHA : PNG_COLOR_TYPE_RGB,
                         PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
            png_write_info(png, info);

            png_write_image(png, rows.data());
            png_write_end(png, nullptr);

            png_destroy_write_struct(&png, &info);
            const bool closed = (std::fclose(fp) == 0);
            if (!closed) {
                std::remove(path.c_str());
                error = "failed to flush " + path;
                return false;
            }
            return true;
        }

        long FileSize(const std::string& path) {
            struct stat st {};
            if (stat(path.c_str(), &st) != 0) return -1;
            return (long)st.st_size;
        }

        // `height_locked` switches the target from a fixed box to a fixed
        // height: the canvas becomes exactly the scaled art, so nothing is
        // padded. target_w is then a maximum rather than the width.
        NsoImageResult ConvertTo(const std::string& sourcePath, const std::string& outputPath,
                                 int target_w, int target_h, bool with_alpha,
                                 bool height_locked = false) {
            NsoImageResult result;
            result.output_width = target_w;
            result.output_height = target_h;

            SDL_Surface* source = IMG_Load(sourcePath.c_str());
            if (!source) {
                const char* err = IMG_GetError();
                result.error = std::string("cannot decode ") + sourcePath + ": " + (err ? err : "unknown");
                return result;
            }
            result.source_width = source->w;
            result.source_height = source->h;

            if (height_locked && source->w > 0 && source->h > 0) {
                int width = (int)((double)source->w * (double)target_h / (double)source->h + 0.5);
                if (width < 1) width = 1;
                if (width > target_w) {
                    // Wider than the cap: fit by width and let the height come
                    // down, rather than cropping or squashing.
                    target_h = (int)((double)source->h * (double)target_w / (double)source->w + 0.5);
                    if (target_h < 1) target_h = 1;
                } else {
                    target_w = width;
                }
                result.output_width = target_w;
                result.output_height = target_h;
            }

            // Blit as a straight copy: an RGBA source would otherwise be alpha
            // blended over the black canvas and lose its own transparency edges.
            SDL_SetSurfaceBlendMode(source, SDL_BLENDMODE_NONE);

            const Uint32 target_format = with_alpha ? SDL_PIXELFORMAT_RGBA32 : SDL_PIXELFORMAT_RGB24;
            SDL_Surface* canvas = SDL_CreateRGBSurfaceWithFormat(0, target_w, target_h,
                                                                 with_alpha ? 32 : 24, target_format);
            if (!canvas) {
                SDL_FreeSurface(source);
                result.error = std::string("cannot allocate ") + std::to_string(target_w) + "x" +
                               std::to_string(target_h) + " canvas: " + SDL_GetError();
                return result;
            }
            SDL_FillRect(canvas, nullptr, SDL_MapRGBA(canvas->format, 0, 0, 0, 255));

            // Contain-fit: preserve aspect, never upscale past the box, centre
            // the result and leave the rest black.
            double scale_w = (double)target_w / (double)source->w;
            double scale_h = (double)target_h / (double)source->h;
            double scale = (scale_w < scale_h) ? scale_w : scale_h;
            int draw_w = (int)((double)source->w * scale + 0.5);
            int draw_h = (int)((double)source->h * scale + 0.5);
            if (draw_w < 1) draw_w = 1;
            if (draw_h < 1) draw_h = 1;
            if (draw_w > target_w) draw_w = target_w;
            if (draw_h > target_h) draw_h = target_h;

            SDL_Rect dest;
            dest.x = (target_w - draw_w) / 2;
            dest.y = (target_h - draw_h) / 2;
            dest.w = draw_w;
            dest.h = draw_h;

            if (SDL_BlitScaled(source, nullptr, canvas, &dest) != 0) {
                const std::string blit_error = SDL_GetError();
                SDL_FreeSurface(source);
                SDL_FreeSurface(canvas);
                result.error = "scaling blit failed: " + blit_error;
                return result;
            }
            SDL_FreeSurface(source);

            // The canvas is opaque everywhere by construction, but the blit
            // copies the source alpha verbatim — restore full opacity so a cover
            // with transparent corners cannot render as a hole in the NSO UI.
            if (with_alpha) {
                for (int y = 0; y < canvas->h; ++y) {
                    Uint8* row = static_cast<Uint8*>(canvas->pixels) + (size_t)y * (size_t)canvas->pitch;
                    for (int x = 0; x < canvas->w; ++x) {
                        row[x * 4 + 3] = 255;
                    }
                }
            }

            std::string write_error;
            const std::string part = outputPath + ".part";
            std::remove(part.c_str());
            const bool ok = WritePng(canvas, part, with_alpha, write_error);
            SDL_FreeSurface(canvas);

            if (!ok) {
                result.error = write_error;
                return result;
            }

            std::remove(outputPath.c_str()); // sdmc: rename() will not overwrite
            if (std::rename(part.c_str(), outputPath.c_str()) != 0) {
                std::remove(part.c_str());
                result.error = "cannot finalize " + outputPath;
                return result;
            }

            const long size = FileSize(outputPath);
            if (size <= 0) {
                result.error = "generated image " + outputPath + " is empty";
                return result;
            }
            result.output_bytes = (size_t)size;
            result.success = true;
            return result;
        }

    } // namespace

    NsoImageResult ConvertCover(const std::string& sourcePath, const std::string& outputPath) {
        return ConvertTo(sourcePath, outputPath, kCoverWidth, kCoverHeight, true);
    }

    NsoImageResult ConvertDetails(const std::string& sourcePath, const std::string& outputPath) {
        return ConvertTo(sourcePath, outputPath, kDetailsWidth, kDetailsHeight, false);
    }

    NsoImageResult ConvertCoverNes(const std::string& sourcePath, const std::string& outputPath) {
        return ConvertTo(sourcePath, outputPath, kNesCoverMaxWidthPx, kNesCoverTargetHeight, true, true);
    }

    NsoImageResult ConvertCoverGb(const std::string& sourcePath, const std::string& outputPath) {
        return ConvertTo(sourcePath, outputPath, kGbCoverBoxPx, kGbCoverBoxPx, true, true);
    }

    NsoImageResult ConvertDetailsGb(const std::string& sourcePath, const std::string& outputPath) {
        return ConvertTo(sourcePath, outputPath, kGbDetailsWidthPx, kGbDetailsHeightPx, false);
    }

}
