#include "core/network.h"
#include "core/fsutil.h"
#include "core/json.h"
#include "core/natural_sort.h"
#include "core/process.h"
#include "core/xdg.h"

#include <algorithm>
#include <cctype>

#include <unistd.h>

namespace flo {

std::vector<TailnetPeer> parse_tailscale_status(const std::string &text, bool browsable_only) {
    std::vector<TailnetPeer> peers;
    json::Value root;
    if (!json::parse(text, root) || !root.is_object())
        return peers;
    const json::Value &all = root["Peer"];
    if (!all.is_object())
        return peers;
    for (const auto &kv : all.object) {
        const json::Value &p = kv.second;
        TailnetPeer peer;
        peer.name = p["HostName"].str();
        peer.host = p["DNSName"].str();
        if (!peer.host.empty() && peer.host.back() == '.')
            peer.host.pop_back();
        peer.os = p["OS"].str();
        peer.online = p["Online"].truthy();
        if (p["TailscaleIPs"].is_array() && !p["TailscaleIPs"].array.empty())
            peer.ip = p["TailscaleIPs"][0].str();
        // Phones can't be browsed over SFTP, and offline machines would
        // only be dead ends.
        if (browsable_only && (peer.name.empty() || peer.host.empty() || !peer.online ||
                               (peer.os != "linux" && peer.os != "macOS" && peer.os != "windows")))
            continue;
        peers.push_back(std::move(peer));
    }
    std::sort(peers.begin(), peers.end(), [](const TailnetPeer &a, const TailnetPeer &b) { return natural_less(a.name, b.name); });
    return peers;
}

std::string parse_dropbox_status(const std::string &output) {
    std::string t = output;
    while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back())))
        t.pop_back();
    std::string lower = t;
    for (char &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.empty() || lower.find("isn't running") != std::string::npos ||
        lower.find("not running") != std::string::npos)
        return "stopped";
    if (lower.compare(0, 10, "up to date") == 0)
        return "synced";
    return "syncing";
}

std::string sftp_uri(const std::string &host) { return "sftp://" + host + "/"; }

std::string gvfs_root() {
    std::string run = xdg::runtime_dir();
    if (run.empty())
        run = "/run/user/" + std::to_string(::getuid());
    return run + "/gvfs";
}

std::string gvfs_sftp_path(const std::string &host) { return gvfs_root() + "/sftp:host=" + host; }

Network::Network(EventQueue &queue) : poster_(queue.poster()) {}
Network::~Network() = default;

bool Network::is_tailnet_host(const std::string &host) const {
    for (const auto &p : peers_)
        if (p.host == host || p.name == host || p.ip == host)
            return true;
    return false;
}

void Network::refresh_tailnet(std::function<void(std::vector<TailnetPeer>)> done) {
    auto poster = poster_;
    auto weak = alive_.weak();
    exec_.run([poster, weak, done, this] {
        std::vector<TailnetPeer> peers;
        if (!xdg::find_program("tailscale").empty()) {
            RunOptions opt;
            opt.timeout_ms = 10000;
            opt.max_output = 8u << 20;
            const ProcessResult r = run_process({"tailscale", "status", "--json"}, opt);
            if (r.ok())
                peers = parse_tailscale_status(r.out);
        }
        poster.post([weak, done, peers, this] {
            if (weak.expired())
                return;
            peers_ = peers;
            if (done)
                done(peers);
        });
    });
}

void Network::refresh_dropbox(std::function<void(std::string)> done) {
    auto poster = poster_;
    auto weak = alive_.weak();
    exec_.run([poster, weak, done] {
        std::string status;
        if (is_dir(xdg::home() + "/Dropbox") && !xdg::find_program("dropbox-cli").empty()) {
            RunOptions opt;
            opt.timeout_ms = 10000;
            const ProcessResult r = run_process({"dropbox-cli", "status"}, opt);
            if (r.started)
                status = parse_dropbox_status(r.out);
        }
        poster.post([weak, done, status] {
            if (!weak.expired() && done)
                done(status);
        });
    });
}

} // namespace flo
