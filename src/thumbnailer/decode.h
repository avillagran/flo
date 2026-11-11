// Bounded image decoding for the thumbnail worker: JPEG through libjpeg-turbo
// with DCT-domain scaling, PNG through libpng, both streamed into a box
// filter so the full-size image is never held when it can be avoided.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace flo::thumb {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // width * height * 4, straight alpha
    bool has_alpha = false;
};

// Decodes `path` scaled to fit within max_px x max_px (never upscaled).
// Returns false with `error` set for unreadable, oversized or corrupt input.
bool decode_jpeg(const std::string &path, int max_px, Image &out, std::string &error);
bool decode_png(const std::string &path, int max_px, Image &out, std::string &error);

// Fits w x h into max_px keeping the aspect ratio (at least 1 x 1).
void fit_size(int w, int h, int max_px, int &out_w, int &out_h);

// Box-filter downscale of a full RGBA image.
Image downscale(const Image &in, int max_px);

} // namespace flo::thumb
