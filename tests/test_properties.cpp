// Files' properties and permissions (core/properties.h). Every object lives
// in the test's private directory; links point only at siblings there.
#include "harness.h"

#include "core/properties.h"

#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

namespace {
uint32_t mode_of(const std::string &p) {
    struct stat st;
    return ::lstat(p.c_str(), &st) == 0 ? uint32_t(st.st_mode) & 07777 : 0xffff;
}
} // namespace

TEST(properties_read_a_file_a_folder_and_links_without_following) {
    std::string dir = private_dir("props-read");
    std::string f = write_file(dir + "/note.txt", "hello world\n");
    CHECK(::chmod(f.c_str(), 0640) == 0);
    props::Info i;
    CHECK_EQ(props::read(f, i), std::string());
    CHECK(i.kind == props::Kind::File);
    CHECK_EQ(i.apparent, uint64_t(12));
    CHECK_EQ(i.mode, uint32_t(0640));
    CHECK_EQ(i.uid, uint32_t(::geteuid()));
    CHECK(!i.owner.empty() && !i.group.empty());
    CHECK_EQ(i.hardlinks, uint64_t(1));
    CHECK(i.modified.known && i.accessed.known && i.changed.known);
    CHECK_EQ(i.mime, std::string("text/plain"));
    CHECK(i.volume && i.total_bytes > 0);
    CHECK(props::editable(i));
    CHECK_EQ(props::symbolic_mode(i.mode, '-'), std::string("-rw-r-----"));
    CHECK_EQ(props::symbolic_mode(04755, '-'), std::string("-rwsr-xr-x"));
    CHECK_EQ(props::symbolic_mode(01777, 'd'), std::string("drwxrwxrwt"));
    CHECK_EQ(props::symbolic_mode(02640, '-'), std::string("-rw-r-S---"));

    props::Info d;
    CHECK_EQ(props::read(dir, d), std::string());
    CHECK(d.kind == props::Kind::Directory);
    CHECK(props::editable(d));

    std::string link = dir + "/link";
    CHECK(::symlink("note.txt", link.c_str()) == 0);
    props::Info l;
    CHECK_EQ(props::read(link, l), std::string());
    CHECK(l.kind == props::Kind::Symlink);
    CHECK_EQ(l.link_target, std::string("note.txt"));
    CHECK(l.link_resolves);
    CHECK(!props::editable(l));
    std::string broken = dir + "/broken";
    CHECK(::symlink("missing", broken.c_str()) == 0);
    props::Info b;
    CHECK_EQ(props::read(broken, b), std::string());
    CHECK(!b.link_resolves);
    CHECK(!props::read(dir + "/none", b).empty());
    CHECK(!props::read("relative", b).empty());
    CHECK(!props::format_time(i.modified).empty());
    CHECK(props::format_time(props::Timestamp()).empty());
}

TEST(properties_apply_mode_by_descriptor_and_refuse_stale_or_foreign_objects) {
    std::string dir = private_dir("props-apply");
    std::string f = write_file(dir + "/data", "keep these bytes");
    CHECK(::chmod(f.c_str(), 0600) == 0);
    props::Info i, after;
    CHECK_EQ(props::read(f, i), std::string());
    CHECK_EQ(props::apply_mode(i, 0640, after), std::string());
    CHECK_EQ(mode_of(f), uint32_t(0640));
    CHECK_EQ(after.mode, uint32_t(0640));
    CHECK_EQ(after.inode, i.inode);
    CHECK_EQ(read_file(f), std::string("keep these bytes"));

    // Changed elsewhere since it was read: refused, the outside change stays.
    CHECK(::chmod(f.c_str(), 0600) == 0);
    CHECK(!props::apply_mode(after, 0644, i).empty());
    CHECK_EQ(mode_of(f), uint32_t(0600));

    // Replaced by another object under the same name: refused.
    props::Info old;
    CHECK_EQ(props::read(f, old), std::string());
    std::string aside = dir + "/data-old";
    CHECK(::rename(f.c_str(), aside.c_str()) == 0);
    write_file(f, "another");
    CHECK(::chmod(f.c_str(), 0600) == 0);
    CHECK(!props::apply_mode(old, 0644, i).empty());
    CHECK_EQ(mode_of(f), uint32_t(0600));
    CHECK_EQ(mode_of(aside), uint32_t(0600));

    // A link is read-only and its target keeps its bits.
    std::string link = dir + "/link";
    CHECK(::symlink("data", link.c_str()) == 0);
    props::Info l;
    CHECK_EQ(props::read(link, l), std::string());
    CHECK(!props::apply_mode(l, 0666, i).empty());
    CHECK_EQ(mode_of(f), uint32_t(0600));

    // Special bits make it read-only.
    std::string s = write_file(dir + "/sticky", "");
    CHECK(::chmod(s.c_str(), 02640) == 0 || ::chmod(s.c_str(), 0640) == 0);
    props::Info si;
    CHECK_EQ(props::read(s, si), std::string());
    if (si.mode & 02000) {
        CHECK(!props::editable(si));
        CHECK(!props::apply_mode(si, 0600, i).empty());
    }

    // A folder.
    std::string sub = dir + "/sub";
    CHECK(::mkdir(sub.c_str(), 0700) == 0);
    props::Info di;
    CHECK_EQ(props::read(sub, di), std::string());
    CHECK_EQ(props::apply_mode(di, 0750, after), std::string());
    CHECK_EQ(mode_of(sub), uint32_t(0750));
}

TEST(properties_census_counts_without_following_links) {
    std::string dir = private_dir("props-census");
    std::string a = dir + "/a";
    CHECK(::mkdir(a.c_str(), 0700) == 0);
    write_file(a + "/one", "12345");
    write_file(a + "/two", "123");
    CHECK(::mkdir((a + "/inner").c_str(), 0700) == 0);
    write_file(a + "/inner/three", "1234567890");
    CHECK(::symlink("inner", (a + "/link").c_str()) == 0);
    std::atomic<bool> cancel{false};
    props::Census c = props::census({a}, cancel);
    CHECK(c.complete);
    CHECK_EQ(c.bytes, uint64_t(18));
    CHECK_EQ(c.files, uint64_t(3));
    CHECK_EQ(c.folders, uint64_t(1));
    CHECK_EQ(c.links, uint64_t(1));
    std::string b = write_file(dir + "/b", "xy");
    props::Census s = props::census({a, b}, cancel);
    CHECK_EQ(s.files, uint64_t(4));
    CHECK_EQ(s.folders, uint64_t(2));
    CHECK_EQ(s.bytes, uint64_t(20));
    cancel = true;
    CHECK(!props::census({a}, cancel).complete);
}
