#include "core/process.h"
#include "core/fsutil.h"
#include "core/xdg.h"

#include <cerrno>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>

#include <fcntl.h>
#include <poll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace flo {

void apply_limits(const ProcessLimits &l) {
    rlimit r;
    if (l.address_space) {
        r.rlim_cur = r.rlim_max = l.address_space;
        ::setrlimit(RLIMIT_AS, &r);
    }
    if (l.cpu_seconds) {
        r.rlim_cur = l.cpu_seconds;
        r.rlim_max = l.cpu_seconds + 1;
        ::setrlimit(RLIMIT_CPU, &r);
    }
    if (l.file_size) {
        r.rlim_cur = r.rlim_max = l.file_size;
        ::setrlimit(RLIMIT_FSIZE, &r);
    }
    if (l.open_files) {
        r.rlim_cur = r.rlim_max = l.open_files;
        ::setrlimit(RLIMIT_NOFILE, &r);
    }
    if (l.no_core) {
        r.rlim_cur = r.rlim_max = 0;
        ::setrlimit(RLIMIT_CORE, &r);
    }
}

namespace {

// Everything the child needs is prepared before fork(): after it, only
// async-signal-safe calls run.
struct Prepared {
    std::string program;
    std::vector<std::string> args;
    std::vector<std::string> env;
    std::vector<char *> argv, envp;

    bool prepare(const std::vector<std::string> &in, const std::vector<std::string> &set_env) {
        if (in.empty())
            return false;
        program = xdg::find_program(in[0]);
        if (program.empty())
            return false;
        args = in;
        for (char **e = environ; e && *e; ++e) {
            const std::string kv = *e;
            const auto eq = kv.find('=');
            bool replaced = false;
            for (const auto &s : set_env)
                if (eq != std::string::npos && s.compare(0, eq + 1, kv, 0, eq + 1) == 0)
                    replaced = true;
            if (!replaced)
                env.push_back(kv);
        }
        for (const auto &s : set_env)
            env.push_back(s);
        for (auto &a : args)
            argv.push_back(&a[0]);
        argv.push_back(nullptr);
        for (auto &e : env)
            envp.push_back(&e[0]);
        envp.push_back(nullptr);
        return true;
    }
};

void close_from(int lowest) {
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, lowest, ~0U, 0) == 0)
        return;
#endif
    for (int fd = lowest; fd < 1024; ++fd)
        ::close(fd);
}

[[noreturn]] void child_exec(const Prepared &p, const ProcessLimits &limits, int in_fd, int out_fd, int err_fd,
                             const char *cwd, int status_fd) {
    ::setpgid(0, 0);
    // Default signal dispositions and an empty mask for the new program.
    sigset_t none;
    sigemptyset(&none);
    ::sigprocmask(SIG_SETMASK, &none, nullptr);
    ::signal(SIGPIPE, SIG_DFL);
    if (in_fd >= 0 && in_fd != 0)
        ::dup2(in_fd, 0);
    if (out_fd >= 0 && out_fd != 1)
        ::dup2(out_fd, 1);
    if (err_fd >= 0 && err_fd != 2)
        ::dup2(err_fd, 2);
    // The status pipe survives close_from by moving it to fd 3.
    if (status_fd >= 0 && status_fd != 3) {
        ::dup2(status_fd, 3);
        status_fd = 3;
    }
    ::fcntl(3, F_SETFD, FD_CLOEXEC);
    close_from(status_fd >= 0 ? 4 : 3);
    if (cwd && *cwd && ::chdir(cwd) != 0) {
    }
    apply_limits(limits);
    ::execve(p.program.c_str(), p.argv.data(), p.envp.data());
    const int e = errno;
    if (status_fd >= 0) {
        ssize_t ignored = ::write(status_fd, &e, sizeof e);
        (void)ignored;
    }
    ::_exit(127);
}

} // namespace

