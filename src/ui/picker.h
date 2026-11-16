// Flo as the desktop's file chooser (the xdg-desktop-portal backend in
// core/portal_service.h). A picker is an ordinary Flo window with a bar at
// the bottom: the filters, the name to save under, Cancel and the accept
// button (the portal's accept_label, or Open / Save / Select). Opening a
// file in the browser picks it; folders open as usual.
#pragma once

#include "core/entry.h"
#include "core/portal_service.h"

#include <FL/Fl_Group.H>

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fo {
class Button;
class TextField;
} // namespace fo

namespace flo::ui {

class MainWindow;

// One portal request and its answer (given once).
struct PickerSession {
    PickerRequest req;
    PortalService::Reply reply;
    int filter = -1;
    bool answered = false;
    void answer(const PickerResult &r) {
        if (answered)
            return;
        answered = true;
        if (reply)
            reply(r);
    }
    bool saving() const { return req.kind != PickerRequest::Kind::Open; }
    // Whether an entry shows under the current filter (folders always).
    bool shows(const Entry &e) const;
};

class PickerBar : public Fl_Group {
public:
    static constexpr int kHeight = 56;
    PickerBar(int x, int y, int w, int h, MainWindow *win, std::shared_ptr<PickerSession> s);
    void resize(int x, int y, int w, int h) override;
    void draw() override;
    // The name field (Save), to fill from a click on a file.
    void set_name(const std::string &name);
    std::string name() const;
    void focus_name();
    void relabel();

private:
    void layout_children();
    void choose_filter();
    MainWindow *win_;
    std::shared_ptr<PickerSession> s_;
    fo::Button *filter_ = nullptr, *cancel_ = nullptr, *accept_ = nullptr;
    fo::TextField *name_ = nullptr;
};

} // namespace flo::ui

// `flo --portal`: owns the backend's bus name and opens a picker window per
// request; leaves after a minute with nothing open. src/ui/picker.cpp.
int run_portal();
