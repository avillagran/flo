// Listing, item counts, live reload, search.
#include "harness.h"

#include "core/event_queue.h"
#include "core/fsutil.h"
#include "core/lister.h"
#include "core/search.h"
#include "core/watcher.h"

#include <chrono>
#include <cstdio>
#include <set>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace flo;
using namespace flo_test;

static bool pump_until(EventQueue &q, const std::function<bool()> &done, int ms = 5000) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (!done()) {
        if (std::chrono::steady_clock::now() > end)
            return false;
        q.wait(50);
        q.dispatch();
    }
    return true;
}

TEST(list_folder_entries_types_and_links) {
    const std::string d = private_dir("list");
    write_file(d + "/b.txt", "hello");
    write_file(d + "/A.jpg", "");
    write_file(d + "/.hidden", "x");
    write_file(d + "/noext", "%PDF-1.4\n%binary");
    ::mkdir((d + "/sub").c_str(), 0700);
    CHECK(::symlink("sub", (d + "/linkdir").c_str()) == 0);
    CHECK(::symlink("missing", (d + "/broken").c_str()) == 0);
    ListOptions opt;
    Listing l = list_folder(d, opt);
    CHECK(l.error.empty());
    std::string names;
    for (auto &e : l.entries)
        names += e.name + ",";
    CHECK_EQ(names, std::string("linkdir,sub,A.jpg,b.txt,broken,noext,"));
    const Entry &linkdir = l.entries[0];
    CHECK(linkdir.is_dir && linkdir.is_link && linkdir.link_target == "sub");
    const Entry &b = l.entries[3];
    CHECK_EQ(std::string(b.mime), std::string("text/plain"));
    CHECK_EQ(b.size, static_cast<int64_t>(5));
    CHECK(l.entries[4].broken_link);
    CHECK_EQ(std::string(l.entries[5].mime), std::string("application/pdf")); // sniffed
    CHECK(l.entries[5].kind == Kind::Pdf);
    CHECK_EQ(l.path_of(3), d + "/b.txt");
    opt.show_hidden = true;
    CHECK_EQ(list_folder(d, opt).entries.size(), static_cast<std::size_t>(7));
    Listing missing = list_folder(d + "/nope", opt);
    CHECK_EQ(missing.error, std::string("This folder no longer exists."));
    CHECK_EQ(count_items(d, false), 6);
    CHECK_EQ(count_items(d, true), 7);
}

TEST(lister_async_and_counts) {
    const std::string d = private_dir("lister");
    ::mkdir((d + "/f1").c_str(), 0700);
    ::mkdir((d + "/f2").c_str(), 0700);
    write_file(d + "/f1/x", "");
    write_file(d + "/f1/y", "");
    EventQueue q;
    Lister lister(q);
    ListingPtr result;
    lister.list(d, {}, [&](ListingPtr l) {
        if (l->complete)
            result = l;
    });
    CHECK(pump_until(q, [&] { return result != nullptr; }));
    CHECK(result && result->entries.size() == 2);
    std::vector<std::pair<std::string, int>> counts;
    lister.count_items({d + "/f1", d + "/f2", d + "/gone"}, false,
                       [&](std::vector<std::pair<std::string, int>> b) { counts.insert(counts.end(), b.begin(), b.end()); });
    CHECK(pump_until(q, [&] { return counts.size() == 3; }));
    CHECK(counts.size() == 3 && counts[0].second == 2 && counts[1].second == 0 && counts[2].second == -1);
    // A newer request overtakes an older one: only the newer is delivered.
    int delivered = 0;
    std::string last;
    lister.list(d + "/f1", {}, [&](ListingPtr l) { ++delivered; last = l->folder; });
    lister.list(d + "/f2", {}, [&](ListingPtr l) { ++delivered; last = l->folder; });
    CHECK(pump_until(q, [&] { return last == d + "/f2"; }));
    q.wait(100);
    q.dispatch();
    CHECK_EQ(delivered, 1);
}

