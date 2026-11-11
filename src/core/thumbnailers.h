// The installed external thumbnailers: [Thumbnailer Entry] files in
// $XDG_DATA_HOME/thumbnailers and each $XDG_DATA_DIRS/thumbnailers.
#pragma once

#include <map>
#include <string>
#include <vector>

namespace flo {

struct ThumbnailerEntry {
    std::string id;       // file name, e.g. "evince.thumbnailer"
    std::string exec;     // Exec= (unescaped value)
    std::string try_exec; // TryExec=
    std::vector<std::string> mime_types;
};

// Parses one .thumbnailer file's text. False when it has no usable
// [Thumbnailer Entry] (no Exec, or no MimeType).
bool parse_thumbnailer(const std::string &text, ThumbnailerEntry &out);

// Splits an Exec value into arguments following the desktop entry rules
// (double quotes; \" \` \$ \\ inside them) and expands field codes: %i input
// path, %u input URI, %o output path, %s size, %% a percent; other codes are
// dropped. Returns an empty vector for a malformed value.
std::vector<std::string> expand_thumbnailer_exec(const std::string &exec, const std::string &input_path,
                                                 const std::string &input_uri, const std::string &output,
                                                 int size);

class ThumbnailerRegistry {
public:
    // Loads entries from `dirs`, lowest priority first: a later directory's
    // file with the same name replaces an earlier one, and a later entry
    // takes a MIME type over. Entries whose TryExec isn't found are skipped.
    void load(const std::vector<std::string> &dirs);
    // The standard directories, lowest priority first: $XDG_DATA_DIRS in
    // reverse, then $XDG_DATA_HOME.
    static std::vector<std::string> standard_dirs();

    const ThumbnailerEntry *find(const std::string &mime) const;
    std::size_t size() const { return entries_.size(); }
    const std::map<std::string, ThumbnailerEntry> &entries() const { return entries_; }

private:
    std::map<std::string, ThumbnailerEntry> entries_; // by id
    std::map<std::string, std::string> by_mime_;      // mime -> id
};

} // namespace flo
