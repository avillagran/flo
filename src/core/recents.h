// Recents: GTK's recently-used.xbel ($XDG_DATA_HOME/recently-used.xbel),
// which GTK and many other apps add to. Flo reads it for the Recents view
// and adds an entry when it opens a file, the way GTK apps do.
#pragma once

#include "core/event_queue.h"
#include "core/lister.h"

#include <cstdint>
#include <string>
#include <vector>

namespace flo::recents {

std::string xbel_path();

struct Recent {
    std::string path;  // local file
    int64_t when = 0;  // seconds: the later of visited and modified
    std::string mime;
};

// Local entries, most recently used first, duplicates removed. Missing
// files are kept (the caller decides); `max_bytes` bounds the file read.
std::vector<Recent> read(const std::string &xbel = xbel_path(), std::size_t max_bytes = 32u << 20);

// Folio's Recents view: the files still there (hidden ones only when
// shown), most recent first, at most `limit`. Each existence check can wait
// on a slow drive, so it stops when cancelled.
Listing list(const std::string &xbel, bool show_hidden, std::size_t limit = 200, const CancelToken *cancel = nullptr);

// Records that Flo opened `path` (application "flo", exec "flo %u"),
// writing the file atomically with mode 0600.
bool add(const std::string &path, const std::string &mime, const std::string &xbel = xbel_path());

// Files' "Remove from Recents": takes the entries of these local paths off
// the list (the files are never opened or changed). Other apps' entries
// stay; a file GLib can't read back whole is left untouched. Returns how
// many entries went, or -1 when the file couldn't be changed.
int remove(const std::vector<std::string> &paths, const std::string &xbel = xbel_path());

} // namespace flo::recents
