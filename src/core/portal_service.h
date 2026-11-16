// Flo as the desktop's file chooser: the xdg-desktop-portal backend
// interface org.freedesktop.impl.portal.FileChooser (OpenFile, SaveFile,
// SaveFiles) on the bus name org.freedesktop.impl.portal.desktop.flo,
// object /org/freedesktop/portal/desktop. `flo --portal` owns it; D-Bus
// starts that process when xdg-desktop-portal first asks (the service file
// in packaging/), and it leaves once no picker has been open for a while.
//
// Each call becomes a PickerRequest handed to the UI thread; the window
// answers with a PickerResult and the reply goes back over D-Bus. The
// Request object at the call's handle answers Close() by cancelling.
// GDBus runs on the GLib thread (core/gmain_thread.h), like Flo's
// FileManager1 service.
#pragma once

#include "core/event_queue.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

typedef struct _GVariant GVariant;

namespace flo {

// A filter as the portal sends it: a name and rules, each a glob (0) such
// as "*.png" or a MIME type (1) such as "image/png" or "image/*".
struct PickerFilter {
    std::string name;
    std::vector<std::pair<uint32_t, std::string>> rules;
    bool operator==(const PickerFilter &o) const { return name == o.name && rules == o.rules; }
};
// True when a file named `name` of type `mime` passes (globs ignore case).
bool filter_matches(const PickerFilter &f, const std::string &name, const std::string &mime);

// One of the portal's extra choices (a check box or a list), returned as
// its default: Flo shows none of them.
struct PickerChoice {
    std::string id, label, value;
};

struct PickerRequest {
    enum class Kind { Open, Save, SaveFiles };
    Kind kind = Kind::Open;
    std::string handle, app_id, parent_window, title;
    std::string accept_label;
    bool multiple = false;  // OpenFile: more than one item
    bool directory = false; // OpenFile: folders instead of files
    bool modal = true;
    std::vector<PickerFilter> filters;
    int current_filter = -1; // index into filters (the current filter is added when missing)
    std::vector<PickerChoice> choices;
    std::string current_folder; // a local path ("" when none)
    std::string current_name;   // SaveFile: the suggested name
    std::string current_file;   // SaveFile: the file being saved again
    std::vector<std::string> files; // SaveFiles: the names to save
};

// What the picker answered: 0 success, 1 cancelled by the user, 2 ended
// another way (closed by the portal, an error).
struct PickerResult {
    uint32_t response = 2;
    std::vector<std::string> uris;
    int filter = -1; // the filter shown when it was answered
};

// Reads OpenFile/SaveFile/SaveFiles options (an a{sv} GVariant) into
// `out` (its kind already set). Unknown keys are ignored; malformed ones
// are skipped.
void parse_picker_options(GVariant *options, PickerRequest &out);
// The results a{sv} for a reply (floating GVariant): "uris", and
// "current_filter" and "choices" when the request had them.
GVariant *picker_results(const PickerRequest &req, const PickerResult &res);

// The URIs a picker answers with, from what its window has: the folder
// shown, the marked paths (or the focused one) and whether each is a
// folder, and the name typed (SaveFile). Returns "" or the reason it can't
// answer yet (an English source string); `navigate` is set instead when a
// single marked folder should open (OpenFile of files). Pure.
std::string picker_choice(const PickerRequest &req, const std::string &folder, const std::vector<std::string> &marked,
                          const std::vector<bool> &marked_dirs, const std::string &name, std::vector<std::string> &uris,
                          std::string &navigate);

constexpr const char *kPortalObjectPath = "/org/freedesktop/portal/desktop";
constexpr const char *kPortalImplBusName = "org.freedesktop.impl.portal.desktop.flo";

class PortalService {
public:
    // `on_request` runs on the UI thread with the request and a reply
    // function (call it once, from the UI thread). `on_close` runs when the
    // portal closes the request by its handle.
    using Reply = std::function<void(const PickerResult &)>;
    PortalService(EventQueue &queue, std::function<void(PickerRequest, Reply)> on_request,
                  std::function<void(const std::string &handle)> on_close);
    ~PortalService();
    // Owns the bus name; `on_ready(true)` once owned, false when lost or
    // taken by another process.
    void start(std::function<void(bool)> on_ready = {});
    // Requests waiting for an answer.
    int pending() const;

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace flo
