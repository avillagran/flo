#include "thumbnailer/decode.h"

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <jpeglib.h>
#include <png.h>

namespace flo::thumb {

namespace {

constexpr uint64_t kMaxPixels = 400ull * 1000 * 1000;       // streamed decodes
constexpr uint64_t kMaxBufferedPixels = 50ull * 1000 * 1000; // interlaced PNGs

// Accepts source rows in order and averages them into the target size
// (alpha-weighted, so transparent pixels don't darken edges).
class RowScaler {
public:
    RowScaler(int sw, int sh, int dw, int dh) : sw_(sw), sh_(sh), dw_(dw), dh_(dh) {
        acc_.assign(static_cast<std::size_t>(dw) * 5, 0);
        out_.width = dw;
        out_.height = dh;
        out_.rgba.assign(static_cast<std::size_t>(dw) * dh * 4, 0);
        xmap_.resize(static_cast<std::size_t>(sw));
        for (int x = 0; x < sw; ++x)
            xmap_[static_cast<std::size_t>(x)] = static_cast<int>(static_cast<int64_t>(x) * dw / sw);
    }

    void push(const uint8_t *row) {
        const int dy = static_cast<int>(static_cast<int64_t>(sy_) * dh_ / sh_);
        if (dy != cur_dy_) {
            flush();
            cur_dy_ = dy;
        }
        for (int x = 0; x < sw_; ++x) {
            const uint8_t *p = row + x * 4;
            uint64_t *a = &acc_[static_cast<std::size_t>(xmap_[static_cast<std::size_t>(x)]) * 5];
            const uint64_t alpha = p[3];
            a[0] += p[0] * alpha;
            a[1] += p[1] * alpha;
            a[2] += p[2] * alpha;
            a[3] += alpha;
            a[4] += 1;
            if (alpha != 255)
                out_.has_alpha = true;
        }
        ++sy_;
    }

    Image take() {
        flush();
        return std::move(out_);
    }

private:
    void flush() {
        if (cur_dy_ < 0 || cur_dy_ >= dh_)
            return;
        uint8_t *dst = &out_.rgba[static_cast<std::size_t>(cur_dy_) * dw_ * 4];
        for (int x = 0; x < dw_; ++x) {
            uint64_t *a = &acc_[static_cast<std::size_t>(x) * 5];
            if (a[4] && a[3]) {
                dst[x * 4 + 0] = static_cast<uint8_t>(a[0] / a[3]);
                dst[x * 4 + 1] = static_cast<uint8_t>(a[1] / a[3]);
                dst[x * 4 + 2] = static_cast<uint8_t>(a[2] / a[3]);
                dst[x * 4 + 3] = static_cast<uint8_t>(a[3] / a[4]);
            }
            std::fill(a, a + 5, 0);
        }
        // Rows of the target nothing landed in (upscaled height can't happen;
        // rounding can skip none) stay transparent.
    }

