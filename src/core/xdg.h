// XDG base and user directories, read from the environment on every call
// (nothing cached), so tests can point them at a private fixture.
#pragma once

#include <string>
#include <vector>

namespace flo::xdg {

std::string home();          // $HOME, else the passwd entry
std::string config_home();   // $XDG_CONFIG_HOME or ~/.config
std::string data_home();     // $XDG_DATA_HOME or ~/.local/share
std::string cache_home();    // $XDG_CACHE_HOME or ~/.cache
std::string state_home();    // $XDG_STATE_HOME or ~/.local/state
std::string runtime_dir();   // $XDG_RUNTIME_DIR or ""
std::vector<std::string> data_dirs(); // $XDG_DATA_DIRS or /usr/local/share:/usr/share

// The user directories from $XDG_CONFIG_HOME/user-dirs.dirs, by key
// ("DESKTOP", "DOWNLOAD", "TEMPLATES", "PUBLICSHARE", "DOCUMENTS", "MUSIC",
// "PICTURES", "VIDEOS"). Missing keys fall back to ~/Name; a key pointing at
// home itself counts as unset ("" is returned).
std::string user_dir(const std::string &key);
// Parses user-dirs.dirs text; `home` replaces "$HOME".
std::vector<std::pair<std::string, std::string>> parse_user_dirs(const std::string &text,
                                                                  const std::string &home);

// Finds an executable in $PATH (or checks an absolute/relative path).
std::string find_program(const std::string &name);

// Test hooks: the value of environment variable `name`, but only inside a
// private test run (FLO_TEST_RUN is set by tests/run-isolated.sh and
// tools/gui-session.sh); always "" otherwise, so a stray variable never
// changes how Flo behaves for a user. Hooks: FLO_TEST_MOUNTINFO (a
// mountinfo fixture instead of /proc/self/mountinfo, with fake unmount and
// eject of its drives) and FLO_TEST_FAKE_GVFS (a folder standing in for
// GVfs mounts, driving the real GMountOperation questions).
std::string test_hook(const char *name);

} // namespace flo::xdg
