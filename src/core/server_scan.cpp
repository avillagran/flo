#include "core/server_scan.h"

#include "core/fsutil.h"
#include "core/process.h"
#include "core/xdg.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace flo {

namespace {

std::vector<std::string> split(const std::string &s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}

std::vector<std::string> lines_of(const std::string &text) {
    std::vector<std::string> out;
    std::istringstream in(text);
    std::string l;
    while (std::getline(in, l)) {
        if (!l.empty() && l.back() == '\r')
            l.pop_back();
        out.push_back(l);
    }
    return out;
}

struct Service {
    const char *type, *scheme, *protocol;
};
const Service kServices[] = {
    {"_smb._tcp", "smb", "SMB"},          {"_sftp-ssh._tcp", "sftp", "SFTP"}, {"_nfs._tcp", "nfs", "NFS"},
    {"_webdav._tcp", "dav", "WebDAV"},    {"_webdavs._tcp", "davs", "WebDAV"}, {"_ftp._tcp", "ftp", "FTP"},
};

const Service *service_for(const std::string &type) {
    for (const auto &s : kServices)
        if (type == s.type)
            return &s;
    return nullptr;
}

std::string protocol_of(const std::string &uri) {
    std::string scheme = uri.substr(0, uri.find(':'));
    for (const auto &s : kServices)
        if (scheme == s.scheme)
            return s.protocol;
    if (scheme == "afp")
        return "AFP";
    if (scheme == "ftps")
        return "FTP";
    return {};
}

// A host name usable in a URI ("[fe80::1]" for IPv6 literals).
std::string uri_host(const std::string &host) {
    return host.find(':') != std::string::npos && host.front() != '[' ? "[" + host + "]" : host;
}

bool safe_text(const std::string &s) {
    return !s.empty() && s.size() < 512 &&
           std::none_of(s.begin(), s.end(), [](char c) { return static_cast<unsigned char>(c) < 0x20; });
}

} // namespace

std::string avahi_unescape(const std::string &s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\\' && i + 3 < s.size() && std::isdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isdigit(static_cast<unsigned char>(s[i + 2])) && std::isdigit(static_cast<unsigned char>(s[i + 3]))) {
            int v = (s[i + 1] - '0') * 100 + (s[i + 2] - '0') * 10 + (s[i + 3] - '0');
            out += char(v);
            i += 3;
        } else if (s[i] == '\\' && i + 1 < s.size()) {
            out += s[++i];
        } else {
            out += s[i];
        }
    }
    return out;
}

std::vector<FoundServer> parse_avahi(const std::string &text) {
    std::vector<FoundServer> out;
    for (const auto &line : lines_of(text)) {
        // =;iface;proto;name;type;domain;hostname;address;port;txt
        if (line.size() < 2 || line[0] != '=')
            continue;
        auto f = split(line, ';');
        if (f.size() < 9)
            continue;
        const Service *svc = service_for(f[4]);
        if (!svc)
            continue;
        std::string name = avahi_unescape(f[3]);
        std::string host = f[6];
        if (!host.empty() && host.back() == '.')
            host.pop_back();
        if (host.empty())
            host = f[7];
        int port = std::atoi(f[8].c_str());
        if (!safe_text(name) || !safe_text(host) || host.find_first_of("/ @?#") != std::string::npos)
            continue;
        FoundServer s;
        s.name = name;
        s.protocol = svc->protocol;
        std::string port_part;
        const bool default_port = (svc->scheme == std::string("smb") && port == 445) ||
                                  (svc->scheme == std::string("sftp") && port == 22) ||
                                  (svc->scheme == std::string("ftp") && port == 21) ||
                                  (svc->scheme == std::string("dav") && port == 80) ||
                                  (svc->scheme == std::string("davs") && port == 443) ||
                                  (svc->scheme == std::string("nfs") && port == 2049) || port <= 0;
        if (!default_port)
            port_part = ":" + std::to_string(port);
        s.uri = std::string(svc->scheme) + "://" + uri_host(host) + port_part + "/";
        s.detail = host + (port_part.empty() ? std::string() : port_part);
        if (std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(s);
    }
    return out;
}

std::vector<FoundServer> parse_gio_network(const std::string &text) {
    std::vector<FoundServer> out;
    for (const auto &line : lines_of(text)) {
        // name \t size \t (type) \t standard::display-name=… standard::target-uri=…
        auto f = split(line, '\t');
        std::string uri, name;
        for (const auto &field : f) {
            if (field.compare(0, 21, "standard::target-uri=") == 0)
                uri = field.substr(21);
            else if (field.compare(0, 23, "standard::display-name=") == 0)
                name = field.substr(23);
        }
        if (uri.empty() || uri.compare(0, 8, "network:") == 0 || protocol_of(uri).empty() || !safe_text(uri))
            continue;
        if (name.empty())
            name = f.empty() ? uri : f[0];
        FoundServer s{name, uri, protocol_of(uri), ""};
        size_t a = uri.find("://");
        if (a != std::string::npos) {
            std::string rest = uri.substr(a + 3);
            s.detail = rest.substr(0, rest.find('/'));
        }
        if (std::find(out.begin(), out.end(), s) == out.end())
            out.push_back(s);
    }
    return out;
}

std::vector<FoundServer> parse_smbclient(const std::string &text, const std::string &host) {
    std::vector<FoundServer> out;
    for (const auto &line : lines_of(text)) {
        // Disk|name|comment (-g: one machine-readable line per share)
        auto f = split(line, '|');
        if (f.size() < 2 || f[0] != "Disk")
            continue;
        const std::string &share = f[1];
        if (!safe_text(share) || share.back() == '$' || share.find('/') != std::string::npos)
            continue; // administrative shares (C$, IPC$) stay out
        FoundServer s;
        s.name = share;
        s.protocol = "SMB";
        std::string enc;
        for (unsigned char c : share) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                enc += char(c);
            } else {
                char b[4];
                std::snprintf(b, sizeof b, "%%%02X", c);
                enc += b;
            }
        }
        s.uri = "smb://" + uri_host(host) + "/" + enc + "/";
        s.detail = f.size() > 2 && !f[2].empty() ? f[2] : host;
        out.push_back(s);
    }
    return out;
}

