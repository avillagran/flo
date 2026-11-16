// The Connect dialog's server discovery (core/server_scan.h): the output of
// avahi-browse, gio and smbclient as these tools print it.
#include "harness.h"

#include "core/network.h"
#include "core/server_scan.h"

using namespace flo;

TEST(server_scan_parses_avahi_resolved_lines) {
    const std::string text =
        "+;enp3s0;IPv4;VQ\\032TimeCapsule;_smb._tcp;local\n"
        "=;enp3s0;IPv4;VQ\\032TimeCapsule;_smb._tcp;local;VQ-TimeCapsule.local;169.254.194.141;445;\"netbios=x\"\n"
        "=;enp3s0;IPv6;VQ\\032TimeCapsule;_smb._tcp;local;VQ-TimeCapsule.local;fe80::1;445;\n"
        "=;enp3s0;IPv4;studio;_sftp-ssh._tcp;local;studio.local;192.168.1.5;2222;\n"
        "=;enp3s0;IPv4;files;_webdavs._tcp;local;files.local;192.168.1.6;443;\n"
        "=;enp3s0;IPv4;printer;_ipp._tcp;local;printer.local;192.168.1.7;631;\n"
        "=;enp3s0;IPv4;evil;_smb._tcp;local;bad/host;1.2.3.4;445;\n";
    auto v = parse_avahi(text);
    CHECK_EQ(v.size(), size_t(3));
    CHECK_EQ(v[0].name, std::string("VQ TimeCapsule"));
    CHECK_EQ(v[0].uri, std::string("smb://VQ-TimeCapsule.local/"));
    CHECK_EQ(v[0].protocol, std::string("SMB"));
    CHECK_EQ(v[1].uri, std::string("sftp://studio.local:2222/"));
    CHECK_EQ(v[2].uri, std::string("davs://files.local/"));
    CHECK_EQ(avahi_unescape("a\\032b\\.c"), std::string("a b.c"));
    // Every found address passes Files' network::parse.
    for (const auto &s : v) {
        NetworkLocation loc;
        CHECK_EQ(parse_network_uri(s.uri, loc), std::string());
    }
}

TEST(server_scan_parses_gio_network_and_smbclient) {
    const std::string gio =
        "smb-root\t0\t(shortcut)\tstandard::display-name=Windows Network\tstandard::target-uri=network:///smb-root\n"
        "nas\t0\t(shortcut)\tstandard::display-name=nas\tstandard::target-uri=smb://nas/\n"
        "dnssd\t0\t(shortcut)\tstandard::display-name=Photos on nas\tstandard::target-uri=dav://nas.local:8080/photos\n";
    auto v = parse_gio_network(gio);
    CHECK_EQ(v.size(), size_t(2));
    CHECK_EQ(v[0].uri, std::string("smb://nas/"));
    CHECK_EQ(v[1].name, std::string("Photos on nas"));
    CHECK_EQ(v[1].protocol, std::string("WebDAV"));
    CHECK_EQ(v[1].detail, std::string("nas.local:8080"));
    const std::string smb = "Disk|fotos|Family photos\nDisk|C$|Default share\nIPC|IPC$|Remote IPC\nDisk|my files|\n";
    auto s = parse_smbclient(smb, "nas.local");
    CHECK_EQ(s.size(), size_t(2));
    CHECK_EQ(s[0].uri, std::string("smb://nas.local/fotos/"));
    CHECK_EQ(s[0].detail, std::string("Family photos"));
    CHECK_EQ(s[1].uri, std::string("smb://nas.local/my%20files/"));
    std::vector<FoundServer> all = v;
    merge_servers(all, s);
    merge_servers(all, v);
    CHECK_EQ(all.size(), size_t(4));
    CHECK_EQ(all[0].name, std::string("fotos"));
}

TEST(server_scan_remembers_recent_servers) {
    std::vector<std::string> r;
    remember_server(r, "smb://a/");
    remember_server(r, "sftp://b/");
    remember_server(r, "smb://a/");
    CHECK(r == (std::vector<std::string>{"smb://a/", "sftp://b/"}));
    for (int i = 0; i < 20; ++i)
        remember_server(r, "smb://h" + std::to_string(i) + "/", 8);
    CHECK_EQ(r.size(), size_t(8));
    remember_server(r, "smb://x/\nevil");
    CHECK_EQ(r[0], std::string("smb://h19/"));
}
