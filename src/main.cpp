// flo: entry point. Dispatch stays simple on purpose:
//   1. CLI commands (src/cli.cpp) run without a display;
//   2. `flo preview PATH` is the preview window's own process (src/ui/preview.cpp);
//   3. otherwise a running Flo is asked to open the window (single instance);
//   4. otherwise the GUI starts (src/ui/).

#include "cli.h"
#include "core/dbus_service.h"
#include "core/fsutil.h"
#include "core/uri.h"
#include "core/xdg.h"

#include <cstdio>
#include <string>

// Accepts a path, a file:// URI (how launchers and xdg-open pass folders),
// or nothing for home. A file opens its folder with the file selected.
static void start_location(int argc, char **argv, std::string &folder, std::string &select) {
    folder = flo::xdg::home();
    if (argc < 2)
        return;
    const std::string arg = argv[1];
    std::string path = flo::uri_to_path(arg);
    if (path.empty())
        path = flo::absolute_path(arg);
    if (flo::is_dir(path)) {
        folder = path;
    } else if (flo::exists_nofollow(path)) {
        folder = flo::path_dirname(path);
        select = flo::path_basename(path);
    }
}

#ifdef FLO_HAVE_GUI
int run_gui(const std::string &folder, const std::string &select); // src/ui/app.cpp
int run_preview(int argc, char **argv);                             // src/ui/preview.cpp
#endif

int main(int argc, char **argv) {
    if (flo::cli::is_command(argc, argv))
        return flo::cli::run(argc, argv);
#ifdef FLO_HAVE_GUI
    if (argc >= 2 && std::string(argv[1]) == "preview")
        return run_preview(argc, argv);
#endif

    std::string folder, select;
    start_location(argc, argv, folder, select);
    if (flo::forward_open_to_running_flo(folder, select))
        return 0;

#ifdef FLO_HAVE_GUI
    return run_gui(folder, select);
#else
    std::fprintf(stderr, "flo: GUI not built yet (would open %s%s%s). Try `flo help`.\n", folder.c_str(),
                 select.empty() ? "" : ", selecting ", select.c_str());
    return 1;
#endif
}
