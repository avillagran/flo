// GVfs mounts (SFTP to tailnet machines, sftp:// and smb:// bookmarks)
// through g_file_mount_enclosing_volume on the GLib thread. Mounted
// locations appear as ordinary paths under $XDG_RUNTIME_DIR/gvfs.
//
// Questions: for a trusted (tailnet) host, GVfs's host-key questions are
// answered "Log In Anyway" automatically, since Tailscale already
// authenticated the peer; for any other host they go to the UI. Passwords
// always go to the UI, and are given to GVfs for this session only.
#pragma once

#include "core/event_queue.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo {

struct MountRequest {
    uint64_t id = 0;
    enum Kind { Password, Question } kind = Password;
    std::string message;
    std::string user, domain;           // Password
    bool needs_user = false, needs_domain = false;
    std::vector<std::string> choices;   // Question
};

struct MountCallbacks {
    // The UI answers with Mounter::reply_password / reply_choice / abort.
    std::function<void(const MountRequest &)> on_request;
    // Mounted (or already mounted): `local_path` is the GVfs path of the
    // URI; `landing` is where to open it (for tailnet machines the matching
    // home folder when it exists, as Folio does).
    std::function<void(const std::string &local_path, const std::string &landing)> on_mounted;
    // Failed; `error` is empty when the user cancelled.
    std::function<void(const std::string &error)> on_failed;
};

class Mounter {
public:
    explicit Mounter(EventQueue &queue);
    ~Mounter();

    // One mount at a time per Mounter; returns false while one is running.
    bool mount(const std::string &uri, bool trusted_host, MountCallbacks callbacks);
    bool busy() const;

    void reply_password(uint64_t request, const std::string &user, const std::string &password,
                        const std::string &domain = {});
    void reply_choice(uint64_t request, int choice);
    void abort(uint64_t request);

    struct Impl;

private:
    std::shared_ptr<Impl> impl_;
};

} // namespace flo
