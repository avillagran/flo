#include "harness.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <stdexcept>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "core/fsutil.h"

namespace flo_test {

int g_checks = 0;
static int g_failures = 0;
static const char *g_current = "";

std::vector<Case> &registry() {
    static std::vector<Case> r;
    return r;
}

void fail(const char *file, int line, const std::string &what) {
    ++g_failures;
    std::fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", g_current, file, line, what.c_str());
}

static std::string run_root() {
    const char *r = std::getenv("FLO_TEST_RUN");
    return r ? r : "";
}

void assert_inside(const std::string &path) {
    const std::string root = run_root();
    if (root.empty() || !flo::path_inside(flo::absolute_path(path), root)) {
        std::fprintf(stderr, "refusing: %s is outside the private test run %s\n", path.c_str(), root.c_str());
        std::abort();
    }
}

std::string private_dir(const std::string &label) {
    const char *t = std::getenv("TMPDIR");
    std::string tmpl = std::string(t) + "/" + label + "-XXXXXX";
    if (!::mkdtemp(&tmpl[0])) {
        std::perror("mkdtemp");
        std::abort();
    }
    assert_inside(tmpl);
    return tmpl;
}

std::string write_file(const std::string &path, const std::string &data) {
    assert_inside(path);
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) {
        std::perror(path.c_str());
        std::abort();
    }
    if (!data.empty() && ::write(fd, data.data(), data.size()) != static_cast<ssize_t>(data.size()))
        std::abort();
    ::close(fd);
    return path;
}

std::string read_file(const std::string &path) {
    std::string out;
    flo::read_file_capped(path, 64u << 20, out, false);
    return out;
}

} // namespace flo_test

// Refuses to run against real user data: every location must be inside the
// private run directory that run-isolated.sh created.
static bool environment_is_private() {
    const std::string root = flo_test::run_root();
    if (root.empty())
        return false;
    for (const char *v : {"TMPDIR", "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_CACHE_HOME", "XDG_STATE_HOME"}) {
        const char *val = std::getenv(v);
        if (!val || !flo::path_inside(val, root)) {
            std::fprintf(stderr, "%s is not inside the private run %s\n", v, root.c_str());
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv) {
    if (!environment_is_private()) {
        std::fprintf(stderr, "flo-tests must run through tests/run-isolated.sh (ctest does this).\n");
        return 2;
    }
    const char *only = argc > 1 ? argv[1] : nullptr;
    int ran = 0, failed_tests = 0;
    for (const auto &c : flo_test::registry()) {
        if (only && !std::strstr(c.name, only))
            continue;
        flo_test::g_current = c.name;
        const int before = flo_test::g_failures;
        try {
            c.fn();
        } catch (const std::exception &e) {
            flo_test::fail(__FILE__, __LINE__, std::string("exception: ") + e.what());
        }
        ++ran;
        const bool ok = flo_test::g_failures == before;
        if (!ok)
            ++failed_tests;
        std::printf("%s %s\n", ok ? "ok  " : "FAIL", c.name);
    }
    std::printf("\n%d tests, %d checks, %d failed\n", ran, flo_test::g_checks, failed_tests);
    return failed_tests == 0 ? 0 : 1;
}
