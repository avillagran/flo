#include "core/name_filter.h"

#include <cctype>

namespace flo {

namespace {

std::string lower(std::string s) {
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string extension_of(const std::string &name) {
    size_t slash = name.rfind('/');
    std::string base = slash == std::string::npos ? name : name.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot == std::string::npos || dot == 0)
        return {};
    return lower(base.substr(dot + 1));
}

} // namespace

bool glob_match(const std::string &p, const std::string &t) {
    size_t pi = 0, ti = 0, star = std::string::npos, mark = 0;
    while (ti < t.size()) {
        char pc = pi < p.size() ? char(std::tolower(static_cast<unsigned char>(p[pi]))) : 0;
        char tc = char(std::tolower(static_cast<unsigned char>(t[ti])));
        if (pi < p.size() && (pc == '?' || pc == tc)) {
            ++pi, ++ti;
        } else if (pi < p.size() && pc == '*') {
            star = pi++;
            mark = ti;
        } else if (star != std::string::npos) {
            pi = star + 1;
            ti = ++mark;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*')
        ++pi;
    return pi == p.size();
}

NameFilter::NameFilter(const std::string &text) {
    size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && text[i] == ' ')
            ++i;
        size_t j = text.find(' ', i);
        std::string w = text.substr(i, j == std::string::npos ? std::string::npos : j - i);
        i = j == std::string::npos ? text.size() : j;
        if (w.empty())
            continue;
        Word word;
        word.kind = Kind::Contains;
        if (w[0] == '!' && w.size() > 1) {
            word.negate = true;
            w = w.substr(1);
        }
        std::string lw = lower(w);
        if (lw.compare(0, 4, "ext:") == 0) {
            word.kind = Kind::Ext;
            std::string rest = lw.substr(4);
            size_t k = 0;
            while (k <= rest.size()) {
                size_t c = rest.find(',', k);
                std::string e = rest.substr(k, c == std::string::npos ? std::string::npos : c - k);
                while (!e.empty() && e[0] == '.')
                    e.erase(0, 1);
                if (!e.empty())
                    word.exts.push_back(e);
                if (c == std::string::npos)
                    break;
                k = c + 1;
            }
            if (word.exts.empty()) {
                error_ = "ext: needs an extension";
                continue;
            }
        } else if (lw.compare(0, 5, "type:") == 0) {
            word.kind = Kind::Type;
            std::string v = lw.substr(5);
            if (v == "dir" || v == "folder" || v == "d")
                word.dirs = true;
            else if (v == "file" || v == "f")
                word.dirs = false;
            else {
                error_ = "type: takes dir or file";
                continue;
            }
        } else if (lw.find_first_of("*?") != std::string::npos) {
            word.kind = Kind::Glob;
            word.text = lw;
        } else {
            word.text = lw;
        }
        words_.push_back(std::move(word));
    }
}

bool NameFilter::matches(const std::string &name, bool is_dir) const {
    std::string lname;
    for (const Word &w : words_) {
        bool ok = false;
        switch (w.kind) {
        case Kind::Contains:
            if (lname.empty())
                lname = lower(name);
            ok = lname.find(w.text) != std::string::npos;
            break;
        case Kind::Glob: ok = glob_match(w.text, name); break;
        case Kind::Ext: {
            std::string e = extension_of(name);
            for (const auto &x : w.exts)
                ok = ok || (!is_dir && e == x);
            break;
        }
        case Kind::Type: ok = is_dir == w.dirs; break;
        }
        if (ok == w.negate)
            return false;
    }
    return true;
}

bool NameFilter::matches(const Entry &e) const {
    size_t slash = e.name.rfind('/');
    return matches(slash == std::string::npos ? e.name : e.name.substr(slash + 1), e.is_dir);
}

} // namespace flo
