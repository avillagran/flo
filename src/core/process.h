// Child processes: fixed argv, no shell, optional rlimits, a wall timeout
// that kills the whole process group, cancellation and capped output.
#pragma once

#include "core/event_queue.h"

#include <cstdint>
#include <string>
#include <vector>
#include <sys/types.h>

namespace flo {

struct ProcessLimits {
    uint64_t address_space = 0; // RLIMIT_AS bytes, 0 = inherit
    unsigned cpu_seconds = 0;   // RLIMIT_CPU, 0 = inherit
    uint64_t file_size = 0;     // RLIMIT_FSIZE bytes, 0 = inherit
    unsigned open_files = 0;    // RLIMIT_NOFILE, 0 = inherit
    bool no_core = true;        // RLIMIT_CORE = 0
};

struct RunOptions {
    int timeout_ms = 15000;           // -1 = none
    std::size_t max_output = 1 << 20; // per stream; the rest is discarded
    ProcessLimits limits;
    const CancelToken *cancel = nullptr;
    std::string cwd;
    std::vector<std::string> set_env; // "NAME=value" added to the environment
};

struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    bool cancelled = false;
    int exit_code = -1;  // valid when exited normally
    int term_signal = 0; // non-zero when killed by a signal
    std::string out, err;
    bool ok() const { return started && !timed_out && !cancelled && term_signal == 0 && exit_code == 0; }
};

// Runs argv[0] (searched in $PATH when it has no slash) to completion.
ProcessResult run_process(const std::vector<std::string> &argv, const RunOptions &options = {});

// A long-lived child connected by a socket pair as its stdin and stdout
// (stderr is inherited). Used for the thumbnail worker.
struct PipeChild {
    pid_t pid = -1;
    int to_child = -1;   // write side (the same socket as from_child)
    int from_child = -1; // read side
};
bool spawn_pipe_child(const std::vector<std::string> &argv, const ProcessLimits &limits, PipeChild &child,
                      const std::vector<std::string> &set_env = {});
// Writes all of `data` without ever raising SIGPIPE.
bool write_to_child(const PipeChild &child, const std::string &data);
// SIGKILLs the child's process group, reaps it and closes the pipes.
void kill_pipe_child(PipeChild &child);

// Applies limits to the calling process (used by the worker on itself).
void apply_limits(const ProcessLimits &limits);

// The directory holding the running executable (/proc/self/exe).
std::string executable_dir();

} // namespace flo