    int sw_, sh_, dw_, dh_;
    int sy_ = 0, cur_dy_ = -1;
    std::vector<uint64_t> acc_;
    std::vector<int> xmap_;
    Image out_;
};

FILE *open_regular(const std::string &path, std::string &error) {
    const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK | O_NOCTTY);
    if (fd < 0) {
        error = std::strerror(errno);
        return nullptr;
    }
    struct stat st;
    if (::fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ::close(fd);
        error = "not a regular file";
        return nullptr;
    }
    FILE *f = ::fdopen(fd, "rb");
    if (!f)
        ::close(fd);
    return f;
}

// ---- JPEG -----------------------------------------------------------------

struct JpegError {
    jpeg_error_mgr mgr;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

void jpeg_fail(j_common_ptr cinfo) {
    auto *err = reinterpret_cast<JpegError *>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, err->message);
    longjmp(err->jump, 1);
}

void jpeg_quiet(j_common_ptr, int) {}

// EXIF orientation (1-8) from an APP1 marker, 1 when absent.
int exif_orientation(jpeg_decompress_struct &cinfo) {
    for (jpeg_saved_marker_ptr m = cinfo.marker_list; m; m = m->next) {
        if (m->marker != JPEG_APP0 + 1 || m->data_length < 14 || std::memcmp(m->data, "Exif\0\0", 6) != 0)
            continue;
        const uint8_t *t = m->data + 6;
        const std::size_t len = m->data_length - 6;
        const bool le = t[0] == 'I' && t[1] == 'I';
        if (!le && !(t[0] == 'M' && t[1] == 'M'))
            return 1;
        auto u16 = [&](std::size_t o) -> unsigned {
            return le ? (t[o] | (t[o + 1] << 8)) : ((t[o] << 8) | t[o + 1]);
        };
        auto u32 = [&](std::size_t o) -> uint32_t {
            return le ? (uint32_t(t[o]) | (uint32_t(t[o + 1]) << 8) | (uint32_t(t[o + 2]) << 16) | (uint32_t(t[o + 3]) << 24))
                      : ((uint32_t(t[o]) << 24) | (uint32_t(t[o + 1]) << 16) | (uint32_t(t[o + 2]) << 8) | uint32_t(t[o + 3]));
        };
        const uint32_t ifd = u32(4);
        if (ifd + 2 > len)
            return 1;
        const unsigned count = u16(ifd);
        for (unsigned i = 0; i < count; ++i) {
            const std::size_t e = ifd + 2 + i * 12;
            if (e + 12 > len)
                break;
            if (u16(e) == 0x0112) {
                const unsigned v = u16(e + 8);
                return v >= 1 && v <= 8 ? static_cast<int>(v) : 1;
            }
        }
    }
    return 1;
}

Image orient(Image in, int orientation) {
    if (orientation <= 1 || orientation > 8)
        return in;
    const bool swap = orientation >= 5;
    Image out;
    out.width = swap ? in.height : in.width;
    out.height = swap ? in.width : in.height;
    out.has_alpha = in.has_alpha;
    out.rgba.resize(in.rgba.size());
    for (int y = 0; y < in.height; ++y) {
        for (int x = 0; x < in.width; ++x) {
            int nx = x, ny = y;
            const int W = in.width - 1, H = in.height - 1;
            switch (orientation) {
            case 2: nx = W - x; ny = y; break;
            case 3: nx = W - x; ny = H - y; break;
            case 4: nx = x; ny = H - y; break;
            case 5: nx = y; ny = x; break;
            case 6: nx = H - y; ny = x; break;
            case 7: nx = H - y; ny = W - x; break;
            case 8: nx = y; ny = W - x; break;
            }
            std::memcpy(&out.rgba[(static_cast<std::size_t>(ny) * out.width + nx) * 4],
                        &in.rgba[(static_cast<std::size_t>(y) * in.width + x) * 4], 4);
        }
    }
    return out;
}

} // namespace

void fit_size(int w, int h, int max_px, int &ow, int &oh) {
    if (w <= max_px && h <= max_px) {
        ow = w;
        oh = h;
        return;
    }
    if (w >= h) {
        ow = max_px;
        oh = std::max(1, static_cast<int>(static_cast<int64_t>(h) * max_px / w));
    } else {
        oh = max_px;
        ow = std::max(1, static_cast<int>(static_cast<int64_t>(w) * max_px / h));
    }
}

Image downscale(const Image &in, int max_px) {
    int w, h;
    fit_size(in.width, in.height, max_px, w, h);
    if (w == in.width && h == in.height)
        return in;
    RowScaler s(in.width, in.height, w, h);
    for (int y = 0; y < in.height; ++y)
        s.push(&in.rgba[static_cast<std::size_t>(y) * in.width * 4]);
    return s.take();
}

