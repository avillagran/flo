#include "ui/connect_dialog.h"

#include "core/network.h"
#include "ui/app.h"

#include "fltk-omarchy/anim.h"
#include "fltk-omarchy/i18n.h"
#include "fltk-omarchy/icons.h"
#include "fltk-omarchy/paint.h"
#include "fltk-omarchy/shell.h"
#include "fltk-omarchy/text.h"
#include "fltk-omarchy/theme.h"
#include "fltk-omarchy/widgets.h"

#include <FL/Fl.H>
#include <FL/fl_draw.H>

#include <algorithm>

namespace flo::ui {

namespace {
// The toolkit's dialog metrics (Files' sheets): 28 px padding, 22 px
// title, 38 px fields, 36 px buttons, 16 px gaps; rows of two lines.
const int kPad = 28, kTitleH = 22, kFieldH = 38, kButtonH = 36, kGap = 16, kRowH = 44, kSectionH = 24;
const char *kStart = "smb://"; // Files' connect_server_prompt starts here

ConnectDialog *g_open = nullptr; // the dialog a scan reports to

std::string protocol_of(const std::string &uri) {
    std::string scheme = uri.substr(0, uri.find(':'));
    if (scheme == "smb")
        return "SMB";
    if (scheme == "sftp" || scheme == "ssh")
        return "SFTP";
    if (scheme == "ftp" || scheme == "ftps")
        return "FTP";
    if (scheme == "dav" || scheme == "davs")
        return "WebDAV";
    if (scheme == "nfs")
        return "NFS";
    if (scheme == "afp")
        return "AFP";
    return {};
}

const char *icon_of(const std::string &protocol) {
    if (protocol == "SFTP")
        return "terminal";
    if (protocol == "WebDAV")
        return "globe";
    return "server";
}
} // namespace

ConnectDialog *ConnectDialog::open(fo::Window *win, std::function<void(const std::string &)> connect) {
    if (!win)
        return nullptr;
    if (g_open)
        g_open->close();
    auto *d = new ConnectDialog();
    g_open = d;
    d->connect_ = std::move(connect);
    d->begin();
    d->field_ = new fo::TextField(0, 0, 100, kFieldH);
    d->rescan_ = new fo::Button(0, 0, 100, kButtonH);
    d->cancel_ = new fo::Button(0, 0, 100, kButtonH);
    d->ok_ = new fo::Button(0, 0, 100, kButtonH);
    d->end();
    d->rescan_->copy_label(fo::tr("Search again"));
    d->cancel_->copy_label(fo::tr("Cancel"));
    d->ok_->copy_label(fo::tr("Connect"));
    d->ok_->primary(true);
    d->field_->placeholder(fo::tr("smb://, sftp://, ftp://, dav:// or nfs:// and the server"));
    d->field_->set_text(kStart, int(std::char_traits<char>::length(kStart)), -1);
    d->field_->on_enter([d] { d->submit(); });
    d->field_->on_escape([d] { d->close(); });
    d->field_->on_change([d] {
        if (!d->error_.empty()) {
            d->error_.clear();
            d->redraw();
        }
    });
    d->field_->on_key([d](int k) {
        if (k == FL_Down || k == FL_Up) {
            d->move(k == FL_Down ? 1 : -1);
            return true;
        }
        return false;
    });
    d->rescan_->on_click([d] { d->rescan(); });
    d->cancel_->on_click([d] { d->close(); });
    d->ok_->on_click([d] { d->submit(); });
    d->servers_ = App::get().found_servers();
    d->fo::Layer::open(win);
    d->rebuild();
    d->relayout();
    d->field_->take_focus();
    d->fade_in();
    d->rescan();
    return d;
}

void ConnectDialog::closing() {
    Fl::remove_timeout(spin, this);
    if (g_open == this)
        g_open = nullptr;
}

void ConnectDialog::rescan() {
    if (scanning_)
        return;
    scanning_ = App::get().server_scan().start([](std::vector<FoundServer> found) {
        App::get().found_servers() = found;
        if (g_open)
            g_open->found(found);
    });
    if (scanning_ && fo::anim::enabled())
        Fl::add_timeout(1.0 / 30, spin, this);
    rescan_->deactivate();
    redraw();
}

void ConnectDialog::spin(void *data) {
    auto *d = static_cast<ConnectDialog *>(data);
    if (!d->scanning_)
        return;
    d->redraw();
    Fl::repeat_timeout(1.0 / 30, spin, data);
}

void ConnectDialog::found(const std::vector<FoundServer> &servers) {
    scanning_ = false;
    Fl::remove_timeout(spin, this);
    servers_ = servers;
    rescan_->activate();
    rebuild();
    redraw();
}

void ConnectDialog::rebuild() {
    std::string keep = hi_ >= 0 && hi_ < int(rows_.size()) ? rows_[size_t(hi_)].uri : std::string();
    rows_.clear();
    for (const auto &uri : App::get().settings().recent_servers)
        rows_.push_back({uri, uri, protocol_of(uri), "", true});
    for (const auto &s : servers_)
        rows_.push_back({s.name, s.uri, s.protocol, s.detail, false});
    hi_ = -1;
    for (size_t i = 0; i < rows_.size(); ++i)
        if (!keep.empty() && rows_[i].uri == keep)
            hi_ = int(i);
    top_ = std::max(0, std::min(top_, int(rows_.size()) - visible_));
}

void ConnectDialog::relayout() {
    if (!host())
        return;
    cw_ = std::min(w() - 48, 600);
    visible_ = std::max(2, std::min(6, (h() - 360) / kRowH));
    // Title, status, list (with up to two section labels), field, error, buttons.
    int list_h = visible_ * kRowH + 2 * kSectionH;
    ch_ = 2 * kPad + kTitleH + 8 + 20 + kGap + list_h + kGap + kFieldH + 6 + 18 + kGap + kButtonH;
    cx_ = (w() - cw_) / 2;
    cy_ = std::max(16, (h() - ch_) / 2);
    int inner = cw_ - 2 * kPad;
    list_y_ = cy_ + kPad + kTitleH + 8 + 20 + kGap;
    int fy = list_y_ + list_h + kGap;
    field_->resize(cx_ + kPad, fy, inner, kFieldH);
    int by = cy_ + ch_ - kPad - kButtonH;
    int right = cx_ + cw_ - kPad;
    int ow = std::max(100, ok_->preferred_width());
    ok_->resize(right - ow, by, ow, kButtonH);
    right -= ow + 8;
    int cw = std::max(100, cancel_->preferred_width());
    cancel_->resize(right - cw, by, cw, kButtonH);
    int rw = std::max(100, rescan_->preferred_width());
    rescan_->resize(cx_ + kPad, by, rw, kButtonH);
}

// Rows and section labels laid out top to bottom from `top_`.
int ConnectDialog::row_at(int ex, int ey) const {
    if (ex < cx_ + kPad - 8 || ex >= cx_ + cw_ - kPad + 8)
        return -1;
    int y = list_y_;
    bool recent_label = false, network_label = false;
    for (int i = top_, shown = 0; i < int(rows_.size()) && shown < visible_; ++i, ++shown) {
        if (rows_[size_t(i)].recent && !recent_label) {
            recent_label = true;
            y += kSectionH;
        } else if (!rows_[size_t(i)].recent && !network_label) {
            network_label = true;
            y += kSectionH;
        }
        if (ey >= y && ey < y + kRowH)
            return i;
        y += kRowH;
    }
    return -1;
}

void ConnectDialog::move(int dir) {
    int n = int(rows_.size());
    if (!n)
        return;
    hi_ = hi_ < 0 ? (dir > 0 ? 0 : n - 1) : std::max(0, std::min(n - 1, hi_ + dir));
    if (hi_ < top_)
        top_ = hi_;
    if (hi_ >= top_ + visible_)
        top_ = hi_ - visible_ + 1;
    pick(hi_, false);
}

void ConnectDialog::pick(int i, bool connect_now) {
    if (i < 0 || i >= int(rows_.size()))
        return;
    hi_ = i;
    const std::string &uri = rows_[size_t(i)].uri;
    field_->set_text(uri, int(uri.size()), -1);
    error_.clear();
    redraw();
    if (connect_now)
        submit();
}

void ConnectDialog::submit() {
    std::string text = field_->value();
    NetworkLocation loc;
    std::string why = parse_network_uri(text, loc);
    if (!why.empty()) {
        // Files' parse() reasons, under the field; the dialog stays.
        error_ = fo::tr(why.c_str());
        redraw();
        field_->take_focus();
        return;
    }
    auto fn = connect_;
    close();
    if (fn)
        fn(text);
}

int ConnectDialog::handle(int event) {
    const int ex = Fl::event_x(), ey = Fl::event_y();
    switch (event) {
    case FL_PUSH:
        if (ex < cx_ || ex >= cx_ + cw_ || ey < cy_ || ey >= cy_ + ch_) {
            close(); // a click on the scrim cancels
            return 1;
        }
        if (int i = row_at(ex, ey); i >= 0) {
            pick(i, Fl::event_clicks() > 0);
            return 1;
        }
        break;
    case FL_MOUSEWHEEL:
        if (ey >= list_y_ && ey < field_->y()) {
            top_ = std::max(0, std::min(std::max(0, int(rows_.size()) - visible_), top_ + Fl::event_dy()));
            redraw();
            return 1;
        }
        break;
    case FL_KEYBOARD:
        if (Fl::event_key() == FL_Escape) {
            close();
            return 1;
        }
        break;
    }
    int r = fo::Layer::handle(event);
    return r ? r : 1; // modal: nothing below gets the event
}

void ConnectDialog::draw() {
    const fo::Theme &t = fo::theme();
    if (cw_ == 0)
        relayout();
    double a = presence();
    fo::paint::fill(x(), y(), w(), h(), fo::Rgb(0, 0, 0), 0.55 * a);
    fo::paint::push_opacity();
    const int r = t.radius() > 0 ? 10 : 0;
    fo::paint::round_fill(cx_, cy_, cw_, ch_, r, t.accent());
    fo::paint::round_fill(cx_ + 1, cy_ + 1, cw_ - 2, ch_ - 2, r, t.background());
    const int inner = cw_ - 2 * kPad;
    fo::text::draw(fo::tr("Connect to server"), cx_ + kPad, cy_ + kPad, inner, kTitleH, t.bright_foreground(),
                   fo::text::SEMIBOLD, fo::text::size::HEADING);
    // The scan's state under the title.
    int sy = cy_ + kPad + kTitleH + 8;
    std::string status;
    if (scanning_) {
        fo::paint::spinner(cx_ + kPad + 7, sy + 10, 7, t.accent(), t.background());
        status = fo::tr("Looking for servers on this network\xE2\x80\xA6");
    } else if (servers_.empty()) {
        status = fo::tr("No servers found on this network; type an address below");
    } else {
        status = fo::trn("%n servers found on this network", int(servers_.size()));
    }
    fo::text::draw(status, cx_ + kPad + (scanning_ ? 22 : 0), sy, inner - (scanning_ ? 22 : 0), 20, t.dim(),
                   fo::text::REGULAR, 12);
    // The list: recent servers first, then what the scan found.
    int list_h = visible_ * kRowH + 2 * kSectionH;
    fl_push_clip(cx_ + 1, list_y_, cw_ - 2, list_h);
    int y = list_y_;
    bool recent_label = false, network_label = false;
    for (int i = top_, shown = 0; i < int(rows_.size()) && shown < visible_; ++i, ++shown) {
        const Row &row = rows_[size_t(i)];
        if (row.recent && !recent_label) {
            recent_label = true;
            fo::draw_section_label(fo::tr("Recent servers"), cx_ + kPad, y, inner, kSectionH);
            y += kSectionH;
        } else if (!row.recent && !network_label) {
            network_label = true;
            fo::draw_section_label(fo::tr("On this network"), cx_ + kPad, y, inner, kSectionH);
            y += kSectionH;
        }
        if (i == hi_)
            fo::paint::round_fill(cx_ + kPad - 8, y + 2, inner + 16, kRowH - 4, t.radius() > 0 ? 5 : 0,
                                  t.selection());
        fo::icon::draw(row.recent ? "history" : icon_of(row.protocol), cx_ + kPad, y + (kRowH - 16) / 2, 16,
                       i == hi_ ? t.accent() : t.dim());
        int lx = cx_ + kPad + 28, lw = inner - 28 - 70;
        fo::text::draw(row.name, lx, y + 5, lw, 18, t.bright_foreground(), fo::text::REGULAR, 13);
        std::string sub = row.recent ? std::string() : row.uri;
        if (!row.detail.empty() && !row.recent)
            sub += " \xC2\xB7 " + row.detail;
        if (!sub.empty())
            fo::text::draw(sub, lx, y + 23, lw, 16, t.dim(), fo::text::REGULAR, 11, FL_ALIGN_LEFT,
                           fo::text::Elide::Middle);
        fo::text::draw(row.protocol, cx_ + cw_ - kPad - 70, y, 70, kRowH, t.dim(), fo::text::REGULAR, 11,
                       FL_ALIGN_RIGHT);
        y += kRowH;
    }
    if (rows_.empty())
        fo::text::draw(scanning_ ? std::string() : fo::tr("Nothing yet: servers you connect to are listed here"),
                       cx_ + kPad, list_y_, inner, kRowH, t.muted(), fo::text::REGULAR, 12);
    fl_pop_clip();
    if (!error_.empty())
        fo::text::draw(error_, cx_ + kPad, field_->y() + kFieldH + 4, inner, 18, t.red(), fo::text::REGULAR, 12);
    draw_children();
    fo::paint::pop_opacity(a, card_scale(a), cx_ + cw_ / 2.0, cy_ + ch_ / 2.0);
}

} // namespace flo::ui
