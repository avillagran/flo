// Files' calculating mascot (omi.rs): the logo grid, symmetric eyes, the
// lean toward the pointer and frames only while something moves.
#include "harness.h"

#include "ui/omi.h"

#include <cstdlib>

using namespace flo::ui;

TEST(omi_paints_inside_its_square_with_symmetric_eyes) {
    Omi omi(0);
    const Omi::Rect *r = omi.layout(10, 20, 60, 60, false, 0, 0, 0);
    CHECK(r != nullptr);
    int filled = 0;
    for (int i = 0; i < Omi::kRects; ++i) {
        CHECK(r[i].x >= 10 && r[i].y >= 20 && r[i].x + r[i].w <= 70 && r[i].y + r[i].h <= 80);
        if (i < 13)
            filled += r[i].w * r[i].h;
    }
    // The logo covers 380 of its 900 cells (Files counts them), 2 px a cell.
    CHECK_EQ(filled, 380 * 4);
    const Omi::Rect l = r[13], rr = r[14];
    CHECK(l.y == rr.y && l.w == rr.w && l.h == rr.h);
    CHECK(std::abs((l.x + l.x + l.w) + (rr.x + rr.x + rr.w) - 2 * (2 * 10 + 60)) <= 2);
    CHECK(omi.layout(0, 0, 0, 40, false, 0, 0, 0) == nullptr);
}

TEST(omi_eyes_lean_toward_the_pointer_within_the_cap) {
    Omi omi(0);
    const Omi::Rect rest = omi.layout(0, 0, 100, 100, false, 0, 0, 0)[13];
    // Far to the right: 3 % of 100 px once the glide has caught up.
    Omi::Rect leaned = omi.layout(0, 0, 100, 100, true, 5000, 50, 1000)[13];
    CHECK_EQ(leaned.x - rest.x, 3);
    CHECK_EQ(leaned.y, rest.y);
    leaned = omi.layout(0, 0, 100, 100, true, 50, -5000, 2000)[13];
    CHECK_EQ(leaned.x, rest.x);
    CHECK_EQ(leaned.y - rest.y, -3);
}

TEST(omi_asks_frames_only_while_something_moves) {
    Omi omi(0);
    omi.layout(0, 0, 72, 72, false, 0, 0, 0);
    // At rest, the next frame is the next turn, 3 to 7.5 s away.
    const double wait = omi.next_frame(0, false, 0, 0);
    CHECK(wait >= 3000 && wait <= 7500);
    CHECK(!omi.turning());
    // A turn runs at 33 ms frames, then rests again.
    const double at = omi.next_turn();
    CHECK(omi.next_frame(at, false, 0, 0) <= 33);
    CHECK(omi.turning());
    double l, r, a;
    omi.pose(at + 150, l, r, a);
    CHECK(l == 0.0 && r == 0.0 && a == 0.0); // the first turn blinks both eyes
    CHECK(omi.next_frame(at + 1000, false, 0, 0) >= 3000);
    CHECK(!omi.turning());
    // A pointer the eyes have not reached yet asks for a glide frame.
    CHECK(omi.next_frame(at + 1000, true, 4000, 36) <= 33);
    omi.reset(0);
    CHECK(!omi.turning());
}
