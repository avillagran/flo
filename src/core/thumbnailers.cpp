#include "core/thumbnailers.h"
#include "core/fsutil.h"
#include "core/xdg.h"

#include <algorithm>
#include <sstream>

#include <dirent.h>

namespace flo {

static std::string trim(const std::string &s) {
    const auto a = s.find_first_not_of(" \t\r");
    if (a == std::string::npos)
        return {};
    const auto b = s.find_last_not_of(" \t\r");
    return s.substr(a, b - a + 1);
}

// Desktop entry value escapes: \s \n \t \r \\ (and \; kept for lists).
static std::string unescape_value(const std::string &v) {
    std::string out;
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] != '\\' || i + 1 >= v.size()) {
            out += v[i];
            continue;
        }
        const char c = v[++i];
        switch (c) {
        case 's': out += ' '; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case '\\': out += '\\'; break;
        default:
            out += '\\';
            out += c;
        }
    }
    return out;
}

bool parse_thumbnailer(const std::string &text, ThumbnailerEntry &out) {
    std::istringstream in(text);
    std::string line;
    bool in_entry = false;
    while (std::getline(in, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#')
            continue;
        if (line[0] == '[') {
            in_entry = line == "[Thumbnailer Entry]";
            continue;
        }
        if (!in_entry)
            continue;
        const auto eq = line.find('=');
        if (eq == std::string::npos)
            continue;
        const std::string key = trim(line.substr(0, eq));
        const std::string value = trim(line.substr(eq + 1));
        if (key == "Exec") {
            out.exec = unescape_value(value);
        } else if (key == "TryExec") {
            out.try_exec = unescape_value(value);
        } else if (key == "MimeType") {
            out.mime_types.clear();
            std::stringstream ss(value);
            std::string m;
            while (std::getline(ss, m, ';')) {
                m = trim(m);
                if (!m.empty())
                    out.mime_types.push_back(m);
            }
        }
    }
    return !out.exec.empty() && !out.mime_types.empty();
}

static bool split_exec(const std::string &exec, std::vector<std::string> &args) {
    std::string cur;
    bool in_arg = false, quoted = false;
    for (std::size_t i = 0; i < exec.size(); ++i) {
        const char c = exec[i];
        if (quoted) {
            if (c == '"') {
                quoted = false;
            } else if (c == '\\' && i + 1 < exec.size() &&
                       (exec[i + 1] == '"' || exec[i + 1] == '`' || exec[i + 1] == '$' || exec[i + 1] == '\\')) {
                cur += exec[++i];
            } else {
                cur += c;
            }
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (in_arg) {
                args.push_back(cur);
                cur.clear();
                in_arg = false;
            }
            continue;
        }
        in_arg = true;
        if (c == '"')
            quoted = true;
        else
            cur += c;
    }
    if (quoted)
        return false;
    if (in_arg)
        args.push_back(cur);
    return !args.empty();
}

std::vector<std::string> expand_thumbnailer_exec(const std::string &exec, const std::string &input_path,
                                                 const std::string &input_uri, const std::string &output, int size) {
    std::vector<std::string> raw;
    if (!split_exec(exec, raw))
        return {};
    std::vector<std::string> args;
    for (const std::string &a : raw) {
        std::string out;
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (a[i] != '%' || i + 1 >= a.size()) {
                out += a[i];
                continue;
            }
            switch (a[++i]) {
            case 'i': out += input_path; break;
            case 'u': out += input_uri; break;
            case 'o': out += output; break;
            case 's': out += std::to_string(size); break;
            case '%': out += '%'; break;
            default: break; // unknown codes are dropped
            }
        }
        args.push_back(out);
    }
    return args;
}

std::vector<std::string> ThumbnailerRegistry::standard_dirs() {
    std::vector<std::string> dirs;
    auto data = xdg::data_dirs();
    std::reverse(data.begin(), data.end());
    for (const auto &d : data)
        dirs.push_back(d + "/thumbnailers");
    dirs.push_back(xdg::data_home() + "/thumbnailers");
    return dirs;
}

void ThumbnailerRegistry::load(const std::vector<std::string> &dirs) {
    entries_.clear();
    by_mime_.clear();
    std::vector<std::string> order; // ids in the order they were (last) seen
    for (const auto &dir : dirs) {
        DIR *d = ::opendir(dir.c_str());
        if (!d)
            continue;
        std::vector<std::string> names;
        while (dirent *e = ::readdir(d)) {
            const std::string name = e->d_name;
            if (name.size() > 12 && name.compare(name.size() - 12, 12, ".thumbnailer") == 0)
                names.push_back(name);
        }
        ::closedir(d);
        std::sort(names.begin(), names.end());
        for (const auto &name : names) {
            std::string text;
            ThumbnailerEntry entry;
            if (!read_file_capped(dir + "/" + name, 64 * 1024, text) || !parse_thumbnailer(text, entry))
                continue;
            entry.id = name;
            // A higher-priority file of the same name replaces the lower
            // one, even when its TryExec isn't installed (it masks it).
            entries_.erase(name);
            order.erase(std::remove(order.begin(), order.end(), name), order.end());
            if (!entry.try_exec.empty() && xdg::find_program(entry.try_exec).empty())
                continue;
            entries_[name] = entry;
            order.push_back(name);
        }
    }
    for (const auto &id : order)
        for (const auto &m : entries_[id].mime_types)
            by_mime_[m] = id;
}

const ThumbnailerEntry *ThumbnailerRegistry::find(const std::string &mime) const {
    auto it = by_mime_.find(mime);
    if (it == by_mime_.end())
        return nullptr;
    auto e = entries_.find(it->second);
    return e == entries_.end() ? nullptr : &e->second;
}

} // namespace flo
