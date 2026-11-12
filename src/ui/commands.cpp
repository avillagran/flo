#include "ui/commands.h"

#include "ui/util.h"

#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/text.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <cctype>

namespace flo::ui {

namespace {
const int kRowH = 40, kPad = 24, kFieldH = 34;

std::string lower(std::string s) {
    for (auto &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
} // namespace

bool command_matches(const std::string &query, const std::string &text) {
    std::string q = lower(query), t = lower(text);
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && q[i] == ' ')
            ++i;
        size_t j = q.find(' ', i);
        std::string w = q.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!w.empty() && t.find(w) == std::string::npos)
            return false;
        i = j == std::string::npos ? q.size() : j;
    }
    return true;
}

CommandPalette *CommandPalette::open(fo::Window *win, const std::string &title, const std::string &placeholder,
                                     std::vector<Command> commands, const std::string &query,
                                     std::function<std::vector<Command>()> recapture) {
    if (!win)
        return nullptr;
    auto *p = new CommandPalette();
    p->title_ = title;
    p->placeholder_ = placeholder;
    p->all_ = std::move(commands);
    p->recapture_ = std::move(recapture);
    p->begin();
    p->field_ = new fo::TextField(0, 0, 100, kFieldH);
    p->end();
    p->field_->placeholder(placeholder);
    p->field_->set_text(query, int(query.size()), int(query.size()));
    p->field_->on_change([p] { p->refilter(); });
    p->field_->on_escape([p] { p->close(); });
    p->field_->on_enter([p] {
        if (!p->shown_.empty())
            p->run(p->hi_);
    });
    p->field_->on_key([p](int k) {
        int n = int(p->shown_.size());
        if (!n)
            return false;
        if (k == FL_Down || k == FL_Up || k == FL_Page_Down || k == FL_Page_Up) {
            int step = k == FL_Down ? 1 : k == FL_Up ? -1 : k == FL_Page_Down ? p->rows_ : -p->rows_;
            p->hi_ = std::max(0, std::min(n - 1, p->hi_ + step));
            if (p->hi_ < p->top_)
                p->top_ = p->hi_;
            if (p->hi_ >= p->top_ + p->rows_)
                p->top_ = p->hi_ - p->rows_ + 1;
            p->redraw();
            return true;
        }
        return false;
    });
    p->fo::Layer::open(win);
    p->refilter();
    p->place();
    p->field_->take_focus();
    p->fade_in();
    return p;
}

void CommandPalette::refilter() {
    shown_.clear();
    std::string q = field_->value();
    for (size_t i = 0; i < all_.size(); ++i)
        if (command_matches(q, all_[i].label + " " + all_[i].keys))
            shown_.push_back(int(i));
    hi_ = 0;
    top_ = 0;
    redraw();
}

void CommandPalette::run(int i) {
    if (i < 0 || i >= int(shown_.size()))
        return;
    Command c = all_[size_t(shown_[size_t(i)])];
    if (!c.enabled)
        return;
    close();
    if (c.run)
        defer(c.run);
}

bool CommandPalette::runnable() const {
    return hi_ >= 0 && hi_ < int(shown_.size()) && all_[size_t(shown_[size_t(hi_)])].enabled;
}

void CommandPalette::use_current_context() {
    if (!recapture_)
        return;
    // Keep the highlighted entry when it is still there.
    std::string label = hi_ >= 0 && hi_ < int(shown_.size()) ? all_[size_t(shown_[size_t(hi_)])].label : "";
    all_ = recapture_();
    refilter();
    for (size_t i = 0; i < shown_.size(); ++i)
        if (all_[size_t(shown_[i])].label == label)
            hi_ = int(i);
    redraw();
}

// Files' footer buttons (command_palette.rs): Run 132 px, then Use current
// context up to 206 px, 40 px from the bottom.
static void footer_buttons(int cx, int cy, int cw, int ch, int &rx, int &ux, int &uw, int &by) {
    rx = cx + kPad;
    by = cy + ch - 40;
    ux = rx + 144;
    uw = std::min(cw - 2 * kPad - 144, 206);
}

void CommandPalette::place() {
    // Files' palette covers the window under the header.
    cx_ = 0;
    cy_ = 58;
    cw_ = w();
    ch_ = std::max(120, h() - 58 - 32);
    field_->resize(cx_ + kPad, cy_ + 56, std::min(cw_ - 2 * kPad, 760), kFieldH);
    list_y_ = cy_ + 56 + kFieldH + 34;
    rows_ = std::max(1, (cy_ + ch_ - 80 - list_y_) / kRowH);
}

void CommandPalette::relayout() { place(); }

