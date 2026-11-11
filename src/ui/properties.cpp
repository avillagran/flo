#include "ui/properties.h"

#include "core/fsutil.h"
#include "core/tree_size.h"
#include "ui/app.h"
#include "ui/util.h"

#include "fltk-omarchy/dialog.h"
#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/keys.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/text.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <thread>

namespace flo::ui {

namespace {

const uint32_t kBits[9] = {0400, 0200, 0100, 0040, 0020, 0010, 0004, 0002, 0001};
const int kPathLine = 18, kInfoLine = 24;

std::string octal(uint32_t mode) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04o", mode & 07777);
    return buf;
}

std::string args(std::string text, std::initializer_list<std::string> values) {
    for (const auto &v : values)
        text = fo::i18n::arg(text, v);
    return text;
}

// Paths as Files labels them: control bytes escaped, never re-read.
std::string escaped(const std::string &s) {
    std::string out;
    for (unsigned char c : s) {
        if (c < 0x20 || c == 0x7f) {
            char buf[8];
            std::snprintf(buf, sizeof buf, "\\x%02X", c);
            out += buf;
        } else {
            out += char(c);
        }
    }
    return out;
}

} // namespace

PropertiesDialog *PropertiesDialog::open(fo::Window *win, const std::vector<std::string> &paths) {
    if (!win || win->modal_open())
        return nullptr;
    std::vector<std::string> objects;
    for (const auto &p : paths)
        if (!p.empty() && p[0] == '/' && std::find(objects.begin(), objects.end(), p) == objects.end())
            objects.push_back(p);
    if (objects.empty() || objects.size() > kMaxObjects) {
        fo::Toast::show(win, fo::tr("Properties: choose between 1 and 32 objects"), 3);
        return nullptr;
    }
    auto *d = new PropertiesDialog();
    d->objects_ = std::move(objects);
    d->end();
    d->fo::Layer::open(win);
    d->start_read();
    if (d->objects_.size() > 1)
        d->start_census();
    d->take_focus();
    d->fade_in();
    return d;
}

PropertiesDialog::~PropertiesDialog() { census_stop_->store(true); }

void PropertiesDialog::closing() { census_stop_->store(true); }

void PropertiesDialog::start_read() {
    pending_ = true;
    observed_ = false;
    stale_ = false;
    message_ = fo::tr("Reading properties\xE2\x80\xA6");
    message_error_ = false;
    const uint64_t gen = ++generation_;
    const std::string path = objects_[index_];
    auto poster = App::get().queue().poster();
    std::weak_ptr<char> weak = alive_.weak();
    PropertiesDialog *self = this;
    exec_.run([poster, weak, self, gen, path] {
        auto info = std::make_shared<props::Info>();
        auto why = std::make_shared<std::string>(props::read(path, *info));
        // The path as it resolves now: the real parent and the name.
        auto verified = std::make_shared<std::string>();
        std::string parent = real_path(path_dirname(path));
        if (why->empty() && !parent.empty())
            *verified = path_join(parent, path_basename(path));
        poster.post([weak, self, gen, info, why, verified] {
            if (weak.expired() || gen != self->generation_)
                return;
            self->pending_ = false;
            if (!why->empty()) {
                self->message_ = fo::tr(why->c_str());
                self->message_error_ = true;
                self->redraw();
                return;
            }
            self->info_ = *info;
            self->observed_ = true;
            self->verified_ = *verified;
            self->requested_ = info->mode & 0777;
            self->default_app_.clear();
            if (!info->mime.empty()) {
                for (const auto &a : apps_for(info->mime))
                    if (a.is_default) {
                        self->default_app_ = a.name;
                        break;
                    }
            }
            self->message_ = fo::tr("Properties read");
            self->message_error_ = false;
            if (self->objects_.size() == 1 && info->kind == props::Kind::Directory && !self->census_done_)
                self->start_census();
            self->layout_body();
            self->redraw();
        });
    });
    layout_body();
    redraw();
}

