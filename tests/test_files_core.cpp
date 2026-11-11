// Files' type-to-filter syntax and the folder size walker.
#include "harness.h"

#include "core/name_filter.h"
#include "core/tree_size.h"

#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

TEST(name_filter_follows_files_syntax) {
    CHECK(NameFilter("").empty());
    CHECK(NameFilter("pho").matches("Photo.JPG", false));
    CHECK(!NameFilter("pho").matches("notes.txt", false));
    CHECK(NameFilter("*.rs").matches("main.RS", false));
    CHECK(!NameFilter("*.rs").matches("main.rs.bak", false));
    CHECK(NameFilter("img_00?1*").matches("IMG_0001.jpg", false));
    NameFilter ext("ext:pdf,md");
    CHECK(ext.matches("a.pdf", false) && ext.matches("README.MD", false) && !ext.matches("a.txt", false));
    CHECK(!ext.matches("folder.md", true));
    CHECK(NameFilter("type:dir").matches("src", true) && !NameFilter("type:dir").matches("a.c", false));
    CHECK(NameFilter("!draft").matches("final.txt", false) && !NameFilter("!draft").matches("Draft-2.txt", false));
    CHECK(NameFilter("img ext:jpg").matches("IMG_1.jpg", false) && !NameFilter("img ext:jpg").matches("IMG_1.png", false));
    CHECK(!NameFilter("ext:").error().empty());
    CHECK(!NameFilter("type:nope").error().empty());
    Entry e;
    e.name = "sub/dir/Report.pdf";
    CHECK(NameFilter("rep").matches(e) && !NameFilter("dir").matches(e));
}

TEST(tree_size_sums_without_following_links) {
    const std::string d = private_dir("tree-size");
    ::mkdir((d + "/a").c_str(), 0700);
    ::mkdir((d + "/a/b").c_str(), 0700);
    write_file(d + "/one", std::string(100, 'x'));
    write_file(d + "/a/two", std::string(250, 'y'));
    write_file(d + "/a/b/three", std::string(1000, 'z'));
    // A link to a big file elsewhere inside the fixture: its own size counts,
    // never its target's (and a link to a folder is not walked).
    const std::string outside = private_dir("tree-size-target");
    write_file(outside + "/big", std::string(5000, 'q'));
    CHECK(::symlink((outside + "/big").c_str(), (d + "/link").c_str()) == 0);
    CHECK(::symlink(outside.c_str(), (d + "/dirlink").c_str()) == 0);
    std::atomic<bool> cancel{false};
    TreeSize t = tree_size(d, cancel);
    CHECK(t.complete);
    CHECK_EQ(t.files, uint64_t(5)); // one, two, three, link, dirlink
    CHECK_EQ(t.folders, uint64_t(2));
    struct stat sa, sb, sl1, sl2;
    CHECK(::lstat((d + "/a").c_str(), &sa) == 0 && ::lstat((d + "/a/b").c_str(), &sb) == 0);
    CHECK(::lstat((d + "/link").c_str(), &sl1) == 0 && ::lstat((d + "/dirlink").c_str(), &sl2) == 0);
    uint64_t want = 100 + 250 + 1000 + uint64_t(sa.st_size + sb.st_size + sl1.st_size + sl2.st_size);
    CHECK_EQ(t.bytes, want);
    // A link as the root is not walked.
    TreeSize l = tree_size(d + "/dirlink", cancel);
    CHECK(!l.complete && l.bytes == 0);
    std::atomic<bool> stop{true};
    CHECK(!tree_size(d, stop).complete);
}

TEST(si_size_matches_files_labels) {
    CHECK_EQ(si_size(0), std::string("0 B"));
    CHECK_EQ(si_size(630), std::string("630 B"));
    CHECK_EQ(si_size(4200), std::string("4.2 kB"));
    CHECK_EQ(si_size(1100000), std::string("1.1 MB"));
    CHECK_EQ(si_size(640000000000ull), std::string("640 GB"));
}
