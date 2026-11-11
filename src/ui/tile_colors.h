// Files' Space block colours (space_map_view.rs tile_fill and readable),
// without toolkit types so the core tests can check them.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>

namespace flo::ui::tile_colors {

struct Rgb8 {
    unsigned char r = 0, g = 0, b = 0;
};

// The block colour of a name: 0 blue, 1 cyan, 2 magenta, 3 green,
// 4 yellow, by Files' stable hash (v * 31 + byte), so a folder keeps its
// colour in its parent and while it zooms.
inline std::size_t index(const std::string &name) {
    uint64_t hash = 0;
    for (char c : name)
        hash = hash * 31 + static_cast<unsigned char>(c);
    return std::size_t(hash % 5);
}

inline double luminance(Rgb8 c) {
    auto channel = [](unsigned char v) {
        double n = v / 255.0;
        return n <= 0.04045 ? n / 12.92 : std::pow((n + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel(c.r) + 0.7152 * channel(c.g) + 0.0722 * channel(c.b);
}

// WCAG contrast ratio.
inline double contrast(Rgb8 a, Rgb8 b) {
    double la = luminance(a), lb = luminance(b);
    return (std::max(la, lb) + 0.05) / (std::min(la, lb) + 0.05);
}

// The label colour on `fill`: the theme's darker background or bright
// foreground, whichever contrasts more, when that reaches 4.5; else black
// or white.
inline Rgb8 readable(Rgb8 fill, Rgb8 darker_background, Rgb8 bright_foreground) {
    Rgb8 best = contrast(fill, darker_background) > contrast(fill, bright_foreground) ? darker_background
                                                                                      : bright_foreground;
    if (contrast(fill, best) >= 4.5)
        return best;
    const Rgb8 black{0, 0, 0}, white{255, 255, 255};
    return contrast(fill, black) > contrast(fill, white) ? black : white;
}

} // namespace flo::ui::tile_colors
