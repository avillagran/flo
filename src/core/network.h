// The sidebar's network part, as Folio has it: online tailnet machines from
// `tailscale status --json`, and Dropbox's sync state from `dropbox-cli
// status`. Both commands run as child processes off the UI thread; nothing
// polls in the background unless the UI asks (Folio refreshes every 15 s
// while a window is open).
#pragma once

#include "core/event_queue.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo {

struct TailnetPeer {
    std::string name; // HostName ("laptop")
    std::string host; // MagicDNS name without the trailing dot ("laptop.tail1234.ts.net")
    std::string os;   // "linux", "macOS", "windows"
    std::string ip;   // first Tailscale IP
    bool online = false;
};

// Parses `tailscale status --json`. With `browsable_only`, keeps what Folio
// shows: online machines that can hold files over SFTP (Linux, macOS,
// Windows), sorted by name.
std::vector<TailnetPeer> parse_tailscale_status(const std::string &json, bool browsable_only = true);
// "synced", "syncing" or "stopped" from `dropbox-cli status` output.
std::string parse_dropbox_status(const std::string &output);

// The SFTP URI for a peer and where GVfs mounts it locally
// ($XDG_RUNTIME_DIR/gvfs/sftp:host=HOST).
std::string sftp_uri(const std::string &host);
std::string gvfs_root();
std::string gvfs_sftp_path(const std::string &host);

class Network {
public:
    explicit Network(EventQueue &queue);
    ~Network();

    // Runs `tailscale status --json` (if installed) and reports the peers;
    // an empty list when Tailscale is missing, stopped or logged out.
    void refresh_tailnet(std::function<void(std::vector<TailnetPeer>)> done);
    // Runs `dropbox-cli status` when ~/Dropbox exists and the CLI is
    // installed; reports "" otherwise.
    void refresh_dropbox(std::function<void(std::string)> done);
    // The last peers delivered (UI thread).
    const std::vector<TailnetPeer> &peers() const { return peers_; }
    bool is_tailnet_host(const std::string &host) const;

private:
    EventQueue::Poster poster_;
    Executor exec_;
    AliveGuard alive_;
    std::vector<TailnetPeer> peers_;
};

} // namespace flo
