// Files' Tab completion in the location field (path_edit.rs).
#include "harness.h"

#include "core/path_completion.h"

#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

namespace {
std::vector<std::string> names(const std::vector<PathCandidate> &v) {
    std::vector<std::string> out;
    for (const auto &c : v)
        out.push_back(c.name + (c.dir ? "/" : ""));
    return out;
}
std::string joined(const std::vector<std::string> &v) {
    std::string s;
    for (const auto &n : v)
        s += (s.empty() ? "" : " ") + n;
    return s;
}
} // namespace

TEST(path_completion_matches_like_files) {
    std::vector<PathCandidate> all = {{"Pictures", true}, {"Projects", true}, {"plan.txt", false},
                                      {".pki", true},     {"Public", true},   {"pictures-old", true}};
    // Case-sensitive first; folders win over files.
    CHECK_EQ(joined(names(completion_matches(all, "P"))), "Pictures/ Projects/ Public/");
    // Nothing case-sensitive: case-insensitive, sorted ignoring case.
    CHECK_EQ(joined(names(completion_matches(all, "PI"))), "Pictures/ pictures-old/");
    CHECK_EQ(joined(names(completion_matches(all, "pi"))), "pictures-old/");
    // Files only when no folder matches.
    CHECK_EQ(joined(names(completion_matches(all, "pl"))), "plan.txt");
    // Hidden names only when the typed part starts with a dot.
    CHECK_EQ(joined(names(completion_matches(all, ""))), "Pictures/ pictures-old/ Projects/ Public/");
    CHECK_EQ(joined(names(completion_matches(all, ".p"))), ".pki/");
    CHECK(completion_matches(all, "zz").empty());
    // At most 256 candidates.
    std::vector<PathCandidate> many;
    for (int i = 0; i < 400; ++i)
        many.push_back({"d" + std::to_string(1000 + i), true});
    CHECK_EQ(completion_matches(many, "d").size(), size_t(256));
}

TEST(path_completion_prefix_split_and_render) {
    std::string base, partial;
    split_component("~/Pic", base, partial);
    CHECK_EQ(base, std::string("~/"));
    CHECK_EQ(partial, std::string("Pic"));
    split_component("Pic", base, partial);
    CHECK(base.empty());
    CHECK_EQ(partial, std::string("Pic"));
    split_component("/", base, partial);
    CHECK_EQ(base, std::string("/"));
    CHECK(partial.empty());

    CHECK_EQ(common_prefix({{"Projects", true}, {"Prompts", true}}, false), std::string("Pro"));
    CHECK_EQ(common_prefix({{"photos", true}, {"Photos-2", true}}, true), std::string("photos"));
    CHECK_EQ(common_prefix({{"photos", true}, {"Photos-2", true}}, false), std::string(""));
    // Never half a UTF-8 character: "é" and "è" share their lead byte.
    CHECK_EQ(common_prefix({{"caf\xC3\xA9", true}, {"caf\xC3\xA8", true}}, false), std::string("caf"));

    PathCompletion c{"~/", {{"a", true}, {"b", false}, {"c", true}}, 1};
    CHECK_EQ(c.text(0), std::string("~/a/"));
    CHECK_EQ(c.text(1), std::string("~/b"));
    CHECK_EQ(c.render(), std::string("Tab \xC2\xB7 a/ [b] c/"));
}

TEST(path_completion_reads_one_folder_without_following_links) {
    const std::string d = private_dir("path-completion");
    ::mkdir((d + "/Pictures").c_str(), 0700);
    ::mkdir((d + "/Projects").c_str(), 0700);
    ::mkdir((d + "/.hidden").c_str(), 0700);
    write_file(d + "/plan.txt", "x");
    // A link to a folder inside the fixture browses as one; a dangling link
    // is a plain name. Neither is opened.
    CHECK_EQ(::symlink("Pictures", (d + "/pics-link").c_str()), 0);
    CHECK_EQ(::symlink("missing-target", (d + "/pdangling").c_str()), 0);
    auto found = read_candidates(d, "p");
    CHECK_EQ(joined(names(completion_matches(found, "p"))), "pics-link/");
    found = read_candidates(d, "P");
    CHECK_EQ(joined(names(completion_matches(found, "P"))), "Pictures/ Projects/");
    found = read_candidates(d, "pd");
    CHECK_EQ(joined(names(completion_matches(found, "pd"))), "pdangling");
    CHECK(read_candidates(d + "/no-such-folder", "").empty());
    CHECK(!network_location(d));
}
