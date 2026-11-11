#include "core/natural_sort.h"

#include <glib.h>
#include <cstring>

namespace flo {

namespace {

inline bool is_digit(unsigned char c) { return c >= '0' && c <= '9'; }

// Decodes one UTF-8 character at s[i], advancing i; invalid bytes come back
// as themselves (0xDC00 + byte) so they still order deterministically.
inline gunichar next_char(const std::string &s, std::size_t &i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) {
        ++i;
        return (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    const gunichar u = g_utf8_get_char_validated(s.c_str() + i, static_cast<gssize>(s.size() - i));
    if (u == static_cast<gunichar>(-1) || u == static_cast<gunichar>(-2)) {
        ++i;
        return 0xDC00 + c;
    }
    i = static_cast<std::size_t>(g_utf8_next_char(s.c_str() + i) - s.c_str());
    return g_unichar_tolower(u);
}

} // namespace

int natural_compare(const std::string &a, const std::string &b) {
    std::size_t i = 0, j = 0;
    int zeros_tie = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (is_digit(ca) && is_digit(cb)) {
            std::size_t si = i, sj = j;
            while (si < a.size() && a[si] == '0')
                ++si;
            while (sj < b.size() && b[sj] == '0')
                ++sj;
            std::size_t ei = si, ej = sj;
            while (ei < a.size() && is_digit(static_cast<unsigned char>(a[ei])))
                ++ei;
            while (ej < b.size() && is_digit(static_cast<unsigned char>(b[ej])))
                ++ej;
            const std::size_t la = ei - si, lb = ej - sj;
            if (la != lb)
                return la < lb ? -1 : 1;
            const int c = std::memcmp(a.data() + si, b.data() + sj, la);
            if (c != 0)
                return c < 0 ? -1 : 1;
            // "007" and "7" are equal in value; fewer zeros first, but only
            // if nothing else differs.
            if (zeros_tie == 0 && (si - i) != (sj - j))
                zeros_tie = (si - i) < (sj - j) ? -1 : 1;
            i = ei;
            j = ej;
            continue;
        }
        const gunichar ua = next_char(a, i);
        const gunichar ub = next_char(b, j);
        if (ua != ub)
            return ua < ub ? -1 : 1;
    }
    if (i < a.size())
        return 1;
    if (j < b.size())
        return -1;
    if (zeros_tie)
        return zeros_tie;
    const int c = a.compare(b);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

std::string fold_case(const std::string &s) {
    bool ascii = true;
    for (unsigned char c : s)
        if (c >= 0x80) {
            ascii = false;
            break;
        }
    if (ascii) {
        std::string out = s;
        for (char &c : out)
            if (c >= 'A' && c <= 'Z')
                c = static_cast<char>(c + 32);
        return out;
    }
    if (!g_utf8_validate(s.c_str(), static_cast<gssize>(s.size()), nullptr))
        return s;
    gchar *folded = g_utf8_casefold(s.c_str(), static_cast<gssize>(s.size()));
    std::string out = folded ? folded : s;
    g_free(folded);
    return out;
}

bool contains_folded(const std::string &haystack, const std::string &needle_folded) {
    if (needle_folded.empty())
        return true;
    return fold_case(haystack).find(needle_folded) != std::string::npos;
}

} // namespace flo
