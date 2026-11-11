// flo list|info|thumbnail|trash|restore [--json] PATH...
#include "cli.h"

#include "core/entry.h"
#include "core/event_queue.h"
#include "core/fsutil.h"
#include "core/json.h"
#include "core/lister.h"
#include "core/thumbnail_service.h"
#include "core/trash.h"
#include "core/uri.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

namespace flo::cli {

namespace {

struct Args {
    bool json = false, hidden = false;
    SortMode sort = SortMode::Name;
    int size = 256;
    bool list_trash = false;
    std::vector<std::string> paths;
    std::string error;
};

Args parse(int argc, char **argv) {
    Args a;
    for (int i = 2; i < argc; ++i) {
        const std::string s = argv[i];
        if (s == "--json") a.json = true;
        else if (s == "--hidden" || s == "-a") a.hidden = true;
        else if (s == "--list") a.list_trash = true;
        else if (s == "--sort" && i + 1 < argc) {
            if (!parse_sort_mode(argv[++i], a.sort))
                a.error = std::string("unknown sort mode: ") + argv[i];
        } else if (s == "--size" && i + 1 < argc) {
            a.size = std::atoi(argv[++i]);
            if (a.size < 1 || a.size > 1024)
                a.error = "size must be 1..1024";
        } else if (s == "--") {
            for (++i; i < argc; ++i)
                a.paths.push_back(argv[i]);
        } else if (s.size() > 1 && s[0] == '-') {
            a.error = "unknown option: " + s;
        } else {
            const std::string p = uri_to_path(s);
            a.paths.push_back(p.empty() ? s : p);
        }
    }
    return a;
}

std::string iso_time(int64_t ns) {
    const std::time_t t = static_cast<std::time_t>(ns / 1000000000LL);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[40];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S%z", &tm);
    return buf;
}

std::string entry_json(const Entry &e, const std::string &path) {
    std::string o = "{\"name\":" + json::quote(e.name) + ",\"path\":" + json::quote(path) +
                    ",\"type\":" + json::quote(e.is_dir ? "folder" : (e.is_link && e.broken_link ? "broken-link" : "file")) +
                    ",\"mime\":" + json::quote(e.mime) + ",\"kind\":" + json::quote(kind_name(e.kind)) +
                    ",\"size\":" + std::to_string(e.size) + ",\"modified\":" + json::quote(iso_time(e.mtime_ns)) +
                    ",\"hidden\":" + (e.hidden ? "true" : "false") + ",\"symlink\":" + (e.is_link ? "true" : "false");
    if (e.is_link)
        o += ",\"target\":" + json::quote(e.link_target);
    if (e.item_count >= 0)
        o += ",\"items\":" + std::to_string(e.item_count);
    return o + "}";
}

int fail(const std::string &msg) {
    std::fprintf(stderr, "flo: %s\n", msg.c_str());
    return 1;
}

int cmd_list(const Args &a) {
    const std::string folder = a.paths.empty() ? std::string(".") : a.paths[0];
    ListOptions opt;
    opt.show_hidden = a.hidden;
    opt.sort = a.sort;
    const Listing l = list_folder(absolute_path(folder), opt);
    if (!l.error.empty())
        return fail(folder + ": " + l.error);
    if (a.json) {
        std::string out = "{\"folder\":" + json::quote(l.folder) + ",\"entries\":[";
        for (std::size_t i = 0; i < l.entries.size(); ++i) {
            if (i)
                out += ',';
            out += entry_json(l.entries[i], l.path_of(i));
        }
        out += "]}\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
    } else {
        for (const auto &e : l.entries)
            std::printf("%s%s\n", e.name.c_str(), e.is_dir ? "/" : "");
    }
    return 0;
}

int cmd_info(const Args &a) {
    if (a.paths.empty())
        return fail("info needs a PATH");
    int status = 0;
    std::string out = a.json && a.paths.size() > 1 ? "[" : "";
    for (std::size_t i = 0; i < a.paths.size(); ++i) {
        const std::string path = absolute_path(a.paths[i]);
        Entry e;
        if (!stat_path(path, e, EntryOptions{})) {
            status = fail(a.paths[i] + ": " + std::strerror(errno));
            continue;
        }
        if (e.is_dir)
            e.item_count = count_items(path, a.hidden);
        if (a.json) {
            if (out.size() > 1)
                out += ',';
            out += entry_json(e, path);
        } else {
            std::printf("path: %s\ntype: %s\nmime: %s\nkind: %s\nsize: %lld\nmodified: %s\n", path.c_str(),
                        e.is_dir ? "folder" : "file", e.mime, kind_name(e.kind), static_cast<long long>(e.size),
                        iso_time(e.mtime_ns).c_str());
            if (e.is_link)
                std::printf("target: %s\n", e.link_target.c_str());
            if (e.item_count >= 0)
                std::printf("items: %d\n", e.item_count);
        }
    }
    if (a.json) {
        if (a.paths.size() > 1)
            out += "]";
        out += "\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return status;
}

int cmd_thumbnail(const Args &a) {
    if (a.paths.empty())
        return fail("thumbnail needs a PATH");
    EventQueue queue;
    ThumbnailService service(queue);
    std::size_t pending = a.paths.size();
    int status = 0;
    std::vector<ThumbnailResult> results;
    for (const auto &p : a.paths)
        service.request(p, a.size, [&](const ThumbnailResult &r) {
            results.push_back(r);
            --pending;
        });
    while (pending > 0) {
        queue.wait(-1);
        queue.dispatch();
    }
    std::string out = a.json ? "[" : "";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto &r = results[i];
        if (r.status != ThumbnailStatus::Ready) {
            status = 1;
            std::fprintf(stderr, "flo: %s: %s (%s)\n", r.path.c_str(), thumbnail_status_name(r.status), r.error.c_str());
        }
        if (a.json) {
            if (i)
                out += ',';
            out += "{\"path\":" + json::quote(r.path) + ",\"status\":" + json::quote(thumbnail_status_name(r.status)) +
                   ",\"thumbnail\":" + json::quote(r.thumbnail) + ",\"size\":" + std::to_string(r.thumbnail_pixels) +
                   ",\"error\":" + json::quote(r.error) + "}";
        } else if (r.status == ThumbnailStatus::Ready) {
            std::printf("%s\n", r.thumbnail.c_str());
        }
    }
    if (a.json) {
        out += "]\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return status;
}

std::string item_json(const trash::Item &i) {
    return "{\"name\":" + json::quote(i.name) + ",\"trashed\":" + json::quote(i.files_path()) +
           ",\"original\":" + json::quote(i.original_path) + ",\"deleted\":" + json::quote(i.deletion_date) +
           ",\"folder\":" + (i.is_dir ? "true" : "false") + ",\"size\":" + std::to_string(i.size) + "}";
}

int cmd_trash(const Args &a) {
    if (a.list_trash) {
        const auto items = trash::list();
        if (a.json) {
            std::string out = "[";
            for (std::size_t i = 0; i < items.size(); ++i)
                out += (i ? "," : "") + item_json(items[i]);
            out += "]\n";
            std::fwrite(out.data(), 1, out.size(), stdout);
        } else {
            for (const auto &i : items)
                std::printf("%s\t%s\t%s\n", i.deletion_date.c_str(), i.original_path.c_str(), i.files_path().c_str());
        }
        return 0;
    }
    if (a.paths.empty())
        return fail("trash needs a PATH (or --list)");
    int status = 0;
    std::string out = a.json ? "[" : "";
    bool first = true;
    for (const auto &p : a.paths) {
        trash::Receipt r;
        std::string error;
        if (!trash::put(p, r, error)) {
            status = fail(p + ": " + error);
            continue;
        }
        if (a.json) {
            out += (first ? "" : ",") + std::string("{\"original\":") + json::quote(r.original_path) +
                   ",\"trashed\":" + json::quote(r.files_path()) + "}";
            first = false;
        } else {
            std::printf("%s\n", r.files_path().c_str());
        }
    }
    if (a.json) {
        out += "]\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return status;
}

// PATH is an item in a trash's files/ folder, or an original path (the
// most recently trashed item from there is restored).
int cmd_restore(const Args &a) {
    if (a.paths.empty())
        return fail("restore needs a PATH");
    int status = 0;
    std::string out = a.json ? "[" : "";
    bool first = true;
    for (const auto &p : a.paths) {
        trash::Item item;
        bool found = trash::item_for_trashed_path(p, item);
        if (!found) {
            const std::string original = absolute_path(p);
            for (const auto &i : trash::list())
                if (i.original_path == original) {
                    item = i;
                    found = true;
                    break; // newest first
                }
        }
        if (!found) {
            status = fail(p + ": not in the trash");
            continue;
        }
        std::string to, error;
        if (!trash::restore(item, trash::OnConflict::KeepBoth, to, error)) {
            status = fail(p + ": " + error);
            continue;
        }
        if (a.json) {
            out += (first ? "" : ",") + std::string("{\"trashed\":") + json::quote(item.files_path()) +
                   ",\"restored\":" + json::quote(to) + "}";
            first = false;
        } else {
            std::printf("%s\n", to.c_str());
        }
    }
    if (a.json) {
        out += "]\n";
        std::fwrite(out.data(), 1, out.size(), stdout);
    }
    return status;
}

void usage(FILE *f) {
    std::fprintf(f,
                 "usage: flo [PATH]                  open a window (a file: its folder, selected)\n"
                 "       flo list [--json] [--hidden] [--sort name|kind|modified|oldest|size] [PATH]\n"
                 "       flo info [--json] PATH...\n"
                 "       flo thumbnail [--json] [--size N] PATH...\n"
                 "       flo trash [--json] PATH...   |   flo trash --list [--json]\n"
                 "       flo restore [--json] PATH... (a trashed item, or the original path)\n");
}

} // namespace

bool is_command(int argc, char **argv) {
    if (argc < 2)
        return false;
    const std::string c = argv[1];
    return c == "list" || c == "info" || c == "thumbnail" || c == "trash" || c == "restore" || c == "help" ||
           c == "--help" || c == "-h" || c == "version" || c == "--version";
}

int run(int argc, char **argv) {
    const std::string c = argv[1];
    if (c == "help" || c == "--help" || c == "-h") {
        usage(stdout);
        return 0;
    }
    if (c == "version" || c == "--version") {
        std::printf("flo %s\n", FLO_VERSION);
        return 0;
    }
    const Args a = parse(argc, argv);
    if (!a.error.empty()) {
        std::fprintf(stderr, "flo: %s\n", a.error.c_str());
        usage(stderr);
        return 2;
    }
    if (c == "list") return cmd_list(a);
    if (c == "info") return cmd_info(a);
    if (c == "thumbnail") return cmd_thumbnail(a);
    if (c == "trash") return cmd_trash(a);
    if (c == "restore") return cmd_restore(a);
    usage(stderr);
    return 2;
}

} // namespace flo::cli
