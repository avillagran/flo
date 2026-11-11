// Shared thumbnail cache, thumbnailer entries, generation in the worker.
#include "harness.h"

#include "core/event_queue.h"
#include "core/fsutil.h"
#include "core/thumbnail_cache.h"
#include "core/thumbnail_service.h"
#include "core/thumbnailers.h"
#include "core/uri.h"
#include "thumbnailer/decode.h"
#include "thumbnailer/generate.h"
#include "thumbnailer/png_write.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <glib.h>
#include <jpeglib.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

static thumb::Image test_image(int w, int h, bool alpha = false) {
    thumb::Image img;
    img.width = w;
    img.height = h;
    img.has_alpha = alpha;
    img.rgba.resize(static_cast<std::size_t>(w) * h * 4);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &img.rgba[(static_cast<std::size_t>(y) * w + x) * 4];
            p[0] = static_cast<uint8_t>(x);
            p[1] = static_cast<uint8_t>(y);
            p[2] = 128;
            p[3] = alpha ? static_cast<uint8_t>(x) : 255;
        }
    return img;
}

static std::string write_png(const std::string &path, const thumb::Image &img, const thumb::TextChunks &text = {}) {
    std::string png;
    if (!thumb::encode_png(img, text, png))
        std::abort();
    return write_file(path, png);
}