void PropertiesDialog::start_census() {
    if (census_running_)
        return;
    census_running_ = true;
    census_done_ = false;
    census_stop_->store(true);
    census_stop_ = std::make_shared<std::atomic<bool>>(false);
    auto stop = census_stop_;
    const std::vector<std::string> roots = objects_;
    auto poster = App::get().queue().poster();
    std::weak_ptr<char> weak = alive_.weak();
    PropertiesDialog *self = this;
    // Its own thread, so a long count never delays reading the next object;
    // closing the dialog stops it between entries.
    std::thread([poster, weak, self, stop, roots] {
        props::Census c = props::census(roots, *stop);
        auto result = std::make_shared<props::Census>(c);
        poster.post([weak, self, result] {
            if (weak.expired())
                return;
            self->census_ = *result;
            self->census_running_ = false;
            self->census_done_ = true;
            self->layout_body();
            self->redraw();
        });
    }).detach();
}

std::string PropertiesDialog::contents_line() const {
    const bool selection = objects_.size() > 1;
    const bool folder = observed_ && info_.kind == props::Kind::Directory;
    if (!selection && !folder)
        return {};
    std::string summary;
    if (!census_done_) {
        summary = fo::tr("calculating\xE2\x80\xA6");
    } else {
        const props::Census &c = census_;
        summary = args(fo::tr("%1 · %2 files, %3 folders"),
                       {si_size(c.bytes), std::to_string(c.files), std::to_string(c.folders)});
        if (c.links > 0)
            summary += fo::i18n::arg(fo::tr(", %1 links (not followed)"), std::to_string(c.links));
        if (!c.complete) {
            summary += fo::tr(" \xC2\xB7 PARTIAL");
            if (c.unreadable > 0)
                summary += fo::i18n::arg(fo::tr(": %1 without read permission"), std::to_string(c.unreadable));
        }
    }
    return selection ? args(fo::tr("Selection of %1 objects: %2"), {std::to_string(objects_.size()), summary})
                     : fo::i18n::arg(fo::tr("Contents: %1"), summary);
}

std::vector<std::string> PropertiesDialog::info_lines() const {
    std::vector<std::string> l;
    if (!observed_)
        return l;
    const props::Info &i = info_;
    const bool link = i.kind == props::Kind::Symlink;
    switch (i.kind) {
    case props::Kind::File: l.push_back(fo::i18n::arg(fo::tr("File \xC2\xB7 apparent %1"), si_size(i.apparent))); break;
    case props::Kind::Directory: l.push_back(fo::tr("Folder")); break;
    case props::Kind::Symlink: l.push_back(fo::tr("Symbolic link (not followed)")); break;
    case props::Kind::Other:
        l.push_back(fo::i18n::arg(fo::tr("Special object \xC2\xB7 apparent %1"), si_size(i.apparent)));
        break;
    }
    std::string contents = contents_line();
    if (!contents.empty())
        l.push_back(contents);
    l.push_back(args(fo::tr("Allocated %1 \xC2\xB7 hard links %2"), {si_size(i.allocated), std::to_string(i.hardlinks)}));
    l.push_back(args(fo::tr("Owner: %1 \xC2\xB7 Group: %2"), {i.owner, i.group}));
    if (link)
        l.push_back(args(fo::tr("Link permissions (lstat, not the target's): %1 \xC2\xB7 mode %2"),
                         {props::symbolic_mode(i.mode, 'l'), octal(i.mode)}));
    else
        l.push_back(args(fo::tr("Permissions: %1 \xC2\xB7 octal mode %2"),
                         {props::symbolic_mode(i.mode, i.kind == props::Kind::Directory ? 'd' : '-'), octal(i.mode)}));
    if (link)
        l.push_back(i.link_resolves ? fo::tr("Link target: exists") : fo::tr("Link target: missing (broken link)"));
    if (!i.mime.empty()) {
        l.push_back(args(fo::tr("MIME type: %1 \xC2\xB7 Open with: %2"),
                         {i.mime, default_app_.empty() ? fo::tr("(no application)") : default_app_}));
        l.push_back(fo::tr("Change the application: Ctrl+Shift+O in the list"));
    }
    if (i.volume)
        l.push_back(args(fo::tr("Volume: %1 \xC2\xB7 %2 free of %3"),
                         {i.fs_type.empty() ? fo::tr("unknown filesystem") : i.fs_type, si_size(i.free_bytes),
                          si_size(i.total_bytes)}));
    else
        l.push_back(fo::tr("Volume: unavailable"));
    l.push_back(args(fo::tr("Device %1 \xC2\xB7 inode %2"), {std::to_string(i.device), std::to_string(i.inode)}));
    l.push_back(fo::i18n::arg(fo::tr("Modified: %1"), props::format_time(i.modified)));
    l.push_back(fo::i18n::arg(fo::tr("Accessed: %1"), props::format_time(i.accessed)));
    l.push_back(i.birth.known ? fo::i18n::arg(fo::tr("Created: %1"), props::format_time(i.birth))
                              : fo::tr("Created: not available on this filesystem"));
    l.push_back(fo::i18n::arg(fo::tr("Inode change: %1"), props::format_time(i.changed)));
    return l;
}

