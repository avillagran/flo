// Files' type-to-filter syntax (files-rust crates/core name_filter.rs):
// words separated by spaces, all of which must match a name.
//
//   pho          the name contains "pho" (case-insensitive)
//   *.rs  a?c    a glob over the whole name (* and ?)
//   ext:pdf,md   the extension is one of these
//   type:dir     folders only (type:file for the rest)
//   !draft       negates the word after "!"
#pragma once

#include "core/entry.h"

#include <string>
#include <vector>

namespace flo {

class NameFilter {
public:
    explicit NameFilter(const std::string &text = {});
    bool empty() const { return words_.empty(); }
    // Why the text can't be used ("ext: needs an extension"), or "".
    const std::string &error() const { return error_; }
    bool matches(const Entry &e) const;
    bool matches(const std::string &name, bool is_dir) const;

private:
    enum class Kind { Contains, Glob, Ext, Type };
    struct Word {
        Kind kind = Kind::Contains;
        bool negate = false;
        std::string text;             // lowercased
        std::vector<std::string> exts; // lowercased, without dots
        bool dirs = false;
    };
    std::vector<Word> words_;
    std::string error_;
};

// Case-insensitive (ASCII) glob over a whole string: * and ?.
bool glob_match(const std::string &pattern, const std::string &text);

} // namespace flo
