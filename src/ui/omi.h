// Omi, the Omarchy mascot Files shows while it is busy (omi.rs): the
// official Omarchy logo drawn as 13 rectangles on a 30-cell grid, with two
// eyes that blink now and then (both, left first, right first), roll a
// full circle every fourth turn and lean toward the pointer.
//
// layout() is pure (the caller fills the rectangles in its tint, so this
// file needs no toolkit); next_frame() says when the next frame is due, and
// nothing is due at rest between turns except the next turn itself, so an
// idle window never repaints for the mascot. Times are milliseconds.
#pragma once

#include <cstdint>

namespace flo::ui {

class Omi {
public:
    explicit Omi(double now_ms = 0);
    struct Rect {
        int x, y, w, h;
    };
    static constexpr int kRects = 15; // 13 for the body, then the two eyes
    // Lays the mascot out in the square centred in (x, y, w, h), leaning its
    // eyes toward the pointer (window coordinates) when there is one, and
    // returns its kRects rectangles (none when the area is empty).
    const Rect *layout(int x, int y, int w, int h, bool pointer, int px, int py, double now_ms);
    // Milliseconds until the next frame is needed: 33 ms frames while a
    // turn runs or the eyes still glide, else the time to the next turn.
    double next_frame(double now_ms, bool pointer, int px, int py);
    // Busy ended: back at rest, the schedule restarted.
    void reset(double now_ms);

    // Eye openness (left, right) and roll angle at `now_ms`.
    void pose(double now_ms, double &open_l, double &open_r, double &angle) const;
    bool turning() const { return turn_ >= 0; }
    double next_turn() const { return next_turn_; }

private:
    double pause();
    void advance(double now_ms);
    void look_target(bool pointer, int px, int py, double &tx, double &ty) const;
    double next_turn_ = 0;
    uint32_t step_ = 0;
    uint64_t seed_ = 0x9e3779b97f4a7c15ull;
    int turn_ = -1; // 0 both, 1 left first, 2 right first, 3 roll
    double turn_at_ = 0, last_frame_ = -1;
    double look_x_ = 0, look_y_ = 0;
    bool has_center_ = false;
    double cx_ = 0, cy_ = 0;
    Rect rects_[kRects] = {};
};

} // namespace flo::ui