TEST(listing_50k_entries_under_a_second) {
    const std::string d = private_dir("big");
    for (int i = 0; i < 50000; ++i) {
        char name[32];
        std::snprintf(name, sizeof name, "/file %d.txt", i);
        const int fd = ::open((d + name).c_str(), O_WRONLY | O_CREAT | O_EXCL, 0644);
        ::close(fd);
    }
    const auto t0 = std::chrono::steady_clock::now();
    int progress = 0;
    Listing l = list_folder(d, {}, nullptr, [&](const Listing &) { ++progress; });
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    std::printf("     50k listing: %lld ms, %d progressive snapshots\n", static_cast<long long>(ms), progress);
    CHECK_EQ(l.entries.size(), static_cast<std::size_t>(50000));
    CHECK_EQ(l.entries[2].name, std::string("file 2.txt"));
    CHECK(ms < 1000);
}

TEST(watcher_debounces_changes) {
    const std::string d = private_dir("watch");
    EventQueue q;
    Watcher w(q, 100);
    int changes = 0;
    const int id = w.watch_folder(d, [&] { ++changes; });
    CHECK(id > 0);
    for (int i = 0; i < 20; ++i)
        write_file(d + "/f" + std::to_string(i), "x");
    CHECK(pump_until(q, [&] { return changes > 0; }, 3000));
    q.wait(400);
    q.dispatch();
    CHECK_EQ(changes, 1);
    // A watched file is seen when replaced (rename over it).
    int file_changes = 0;
    write_file(d + "/watched.xbel", "a");
    w.watch_file(d + "/watched.xbel", [&] { ++file_changes; });
    CHECK(atomic_write_file(d + "/watched.xbel", "b", 0600));
    CHECK(pump_until(q, [&] { return file_changes > 0; }, 3000));
    w.unwatch(id);
}

TEST(search_walks_breadth_first_without_following_links) {
    const std::string d = private_dir("search");
    ::mkdir((d + "/a").c_str(), 0700);
    ::mkdir((d + "/a/deep").c_str(), 0700);
    ::mkdir((d + "/outside").c_str(), 0700);
    ::mkdir((d + "/root").c_str(), 0700);
    write_file(d + "/root/Report.txt", "");
    ::mkdir((d + "/root/a").c_str(), 0700);
    write_file(d + "/root/a/report-2.txt", "");
    ::mkdir((d + "/root/a/b").c_str(), 0700);
    write_file(d + "/root/a/b/old report", "");
    write_file(d + "/root/.report-hidden", "");
    write_file(d + "/outside/report-outside", "");
    // A symlinked folder inside the root points elsewhere in the fixture.
    CHECK(::symlink((d + "/outside").c_str(), (d + "/root/link-report").c_str()) == 0);
    std::vector<std::string> found;
    SearchOptions opt;
    const SearchStats st = search_walk(d + "/root", "REPORT", opt, nullptr, [&](std::vector<Entry> &&b) {
        for (auto &e : b)
            found.push_back(e.name);
    });
    const std::vector<std::string> want{"Report.txt", "link-report", "a/report-2.txt", "a/b/old report"};
    std::set<std::string> first_level(found.begin(), found.begin() + std::min<std::size_t>(2, found.size()));
    CHECK_EQ(found.size(), static_cast<std::size_t>(4));
    CHECK(first_level.count("Report.txt") && first_level.count("link-report"));
    CHECK(found.size() == 4 && found[2] == "a/report-2.txt" && found[3] == "a/b/old report");
    CHECK(!st.truncated && !st.cancelled);
    opt.limit = 1;
    found.clear();
    CHECK(search_walk(d + "/root", "report", opt, nullptr, [&](std::vector<Entry> &&b) {
              for (auto &e : b) found.push_back(e.name);
          }).truncated);
    CHECK_EQ(found.size(), static_cast<std::size_t>(1));
    opt.limit = 100;
    opt.show_hidden = true;
    found.clear();
    search_walk(d + "/root", "report", opt, nullptr, [&](std::vector<Entry> &&b) {
        for (auto &e : b) found.push_back(e.name);
    });
    CHECK_EQ(found.size(), static_cast<std::size_t>(5));
    // Async, cancellable.
    EventQueue q;
    Searcher s(q);
    bool done = false;
    std::size_t n = 0;
    s.start(d + "/root", "report", {}, [&](std::vector<Entry> &&b) { n += b.size(); },
            [&](const SearchStats &) { done = true; });
    CHECK(pump_until(q, [&] { return done; }));
    CHECK_EQ(n, static_cast<std::size_t>(4));
}
