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

// Files' network locations (network.rs): a URI typed after "Connect…"
// is checked here and mounted through the session's GVfs; every connected
// location is a folder under the FUSE bridge ($XDG_RUNTIME_DIR/gvfs).

// URI schemes handed to GVfs ("ssh" is GVfs's alias of "sftp").
bool is_network_uri(const std::string &text);
// A checked location: the URI without any password, its user, and the
// password when one was typed inline (ftp://u:pw@host).
struct NetworkLocation {
    std::string uri, user, inline_password;
};
// Files' parse(): known scheme, a server, one line, no spaces, at most
// 2048 bytes. Returns "" or the reason (an English source string).
std::string parse_network_uri(const std::string &text, NetworkLocation &out);

// A connected location as the FUSE bridge names it
// ("smb-share:server=nas,share=fotos", "sftp:host=h,port=2222,user=ana").
struct GvfsShare {
    std::string path;     // the bridge folder
    std::string uri;      // what disconnecting takes (the path when unknown)
    std::string protocol; // SMB, SFTP, FTP, FTPS, WebDAV, NFS, AFP, "" when unknown
    std::string title;    // the rail label: "nas", "nas/dav", "nas:/export"
    std::string share;    // with a share or volume the label is "%1 on %2" (share, title)
    std::string user;
    bool operator==(const GvfsShare &o) const { return path == o.path && uri == o.uri && title == o.title; }
};
GvfsShare gvfs_share(const std::string &path, const std::string &name);
// Connected locations, sorted by label (hidden names skipped). Reading the
// bridge root is answered by gvfsd-fuse from memory, never by a server.
std::vector<GvfsShare> scan_gvfs(const std::string &dir);
// The bridge folder when it exists ("" otherwise); with FLO_TEST_RUN, the
// FLO_TEST_FAKE_GVFS fixture folder.
std::string gvfs_dir();
// Files' unmount(): `gio mount -u`, retried a few times while the bridge
// still holds a closed file. Returns "" or the reason. Under the fake GVfs
// the bridge folder is moved aside (renamed), never deleted.
std::string unmount_location(const GvfsShare &share);

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

    // Lists the connected GVfs locations off the UI thread.
    void refresh_shares(std::function<void(std::vector<GvfsShare>)> done);
    const std::vector<GvfsShare> &shares() const { return shares_; }
    // Disconnects one (Files: Desconectar); `done` gets "" or the reason.
    void disconnect(const GvfsShare &share, std::function<void(std::string)> done);
    bool disconnecting() const { return disconnecting_; }

private:
    EventQueue::Poster poster_;
    Executor exec_;
    AliveGuard alive_;
    std::vector<TailnetPeer> peers_;
    std::vector<GvfsShare> shares_;
    Executor gio_;
    bool disconnecting_ = false;
};

} // namespace flo
