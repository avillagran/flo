// Files' properties and permissions dialog (Alt+Enter; properties_dialog.rs
// and properties_ui.rs): one local object at a time, up to 32 from the
// selection (Alt+Left / Alt+Right, Previous / Next). It shows the kind,
// sizes, owner, permissions, link target, type and default application,
// volume, identity and dates, the contents of folders and selections, and
// lets the user prepare the r/w/x bits and apply them explicitly (Enter or
// "Apply permissions"). Reading and the contents census run off the UI
// thread; the change works by descriptor after checking the object is the
// one read (core/properties.h). Esc closes; Tab and Space edit the bits;
// the wheel, arrows, Page keys and Home/End scroll the details.
#pragma once

#include "core/event_queue.h"
#include "core/properties.h"
#include "fltk-omarchy/window.h"

#include <atomic>
#include <memory>
#include <string>
#include <vector>

namespace flo::ui {

class PropertiesDialog : public fo::Layer {
public:
    static constexpr size_t kMaxObjects = 32;
    // Opens over `win` for `paths` (the first shown); a toast says why not
    // when the selection is empty or too large.
    static PropertiesDialog *open(fo::Window *win, const std::vector<std::string> &paths);
    ~PropertiesDialog() override;
    void draw() override;
    int handle(int event) override;
    void relayout() override;

private:
    enum Button { kPrev = 9, kNext, kReload, kClose, kApply, kExec };
    struct Hit {
        int x, y, w, h, id;
    };
    PropertiesDialog() = default;
    void start_read();
    void start_census();
    void step(int delta);
    void apply();
    void toggle(int bit);
    void press(int id);
    void scroll_by(int delta);
    void reveal_focus();
    bool key(int k);
    bool editable() const;
    bool has_edits() const;
    bool can_step(int delta) const;
    std::vector<std::string> info_lines() const;
    std::string contents_line() const;
    void layout_body();
    void closing() override;

    std::vector<std::string> objects_;
    size_t index_ = 0;
    bool pending_ = true, observed_ = false, stale_ = false;
    props::Info info_;
    std::string verified_, default_app_;
    uint32_t requested_ = 0;
    int focus_ = 0, scroll_ = 0;
    std::string message_;
    bool message_error_ = false;
    // Contents of a folder or a selection.
    bool census_running_ = false, census_done_ = false;
    props::Census census_;
    std::shared_ptr<std::atomic<bool>> census_stop_ = std::make_shared<std::atomic<bool>>(false);
    uint64_t generation_ = 0;
    AliveGuard alive_;
    Executor exec_;
    // Layout (window coordinates).
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    int bx_ = 0, by_ = 0, bw_ = 0, bh_ = 0; // the scrolled body
    struct Lines {
        std::vector<std::string> requested, info, verified, link;
        int info_top = 0, verified_top = 0, link_top = 0, permission_label = 0, exec = 0, permissions = 0, note = 0,
            height = 0;
    } lay_;
    std::vector<Hit> hits_;
};

} // namespace flo::ui
