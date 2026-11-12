#include "core/rail_order.h"

#include <algorithm>
#include <sstream>

namespace flo::rail_order {

static void hex(std::string &out, const std::string &bytes) {
    static const char digits[] = "0123456789abcdef";
    for (unsigned char c : bytes) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
}

std::string path_key(const std::string &section, const std::string &path) {
    std::string key = section + ":";
    hex(key, path);
    return key;
}

std::string place_key(const std::string &path, const std::string &label) {
    std::string key = path_key("place", path);
    key += "00";
    hex(key, label);
    return key;
}

std::string device_key(const std::string &name) { return path_key("dev", name); }

std::vector<std::size_t> ordered_indices(const std::vector<std::string> &keys,
                                         const std::vector<std::string> &saved) {
    std::vector<std::size_t> idx(keys.size());
    for (std::size_t i = 0; i < idx.size(); ++i)
        idx[i] = i;
    auto rank = [&](std::size_t i) {
        auto it = std::find(saved.begin(), saved.end(), keys[i]);
        return it == saved.end() ? saved.size() : std::size_t(it - saved.begin());
    };
    std::stable_sort(idx.begin(), idx.end(), [&](std::size_t a, std::size_t b) { return rank(a) < rank(b); });
    return idx;
}

static std::size_t bytes(const std::vector<std::string> &v) {
    std::size_t n = 0;
    for (const auto &s : v)
        n += s.size();
    return n;
}

static bool has_duplicates(const std::vector<std::string> &v) {
    for (std::size_t i = 0; i < v.size(); ++i)
        if (std::find(v.begin(), v.begin() + std::ptrdiff_t(i), v[i]) != v.begin() + std::ptrdiff_t(i))
            return true;
    return false;
}

bool moved(const std::vector<std::string> &saved, const std::vector<std::string> &visible, const std::string &key,
           std::size_t index, std::vector<std::string> &out) {
    if (saved.size() > kMaxEntries || visible.size() > kMaxEntries || bytes(saved) > kMaxBytes ||
        has_duplicates(saved) || has_duplicates(visible))
        return false;
    auto from = std::find(visible.begin(), visible.end(), key);
    if (from == visible.end())
        return false;
    std::vector<std::string> ordered = visible;
    std::string item = *(ordered.begin() + (from - visible.begin()));
    ordered.erase(ordered.begin() + (from - visible.begin()));
    ordered.insert(ordered.begin() + std::ptrdiff_t(std::min(index, ordered.size())), item);
    std::vector<std::string> result = saved;
    for (const auto &k : visible)
        if (std::find(result.begin(), result.end(), k) == result.end())
            result.push_back(k);
    if (result.size() > kMaxEntries || bytes(result) > kMaxBytes)
        return false;
    std::size_t next = 0;
    for (auto &slot : result)
        if (std::find(visible.begin(), visible.end(), slot) != visible.end())
            slot = ordered[next++];
    out = std::move(result);
    return true;
}

static bool well_formed(const std::string &key) {
    const auto colon = key.find(':');
    if (colon == std::string::npos || colon == 0 || key.size() > 4096)
        return false;
    for (std::size_t i = 0; i < colon; ++i)
        if (key[i] < 'a' || key[i] > 'z')
            return false;
    for (std::size_t i = colon + 1; i < key.size(); ++i)
        if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f')))
            return false;
    return (key.size() - colon - 1) % 2 == 0;
}

std::string serialize(const std::vector<std::string> &order) {
    std::string out;
    for (const auto &k : order) {
        if (!out.empty())
            out += ',';
        out += k;
    }
    return out;
}

std::vector<std::string> parse(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string key;
    std::size_t total = 0;
    while (std::getline(in, key, ',')) {
        if (!well_formed(key) || std::find(out.begin(), out.end(), key) != out.end())
            continue;
        if (out.size() >= kMaxEntries || total + key.size() > kMaxBytes)
            break;
        total += key.size();
        out.push_back(key);
    }
    return out;
}

} // namespace flo::rail_order
