// Files' Space block colours (space_map_view.rs tile_fill and readable).
#include "harness.h"

#include "ui/tile_colors.h"

using namespace flo::ui;
using namespace flo_test;

TEST(space_tile_colours_follow_files_hash) {
    // index: 0 blue, 1 cyan, 2 magenta, 3 green, 4 yellow (v * 31 + byte).
    CHECK_EQ(tile_colors::index("Projects"), size_t(2));
    CHECK_EQ(tile_colors::index("alpha"), size_t(3));
    CHECK_EQ(tile_colors::index("gamma"), size_t(0));
    CHECK_EQ(tile_colors::index("V\xC3\xAD" "deos"), size_t(1)); // bytes, not characters
    CHECK_EQ(tile_colors::index(""), size_t(0));
    CHECK_EQ(tile_colors::index("report.pdf"), tile_colors::index("report.pdf"));
}

TEST(space_labels_stay_readable_on_every_fill) {
    // Files' test: any fill on a 51-step grid keeps a contrast of 4.5.
    const tile_colors::Rgb8 darker{0x0e, 0x0e, 0x14}, bright{0xc0, 0xca, 0xf5};
    for (int r = 0; r <= 255; r += 51)
        for (int g = 0; g <= 255; g += 51)
            for (int b = 0; b <= 255; b += 51) {
                tile_colors::Rgb8 fill{(unsigned char)r, (unsigned char)g, (unsigned char)b};
                CHECK(tile_colors::contrast(fill, tile_colors::readable(fill, darker, bright)) >= 4.5);
            }
    // A dark theme colour on a light fill: the theme's own colour is kept.
    const tile_colors::Rgb8 yellow{0xe0, 0xaf, 0x68};
    tile_colors::Rgb8 fg = tile_colors::readable(yellow, darker, bright);
    CHECK(fg.r == darker.r && fg.g == darker.g && fg.b == darker.b);
}
