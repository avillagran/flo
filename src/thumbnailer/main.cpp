// flo-thumbnailer: the thumbnail worker process. ThumbnailService starts a
// few of these and talks to them over stdin/stdout (core/thumbnail_protocol.h).
// Decoders and external thumbnailers run here, never in the UI process.
//
//   flo-thumbnailer                  serve requests on stdin
//   flo-thumbnailer --once [--size N] FILE
//                                    make one thumbnail, print its path
// The cache root is $XDG_CACHE_HOME/thumbnails unless FLO_THUMBNAIL_CACHE
// names another (tests use that).

#include "core/process.h"
#include "core/thumbnail_protocol.h"
#include "thumbnailer/generate.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <sys/resource.h>

using namespace flo;

static void limit_self() {
    // Soft limits only, so external thumbnailers can be given their own.
    rlimit r;
    if (::getrlimit(RLIMIT_AS, &r) == 0) {
        r.rlim_cur = std::min<rlim_t>(r.rlim_max, 1ull << 30);
        ::setrlimit(RLIMIT_AS, &r);
    }
    r.rlim_cur = r.rlim_max = 0;
    ::setrlimit(RLIMIT_CORE, &r);
}

static thumb::Generator make_generator() {
    const char *root = std::getenv("FLO_THUMBNAIL_CACHE");
    ThumbnailCache cache(root && *root == '/' ? std::string(root) : ThumbnailCache::default_root());
    ThumbnailerRegistry registry;
    registry.load(ThumbnailerRegistry::standard_dirs());
    return thumb::Generator(std::move(cache), std::move(registry));
}

int main(int argc, char **argv) {
    std::signal(SIGPIPE, SIG_IGN);
    limit_self();
    thumb::Generator gen = make_generator();

    if (argc >= 2 && std::strcmp(argv[1], "--once") == 0) {
        int size = 256;
        std::string file;
        for (int i = 2; i < argc; ++i) {
            if (!std::strcmp(argv[i], "--size") && i + 1 < argc)
                size = std::atoi(argv[++i]);
            else
                file = argv[i];
        }
        if (file.empty()) {
            std::fprintf(stderr, "usage: flo-thumbnailer --once [--size N] FILE\n");
            return 2;
        }
        std::string thumbnail, error;
        const auto o = gen.make(file, "", size, thumbnail, error);
        if (o == thumb::Outcome::Ready) {
            std::printf("%s\n", thumbnail.c_str());
            return 0;
        }
        std::fprintf(stderr, "%s: %s\n", thumb::outcome_name(o), error.c_str());
        return 1;
    }

    char *buf = nullptr;
    size_t cap = 0;
    ssize_t len;
    while ((len = ::getline(&buf, &cap, stdin)) >= 0) {
        std::string line(buf, static_cast<std::size_t>(len));
        if (!line.empty() && line.back() == '\n')
            line.pop_back();
        const auto f = thumbproto::split_tabs(line);
        if (f.empty())
            continue;
        std::string reply;
        if (f[0] == "T" && f.size() >= 5) {
            std::string thumbnail, error;
            const std::string mime = f[4] == "-" ? std::string() : f[4];
            const auto o = gen.make(thumbproto::decode(f[3]), mime, std::atoi(f[2].c_str()), thumbnail, error);
            reply = f[1] + "\t" + thumb::outcome_name(o) + "\t" +
                    thumbproto::encode(o == thumb::Outcome::Ready ? thumbnail : error);
        } else if (f[0] == "F" && f.size() >= 3) {
            gen.mark_failed(thumbproto::decode(f[2]));
            reply = f[1] + "\tfail\tmarked";
        } else if (f[0] == "Q") {
            break;
        } else {
            continue;
        }
        reply += '\n';
        std::fwrite(reply.data(), 1, reply.size(), stdout);
        std::fflush(stdout);
    }
    std::free(buf);
    return 0;
}
