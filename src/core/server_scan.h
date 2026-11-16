// Network servers nearby, for the Connect dialog, found without root:
//   - GVfs's network:/// (gio list), which knows what gvfsd-network sees
//     (Windows shares, DNS-SD services);
//   - avahi-browse for _smb._tcp, _sftp-ssh._tcp, _nfs._tcp, _webdav._tcp,
//     _webdavs._tcp and _ftp._tcp, when Avahi's tools are installed;
//   - smbclient -L for the shares of each SMB server, only when smbclient
//     is installed.
// Every tool is optional; a missing or failing one simply adds nothing.
// The scan runs on a worker thread with short timeouts and reports once on
// the UI thread. The parsers are pure, for tests.
#pragma once

#include "core/event_queue.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace flo {

struct FoundServer {
    std::string name;     // "nas", "VQ TimeCapsule"
    std::string uri;      // "smb://nas.local/", "smb://nas.local/fotos/"
    std::string protocol; // SMB, SFTP, NFS, WebDAV, FTP
    std::string detail;   // host and port, or the share's comment
    bool operator==(const FoundServer &o) const { return uri == o.uri; }
};

// `avahi-browse -tprk TYPE…` output (the resolved "=" lines).
std::vector<FoundServer> parse_avahi(const std::string &text);
// `gio list -a standard::target-uri,standard::display-name network:///`.
std::vector<FoundServer> parse_gio_network(const std::string &text);
// `smbclient -N -g -L //HOST` output: the disk shares of `host`.
std::vector<FoundServer> parse_smbclient(const std::string &text, const std::string &host);
// Merges `more` into `into` without repeating a URI; sorted by name.
void merge_servers(std::vector<FoundServer> &into, const std::vector<FoundServer> &more);
// Avahi's "\032" decimal escapes, decoded.
std::string avahi_unescape(const std::string &s);

class ServerScan {
public:
    explicit ServerScan(EventQueue &queue);
    ~ServerScan();
    // Runs a scan; `done` gets everything found (UI thread). A scan while
    // one runs is ignored (false).
    bool start(std::function<void(std::vector<FoundServer>)> done);
    bool running() const { return running_; }
    void cancel();

private:
    EventQueue::Poster poster_;
    Executor exec_;
    AliveGuard alive_;
    CancelToken cancel_;
    bool running_ = false;
};

// Files keeps no list of servers; Flo remembers the last few it connected
// to from the Connect dialog or the location field ([network] recent).
void remember_server(std::vector<std::string> &recent, const std::string &uri, std::size_t max = 8);

} // namespace flo
