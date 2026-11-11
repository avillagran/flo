#include "thumbnailer/generate.h"
#include "thumbnailer/decode.h"
#include "thumbnailer/png_write.h"

#include "core/entry.h"
#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace flo::thumb {

const char *outcome_name(Outcome o) {
    switch (o) {
    case Outcome::Ready: return "ok";
    case Outcome::Failed: return "fail";
    case Outcome::Unsupported: return "none";
    case Outcome::Skipped: return "skip";
    }
    return "fail";
}

bool ensure_private_dir(const std::string &dir) {
    struct stat st;
    if (::lstat(dir.c_str(), &st) != 0) {
        if (errno != ENOENT)
            return false;
        const std::string parent = path_dirname(dir);
        if (parent != dir && !exists_nofollow(parent) && !ensure_private_dir(parent))
            return false;
        if (::mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST)
            return false;
        if (::lstat(dir.c_str(), &st) != 0)
            return false;
    }
    return S_ISDIR(st.st_mode) && st.st_uid == ::getuid() && (st.st_mode & 022) == 0;
}

Generator::Generator(ThumbnailCache cache, ThumbnailerRegistry registry)
    : cache_(std::move(cache)), registry_(std::move(registry)) {
    child_limits.address_space = 2ull << 30; // glycin and ffmpeg map a lot
    child_limits.cpu_seconds = 20;
    child_limits.file_size = 64ull << 20;
    child_limits.open_files = 512;
}

Generator::~Generator() {
    if (!scratch_.empty())
        ::rmdir(scratch_.c_str()); // only if empty: never a tree
}

bool Generator::scratch_dir(std::string &dir) {
    if (scratch_.empty()) {
        std::string base = xdg::runtime_dir();
        if (base.empty()) {
            const char *t = std::getenv("TMPDIR");
            base = t && *t == '/' ? t : "/tmp";
        }
        std::string tmpl = base + "/flo-thumbnailer-XXXXXX";
        if (!::mkdtemp(&tmpl[0]))
            return false;
        scratch_ = tmpl;
    }
    dir = scratch_;
    return true;
}

bool Generator::store(const std::string &target, const Image &image, const std::string &path,
                      const struct stat &st) {
    if (!ensure_private_dir(cache_.root()) || !ensure_private_dir(path_dirname(target)))
        return false;
    TextChunks text = {
        {"Thumb::URI", path_to_uri(path)},
        {"Thumb::MTime", std::to_string(static_cast<long long>(st.st_mtim.tv_sec))},
        {"Thumb::Size", std::to_string(static_cast<long long>(st.st_size))},
        {"Software", "flo"},
    };
    std::string png;
    if (!encode_png(image, text, png))
        return false;
    return atomic_write_file(target, png, 0600);
}

bool Generator::mark_failed(const std::string &path) {
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || cache_.inside_cache(path))
        return false;
    Image marker;
    marker.width = marker.height = 1;
    marker.rgba = {0, 0, 0, 0};
    marker.has_alpha = true;
    const std::string target = cache_.fail_path_for_uri(path_to_uri(path));
    return ensure_private_dir(cache_.root() + "/fail") && store(target, marker, path, st);
}

bool Generator::run_external(const ThumbnailerEntry &t, const std::string &path, int pixels, Image &out,
                             std::string &error) {
    std::string dir;
    if (!scratch_dir(dir)) {
        error = "no scratch directory";
        return false;
    }
    const std::string output = dir + "/out-" + std::to_string(counter_++) + ".png";
    const auto argv = expand_thumbnailer_exec(t.exec, path, path_to_uri(path), output, pixels);
    if (argv.empty()) {
        error = "malformed Exec in " + t.id;
        return false;
    }
    RunOptions opt;
    opt.timeout_ms = child_timeout_ms;
    opt.limits = child_limits;
    opt.max_output = 16 * 1024;
    opt.cwd = dir;
    const ProcessResult r = run_process(argv, opt);
    bool ok = false;
    if (!r.started)
        error = argv[0] + " could not be started";
    else if (r.timed_out)
        error = t.id + " timed out";
    else if (!r.ok())
        error = t.id + " failed";
    else
        ok = true;
    if (ok) {
        struct stat st;
        // The output is untrusted too: a regular file of bounded size.
        if (::lstat(output.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > (32 << 20)) {
            error = t.id + " wrote no usable output";
            ok = false;
        } else if (!decode_png(output, pixels, out, error)) {
            error = t.id + ": " + error;
            ok = false;
        }
    }
    ::unlink(output.c_str()); // the one scratch file this run created
    return ok;
}

Outcome Generator::make(const std::string &path_in, const std::string &mime_hint, int pixels, std::string &thumbnail,
                        std::string &error) {
    const std::string path = absolute_path(path_in);
    struct stat st;
    if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
        error = "not a regular file";
        return Outcome::Skipped;
    }
    if (cache_.inside_cache(path)) {
        error = "inside the thumbnail cache";
        return Outcome::Skipped;
    }
    ThumbnailCache::Hit hit;
    if (cache_.lookup(path, st, pixels, hit)) {
        thumbnail = hit.path;
        return Outcome::Ready;
    }
    if (cache_.failed_before(path, st)) {
        error = "failed before";
        return Outcome::Failed;
    }
    std::string mime = mime_hint;
    if (mime.empty() || mime == "application/octet-stream") {
        const int dfd = ::open(path_dirname(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dfd >= 0) {
            mime = mime_for_file(dfd, path_basename(path), st, true);
            ::close(dfd);
        }
    }
    const int px = ThumbnailCache::bucket_pixels(ThumbnailCache::bucket_for(pixels));
    Image image;
    bool attempted = false, ok = false;
    if (mime == "image/jpeg") {
        attempted = true;
        ok = decode_jpeg(path, px, image, error);
    } else if (mime == "image/png") {
        attempted = true;
        ok = decode_png(path, px, image, error);
    }
    if (!ok) {
        if (const ThumbnailerEntry *t = registry_.find(mime)) {
            attempted = true;
            error.clear();
            ok = run_external(*t, path, px, image, error);
        }
    }
    if (!ok) {
        // Only a decoder or thumbnailer that ran and failed is remembered,
        // so one installed later still gets its turn.
        if (!attempted) {
            error = "no thumbnailer for " + mime;
            return Outcome::Unsupported;
        }
        mark_failed(path);
        return Outcome::Failed;
    }
    if (image.width > px || image.height > px)
        image = downscale(image, px);
    // The file may have changed while it was read; the thumbnail is
    // labelled with the revision it was made from only if it didn't.
    struct stat after;
    if (::stat(path.c_str(), &after) != 0 || after.st_mtim.tv_sec != st.st_mtim.tv_sec ||
        after.st_size != st.st_size) {
        error = "changed while thumbnailing";
        return Outcome::Skipped;
    }
    const std::string target = cache_.path_for_uri(path_to_uri(path), px);
    if (!store(target, image, path, st)) {
        error = "couldn't write " + target;
        return Outcome::Failed;
    }
    thumbnail = target;
    return Outcome::Ready;
}

} // namespace flo::thumb
