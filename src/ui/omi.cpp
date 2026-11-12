#include "ui/omi.h"

#include <algorithm>
#include <cmath>

namespace flo::ui {

namespace {
// Body rectangles (x, y, w, h) in 30-cell units.
const int kBody[13][4] = {{0, 0, 30, 2},  {0, 2, 2, 28},  {14, 2, 2, 4},  {28, 2, 2, 28}, {4, 4, 10, 2},
                          {22, 4, 4, 2},  {4, 6, 2, 20},  {24, 6, 2, 20}, {2, 14, 2, 2},  {6, 24, 18, 2},
                          {14, 26, 2, 4}, {2, 28, 12, 2}, {18, 28, 10, 2}};
// Eyes as fractions of the mascot: (400,450,73,161) and (715,450,73,161)
// in logo units.
const double kEyeW = 0.0608, kEyeTop = 0.375, kEyeH = 0.1342, kEyeLX = 0.3384, kEyeRX = 0.6008;
const double kRollR = 0.022;
// Full lean at 240 px from the centre, capped at 3% of the mascot size.
const double kLookReach = 240.0, kLookMax = 0.030;
const double kFrame = 33, kBlink = 350, kLag = 90, kRoll = 850;

double duration(int turn) { return turn == 0 ? kBlink : turn == 3 ? kRoll : kBlink + kLag; }

// Openness during one blink: close 110 ms, hold 90 ms, open 150 ms.
double blink(double ms) {
    if (ms < 0)
        return 1.0;
    if (ms < 110) {
        double f = ms / 110;
        return 1 - f * f;
    }
    if (ms < 200)
        return 0.0;
    if (ms < 350) {
        double f = (ms - 200) / 150;
        return 1 - (1 - f) * (1 - f);
    }
    return 1.0;
}
} // namespace

Omi::Omi(double now_ms) { reset(now_ms); }

void Omi::reset(double now_ms) {
    turn_ = -1;
    last_frame_ = -1;
    has_center_ = false;
    look_x_ = look_y_ = 0;
    next_turn_ = now_ms + pause();
}

// 3 to 7.5 s between turns, from a small deterministic generator.
double Omi::pause() {
    seed_ ^= seed_ << 13;
    seed_ ^= seed_ >> 7;
    seed_ ^= seed_ << 17;
    return 3000.0 + double(seed_ % 4500);
}

void Omi::advance(double now) {
    if (turn_ >= 0 && now - turn_at_ > duration(turn_))
        turn_ = -1;
    if (turn_ < 0 && now >= next_turn_) {
        turn_ = int(step_ % 4);
        ++step_;
        turn_at_ = now;
        next_turn_ = now + duration(turn_) + pause();
    }
}

void Omi::look_target(bool pointer, int px, int py, double &tx, double &ty) const {
    tx = ty = 0;
    if (!pointer || !has_center_)
        return;
    double dx = px - cx_, dy = py - cy_;
    double d = std::sqrt(dx * dx + dy * dy);
    if (d < 1.0)
        return;
    double f = std::min(d / kLookReach, 1.0) * kLookMax;
    tx = dx / d * f;
    ty = dy / d * f;
}

double Omi::next_frame(double now, bool pointer, int px, int py) {
    advance(now);
    double tx, ty;
    look_target(pointer, px, py, tx, ty);
    bool moving = turn_ >= 0 || std::fabs(look_x_ - tx) > 0.0005 || std::fabs(look_y_ - ty) > 0.0005;
    if (!moving)
        return std::max(0.0, next_turn_ - now);
    double since = last_frame_ < 0 ? kFrame : now - last_frame_;
    return std::max(0.0, kFrame - since);
}

void Omi::pose(double now, double &open_l, double &open_r, double &angle) const {
    open_l = open_r = 1.0;
    angle = 0.0;
    if (turn_ < 0)
        return;
    double t = now - turn_at_;
    switch (turn_) {
    case 0: open_l = open_r = blink(t); break;
    case 1: open_l = blink(t), open_r = blink(t - kLag); break;
    case 2: open_l = blink(t - kLag), open_r = blink(t); break;
    default: {
        double f = std::clamp(t / kRoll, 0.0, 1.0);
        double eased = f < 0.5 ? 2 * f * f : 1 - std::pow(-2 * f + 2, 2) / 2;
        angle = f >= 1 ? 0.0 : eased * 2 * M_PI;
    }
    }
}

const Omi::Rect *Omi::layout(int x, int y, int w, int h, bool pointer, int px, int py, double now) {
    int size = std::min(w, h);
    if (size <= 0)
        return nullptr;
    advance(now);
    int x0 = x + (w - size) / 2, y0 = y + (h - size) / 2;
    double s = size;
    auto cell = [&](int v) { return int(std::lround(v * s / 30.0)); };
    int n = 0;
    for (const auto &b : kBody) {
        Rect r{x0 + cell(b[0]), y0 + cell(b[1]), cell(b[0] + b[2]) - cell(b[0]), cell(b[1] + b[3]) - cell(b[1])};
        rects_[n++] = r;
    }
    cx_ = x0 + s / 2;
    cy_ = y0 + s / 2;
    has_center_ = true;
    double tx, ty;
    look_target(pointer, px, py, tx, ty);
    double k = last_frame_ < 0 ? 1.0 : std::clamp((now - last_frame_) / 140.0, 0.0, 1.0);
    look_x_ += (tx - look_x_) * k;
    look_y_ += (ty - look_y_) * k;
    last_frame_ = now;
    double open_l, open_r, angle;
    pose(now, open_l, open_r, angle);
    // The orbit starts at the resting place, so rest has no offset.
    double rx = (std::cos(angle) - 1.0) * kRollR, ry = std::sin(angle) * kRollR;
    for (int e = 0; e < 2; ++e) {
        double ex = e == 0 ? kEyeLX : kEyeRX, open = e == 0 ? open_l : open_r;
        double eh = std::max(kEyeH * s * open, 1.2);
        double top = kEyeTop + kEyeH * (1.0 - open) / 2.0 + ry + look_y_;
        Rect r{x0 + int(std::lround((ex + rx + look_x_) * s)), y0 + int(std::lround(top * s)),
               std::max(1, int(std::lround(kEyeW * s))), std::max(1, int(std::lround(eh)))};
        rects_[n++] = r;
    }
    return rects_;
}

} // namespace flo::ui