bool decode_jpeg(const std::string &path, int max_px, Image &out, std::string &error) {
    FILE *f = open_regular(path, error);
    if (!f)
        return false;
    jpeg_decompress_struct cinfo;
    JpegError err;
    cinfo.err = jpeg_std_error(&err.mgr);
    err.mgr.error_exit = jpeg_fail;
    err.mgr.emit_message = jpeg_quiet;
    std::vector<uint8_t> row;
    // Declared before setjmp so a longjmp leaves them in a defined state.
    RowScaler *volatile scaler = nullptr;
    if (setjmp(err.jump)) {
        error = err.message;
        delete scaler;
        jpeg_destroy_decompress(&cinfo);
        std::fclose(f);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    cinfo.mem->max_memory_to_use = 256L << 20;
    jpeg_stdio_src(&cinfo, f);
    jpeg_save_markers(&cinfo, JPEG_APP0 + 1, 0xFFFF);
    jpeg_read_header(&cinfo, TRUE);
    if (static_cast<uint64_t>(cinfo.image_width) * cinfo.image_height > kMaxPixels) {
        error = "image too large";
        jpeg_destroy_decompress(&cinfo);
        std::fclose(f);
        return false;
    }
    const int orientation = exif_orientation(cinfo);
    // DCT scaling: the largest reduction (1/8 .. 1/1) that still leaves at
    // least the target size, so only a fraction of the pixels is decoded.
    const unsigned long longest = std::max(cinfo.image_width, cinfo.image_height);
    unsigned denom = 8;
    while (denom > 1 && longest / denom < static_cast<unsigned long>(max_px))
        denom /= 2;
    cinfo.scale_num = 1;
    cinfo.scale_denom = denom;
    const bool cmyk = cinfo.jpeg_color_space == JCS_CMYK || cinfo.jpeg_color_space == JCS_YCCK;
    cinfo.out_color_space = cmyk ? JCS_CMYK : JCS_EXT_RGBA;
    cinfo.dct_method = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&cinfo);
    const int sw = static_cast<int>(cinfo.output_width), sh = static_cast<int>(cinfo.output_height);
    int dw, dh;
    fit_size(sw, sh, max_px, dw, dh);
    scaler = new RowScaler(sw, sh, dw, dh);
    row.resize(static_cast<std::size_t>(sw) * 4);
    const bool inverted = cmyk && cinfo.saw_Adobe_marker;
    while (cinfo.output_scanline < cinfo.output_height) {
        JSAMPROW rows[1] = {row.data()};
        jpeg_read_scanlines(&cinfo, rows, 1);
        if (cmyk) {
            for (int x = 0; x < sw; ++x) {
                uint8_t *p = &row[static_cast<std::size_t>(x) * 4];
                int c = p[0], m = p[1], y = p[2], k = p[3];
                if (inverted) {
                    p[0] = static_cast<uint8_t>(c * k / 255);
                    p[1] = static_cast<uint8_t>(m * k / 255);
                    p[2] = static_cast<uint8_t>(y * k / 255);
                } else {
                    p[0] = static_cast<uint8_t>((255 - c) * (255 - k) / 255);
                    p[1] = static_cast<uint8_t>((255 - m) * (255 - k) / 255);
                    p[2] = static_cast<uint8_t>((255 - y) * (255 - k) / 255);
                }
                p[3] = 255;
            }
        }
        scaler->push(row.data());
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    std::fclose(f);
    out = orient(scaler->take(), orientation);
    delete scaler;
    out.has_alpha = false;
    return true;
}

// ---- PNG ------------------------------------------------------------------

namespace {
struct PngJump {
    jmp_buf jump;
    char message[256] = {0};
};
void png_fail(png_structp png, png_const_charp msg) {
    auto *j = static_cast<PngJump *>(png_get_error_ptr(png));
    std::snprintf(j->message, sizeof j->message, "%s", msg ? msg : "PNG error");
    longjmp(j->jump, 1);
}
void png_quiet(png_structp, png_const_charp) {}
} // namespace

bool decode_png(const std::string &path, int max_px, Image &out, std::string &error) {
    FILE *f = open_regular(path, error);
    if (!f)
        return false;
    PngJump j;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, &j, png_fail, png_quiet);
    if (!png) {
        std::fclose(f);
        error = "libpng init failed";
        return false;
    }
    png_infop info = png_create_info_struct(png);
    RowScaler *volatile scaler = nullptr;
    // Buffers live at function scope: a longjmp from libpng must not skip
    // their destructors.
    std::vector<uint8_t> full, row;
    std::vector<png_bytep> rows;
    if (!info || setjmp(j.jump)) {
        error = j.message[0] ? j.message : "PNG error";
        delete scaler;
        png_destroy_read_struct(&png, info ? &info : nullptr, nullptr);
        std::fclose(f);
        return false;
    }
    png_set_user_limits(png, 30000, 30000);
    png_set_chunk_malloc_max(png, 8u << 20);
    png_init_io(png, f);
    png_read_info(png, info);
    const png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    const int interlace = png_get_interlace_type(png, info);
    if (static_cast<uint64_t>(w) * h > (interlace != PNG_INTERLACE_NONE ? kMaxBufferedPixels : kMaxPixels))
        png_error(png, "image too large");
    png_set_expand(png);
    png_set_strip_16(png);
    png_set_gray_to_rgb(png);
    png_set_add_alpha(png, 0xFF, PNG_FILLER_AFTER);
    const int passes = png_set_interlace_handling(png);
    png_read_update_info(png, info);
    if (png_get_rowbytes(png, info) != static_cast<std::size_t>(w) * 4)
        png_error(png, "unexpected row layout");
    int dw, dh;
    fit_size(static_cast<int>(w), static_cast<int>(h), max_px, dw, dh);
    scaler = new RowScaler(static_cast<int>(w), static_cast<int>(h), dw, dh);
    if (passes > 1) {
        full.resize(static_cast<std::size_t>(w) * h * 4);
        rows.resize(h);
        for (png_uint_32 y = 0; y < h; ++y)
            rows[y] = full.data() + static_cast<std::size_t>(y) * w * 4;
        png_read_image(png, rows.data());
        for (png_uint_32 y = 0; y < h; ++y)
            scaler->push(rows[y]);
    } else {
        row.resize(static_cast<std::size_t>(w) * 4);
        for (png_uint_32 y = 0; y < h; ++y) {
            png_read_row(png, row.data(), nullptr);
            scaler->push(row.data());
        }
    }
    png_read_end(png, nullptr);
    png_destroy_read_struct(&png, &info, nullptr);
    std::fclose(f);
    out = scaler->take();
    delete scaler;
    return true;
}

} // namespace flo::thumb
