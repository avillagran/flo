// The line protocol between ThumbnailService and flo-thumbnailer.
//
// Requests (one per line, fields separated by tabs, paths percent-encoded so
// they never contain a tab or newline):
//   T <id> <pixels> <path> <mime>   make (or find) a thumbnail
//   F <id> <path>                   write a failure marker (the previous
//                                   worker died or hung on this file)
// Replies:
//   <id> ok <thumbnail-path>
//   <id> fail|none|skip <reason>
#pragma once

#include "core/uri.h"

#include <string>
#include <vector>

namespace flo::thumbproto {

inline std::vector<std::string> split_tabs(const std::string &line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos)
            break;
        start = tab + 1;
    }
    return out;
}

inline std::string encode(const std::string &s) { return percent_encode_path(s); }

inline std::string decode(const std::string &s) {
    std::string out;
    return percent_decode(s, out) ? out : std::string();
}

} // namespace flo::thumbproto
