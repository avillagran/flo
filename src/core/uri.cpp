#include "core/uri.h"

#include <cctype>

namespace flo {

static bool acceptable(unsigned char c) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))
        return true;
    switch (c) {
    case '!': case '$': case '&': case '\'': case '(': case ')': case '*': case '+':
    case ',': case '-': case '.': case '/': case ':': case '=': case '@': case '_': case '~':
        return true;
    default:
        return false;
    }
}

std::string percent_encode_path(const std::string &path) {
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size() + 8);
    for (unsigned char c : path) {
        if (acceptable(c)) {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

std::string path_to_uri(const std::string &absolute_path) {
    return "file://" + percent_encode_path(absolute_path);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool percent_decode(const std::string &in, std::string &out) {
    out.clear();
    out.reserve(in.size());
    for (std::size_t i = 0; i < in.size(); ++i) {
        if (in[i] != '%') {
            if (in[i] == '\0')
                return false;
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size())
            return false;
        const int hi = hexval(in[i + 1]), lo = hexval(in[i + 2]);
        if (hi < 0 || lo < 0)
            return false;
        const char c = static_cast<char>(hi * 16 + lo);
        if (c == '\0')
            return false;
        out += c;
        i += 2;
    }
    return true;
}

std::string uri_to_path(const std::string &uri) {
    static const std::string prefix = "file://";
    if (uri.compare(0, prefix.size(), prefix) != 0)
        return {};
    std::string rest = uri.substr(prefix.size());
    if (rest.compare(0, 9, "localhost") == 0)
        rest = rest.substr(9);
    if (rest.empty() || rest[0] != '/')
        return {};
    // A fragment or query never belongs to a local file URI from a file manager.
    std::string decoded;
    if (!percent_decode(rest, decoded))
        return {};
    return decoded;
}

std::string uri_scheme(const std::string &uri) {
    for (std::size_t i = 0; i < uri.size(); ++i) {
        const char c = uri[i];
        if (c == ':')
            return i > 0 ? uri.substr(0, i) : std::string();
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '-' || c == '.'))
            return {};
    }
    return {};
}

} // namespace flo