void PropertiesDialog::layout_body() {
    const int width = std::max(1, bw_);
    auto small = fo::text::fltk_measure(fo::text::REGULAR, 10);
    auto body = fo::text::fltk_measure(fo::text::REGULAR, 12);
    fl_font(fo::text::REGULAR, 10);
    lay_.requested = fo::text::wrap(fo::i18n::arg(fo::tr("Requested path: %1"), escaped(objects_[index_])), width, small, 16);
    lay_.verified = fo::text::wrap(verified_.empty() ? fo::tr("Current path unavailable")
                                                     : fo::i18n::arg(fo::tr("Verified path: %1"), escaped(verified_)),
                                   width, small, 16);
    lay_.link.clear();
    if (observed_ && info_.kind == props::Kind::Symlink)
        lay_.link = fo::text::wrap(fo::i18n::arg(fo::tr("Raw target (not opened): %1"), escaped(info_.link_target)), width,
                                   small, 16);
    fl_font(fo::text::REGULAR, 12);
    lay_.info.clear();
    for (const auto &line : info_lines())
        lay_.info.push_back(fo::text::elide_middle(line, width, body));
    lay_.info_top = int(lay_.requested.size()) * kPathLine + 8;
    lay_.verified_top = lay_.info_top + int(lay_.info.size()) * kInfoLine + 6;
    lay_.link_top = lay_.verified_top + int(lay_.verified.size()) * kPathLine + 8;
    lay_.permission_label = lay_.link_top + (lay_.link.empty() ? 0 : int(lay_.link.size()) * kPathLine + 8);
    lay_.exec = lay_.permission_label + 28;
    lay_.permissions = lay_.exec + 36;
    lay_.note = lay_.permissions + 104;
    lay_.height = observed_ ? lay_.note + 36 : lay_.info_top + 40;
    scroll_by(0);
}

bool PropertiesDialog::editable() const { return !pending_ && !stale_ && observed_ && props::editable(info_); }

bool PropertiesDialog::has_edits() const { return observed_ && (info_.mode & 0777) != requested_; }

bool PropertiesDialog::can_step(int delta) const {
    if (pending_ || has_edits() || delta == 0)
        return false;
    long to = long(index_) + delta;
    return to >= 0 && to < long(objects_.size());
}

void PropertiesDialog::step(int delta) {
    if (!can_step(delta))
        return;
    index_ = size_t(long(index_) + delta);
    scroll_ = 0;
    focus_ = 0;
    verified_.clear();
    start_read();
}

void PropertiesDialog::toggle(int bit) {
    if (!editable() || bit < 0 || bit > 8)
        return;
    requested_ ^= kBits[bit];
    focus_ = bit;
    redraw();
}

