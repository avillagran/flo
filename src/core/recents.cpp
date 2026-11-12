#include "core/recents.h"
#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <algorithm>
#include <set>

#include <glib.h>
#include <sys/stat.h>

namespace flo::recents {

std::string xbel_path() { return xdg::data_home() + "/recently-used.xbel"; }

static int64_t date_of(GDateTime *dt) { return dt ? static_cast<int64_t>(g_date_time_to_unix(dt)) : 0; }

std::vector<Recent> read(const std::string &xbel, std::size_t max_bytes) {
    std::vector<Recent> out;
    std::string data;
    if (!read_file_capped(xbel, max_bytes, data))
        return out;
    GBookmarkFile *bf = g_bookmark_file_new();
    if (!g_bookmark_file_load_from_data(bf, data.data(), data.size(), nullptr)) {
        g_bookmark_file_free(bf);
        return out;
    }
    gsize n = 0;
    gchar **uris = g_bookmark_file_get_uris(bf, &n);
    for (gsize i = 0; i < n; ++i) {
        const std::string path = uri_to_path(uris[i]);
        if (path.empty())
            continue;
        Recent r;
        r.path = path;
        const int64_t visited = date_of(g_bookmark_file_get_visited_date_time(bf, uris[i], nullptr));
        const int64_t modified = date_of(g_bookmark_file_get_modified_date_time(bf, uris[i], nullptr));
        r.when = std::max(visited, modified);
        if (gchar *mime = g_bookmark_file_get_mime_type(bf, uris[i], nullptr)) {
            r.mime = mime;
            g_free(mime);
        }
        out.push_back(std::move(r));
    }
    g_strfreev(uris);
    g_bookmark_file_free(bf);
    std::stable_sort(out.begin(), out.end(), [](const Recent &a, const Recent &b) { return a.when > b.when; });
    std::set<std::string> seen;
    out.erase(std::remove_if(out.begin(), out.end(), [&](const Recent &r) { return !seen.insert(r.path).second; }),
              out.end());
    return out;
}

Listing list(const std::string &xbel, bool show_hidden, std::size_t limit, const CancelToken *cancel) {
    Listing l;
    l.folder = "recent://";
    EntryOptions opt;
    opt.sniff = false;
    for (const auto &r : read(xbel)) {
        if (cancel && cancel->cancelled())
            break;
        const std::string name = path_basename(r.path);
        if (!show_hidden && !name.empty() && name[0] == '.')
            continue;
        Entry e;
        if (!stat_path(r.path, e, opt))
            continue;
        e.name = r.path; // Recents span folders: the entry carries its full path
        l.entries.push_back(std::move(e));
        if (l.entries.size() >= limit)
            break;
    }
    return l;
}

bool add(const std::string &path, const std::string &mime, const std::string &xbel) {
    const std::string uri = path_to_uri(absolute_path(path));
    GBookmarkFile *bf = g_bookmark_file_new();
    std::string data;
    if (read_file_capped(xbel, 32u << 20, data) &&
        !g_bookmark_file_load_from_data(bf, data.data(), data.size(), nullptr)) {
        // An unreadable file is GTK's to repair; never replace it with ours.
        g_bookmark_file_free(bf);
        return false;
    }
    const gint before = g_bookmark_file_get_size(bf);
    const bool known = g_bookmark_file_has_item(bf, uri.c_str());
    if (!mime.empty())
        g_bookmark_file_set_mime_type(bf, uri.c_str(), mime.c_str());
    // Adds the application (and bumps its count and stamp), like GTK's
    // recent manager does.
    g_bookmark_file_add_application(bf, uri.c_str(), "flo", "flo %u");
    GDateTime *now = g_date_time_new_now_utc();
    g_bookmark_file_set_visited_date_time(bf, uri.c_str(), now);
    g_bookmark_file_set_modified_date_time(bf, uri.c_str(), now);
    g_date_time_unref(now);
    gsize len = 0;
    gchar *out = g_bookmark_file_to_data(bf, &len, nullptr);
    g_bookmark_file_free(bf);
    if (!out)
        return false;
    // GLib leaves out items that have no application; writing that back
    // would drop other apps' entries, so such a file is left as it is.
    GBookmarkFile *check = g_bookmark_file_new();
    const bool complete = g_bookmark_file_load_from_data(check, out, len, nullptr) &&
                          g_bookmark_file_get_size(check) == before + (known ? 0 : 1);
    g_bookmark_file_free(check);
    if (!complete) {
        g_free(out);
        return false;
    }
    const bool ok = make_dirs(path_dirname(xbel), 0700) && atomic_write_file(xbel, std::string(out, len), 0600);
    g_free(out);
    return ok;
}

int remove(const std::vector<std::string> &paths, const std::string &xbel) {
    std::string data;
    if (!read_file_capped(xbel, 32u << 20, data))
        return 0;
    GBookmarkFile *bf = g_bookmark_file_new();
    if (!g_bookmark_file_load_from_data(bf, data.data(), data.size(), nullptr)) {
        g_bookmark_file_free(bf);
        return -1;
    }
    const gint before = g_bookmark_file_get_size(bf);
    int removed = 0;
    gsize n = 0;
    gchar **uris = g_bookmark_file_get_uris(bf, &n);
    for (gsize i = 0; i < n; ++i) {
        const std::string p = uri_to_path(uris[i]);
        if (p.empty())
            continue;
        for (const auto &want : paths)
            if (clean_path(p) == clean_path(want)) {
                removed += g_bookmark_file_remove_item(bf, uris[i], nullptr) ? 1 : 0;
                break;
            }
    }
    g_strfreev(uris);
    if (removed == 0) {
        g_bookmark_file_free(bf);
        return 0;
    }
    gsize len = 0;
    gchar *out = g_bookmark_file_to_data(bf, &len, nullptr);
    g_bookmark_file_free(bf);
    if (!out)
        return -1;
    // As in add(): only a file that reads back with every other entry.
    GBookmarkFile *check = g_bookmark_file_new();
    const bool complete = g_bookmark_file_load_from_data(check, out, len, nullptr) &&
                          g_bookmark_file_get_size(check) == before - removed;
    g_bookmark_file_free(check);
    const bool ok = complete && atomic_write_file(xbel, std::string(out, len), 0600);
    g_free(out);
    return ok ? removed : -1;
}

} // namespace flo::recents