static void write_jpeg(const std::string &path, int w, int h) {
    assert_inside(path);
    FILE *f = std::fopen(path.c_str(), "wbx");
    jpeg_compress_struct c;
    jpeg_error_mgr err;
    c.err = jpeg_std_error(&err);
    jpeg_create_compress(&c);
    jpeg_stdio_dest(&c, f);
    c.image_width = static_cast<JDIMENSION>(w);
    c.image_height = static_cast<JDIMENSION>(h);
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_start_compress(&c, TRUE);
    std::vector<uint8_t> row(static_cast<std::size_t>(w) * 3);
    while (c.next_scanline < c.image_height) {
        for (int x = 0; x < w; ++x) {
            row[x * 3] = static_cast<uint8_t>(x);
            row[x * 3 + 1] = static_cast<uint8_t>(c.next_scanline);
            row[x * 3 + 2] = 200;
        }
        JSAMPROW r[1] = {row.data()};
        jpeg_write_scanlines(&c, r, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    std::fclose(f);
}

static thumb::TextChunks meta_for(const std::string &file) {
    struct stat st;
    ::stat(file.c_str(), &st);
    return {{"Thumb::URI", path_to_uri(file)},
            {"Thumb::MTime", std::to_string(static_cast<long long>(st.st_mtim.tv_sec))},
            {"Thumb::Size", std::to_string(static_cast<long long>(st.st_size))}};
}

TEST(cache_key_bucket_and_paths) {
    const std::string uri = path_to_uri("/home/user/a b/ñ.png");
    CHECK_EQ(uri, std::string("file:///home/user/a%20b/%C3%B1.png"));
    gchar *md5 = g_compute_checksum_for_string(G_CHECKSUM_MD5, uri.c_str(), -1);
    CHECK_EQ(ThumbnailCache::key_for_uri(uri), std::string(md5) + ".png");
    g_free(md5);
    // Known value from the spec's example URI.
    CHECK_EQ(ThumbnailCache::key_for_uri("file:///home/jens/photos/me.png"),
             std::string("c6ee772d9e49320e97ec29a7eb5b1697.png"));
    CHECK_EQ(std::string(ThumbnailCache::bucket_for(64)), std::string("normal"));
    CHECK_EQ(std::string(ThumbnailCache::bucket_for(128)), std::string("normal"));
    CHECK_EQ(std::string(ThumbnailCache::bucket_for(129)), std::string("large"));
    CHECK_EQ(std::string(ThumbnailCache::bucket_for(512)), std::string("x-large"));
    CHECK_EQ(std::string(ThumbnailCache::bucket_for(513)), std::string("xx-large"));
    CHECK_EQ(std::string(ThumbnailCache::bucket_for(4000)), std::string("xx-large"));
    ThumbnailCache c("/r/thumbnails");
    CHECK_EQ(c.path_for_uri(uri, 256), "/r/thumbnails/large/" + ThumbnailCache::key_for_uri(uri));
    CHECK_EQ(c.fail_path_for_uri(uri), "/r/thumbnails/fail/flo/" + ThumbnailCache::key_for_uri(uri));
    CHECK(c.inside_cache("/r/thumbnails/normal/x.png"));
    CHECK(!c.inside_cache("/r/thumbs.png"));
    // The default root honours XDG_CACHE_HOME (private in tests).
    CHECK(path_inside(ThumbnailCache::default_root(), std::getenv("FLO_TEST_RUN")));
}

TEST(untrusted_cache_validation) {
    const std::string d = private_dir("cachevalid");
    const std::string root = d + "/thumbnails";
    ThumbnailCache cache(root);
    const std::string src = write_file(d + "/photo.png", "pretend");
    struct stat st;
    ::stat(src.c_str(), &st);
    const std::string uri = path_to_uri(src);
    ::mkdir(root.c_str(), 0700);
    ::mkdir((root + "/large").c_str(), 0700);
    ::mkdir((root + "/normal").c_str(), 0700);
    ThumbnailCache::Hit hit;
    CHECK(!cache.lookup(src, st, 256, hit));
    // Another app's thumbnail in `large` is reused, and also serves `normal`.
    write_png(cache.path_for_uri(uri, 256), test_image(200, 100), meta_for(src));
    CHECK(cache.lookup(src, st, 256, hit) && hit.pixels == 256);
    CHECK(cache.lookup(src, st, 100, hit) && hit.pixels == 256);
    CHECK(!cache.lookup(src, st, 512, hit)); // smaller buckets never serve bigger requests
    PngMeta meta;
    CHECK(read_png_meta(hit.path, 1 << 20, 256, meta));
    CHECK_EQ(meta.width, 200);
    CHECK_EQ(meta.text["Thumb::URI"], uri);

    // Wrong MTime, wrong Size, wrong URI: misses.
    auto m = meta_for(src);
    m[1].second = "12345";
    write_png(cache.path_for_uri(uri, 128), test_image(10, 10), m);
    CHECK(read_png_meta(cache.path_for_uri(uri, 128), 1 << 20, 128, meta) && !thumbnail_matches(meta, uri, st));
    m = meta_for(src);
    m[2].second = "999";
    CHECK(!thumbnail_matches(PngMeta{1, 1, {{"Thumb::URI", uri}, {"Thumb::MTime", m[1].second}, {"Thumb::Size", "999"}}}, uri, st));
    CHECK(!thumbnail_matches(PngMeta{1, 1, {{"Thumb::URI", uri + "x"}, {"Thumb::MTime", m[1].second}}}, uri, st));
    CHECK(thumbnail_matches(PngMeta{1, 1, {{"Thumb::URI", uri}, {"Thumb::MTime", m[1].second}}}, uri, st));

    // Duplicate Thumb:: keys, oversize dimensions, symlinks, corrupt CRC.
    std::string png;
    thumb::TextChunks dup = meta_for(src);
    dup.push_back({"Thumb::URI", uri});
    thumb::encode_png(test_image(4, 4), dup, png);
    CHECK(!parse_png_meta(png, 128, meta));
    thumb::encode_png(test_image(300, 10), meta_for(src), png);
    CHECK(!parse_png_meta(png, 256, meta));
    CHECK(parse_png_meta(png, 512, meta));
    std::string corrupt = png;
    const auto at = corrupt.find("Thumb::URI");
    corrupt[at + 12] ^= 1;
    CHECK(!parse_png_meta(corrupt, 512, meta));
    CHECK(!parse_png_meta(png.substr(0, png.size() - 12), 512, meta)); // no IEND
    CHECK(!parse_png_meta("not a png at all, not at all, nope", 512, meta));
    const std::string target = d + "/real.png";
    write_png(target, test_image(10, 10), meta_for(src));
    CHECK(read_png_meta(target, 1 << 20, 128, meta));
    CHECK(::symlink(target.c_str(), (d + "/link.png").c_str()) == 0);
    CHECK(!read_png_meta(d + "/link.png", 1 << 20, 128, meta));
    CHECK(!read_png_meta(target, 10, 128, meta)); // over the size cap
}

TEST(thumbnailer_entries_parse_and_expand) {
    ThumbnailerEntry e;
    CHECK(parse_thumbnailer("[Thumbnailer Entry]\nTryExec=evince-thumbnailer\nExec=evince-thumbnailer -s %s %u %o\n"
                            "MimeType=application/pdf;image/tiff;\n[Other]\nExec=nope\n", e));
    CHECK_EQ(e.exec, std::string("evince-thumbnailer -s %s %u %o"));
    CHECK_EQ(e.mime_types.size(), static_cast<std::size_t>(2));
    CHECK(!parse_thumbnailer("[Thumbnailer Entry]\nMimeType=a/b;\n", e) || e.exec.empty() == false);
    ThumbnailerEntry none;
    CHECK(!parse_thumbnailer("[Desktop Entry]\nExec=x %i\nMimeType=a/b;\n", none));
    auto args = expand_thumbnailer_exec("tool \"--in=%i\" --uri %u -o %o --size=%s 100%% %k", "/a b/c.pdf",
                                        "file:///a%20b/c.pdf", "/tmp/o.png", 256);
    const std::vector<std::string> want{"tool", "--in=/a b/c.pdf", "--uri", "file:///a%20b/c.pdf", "-o",
                                        "/tmp/o.png", "--size=256", "100%", ""};
    CHECK(args == want);
    CHECK(expand_thumbnailer_exec("\"unterminated", "", "", "", 1).empty());
    CHECK((expand_thumbnailer_exec("a \"b \\\"q\\\" \\\\ \\$x\"", "", "", "", 1) ==
           std::vector<std::string>{"a", "b \"q\" \\ $x"}));
}

TEST(thumbnailer_registry_priority_and_tryexec) {
    const std::string d = private_dir("registry");
    ::mkdir((d + "/sys").c_str(), 0700);
    ::mkdir((d + "/user").c_str(), 0700);
    write_file(d + "/sys/a.thumbnailer", "[Thumbnailer Entry]\nExec=sh -c sys %i\nMimeType=x/one;x/two;\n");
    write_file(d + "/sys/b.thumbnailer", "[Thumbnailer Entry]\nTryExec=flo-no-such-program\nExec=nope %i\nMimeType=x/three;\n");
    write_file(d + "/sys/c.thumbnailer", "[Thumbnailer Entry]\nExec=sh c %i\nMimeType=x/four;\n");
    write_file(d + "/user/a.thumbnailer", "[Thumbnailer Entry]\nExec=sh user %i\nMimeType=x/one;\n");
    write_file(d + "/user/z.thumbnailer", "[Thumbnailer Entry]\nTryExec=sh\nExec=sh z %i\nMimeType=x/four;\n");
    ThumbnailerRegistry r;
    r.load({d + "/sys", d + "/user"});
    CHECK(r.find("x/one") && r.find("x/one")->exec == "sh user %i");
    CHECK(r.find("x/two") == nullptr); // the user's a.thumbnailer replaced the system file whole
    CHECK(r.find("x/three") == nullptr);
    CHECK(r.find("x/four") && r.find("x/four")->id == "z.thumbnailer");
    // The real system directories parse (whatever is installed).
    ThumbnailerRegistry sys;
    sys.load(ThumbnailerRegistry::standard_dirs());
    std::printf("     installed thumbnailers usable: %zu\n", sys.size());
}

TEST(generator_makes_jpeg_and_png_thumbnails) {
    const std::string d = private_dir("generate");
    const std::string root = d + "/cache/thumbnails";
    ThumbnailerRegistry none;
    thumb::Generator gen{ThumbnailCache(root), none};
    write_jpeg(d + "/photo.jpg", 1600, 1200);
    write_png(d + "/pic.png", test_image(300, 600, true));
    std::string out, error;
    CHECK(gen.make(d + "/photo.jpg", "", 256, out, error) == thumb::Outcome::Ready);
    struct stat st;
    CHECK(::stat(out.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600);
    CHECK(::stat(root.c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    CHECK(::stat((root + "/large").c_str(), &st) == 0 && (st.st_mode & 0777) == 0700);
    PngMeta meta;
    CHECK(read_png_meta(out, 2 << 20, 256, meta));
    CHECK(meta.width == 256 && meta.height == 192);
    CHECK_EQ(meta.text["Thumb::URI"], path_to_uri(d + "/photo.jpg"));
    CHECK_EQ(meta.text["Software"], std::string("flo"));
    // All keys are plain tEXt, never zTXt.
    const std::string bytes = read_file(out);
    CHECK(bytes.find("zTXt") == std::string::npos && bytes.find("tEXt") != std::string::npos);
    thumb::Image back;
    CHECK(thumb::decode_png(out, 1024, back, error) && back.width == 256);

    CHECK(gen.make(d + "/pic.png", "", 100, out, error) == thumb::Outcome::Ready);
    CHECK(read_png_meta(out, 1 << 20, 128, meta) && meta.width == 64 && meta.height == 128);
    CHECK(out.find("/normal/") != std::string::npos);

    // Broken JPEG: a failure marker in fail/flo, honoured next time.
    write_file(d + "/broken.jpg", "\xff\xd8\xff garbage");
    CHECK(gen.make(d + "/broken.jpg", "", 256, out, error) == thumb::Outcome::Failed);
    ThumbnailCache cache(root);
    ::stat((d + "/broken.jpg").c_str(), &st);
    CHECK(cache.failed_before(d + "/broken.jpg", st));
    CHECK(::stat(cache.fail_path_for_uri(path_to_uri(d + "/broken.jpg")).c_str(), &st) == 0);
    CHECK(gen.make(d + "/broken.jpg", "", 256, out, error) == thumb::Outcome::Failed);
    CHECK_EQ(error, std::string("failed before"));
    // Unknown type: no marker.
    write_file(d + "/data.flobin", std::string("\x01\x02\x03\x04", 4));
    CHECK(gen.make(d + "/data.flobin", "", 256, out, error) == thumb::Outcome::Unsupported);
    // The cache itself is never thumbnailed.
    CHECK(gen.make(root + "/large/" + path_basename(out), "", 256, out, error) == thumb::Outcome::Skipped);
}

TEST(thumbnail_service_uses_worker_process) {
    const char *worker = std::getenv("FLO_THUMBNAILER");
    CHECK(worker != nullptr);
    if (!worker)
        return;
    const std::string d = private_dir("service");
    const std::string root = d + "/thumbnails";
    write_jpeg(d + "/a.jpg", 640, 480);
    write_png(d + "/b.png", test_image(100, 50));
    write_file(d + "/c.unknownext", "zzz");
    EventQueue q;
    ThumbnailServiceOptions opt;
    opt.cache_root = root;
    opt.workers = 2;
    ThumbnailService s(q, opt);
    std::vector<ThumbnailResult> results;
    s.request(d + "/a.jpg", 256, [&](const ThumbnailResult &r) { results.push_back(r); });
    s.request(d + "/b.png", 128, [&](const ThumbnailResult &r) { results.push_back(r); });
    s.request(d + "/c.unknownext", 128, [&](const ThumbnailResult &r) { results.push_back(r); });
    const uint64_t cancelled = s.request(d + "/a.jpg", 512, [&](const ThumbnailResult &r) { results.push_back(r); });
    s.cancel(cancelled);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (results.size() < 3 && std::chrono::steady_clock::now() < end) {
        q.wait(100);
        q.dispatch();
    }
    CHECK_EQ(results.size(), static_cast<std::size_t>(3));
    int ready = 0;
    for (auto &r : results) {
        if (r.path == d + "/c.unknownext")
            CHECK(r.status == ThumbnailStatus::Unsupported);
        else if (r.status == ThumbnailStatus::Ready && path_inside(r.thumbnail, root))
            ++ready;
    }
    CHECK_EQ(ready, 2);
    // Second time: answered from the cache.
    ThumbnailCache::Hit hit;
    CHECK(s.lookup(d + "/a.jpg", 256, hit) && hit.pixels == 256);
}
