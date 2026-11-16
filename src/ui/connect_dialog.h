// Files' "Connect…" as a centered dialog: the servers found on the network
// (core/server_scan.h: GVfs, Avahi, smbclient, whichever are installed; a
// spinner while the scan runs), the servers connected to lately, and a
// field for an address (smb://, sftp://, ftp://, dav://, nfs://) checked as
// Files' network::parse checks it. Choosing a server or typing one connects
// through the pane's GVfs mount (its spinner, sign-in and errors).
//
// Keys: Up/Down walk the list (the field follows), Enter connects what the
// field says, Esc closes; a double click connects a row.
#pragma once

#include "core/server_scan.h"

#include "fltk-omarchy/window.h"

#include <functional>
#include <string>
#include <vector>

namespace fo {
class Button;
class TextField;
} // namespace fo

namespace flo::ui {

class ConnectDialog : public fo::Layer {
public:
    // `connect` gets a checked address (the pane mounts it).
    static ConnectDialog *open(fo::Window *win, std::function<void(const std::string &uri)> connect);
    void draw() override;
    int handle(int event) override;
    void relayout() override;
    // For tests and screenshots.
    int rows() const { return int(rows_.size()); }
    bool scanning() const { return scanning_; }

protected:
    void closing() override;

private:
    struct Row {
        std::string name, uri, protocol, detail;
        bool recent = false;
    };
    ConnectDialog() = default;
    void rescan();
    void found(const std::vector<FoundServer> &servers);
    void rebuild();
    void pick(int i, bool connect_now);
    void submit();
    void move(int dir);
    int row_at(int ex, int ey) const;
    static void spin(void *);
    std::function<void(const std::string &)> connect_;
    std::vector<FoundServer> servers_;
    std::vector<Row> rows_;
    std::string error_;
    int hi_ = -1, top_ = 0, visible_ = 6;
    bool scanning_ = false;
    fo::TextField *field_ = nullptr;
    fo::Button *rescan_ = nullptr, *cancel_ = nullptr, *ok_ = nullptr;
    int cx_ = 0, cy_ = 0, cw_ = 0, ch_ = 0, list_y_ = 0;
};

} // namespace flo::ui
