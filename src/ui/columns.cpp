#include "ui/columns.h"

#include "core/fsutil.h"
#include "core/network.h"
#include "core/xdg.h"
#include "ui/app.h"
#include "ui/file_view.h"
#include "ui/pane.h"

#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/paint.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

namespace flo::ui {

ColumnsView::ColumnsView(int x, int y, int w, int h, Pane *pane) : Fl_Group(x, y, w, h), pane_(pane) {
    box(FL_NO_BOX);
    here_ = new FileView(x, y, kColumn, h, &pane->model());
    here_->mode(FileView::Mode::Column);
    here_->empty_text(fo::tr("Empty"));
    end();
    resizable(nullptr);
    clip_children(1);
}

ColumnsView::~ColumnsView() = default;

std::string ColumnsView::root_of(const std::string &folder) const {
    const std::string home = xdg::home();
    if (path_inside(folder, home))
        return home;
    // A mounted machine: its gvfs folder is the root.
    const std::string gvfs = gvfs_root();
    if (!gvfs.empty() && path_inside(folder, gvfs) && folder != gvfs) {
        std::string rest = folder.substr(gvfs.size() + 1);
        return gvfs + "/" + rest.substr(0, rest.find('/'));
    }
    return "/";
}

ColumnsView::Column *ColumnsView::make_column(const std::string &path) {
    auto c = std::make_unique<Column>();
    c->path = path;
    c->model.base = path;
    c->lister = std::make_unique<Lister>(App::get().queue());
    begin();
    c->view = new FileView(x(), y(), kColumn, h(), &c->model);
    end();
    c->view->mode(FileView::Mode::Column);
    c->view->empty_text(fo::tr("Empty"));
    Column *raw = c.get();
    // Clicking in another column goes there (a file: its folder, selected).
    c->view->on_selection_change([this, raw] {
        int i = raw->view->current();
        if (i < 0 || !raw->view->selected(i) || Fl::event() != FL_PUSH)
            return;
        std::string p = raw->model.path(i);
        // Later: navigating replaces this column.
        if (raw->model.entries[size_t(i)].is_dir)
            defer([this, p] { pane_->navigate(p); });
        else
            defer([this, p] { pane_->show_item(p); });
    });
    c->view->on_activate([this, raw](int i) { pane_->open_path(raw->model.path(i)); });
    c->view->on_context([this, raw](int i, int ex, int ey) { pane_->context_menu(i, ex, ey, &raw->model, raw->path); });
    c->view->accepts_drop([raw](int i) { return i < 0 || raw->model.entries[size_t(i)].is_dir; });
    c->view->on_drop([this, raw](int i, const std::string &text) {
        bool cut = false;
        auto paths = App::get().paths_from_clipboard(text, cut);
        pane_->drop_into(paths, i < 0 ? raw->path : raw->model.path(i));
    });
    c->view->clear_visible_focus(); // the keyboard stays in the current folder
    trail_.push_back(std::move(c));
    return raw;
}

void ColumnsView::list(Column &c) {
    ListOptions o;
    o.show_hidden = pane_->show_hidden();
    o.sort = pane_->sort();
    Column *raw = &c;
    c.model.loading = true;
    c.lister->list(c.path, o, [this, raw](ListingPtr l) {
        raw->model.entries = l->entries;
        raw->model.error = l->error;
        raw->model.loading = !l->complete;
        raw->view->reload(true);
        raw->view->clear_selection();
        int at = raw->model.index_of_name(raw->next);
        if (at >= 0)
            raw->view->ensure_visible(at, false);
        raw->view->redraw();
    });
}

void ColumnsView::sync() {
    const std::string folder = pane_->folder();
    std::vector<std::pair<std::string, std::string>> want; // path, next
    if (folder != kRecents && folder.size() > 0 && folder[0] == '/') {
        std::string root = root_of(folder);
        std::vector<std::string> chain;
        for (std::string p = folder; p != root && p != "/" && !p.empty(); p = path_dirname(p))
            chain.push_back(p);
        chain.push_back(root);
        // chain: folder ... root; ancestors are chain[1..]
        for (size_t i = chain.size(); i-- > 1;)
            want.push_back({chain[i], path_basename(chain[i - 1])});
    }
    size_t same = 0;
    while (same < trail_.size() && same < want.size() && trail_[same]->path == want[same].first)
        ++same;
    for (size_t i = 0; i < same; ++i)
        if (trail_[i]->next != want[i].second) {
            trail_[i]->next = want[i].second;
            trail_[i]->view->trail(want[i].second);
        }
    while (trail_.size() > same) {
        Fl::delete_widget(trail_.back()->view);
        remove(trail_.back()->view);
        trail_.pop_back();
    }
    for (size_t i = same; i < want.size(); ++i) {
        make_column(want[i].first);
        Column &c = *trail_.back();
        c.next = want[i].second;
        c.view->trail(c.next);
        list(c);
    }
    selection_changed();
    place();
}

void ColumnsView::refresh() {
    for (auto &c : trail_)
        list(*c);
    if (inside_)
        list(*inside_);
}

void ColumnsView::selection_changed() {
    int i = here_->current();
    const FolderModel &m = pane_->model();
    std::string want;
    if (i >= 0 && i < m.count() && m.entries[size_t(i)].is_dir && here_->selection_count() <= 1)
        want = m.path(i);
    if (want.empty()) {
        if (inside_) {
            Fl::delete_widget(inside_->view);
            remove(inside_->view);
            inside_.reset();
            place();
        }
        return;
    }
    if (inside_ && inside_->path == want)
        return;
    if (!inside_) {
        begin();
        inside_ = std::make_unique<Column>();
        inside_->lister = std::make_unique<Lister>(App::get().queue());
        inside_->view = new FileView(x(), y(), kColumn, h(), &inside_->model);
        end();
        inside_->view->mode(FileView::Mode::Column);
        inside_->view->empty_text(fo::tr("Empty"));
        inside_->view->clear_visible_focus();
        Column *raw = inside_.get();
        inside_->view->on_selection_change([this, raw] {
            int k = raw->view->current();
            if (k < 0 || !raw->view->selected(k) || Fl::event() != FL_PUSH)
                return;
            std::string p = raw->model.path(k);
            defer([this, p] { pane_->show_item(p); });
        });
        inside_->view->on_activate([this, raw](int k) { pane_->open_path(raw->model.path(k)); });
        inside_->view->on_context(
            [this, raw](int k, int ex, int ey) { pane_->context_menu(k, ex, ey, &raw->model, raw->path); });
    }
    inside_->path = want;
    inside_->model.base = want;
    inside_->model.entries.clear();
    inside_->view->reload(true);
    inside_->view->clear_selection();
    list(*inside_);
    place();
}

void ColumnsView::place() {
    int n = int(trail_.size()) + 1 + (inside_ ? 1 : 0);
    int total = n * (kColumn + 1);
    scroll_x_ = std::max(0, total - w());
    int cx = x() - scroll_x_;
    for (auto &c : trail_) {
        c->view->resize(cx, y(), kColumn, h());
        cx += kColumn + 1;
    }
    here_->resize(cx, y(), kColumn, h());
    cx += kColumn + 1;
    if (inside_)
        inside_->view->resize(cx, y(), kColumn, h());
    redraw();
}

void ColumnsView::resize(int X, int Y, int W, int H) {
    Fl_Widget::resize(X, Y, W, H);
    place();
}

void ColumnsView::draw() {
    const fo::Theme &t = fo::theme();
    fl_push_clip(x(), y(), w(), h());
    if (damage() & ~FL_DAMAGE_CHILD)
        fo::paint::fill(x(), y(), w(), h(), t.background());
    draw_children();
    // Hairlines between the columns.
    int n = int(trail_.size()) + 1 + (inside_ ? 1 : 0);
    for (int i = 1; i <= n; ++i) {
        int lx = x() - scroll_x_ + i * (kColumn + 1) - 1;
        if (lx >= x() && lx < x() + w())
            fo::paint::vline(lx, y(), h(), t.foreground(), 0.10);
    }
    fl_pop_clip();
}

int ColumnsView::handle(int event) { return Fl_Group::handle(event); }

} // namespace flo::ui