ProcessResult run_process(const std::vector<std::string> &argv, const RunOptions &opt) {
    ProcessResult result;
    Prepared p;
    if (!p.prepare(argv, opt.set_env))
        return result;

    int out_pipe[2], err_pipe[2], status_pipe[2];
    if (::pipe2(out_pipe, O_CLOEXEC) != 0)
        return result;
    if (::pipe2(err_pipe, O_CLOEXEC) != 0) {
        ::close(out_pipe[0]);
        ::close(out_pipe[1]);
        return result;
    }
    if (::pipe2(status_pipe, O_CLOEXEC) != 0) {
        for (int fd : {out_pipe[0], out_pipe[1], err_pipe[0], err_pipe[1]})
            ::close(fd);
        return result;
    }
    const int devnull = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
    const pid_t pid = ::fork();
    if (pid == 0)
        child_exec(p, opt.limits, devnull, out_pipe[1], err_pipe[1], opt.cwd.c_str(), status_pipe[1]);
    if (devnull >= 0)
        ::close(devnull);
    ::close(out_pipe[1]);
    ::close(err_pipe[1]);
    ::close(status_pipe[1]);
    if (pid < 0) {
        ::close(out_pipe[0]);
        ::close(err_pipe[0]);
        ::close(status_pipe[0]);
        return result;
    }
    ::setpgid(pid, pid); // also from here, so the group exists before any kill

    int exec_errno = 0;
    {
        ssize_t n;
        do {
            n = ::read(status_pipe[0], &exec_errno, sizeof exec_errno);
        } while (n < 0 && errno == EINTR);
        if (n != sizeof exec_errno)
            exec_errno = 0;
        ::close(status_pipe[0]);
    }
    result.started = exec_errno == 0;

    const auto start = std::chrono::steady_clock::now();
    pollfd fds[2] = {{out_pipe[0], POLLIN, 0}, {err_pipe[0], POLLIN, 0}};
    std::string *bufs[2] = {&result.out, &result.err};
    int open_count = 2;
    bool killed = false;
    char chunk[16384];
    while (open_count > 0) {
        int wait_ms = 100;
        if (opt.timeout_ms >= 0) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now() - start).count();
            if (elapsed >= opt.timeout_ms) {
                result.timed_out = true;
                break;
            }
            wait_ms = static_cast<int>(std::min<long long>(100, opt.timeout_ms - elapsed));
        }
        if (opt.cancel && opt.cancel->cancelled()) {
            result.cancelled = true;
            break;
        }
        const int r = ::poll(fds, 2, wait_ms);
        if (r < 0 && errno != EINTR)
            break;
        for (int k = 0; k < 2; ++k) {
            if (fds[k].fd < 0 || !(fds[k].revents & (POLLIN | POLLHUP | POLLERR)))
                continue;
            const ssize_t n = ::read(fds[k].fd, chunk, sizeof chunk);
            if (n > 0) {
                const std::size_t room = opt.max_output > bufs[k]->size() ? opt.max_output - bufs[k]->size() : 0;
                bufs[k]->append(chunk, std::min<std::size_t>(room, static_cast<std::size_t>(n)));
            } else if (n == 0 || (n < 0 && errno != EINTR && errno != EAGAIN)) {
                ::close(fds[k].fd);
                fds[k].fd = -1;
                --open_count;
            }
        }
    }
    for (auto &f : fds)
        if (f.fd >= 0)
            ::close(f.fd);
    // Wait for the exit without reaping, so the group id stays reserved by
    // the zombie while what's left of the group is killed.
    while (!result.timed_out && !result.cancelled) {
        siginfo_t info{};
        if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 && info.si_pid == pid)
            break;
        if (opt.cancel && opt.cancel->cancelled()) {
            result.cancelled = true;
            break;
        }
        if (opt.timeout_ms >= 0 && std::chrono::steady_clock::now() - start >
                                       std::chrono::milliseconds(opt.timeout_ms)) {
            result.timed_out = true;
            break;
        }
        ::usleep(5000);
    }
    // Kills a timed-out child, and grandchildren left in the group (a
    // thumbnailer's helpers) either way.
    ::kill(-pid, SIGKILL);
    if (result.timed_out || result.cancelled) {
        ::kill(pid, SIGKILL);
        killed = true;
    }
    (void)killed;
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (WIFEXITED(status))
        result.exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        result.term_signal = WTERMSIG(status);
    return result;
}

bool spawn_pipe_child(const std::vector<std::string> &argv, const ProcessLimits &limits, PipeChild &child,
                      const std::vector<std::string> &set_env) {
    Prepared p;
    if (!p.prepare(argv, set_env))
        return false;
    // A socket pair rather than pipes: the parent writes with MSG_NOSIGNAL,
    // so a worker that died never raises SIGPIPE in the UI process.
    int sv[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) != 0)
        return false;
    const pid_t pid = ::fork();
    if (pid == 0)
        child_exec(p, limits, sv[1], sv[1], -1, nullptr, -1);
    ::close(sv[1]);
    if (pid < 0) {
        ::close(sv[0]);
        return false;
    }
    ::setpgid(pid, pid);
    child.pid = pid;
    child.to_child = sv[0];
    child.from_child = sv[0];
    return true;
}

bool write_to_child(const PipeChild &child, const std::string &data) {
    std::size_t done = 0;
    while (done < data.size()) {
        const ssize_t n = ::send(child.to_child, data.data() + done, data.size() - done, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return false;
        done += static_cast<std::size_t>(n);
    }
    return true;
}

void kill_pipe_child(PipeChild &child) {
    if (child.pid > 0) {
        ::kill(-child.pid, SIGKILL);
        ::kill(child.pid, SIGKILL);
        int status;
        while (::waitpid(child.pid, &status, 0) < 0 && errno == EINTR) {
        }
    }
    if (child.to_child >= 0)
        ::close(child.to_child);
    if (child.from_child >= 0 && child.from_child != child.to_child)
        ::close(child.from_child);
    child = PipeChild{};
}

std::string executable_dir() {
    char buf[PATH_MAX];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0)
        return {};
    buf[n] = '\0';
    return path_dirname(buf);
}

} // namespace flo