void PropertiesDialog::apply() {
    if (!editable() || !has_edits())
        return;
    props::Info after;
    std::string why = props::apply_mode(info_, requested_, after);
    if (!why.empty()) {
        stale_ = true;
        message_ = fo::tr(why.c_str());
        message_error_ = true;
        redraw();
        return;
    }
    if (after.inode == 0) {
        // Applied, but the fresh read did not match: reload before editing.
        stale_ = true;
        message_ = fo::tr("Permissions applied; reload the properties. This operation has no Undo.");
        message_error_ = false;
    } else {
        info_ = after;
        requested_ = after.mode & 0777;
        message_ = fo::tr("Permissions applied. This operation has no Undo.");
        message_error_ = false;
    }
    layout_body();
    redraw();
}

void PropertiesDialog::press(int id) {
    if (id >= 0 && id < 9) {
        toggle(id);
        return;
    }
    switch (id) {
    case kExec: toggle(2); break;
    case kPrev: step(-1); break;
    case kNext: step(1); break;
    case kReload:
        if (!pending_) {
            requested_ = info_.mode & 0777;
            start_read();
        }
        break;
    case kClose: close(); break;
    case kApply: apply(); break;
    }
}

void PropertiesDialog::scroll_by(int delta) {
    long s = long(scroll_) + delta;
    int max = std::max(0, lay_.height - bh_);
    scroll_ = int(std::clamp<long>(s, 0, max));
}

void PropertiesDialog::reveal_focus() {
    int top = lay_.permissions + (focus_ / 3) * 32;
    if (top < scroll_)
        scroll_ = top;
    if (top + 28 > scroll_ + bh_)
        scroll_ = std::max(0, top + 28 - bh_);
}

bool PropertiesDialog::key(int k) {
    const int st = Fl::event_state() & (FL_CTRL | FL_ALT | FL_SHIFT | FL_META);
    if ((st & FL_ALT) && (k == FL_Left || k == FL_Right)) {
        step(k == FL_Left ? -1 : 1);
        return true;
    }
    if (k == FL_Escape || fo::event_is("Ctrl+W")) {
        close();
        return true;
    }
    if (k == FL_Enter || k == FL_KP_Enter) {
        apply();
        return true;
    }
    if (k == FL_Tab) {
        focus_ = (focus_ + ((st & FL_SHIFT) ? 8 : 1)) % 9;
        reveal_focus();
        redraw();
        return true;
    }
    const int page = std::max(18, bh_ - 18);
    switch (k) {
    case FL_Up: scroll_by(-18); break;
    case FL_Down: scroll_by(18); break;
    case FL_Page_Up: scroll_by(-page); break;
    case FL_Page_Down: scroll_by(page); break;
    case FL_Home: scroll_by(INT_MIN / 2); break;
    case FL_End: scroll_by(INT_MAX / 2); break;
    case ' ': toggle(focus_); return true;
    case FL_F + 5: press(kReload); return true;
    default: return st == 0 || st == FL_SHIFT; // a dialog: other keys stay here
    }
    redraw();
    return true;
}

void PropertiesDialog::relayout() {
    w_ = std::min(w() - 36, 780);
    h_ = std::min(h() - 68, 640);
    x_ = (w() - w_) / 2;
    y_ = std::max(0, (h() - 32 - h_) / 2);
    bx_ = x_ + 20, by_ = y_ + 74, bw_ = w_ - 40, bh_ = std::max(0, h_ - 176);
    layout_body();
}

int PropertiesDialog::handle(int event) {
    switch (event) {
    case FL_FOCUS:
    case FL_UNFOCUS: return 1;
    case FL_KEYBOARD:
    case FL_SHORTCUT: return key(Fl::event_key()) ? 1 : 0;
    case FL_PUSH: {
        int ex = Fl::event_x(), ey = Fl::event_y();
        take_focus();
        if (ex < x_ || ex >= x_ + w_ || ey < y_ || ey >= y_ + h_) {
            close();
            return 1;
        }
        for (const Hit &h : hits_)
            if (ex >= h.x && ex < h.x + h.w && ey >= h.y && ey < h.y + h.h) {
                press(h.id);
                return 1;
            }
        return 1;
    }
    case FL_MOUSEWHEEL:
        scroll_by(Fl::event_dy() * 36);
        redraw();
        return 1;
    case FL_RELEASE:
    case FL_DRAG:
    case FL_MOVE:
    case FL_ENTER: return 1;
    }
    return fo::Layer::handle(event);
}