void merge_servers(std::vector<FoundServer> &into, const std::vector<FoundServer> &more) {
    for (const auto &s : more)
        if (std::find(into.begin(), into.end(), s) == into.end())
            into.push_back(s);
    std::stable_sort(into.begin(), into.end(), [](const FoundServer &a, const FoundServer &b) {
        std::string x = a.name, y = b.name;
        std::transform(x.begin(), x.end(), x.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        std::transform(y.begin(), y.end(), y.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        return x != y ? x < y : a.uri < b.uri;
    });
}

void remember_server(std::vector<std::string> &recent, const std::string &uri, std::size_t max) {
    if (uri.empty() || uri.size() > 2048 || uri.find_first_of("\n\r") != std::string::npos)
        return;
    recent.erase(std::remove(recent.begin(), recent.end(), uri), recent.end());
    recent.insert(recent.begin(), uri);
    if (recent.size() > max)
        recent.resize(max);
}

// --- The scan ---------------------------------------------------------------------

ServerScan::ServerScan(EventQueue &queue) : poster_(queue.poster()) {}

ServerScan::~ServerScan() { cancel(); }

void ServerScan::cancel() {
    cancel_.cancel();
    exec_.clear();
}

bool ServerScan::start(std::function<void(std::vector<FoundServer>)> done) {
    if (running_)
        return false;
    running_ = true;
    cancel_ = CancelToken();
    auto weak = alive_.weak();
    auto poster = poster_;
    CancelToken token = cancel_;
    exec_.run([this, weak, poster, token, done] {
        std::vector<FoundServer> found;
        RunOptions o;
        o.timeout_ms = 6000;
        o.max_output = 256 * 1024;
        o.cancel = &token;
        // Test runs read fixtures instead of the network.
        const std::string fake_avahi = xdg::test_hook("FLO_TEST_AVAHI");
        const std::string fake_gio = xdg::test_hook("FLO_TEST_GIO_NETWORK");
        if (!fake_gio.empty()) {
            std::string t;
            if (read_file_capped(fake_gio, 1 << 20, t))
                merge_servers(found, parse_gio_network(t));
        } else if (!xdg::find_program("gio").empty()) {
            ProcessResult r =
                run_process({"gio", "list", "-a", "standard::target-uri,standard::display-name", "network:///"}, o);
            if (r.ok())
                merge_servers(found, parse_gio_network(r.out));
        }
        if (!fake_avahi.empty()) {
            std::string t;
            if (read_file_capped(fake_avahi, 1 << 20, t))
                merge_servers(found, parse_avahi(t));
        } else if (!token.cancelled() && !xdg::find_program("avahi-browse").empty()) {
            std::vector<std::string> argv = {"avahi-browse", "-t", "-p", "-r", "-k"};
            // One run per type: avahi-browse takes a single service type.
            for (const auto &svc : kServices) {
                if (token.cancelled())
                    break;
                auto a = argv;
                a.push_back(svc.type);
                RunOptions q = o;
                q.timeout_ms = 3000;
                ProcessResult r = run_process(a, q);
                if (r.started)
                    merge_servers(found, parse_avahi(r.out)); // a timeout still leaves what it printed
            }
        }
        if (fake_avahi.empty() && !token.cancelled() && !xdg::find_program("smbclient").empty()) {
            std::vector<FoundServer> shares;
            int asked = 0;
            for (const auto &s : found) {
                if (s.protocol != "SMB" || token.cancelled() || asked >= 6)
                    continue;
                std::string host = s.detail.substr(0, s.detail.find(':'));
                if (host.empty() || s.uri.find('/', 6) + 1 != s.uri.size())
                    continue; // a share already, or no host
                ++asked;
                RunOptions q = o;
                q.timeout_ms = 4000;
                ProcessResult r = run_process({"smbclient", "-N", "-g", "-L", "//" + host}, q);
                if (r.started)
                    merge_servers(shares, parse_smbclient(r.out, host));
            }
            merge_servers(found, shares);
        }
        poster.post([this, weak, done, found] {
            if (weak.expired())
                return;
            running_ = false;
            if (done)
                done(found);
        });
    });
    return true;
}

} // namespace flo
