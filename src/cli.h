// Flo's command line for agents and scripts (no display needed).
#pragma once

namespace flo::cli {

// True when argv[1] names a CLI command (list, info, thumbnail, trash,
// restore, help, version).
bool is_command(int argc, char **argv);
// Runs it: exit status 0 on success, 1 on failure (errors on stderr), 2 on
// bad usage.
int run(int argc, char **argv);

} // namespace flo::cli
