#include "core/json.h"

#include <cstdlib>
#include <cstdio>
#include <glib.h>

namespace flo::json {

static const Value &null_value() {
    static const Value v;
    return v;
}

const Value &Value::operator[](const std::string &key) const {
    if (type != Object)
        return null_value();
    for (const auto &kv : object)
        if (kv.first == key)
            return kv.second;
    return null_value();
}

const Value &Value::operator[](std::size_t i) const {
    if (type != Array || i >= array.size())
        return null_value();
    return array[i];
}

namespace {

struct Parser {
    const std::string &s;
    std::size_t i = 0;
    int depth = 0;

    void ws() {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
            ++i;
    }

    bool literal(const char *word) {
        std::size_t n = 0;
        while (word[n]) {
            if (i + n >= s.size() || s[i + n] != word[n])
                return false;
            ++n;
        }
        i += n;
        return true;
    }

    static void put_utf8(std::string &out, unsigned cp) {
        char buf[6];
        const int n = g_unichar_to_utf8(cp, buf);
        out.append(buf, static_cast<std::size_t>(n));
    }

    bool hex4(unsigned &v) {
        if (i + 4 > s.size())
            return false;
        v = 0;
        for (int k = 0; k < 4; ++k) {
            const char c = s[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
            else return false;
        }
        return true;
    }

    bool string(std::string &out) {
        if (i >= s.size() || s[i] != '"')
            return false;
        ++i;
        while (i < s.size()) {
            const char c = s[i++];
            if (c == '"')
                return true;
            if (static_cast<unsigned char>(c) < 0x20)
                return false;
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= s.size())
                return false;
            const char e = s[i++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp;
                if (!hex4(cp))
                    return false;
                if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < s.size() && s[i] == '\\' && s[i + 1] == 'u') {
                    i += 2;
                    unsigned lo;
                    if (!hex4(lo) || lo < 0xDC00 || lo > 0xDFFF)
                        return false;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if (cp >= 0xD800 && cp <= 0xDFFF) {
                    cp = 0xFFFD;
                }
                put_utf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool value(Value &v) {
        if (++depth > 64)
            return false;
        ws();
        if (i >= s.size())
            return false;
        bool ok = false;
        const char c = s[i];
        if (c == '{') {
            v.type = Value::Object;
            ++i;
            ws();
            if (i < s.size() && s[i] == '}') {
                ++i;
                ok = true;
            } else {
                for (;;) {
                    ws();
                    std::string key;
                    if (!string(key))
                        break;
                    ws();
                    if (i >= s.size() || s[i] != ':')
                        break;
                    ++i;
                    Value child;
                    if (!value(child))
                        break;
                    v.object.emplace_back(std::move(key), std::move(child));
                    ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == '}') {
                        ++i;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '[') {
            v.type = Value::Array;
            ++i;
            ws();
            if (i < s.size() && s[i] == ']') {
                ++i;
                ok = true;
            } else {
                for (;;) {
                    Value child;
                    if (!value(child))
                        break;
                    v.array.push_back(std::move(child));
                    ws();
                    if (i < s.size() && s[i] == ',') {
                        ++i;
                        continue;
                    }
                    if (i < s.size() && s[i] == ']') {
                        ++i;
                        ok = true;
                    }
                    break;
                }
            }
        } else if (c == '"') {
            v.type = Value::String;
            ok = string(v.string);
        } else if (literal("true")) {
            v.type = Value::Bool;
            v.boolean = true;
            ok = true;
        } else if (literal("false")) {
            v.type = Value::Bool;
            ok = true;
        } else if (literal("null")) {
            v.type = Value::Null;
            ok = true;
        } else if (c == '-' || (c >= '0' && c <= '9')) {
            const std::size_t start = i;
            ++i;
            while (i < s.size() && ((s[i] >= '0' && s[i] <= '9') || s[i] == '.' || s[i] == 'e' ||
                                    s[i] == 'E' || s[i] == '+' || s[i] == '-'))
                ++i;
            const std::string num = s.substr(start, i - start);
            char *end = nullptr;
            v.type = Value::Number;
            v.number = std::strtod(num.c_str(), &end);
            ok = end && *end == '\0';
        }
        --depth;
        return ok;
    }
};

} // namespace

bool parse(const std::string &text, Value &out, std::size_t max_bytes) {
    if (text.size() > max_bytes)
        return false;
    Parser p{text};
    out = Value();
    if (!p.value(out))
        return false;
    p.ws();
    return p.i == text.size();
}

std::string quote(const std::string &in) {
    std::string out = "\"";
    const char *p = in.c_str();
    const char *end = p + in.size();
    while (p < end) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x80) {
            switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
            }
            ++p;
            continue;
        }
        const gunichar u = g_utf8_get_char_validated(p, end - p);
        if (u == static_cast<gunichar>(-1) || u == static_cast<gunichar>(-2)) {
            out += "\xEF\xBF\xBD";
            ++p;
            continue;
        }
        const char *next = g_utf8_next_char(p);
        out.append(p, static_cast<std::size_t>(next - p));
        p = next;
    }
    out += '"';
    return out;
}

} // namespace flo::json
