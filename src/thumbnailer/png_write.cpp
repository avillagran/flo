#include "thumbnailer/png_write.h"

#include <csetjmp>
#include <png.h>

namespace flo::thumb {

namespace {
struct Sink {
    std::string *out;
};
void write_cb(png_structp png, png_bytep data, png_size_t len) {
    auto *s = static_cast<Sink *>(png_get_io_ptr(png));
    s->out->append(reinterpret_cast<const char *>(data), len);
}
void flush_cb(png_structp) {}
void fail_cb(png_structp png, png_const_charp) { longjmp(png_jmpbuf(png), 1); }
void warn_cb(png_structp, png_const_charp) {}
} // namespace

// All libpng work happens here, with no C++ objects of its own, so a longjmp
// from libpng skips nothing that needs destroying.
static bool write_rows(const Image &image, png_text *chunks, int nchunks, uint8_t *rgb, Sink *sink) {
    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, nullptr, fail_cb, warn_cb);
    if (!png)
        return false;
    png_infop info = png_create_info_struct(png);
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, info ? &info : nullptr);
        return false;
    }
    png_set_write_fn(png, sink, write_cb, flush_cb);
    const bool alpha = image.has_alpha;
    png_set_IHDR(png, info, static_cast<png_uint_32>(image.width), static_cast<png_uint_32>(image.height), 8,
                 alpha ? PNG_COLOR_TYPE_RGBA : PNG_COLOR_TYPE_RGB, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    if (nchunks > 0)
        png_set_text(png, info, chunks, nchunks);
    png_set_compression_level(png, 6);
    png_write_info(png, info);
    for (int y = 0; y < image.height; ++y) {
        const uint8_t *src = &image.rgba[static_cast<std::size_t>(y) * image.width * 4];
        if (alpha) {
            png_write_row(png, const_cast<png_bytep>(src));
            continue;
        }
        for (int x = 0; x < image.width; ++x) {
            rgb[x * 3 + 0] = src[x * 4 + 0];
            rgb[x * 3 + 1] = src[x * 4 + 1];
            rgb[x * 3 + 2] = src[x * 4 + 2];
        }
        png_write_row(png, rgb);
    }
    png_write_end(png, info);
    png_destroy_write_struct(&png, &info);
    return true;
}

bool encode_png(const Image &image, const TextChunks &text, std::string &out) {
    out.clear();
    if (image.width <= 0 || image.height <= 0 ||
        image.rgba.size() != static_cast<std::size_t>(image.width) * image.height * 4)
        return false;
    std::vector<png_text> chunks(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        chunks[i].compression = PNG_TEXT_COMPRESSION_NONE; // tEXt, never zTXt
        chunks[i].key = const_cast<char *>(text[i].first.c_str());
        chunks[i].text = const_cast<char *>(text[i].second.c_str());
        chunks[i].text_length = text[i].second.size();
    }
    std::vector<uint8_t> rgb(static_cast<std::size_t>(image.width) * 3);
    Sink sink{&out};
    if (!write_rows(image, chunks.data(), static_cast<int>(chunks.size()), rgb.data(), &sink)) {
        out.clear();
        return false;
    }
    return true;
}

} // namespace flo::thumb