void PropertiesDialog::draw() {
    const fo::Theme &t = fo::theme();
    const int rad = t.radius() > 0 ? 10 : 0, r4 = t.radius() > 0 ? 4 : 0;
    hits_.clear();
    auto button = [&](int x, int y, int w, int h, const std::string &text, int id, bool on, bool active) {
        fo::paint::round_fill(x, y, w, h, r4, active ? fo::mix(t.card(), t.accent(), 0.25) : t.card());
        fo::text::draw(text, x + 8, y, w - 16, h, on ? (active ? t.bright_foreground() : t.accent()) : t.muted(),
                       fo::text::REGULAR, 11, FL_ALIGN_CENTER);
        if (on)
            hits_.push_back({x, y, w, h, id});
    };
    fo::paint::round_fill(x_, y_, w_, h_, rad, t.muted());
    fo::paint::round_fill(x_ + 1, y_ + 1, w_ - 2, h_ - 2, rad, t.dark_background());
    fo::icon::draw("info", x_ + 22, y_ + 16, 15, t.accent());
    fo::text::draw(fo::tr("Properties and permissions"), x_ + 44, y_ + 8, w_ - 64, 30, t.bright_foreground(),
                   fo::text::REGULAR, 17);
    if (objects_.size() > 1) {
        button(x_ + 20, y_ + 40, 88, 24, fo::tr("Previous"), kPrev, can_step(-1), false);
        fo::text::draw(args(fo::tr("Object %1 of %2"), {std::to_string(index_ + 1), std::to_string(objects_.size())}),
                       x_ + 116, y_ + 40, w_ - 232, 24, t.dim(), fo::text::REGULAR, 10, FL_ALIGN_CENTER);
        button(x_ + w_ - 108, y_ + 40, 88, 24, fo::tr("Next"), kNext, can_step(1), false);
    } else {
        fo::text::draw(fo::tr("Full paths in details \xC2\xB7 Home/End \xC2\xB7 Tab permissions"), x_ + 20, y_ + 40,
                       w_ - 40, 24, t.dim(), fo::text::REGULAR, 10);
    }
    fl_push_clip(bx_, by_, bw_, bh_);
    const int origin = by_ - scroll_;
    for (size_t i = 0; i < lay_.requested.size(); ++i)
        fo::text::draw(lay_.requested[i], bx_, origin + int(i) * kPathLine, bw_, kPathLine, t.dim(), fo::text::REGULAR,
                       10, FL_ALIGN_LEFT, fo::text::Elide::None);
    const int top = origin + lay_.info_top;
    if (observed_) {
        for (size_t i = 0; i < lay_.info.size(); ++i)
            fo::text::draw(lay_.info[i], bx_, top + int(i) * kInfoLine, bw_, kInfoLine, t.foreground(),
                           fo::text::REGULAR, 12, FL_ALIGN_LEFT, fo::text::Elide::None);
        for (size_t i = 0; i < lay_.verified.size(); ++i)
            fo::text::draw(lay_.verified[i], bx_, origin + lay_.verified_top + int(i) * kPathLine, bw_, kPathLine,
                           t.dim(), fo::text::REGULAR, 10, FL_ALIGN_LEFT, fo::text::Elide::None);
        for (size_t i = 0; i < lay_.link.size(); ++i)
            fo::text::draw(lay_.link[i], bx_, origin + lay_.link_top + int(i) * kPathLine, bw_, kPathLine, t.accent(),
                           fo::text::REGULAR, 10, FL_ALIGN_LEFT, fo::text::Elide::None);
        const bool link = info_.kind == props::Kind::Symlink;
        fo::text::draw(link ? fo::i18n::arg(fo::tr("Link permissions %1 \xC2\xB7 read-only, the target's are not changed"),
                                            octal(info_.mode))
                            : args(fo::tr("Permissions %1 \xE2\x86\x92 %2 \xC2\xB7 every link to the inode"),
                                   {octal(info_.mode), octal(requested_)}),
                       bx_, origin + lay_.permission_label, bw_, 24, t.bright_foreground(), fo::text::REGULAR, 12);
        auto whole = [&](int y, int h) { return y >= by_ && y + h <= by_ + bh_; };
        const bool edit = editable();
        {
            const int ey = origin + lay_.exec;
            const bool on = requested_ & 0100;
            button(bx_, ey, bw_, 28,
                   std::string(on ? "[x] " : "[ ] ") + fo::tr("Allow executing as a program"), kExec,
                   edit && whole(ey, 28), on);
        }
        const char *rows[3] = {"Owner", "Group", "Others"};
        const int labels = bw_ < 500 ? 96 : 128;
        const int cell = (bw_ - labels - 8 - 16) / 3;
        for (int row = 0; row < 3; ++row) {
            const int ry = origin + lay_.permissions + row * 32;
            fo::text::draw(fo::tr(rows[row]), bx_, ry, labels, 28, t.foreground(), fo::text::REGULAR, 12);
            for (int col = 0; col < 3; ++col) {
                const int bit = row * 3 + col;
                const int cx = bx_ + labels + 8 + col * (cell + 8);
                const bool on = requested_ & kBits[bit];
                const std::string text = std::string("rwx").substr(size_t(col), 1) + (on ? " \xE2\x9C\x93" : " \xE2\x80\x94");
                const bool shown = whole(ry, 28) && cell > 0;
                button(cx, ry, cell, 28, text, bit, edit && shown, on);
                if (bit == focus_ && shown && edit)
                    fo::paint::round_frame(cx, ry, cell, 28, r4, 1, t.accent());
            }
        }
        std::string note = edit ? fo::tr("Only this object; no recursion, ACLs or owner change.")
                                : fo::tr("Read-only or stale: Reload to check again.");
        fo::text::draw(note, bx_, origin + lay_.note, bw_, 28, t.dim(), fo::text::REGULAR, 11);
    } else {
        fo::text::draw(pending_ ? fo::tr("Reading outside the UI\xE2\x80\xA6") : fo::tr("No valid properties; you can reload."),
                       bx_, top, bw_, 40, t.foreground(), fo::text::REGULAR, 13);
    }
    fl_pop_clip();
    fo::text::draw(message_, x_ + 20, y_ + h_ - 94, w_ - 40, 24, (message_error_ || stale_) ? t.red() : t.accent(),
                   fo::text::REGULAR, 11);
    fo::text::draw(objects_.size() > 1 ? fo::tr("Alt+\xE2\x86\x90/\xE2\x86\x92 object \xC2\xB7 wheel/\xE2\x86\x91/\xE2\x86\x93 \xC2\xB7 Tab permissions")
                                       : fo::tr("No Undo \xC2\xB7 wheel/\xE2\x86\x91/\xE2\x86\x93 \xC2\xB7 Home/End \xC2\xB7 Tab permissions"),
                   x_ + 20, y_ + h_ - 70, w_ - 40, 20, t.dim(), fo::text::REGULAR, 10);
    const int third = (w_ - 56) / 3, byy = y_ + h_ - 42;
    button(x_ + 20, byy, third, 30, fo::tr("Reload"), kReload, !pending_, false);
    button(x_ + 28 + third, byy, third, 30, pending_ ? fo::tr("Cancel") : fo::tr("Close"), kClose, true, false);
    button(x_ + 36 + 2 * third, byy, third, 30, w_ < 520 ? fo::tr("Apply") : fo::tr("Apply permissions"), kApply,
           editable() && has_edits(), false);
}

} // namespace flo::ui
