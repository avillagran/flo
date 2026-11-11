// D-Bus: org.freedesktop.FileManager1 ("Show in folder" from other apps) and
// Flo's own single-instance name, io.github.avillagran.Flo, so `flo PATH`
// and Ctrl+N open a window in the running process. GDBus on the GLib thread;
// handlers run on the UI thread.
#pragma once

#include "core/event_queue.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo {

struct DBusHandlers {
    // FileManager1.ShowFolders: open these folders (Folio shows the first).
    std::function<void(const std::vector<std::string> &paths)> show_folders;
    // FileManager1.ShowItems: open each item's folder with it selected.
    std::function<void(const std::vector<std::string> &paths)> show_items;
    // FileManager1.ShowItemProperties: Folio shows the item (no dialog).
    std::function<void(const std::vector<std::string> &paths)> show_item_properties;
    // io.github.avillagran.Flo.Open: a `flo` launched again. Empty `path`
    // means home; `select` names the file to select in it.
    std::function<void(const std::string &folder, const std::string &select)> open_window;
};

constexpr const char *kFloBusName = "io.github.avillagran.Flo";
constexpr const char *kFloObjectPath = "/io/github/avillagran/Flo";
constexpr const char *kFileManager1Name = "org.freedesktop.FileManager1";

class DBusService {
public:
    DBusService(EventQueue &queue, DBusHandlers handlers);
    ~DBusService();

    // Owns Flo's name (and FileManager1 unless told not to). `on_ready`
    // reports whether Flo's name was acquired (false: another Flo has it).
    void start(bool own_filemanager1, std::function<void(bool)> on_ready = {});

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

// Client side, synchronous (before any window exists): asks a running Flo
// to open `folder` (selecting `select`). False when no Flo is running or
// the call failed within `timeout_ms`.
bool forward_open_to_running_flo(const std::string &folder, const std::string &select, int timeout_ms = 2000);

// Turns FileManager1 URIs into local paths (non-local URIs dropped).
std::vector<std::string> local_paths_from_uris(const std::vector<std::string> &uris);

} // namespace flo
