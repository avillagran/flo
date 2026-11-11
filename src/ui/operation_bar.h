// Files' optional MC-style operation bar (Ctrl+Alt+B, operation_bar.rs): a
// 36 px row above the status bar with eight buttons, each showing its key:
// F1 Help, F2 Rename, F3 Preview, F5 Copy (Recalculate in the Space view),
// F6 Move, F7 Folder, F8 Trash, F10 Actions. A click runs the same command
// as the key, on the current selection; a button that cannot run now is
// muted and, when clicked, says why. Shown or hidden from the setting
// `operation-bar`.
#pragma once

#include <FL/Fl_Widget.H>

#include <functional>
#include <string>

namespace flo::ui {

class OperationBar : public Fl_Widget {
public:
    static constexpr int kHeight = 36, kButtons = 8;
    OperationBar(int x, int y, int w, int h);
    // Why button `i` cannot run now ("" when it can).
    void reason(std::function<std::string(int)> fn) { reason_ = std::move(fn); }
    // Runs button `i`.
    void on_run(std::function<void(int)> fn) { run_ = std::move(fn); }
    // Button 3 reads "Recalculate" (the Space view).
    void space(bool on) {
        if (on != space_) {
            space_ = on;
            redraw();
        }
    }
    void draw() override;
    int handle(int event) override;

private:
    int at(int ex) const;
    std::function<std::string(int)> reason_;
    std::function<void(int)> run_;
    bool space_ = false;
    int hover_ = -1;
};

} // namespace flo::ui
