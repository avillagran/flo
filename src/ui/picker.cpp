#include "ui/picker.h"

#include "core/fsutil.h"
#include "ui/app.h"
#include "ui/main_window.h"
#include "ui/pane.h"

#include "fltk-omarchy/dialog.h"
#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/menu.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/theme.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>

namespace flo::ui {

bool PickerSession::shows(const Entry &e) const {
    if (e.is_dir)
        return true;
    if (req.kind == PickerRequest::Kind::Open && req.directory)
        return true; // folders are picked; their files still show for context
    if (filter < 0 || filter >= int(req.filters.size()))
        return true;
    return filter_matches(req.filters[size_t(filter)], e.name, e.mime ? e.mime : "");
}

// The portal's labels carry GTK mnemonics ("_Open"): drop the underscores.
static std::string plain_label(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_' && i + 1 < s.size() && s[i + 1] == '_') {
            out += '_';
            ++i;
        } else if (s[i] != '_') {
            out += s[i];
        }
    }
    return out;
}

PickerBar::PickerBar(int x, int y, int w, int h, MainWindow *win, std::shared_ptr<PickerSession> s)
    : Fl_Group(x, y, w, h), win_(win), s_(std::move(s)) {
    box(FL_NO_BOX);
    filter_ = new fo::Button(x, y, 160, 36);
    name_ = new fo::TextField(x, y, 200, 36);
    cancel_ = new fo::Button(x, y, 100, 36);
    accept_ = new fo::Button(x, y, 100, 36);
    accept_->primary(true);
    end();
    if (!s_->saving() || s_->req.kind == PickerRequest::Kind::SaveFiles)
        name_->hide();
    if (s_->req.filters.empty() || (s_->req.kind == PickerRequest::Kind::Open && s_->req.directory))
        filter_->hide();
    filter_->on_click([this] { choose_filter(); });
    cancel_->on_click([this] { win_->picker_cancel(); });
    accept_->on_click([this] { win_->picker_accept(); });
    name_->on_enter([this] { win_->picker_accept(); });
    name_->on_escape([this] { win_->picker_cancel(); });
    if (!s_->req.current_name.empty()) {
        // Files' rename selects the stem; the extension stays.
        const std::string &n = s_->req.current_name;
        size_t dot = n.rfind('.');
        name_->set_text(n, 0, dot != std::string::npos && dot > 0 ? int(dot) : -1);
    }
    relabel();
}

void PickerBar::relabel() {
    const auto &r = s_->req;
    std::string accept = plain_label(r.accept_label);
    if (accept.empty())
        accept = r.kind != PickerRequest::Kind::Open ? fo::tr("Save")
                 : r.directory                       ? fo::tr("Select")
                                                     : fo::tr("Open");
    accept_->copy_label(accept.c_str());
    cancel_->copy_label(fo::tr("Cancel"));
    name_->placeholder(fo::tr("Name"));
    std::string f = s_->filter >= 0 && s_->filter < int(r.filters.size()) ? r.filters[size_t(s_->filter)].name
                                                                          : fo::tr("All files");
    filter_->copy_label((f + "  \xE2\x96\xBE").c_str());
    layout_children();
}

void PickerBar::layout_children() {
    const int pad = 16, gap = 8, bh = 36, by = y() + (h() - bh) / 2;
    int right = x() + w() - pad;
    int aw = std::max(100, accept_->preferred_width());
    accept_->resize(right - aw, by, aw, bh);
    right -= aw + gap;
    int cw = std::max(100, cancel_->preferred_width());
    cancel_->resize(right - cw, by, cw, bh);
    right -= cw + 2 * gap;
    int left = x() + pad;
    if (filter_->visible()) {
        int fw = std::min(220, std::max(120, filter_->preferred_width()));
        filter_->resize(left, by, fw, bh);
        left += fw + gap;
    }
    if (name_->visible())
        name_->resize(left, by, std::max(80, right - left), bh);
}

void PickerBar::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H); // children are placed by hand
    layout_children();
}

void PickerBar::draw() {
    const fo::Theme &t = fo::theme();
    fo::paint::fill(x(), y(), w(), h(), t.background());
    fo::paint::fill(x(), y(), w(), 1, t.card());
    draw_children();
}

void PickerBar::set_name(const std::string &n) {
    size_t dot = n.rfind('.');
    name_->set_text(n, 0, dot != std::string::npos && dot > 0 ? int(dot) : -1);
}

std::string PickerBar::name() const { return name_->value(); }

void PickerBar::focus_name() {
    if (name_->visible())
        name_->take_focus();
}

void PickerBar::choose_filter() {
    std::vector<fo::MenuItem> m;
    for (size_t i = 0; i < s_->req.filters.size(); ++i)
        m.push_back(fo::MenuItem(s_->req.filters[i].name, "", [this, i] {
                        s_->filter = int(i);
                        relabel();
                        win_->picker_filter_changed();
                    }).check(int(i) == s_->filter));
    fo::PopupMenu::open_below(win_, filter_->x(), filter_->y(), filter_->w(), filter_->h(), m);
}

} // namespace flo::ui

int run_portal() { return flo::ui::App::get().run_portal(); }
