// Files' commands palette (Ctrl+Shift+P, or Ctrl+L then ">"), and its
// places catalog (Ctrl+L then ":"): a searchable list over the window,
// each entry with its shortcut under it. Up/Down or the wheel move,
// Enter runs the highlighted entry (never the first match on its own),
// Esc closes.
#pragma once

#include "fltk-omarchy/window.h"

#include <functional>
#include <string>
#include <vector>

namespace fo {
class TextField;
}

namespace flo::ui {

struct Command {
    std::string label, keys, icon;
    std::function<void()> run;
    bool enabled = true;
};

class CommandPalette : public fo::Layer {
public:
    static CommandPalette *open(fo::Window *win, const std::string &title, const std::string &placeholder,
                                std::vector<Command> commands, const std::string &query = {});
    void draw() override;
    int handle(int event) override;
    void relayout() override;

private:
    CommandPalette() = default;
    void refilter();
    void run(int i);
    void place();
    std::string title_, placeholder_;
    std::vector<Command> all_;
    std::vector<int> shown_;
    int hi_ = 0, top_ = 0;
    fo::TextField *field_ = nullptr;
    int cx_ = 0, cy_ = 0, cw_ = 0, ch_ = 0, list_y_ = 0, rows_ = 0;
};

// Lowercase ASCII words all found in `text`.
bool command_matches(const std::string &query, const std::string &text);

} // namespace flo::ui
