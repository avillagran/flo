#include "core/network.h"
#include "core/fsutil.h"
#include "core/json.h"
#include "core/natural_sort.h"
#include "core/process.h"
#include "core/xdg.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <ctime>
#include <map>
#include <thread>

#include <dirent.h>
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

// --- Network locations (Files' network.rs) ---------------------------------

static const char *const kSchemes[] = {"smb", "sftp", "ssh", "ftp", "ftps", "dav", "davs", "nfs", "afp"};

static std::string lower(std::string s) {
    for (char &c : s)
        c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static std::string scheme_of(const std::string &text) {
    const auto sep = text.find("://");
    if (sep == std::string::npos)
        return {};
    const std::string s = lower(text.substr(0, sep));
    for (const char *k : kSchemes)
        if (s == k)
            return k;
    return {};
}

bool is_network_uri(const std::string &text) { return !scheme_of(text).empty(); }

static std::string unescape(const std::string &v) {
    std::string out;
    auto hex = [](char c) {
        if (c >= '0' && c <= '9')
            return c - '0';
        c = char(std::tolower(static_cast<unsigned char>(c)));
        return c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (std::size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '%' && i + 2 < v.size() && hex(v[i + 1]) >= 0 && hex(v[i + 2]) >= 0) {
            out += char(hex(v[i + 1]) * 16 + hex(v[i + 2]));
            i += 2;
            continue;
        }
        out += v[i];
    }
    return out;
}

static std::string escape(const std::string &v, bool keep_slash) {
    static const char digits[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char b : v) {
        if (std::isalnum(b) || b == '-' || b == '.' || b == '_' || b == '~' || (keep_slash && b == '/')) {
            out += char(b);
        } else {
            out += '%';
            out += digits[b >> 4];
            out += digits[b & 15];
        }
    }
    return out;
}

std::string parse_network_uri(const std::string &raw, NetworkLocation &out) {
    std::string text = raw;
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.pop_back();
    std::size_t lead = 0;
    while (lead < text.size() && std::isspace(static_cast<unsigned char>(text[lead])))
        ++lead;
    text = text.substr(lead);
    const std::string scheme = scheme_of(text);
    if (scheme.empty())
        return "Use smb://, sftp://, ftp://, ftps://, dav://, davs://, nfs:// or afp://";
    if (text.size() > 2048)
        return "The address is too long";
    for (unsigned char c : text)
        if (c < 0x20 || c == 0x7f || c == ' ')
            return "The address can't have spaces or control characters: use %20";
    const std::string rest = text.substr(text.find("://") + 3);
    const auto slash = rest.find('/');
    const std::string authority = rest.substr(0, slash);
    const std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    const auto at = authority.rfind('@');
    const std::string host = at == std::string::npos ? authority : authority.substr(at + 1);
    if (host.empty() || host[0] == ':')
        return "Missing the server: smb://server/share";
    std::string user, password;
    bool has_password = false;
    if (at != std::string::npos) {
        const std::string info = authority.substr(0, at);
        const auto colon = info.find(':');
        user = info.substr(0, colon);
        if (colon != std::string::npos) {
            password = unescape(info.substr(colon + 1));
            has_password = true;
        }
    }
    out.uri = scheme + "://" + (user.empty() ? std::string() : user + "@") + host + path;
    // A domain;user keeps only the user.
    out.user = user.empty() ? std::string() : unescape(user.substr(user.rfind(';') == std::string::npos ? 0 : user.rfind(';') + 1));
    out.inline_password = has_password ? password : std::string();
    return {};
}

GvfsShare gvfs_share(const std::string &path, const std::string &name) {
    GvfsShare fallback;
    fallback.path = path;
    fallback.uri = path;
    fallback.title = name;
    const auto colon = name.find(':');
    if (colon == std::string::npos)
        return fallback;
    const std::string kind = name.substr(0, colon);
    std::map<std::string, std::string> f;
    std::string rest = name.substr(colon + 1);
    std::size_t start = 0;
    while (start <= rest.size()) {
        const auto comma = rest.find(',', start);
        const std::string pair = rest.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        const auto eq = pair.find('=');
        if (eq != std::string::npos)
            f[pair.substr(0, eq)] = unescape(pair.substr(eq + 1));
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    auto get = [&](const char *k) {
        auto it = f.find(k);
        return it == f.end() ? std::string() : it->second;
    };
    const std::string u = get("user"), p = get("port");
    const std::string user = u.empty() ? std::string() : escape(u, false) + "@";
    const std::string port = p.empty() ? std::string() : ":" + p;
    const std::string host = !get("host").empty() ? get("host") : get("server");
    if (host.empty())
        return fallback;
    GvfsShare s;
    s.path = path;
    s.user = u;
    if (kind == "smb-share") {
        const std::string share = get("share");
        if (share.empty())
            return fallback;
        const std::string d = get("domain");
        const std::string domain = !d.empty() && !user.empty() ? escape(d, false) + ";" : std::string();
        s.protocol = "SMB";
        s.uri = "smb://" + domain + user + escape(host, false) + port + "/" + escape(share, false);
        s.title = host;
        s.share = share;
    } else if (kind == "smb-server") {
        s.protocol = "SMB";
        s.uri = "smb://" + escape(host, false) + port + "/";
        s.title = host;
    } else if (kind == "sftp") {
        s.protocol = "SFTP";
        s.uri = "sftp://" + user + escape(host, false) + port + "/";
        s.title = host;
    } else if (kind == "ftp" || kind == "ftps") {
        s.protocol = kind == "ftp" ? "FTP" : "FTPS";
        s.uri = kind + "://" + user + escape(host, false) + port + "/";
        s.title = host;
    } else if (kind == "dav") {
        const std::string scheme = get("ssl") == "true" ? "davs" : "dav";
        std::string prefix = get("prefix");
        if (prefix.empty())
            prefix = "/";
        s.protocol = "WebDAV";
        s.uri = scheme + "://" + user + escape(host, false) + port + escape(prefix, true);
        std::string trimmed = prefix;
        while (!trimmed.empty() && trimmed.back() == '/')
            trimmed.pop_back();
        s.title = host + trimmed;
    } else if (kind == "nfs") {
        std::string prefix = get("prefix");
        if (prefix.empty())
            prefix = "/";
        s.protocol = "NFS";
        s.uri = "nfs://" + escape(host, false) + port + escape(prefix, true);
        s.title = host + ":" + prefix;
    } else if (kind == "afp-volume") {
        const std::string volume = get("volume");
        if (volume.empty())
            return fallback;
        s.protocol = "AFP";
        s.uri = "afp://" + user + escape(host, false) + port + "/" + escape(volume, false);
        s.title = host;
        s.share = volume;
    } else {
        return fallback;
    }
    return s;
}

std::vector<GvfsShare> scan_gvfs(const std::string &dir) {
    std::vector<GvfsShare> out;
    DIR *d = dir.empty() ? nullptr : ::opendir(dir.c_str());
    if (!d)
        return out;
    while (dirent *e = ::readdir(d)) {
        const std::string name = e->d_name;
        if (name.empty() || name[0] == '.')
            continue;
        out.push_back(gvfs_share(dir + "/" + name, name));
        if (out.size() >= 256)
            break;
    }
    ::closedir(d);
    auto label = [](const GvfsShare &s) { return s.share.empty() ? s.title : s.share + " " + s.title; };
    std::sort(out.begin(), out.end(), [&](const GvfsShare &a, const GvfsShare &b) {
        return label(a) != label(b) ? label(a) < label(b) : a.path < b.path;
    });
    return out;
}

std::string gvfs_dir() {
    const std::string fake = xdg::test_hook("FLO_TEST_FAKE_GVFS");
    if (!fake.empty())
        return fake;
    const std::string root = gvfs_root();
    return is_dir(root) ? root : std::string();
}

// gio's error as one line, without its program prefix or the echoed URI.
static std::string gio_reason(const ProcessResult &r) {
    std::string line;
    std::size_t start = 0;
    while (start < r.err.size()) {
        auto nl = r.err.find('\n', start);
        std::string l = r.err.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (!l.empty() && std::isspace(static_cast<unsigned char>(l.back())))
            l.pop_back();
        if (!l.empty())
            line = l;
        if (nl == std::string::npos)
            break;
        start = nl + 1;
    }
    if (line.compare(0, 5, "gio: ") == 0)
        line = line.substr(5);
    const auto sep = line.find(": ");
    if (sep != std::string::npos) {
        const std::string head = line.substr(0, sep);
        if (head.find("://") != std::string::npos || (!head.empty() && head[0] == '/'))
            line = line.substr(sep + 2);
    }
    if (line.size() > 300)
        line.resize(300);
    if (line.empty())
        line = r.timed_out ? "gio didn't answer in time" : "gio failed";
    return line;
}

std::string unmount_location(const GvfsShare &share) {
    const std::string fake = xdg::test_hook("FLO_TEST_FAKE_GVFS");
    if (!fake.empty()) {
        // Only a direct child of the fixture folder, moved aside.
        if (path_dirname(share.path) != clean_path(fake))
            return "Not a location of the fake GVfs";
        const std::string aside =
            fake + "/.unmounted-" + path_basename(share.path) + "-" + std::to_string(std::time(nullptr));
        return ::rename(share.path.c_str(), aside.c_str()) == 0 ? std::string() : "Couldn't disconnect";
    }
    if (xdg::find_program("gio").empty())
        return "gio isn't installed";
    for (int tries = 0;; ++tries) {
        RunOptions opt;
        opt.timeout_ms = 20000;
        opt.max_output = 16 * 1024;
        opt.set_env = {"LC_ALL=C"};
        const ProcessResult r = run_process({"gio", "mount", "-u", share.uri}, opt);
        if (r.ok())
            return {};
        const std::string reason = gio_reason(r);
        if (reason.find("busy") == std::string::npos)
            return reason;
        if (tries >= 5)
            return "The location is still in use: close the files open in it";
        std::this_thread::sleep_for(std::chrono::milliseconds(300 * (tries + 1)));
    }
}

void Network::refresh_shares(std::function<void(std::vector<GvfsShare>)> done) {
    auto poster = poster_;
    auto weak = alive_.weak();
    gio_.run([poster, weak, done, this] {
        std::vector<GvfsShare> list = scan_gvfs(gvfs_dir());
        poster.post([weak, done, list, this] {
            if (weak.expired())
                return;
            shares_ = list;
            if (done)
                done(list);
        });
    });
}

void Network::disconnect(const GvfsShare &share, std::function<void(std::string)> done) {
    auto poster = poster_;
    auto weak = alive_.weak();
    disconnecting_ = true;
    gio_.run([poster, weak, done, share, this] {
        std::string error = unmount_location(share);
        poster.post([weak, done, error, this] {
            if (weak.expired())
                return;
            disconnecting_ = false;
            if (done)
                done(error);
        });
    });
}

} // namespace flo
