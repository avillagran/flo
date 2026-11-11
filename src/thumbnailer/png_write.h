// PNG encoding for thumbnails and failure markers, with the spec's keys in
// plain (uncompressed) tEXt chunks.
#pragma once

#include "thumbnailer/decode.h"

#include <string>
#include <utility>
#include <vector>

namespace flo::thumb {

using TextChunks = std::vector<std::pair<std::string, std::string>>;

// Encodes `image` (RGB when opaque, RGBA otherwise) into `out`.
bool encode_png(const Image &image, const TextChunks &text, std::string &out);

} // namespace flo::thumb
