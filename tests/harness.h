// A tiny test harness: TEST(name) { CHECK(...); CHECK_EQ(a, b); }.
// Fixtures come from private_dir(), a fresh exclusive 0700 directory under
// the run's private TMPDIR that is never deleted.
#pragma once

#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace flo_test {

struct Case {
    const char *name;
    void (*fn)();
};
std::vector<Case> &registry();
struct Register {
    Register(const char *name, void (*fn)()) { registry().push_back({name, fn}); }
};

void fail(const char *file, int line, const std::string &what);
extern int g_checks;

// A fresh directory for one test: $TMPDIR/<label>-XXXXXX, mode 0700.
std::string private_dir(const std::string &label);
// Writes a file (creating it exclusively); returns its path.
std::string write_file(const std::string &path, const std::string &data);
std::string read_file(const std::string &path);
// Aborts unless `path` lies inside the run's private directory; call it
// before any product call that removes or moves things.
void assert_inside(const std::string &path);

template <typename T> std::string show(const T &v) {
    std::ostringstream o;
    o << v;
    return o.str();
}

} // namespace flo_test

#define TEST(name)                                                                    \
    static void test_##name();                                                        \
    static flo_test::Register reg_##name(#name, test_##name);                         \
    static void test_##name()

#define CHECK(cond)                                                                   \
    do {                                                                              \
        ++flo_test::g_checks;                                                         \
        if (!(cond))                                                                  \
            flo_test::fail(__FILE__, __LINE__, "CHECK(" #cond ")");                   \
    } while (0)

#define CHECK_EQ(a, b)                                                                \
    do {                                                                              \
        ++flo_test::g_checks;                                                         \
        const auto &va_ = (a);                                                        \
        const auto &vb_ = (b);                                                        \
        if (!(va_ == vb_))                                                            \
            flo_test::fail(__FILE__, __LINE__,                                        \
                           "CHECK_EQ(" #a ", " #b "): " + flo_test::show(va_) + " vs " + \
                               flo_test::show(vb_));                                  \
    } while (0)

#include <dirent.h>
// Names in a folder (helper for tests).
inline std::vector<std::string> list_folder_names(const std::string &dir) {
    std::vector<std::string> out;
    if (DIR *d = opendir(dir.c_str())) {
        while (dirent *e = readdir(d))
            if (std::string(e->d_name) != "." && std::string(e->d_name) != "..")
                out.push_back(e->d_name);
        closedir(d);
    }
    return out;
}