int CommandPalette::handle(int event) {
    switch (event) {
    case FL_PUSH: {
        int ex = Fl::event_x(), ey = Fl::event_y();
        if (ey < cy_ || ey >= cy_ + ch_ ||
            (ey < cy_ + 40 && ex >= cx_ + cw_ - kPad - 80)) { // outside, or "× Esc"
            close();
            return 1;
        }
        int rx, ux, uw, by;
        footer_buttons(cx_, cy_, cw_, ch_, rx, ux, uw, by);
        if (ey >= by && ey < by + 28) {
            if (ex >= rx && ex < rx + 132 && runnable())
                run(hi_);
            else if (ex >= ux && ex < ux + uw)
                use_current_context();
            return 1;
        }
        if (ey >= list_y_ && ey < list_y_ + rows_ * kRowH) {
            int i = top_ + (ey - list_y_) / kRowH;
            if (i < int(shown_.size())) {
                hi_ = i;
                redraw();
                if (Fl::event_clicks())
                    run(i);
            }
            return 1;
        }
        break;
    }
    case FL_MOUSEWHEEL: {
        int n = int(shown_.size());
        top_ = std::max(0, std::min(std::max(0, n - rows_), top_ + Fl::event_dy()));
        redraw();
        return 1;
    }
    }
    int r = fo::Layer::handle(event);
    return r ? r : 1;
}

void CommandPalette::draw() {
    const fo::Theme &t = fo::theme();
    double a = presence();
    (void)a;
    fo::paint::fill(cx_, cy_, cw_, ch_, t.background());
    fo::paint::hline(cx_, cy_, cw_, t.card());
    fo::paint::hline(cx_, cy_ + ch_ - 1, cw_, t.card());
    fo::icon::draw("terminal", cx_ + kPad, cy_ + 18, 16, t.foreground());
    fo::text::draw(title_, cx_ + kPad + 26, cy_ + 12, cw_ - 2 * kPad, 28, t.bright_foreground(), fo::text::REGULAR,
                   15);
    fo::text::draw("\xC3\x97 Esc", cx_ + cw_ - kPad - 70, cy_ + 12, 70, 28, t.accent(), fo::text::REGULAR, 12,
                   FL_ALIGN_RIGHT);
    std::string count = fo::trn("%n commands", int(shown_.size()));
    if (hi_ >= 0 && hi_ < int(shown_.size()))
        count += " \xC2\xB7 " + all_[size_t(shown_[size_t(hi_)])].label;
    fo::text::draw(count, cx_ + kPad, cy_ + 56 + kFieldH + 6, cw_ - 2 * kPad, 22, t.dim(), fo::text::REGULAR, 11);
    draw_children();
    fl_push_clip(cx_, list_y_, cw_, rows_ * kRowH);
    for (int k = 0; k < rows_ && top_ + k < int(shown_.size()); ++k) {
        int i = top_ + k;
        const Command &c = all_[size_t(shown_[size_t(i)])];
        int ry = list_y_ + k * kRowH;
        if (i == hi_)
            fo::paint::round_fill(cx_ + kPad - 6, ry, cw_ - 2 * kPad + 12, kRowH - 2, t.radius() > 0 ? 5 : 0,
                                  t.selection());
        if (!c.icon.empty())
            fo::icon::draw(c.icon, cx_ + kPad, ry + 6, 12, c.enabled ? t.foreground() : t.muted());
        fo::text::draw(c.label, cx_ + kPad + 20, ry + 2, cw_ - 2 * kPad - 20, 20,
                       c.enabled ? t.bright_foreground() : t.muted(), fo::text::REGULAR, 12);
        fo::text::draw(c.keys.empty() ? fo::tr("Enter / double click") : c.keys, cx_ + kPad + 20, ry + 20,
                       cw_ - 2 * kPad - 20, 16, t.dim(), fo::text::REGULAR, 10);
    }
    fl_pop_clip();
    fo::text::draw(fo::tr("\xE2\x86\x91\xE2\x86\x93 or wheel \xC2\xB7 Enter runs \xC2\xB7 Esc closes"), cx_ + kPad,
                   cy_ + ch_ - 72, cw_ - 2 * kPad, 22, t.dim(), fo::text::REGULAR, 11);
    int rx, ux, uw, by;
    footer_buttons(cx_, cy_, cw_, ch_, rx, ux, uw, by);
    const int r = t.radius() > 0 ? 4 : 0;
    const bool can = runnable();
    fo::paint::round_fill(rx, by, 132, 28, r, can ? t.card() : t.background());
    fo::icon::draw("play", rx + 8, by + 7, 14, can ? t.accent() : t.muted());
    fo::text::draw(fo::tr("Run"), rx + 28, by, 96, 28, can ? t.accent() : t.muted(), fo::text::REGULAR, 12);
    if (uw > 40) {
        fo::paint::round_fill(ux, by, uw, 28, r, t.card());
        fo::icon::draw("refresh-cw", ux + 8, by + 7, 14, t.accent());
        fo::text::draw(fo::tr("Use current context"), ux + 28, by, uw - 36, 28, t.accent(), fo::text::REGULAR, 12);
    }
}

} // namespace flo::ui
