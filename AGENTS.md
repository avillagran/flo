# Flo instructions

Flo is a lightweight C++ file manager for Omarchy. Folio
(github.com/omacom/folio, MIT) is the reference for behavior, keys and
desktop standards: when in doubt, do what Folio does, unless it costs
memory. The Rust predecessor (`~/Work/files`, GitHub `files-rust`) is the
reference for safety rules (undo through the trash, conflict handling,
sandboxed child processes); never edit it from here.

The toolkit is the `lib-fltk-omarchy` submodule in `third_party/`. Change
it in its own repository, then bump the submodule.

## Language and attribution

All documentation, code comments, commit messages and PR text are in
English. Never add an AI assistant, model or tool as author, co-author,
reviewer or signer anywhere (no Co-Authored-By trailers, no "Generated
with" footers).

## Lightweight first

Memory and startup are the product. Before adding a dependency, check
whether FLTK, the C++17 standard library, or a library FLTK already loads
(GLib/GIO, D-Bus, cairo, pango) covers it. Keep heavy or risky work
(decoders, thumbnailers, mounts) in child processes so the main process
stays small. Measure PSS (`/proc/<pid>/smaps_rollup`) for changes that
could grow memory and record the number in the commit or docs.

## Builds

CMake with out-of-tree builds under `build/` only (ignored by git). FLTK
1.4 comes from the system (`find_package(FLTK 1.4 CONFIG)`); on the NODE
development machine it lives in `~/.local/opt/fltk-1.4.5`, so configure
with `-DCMAKE_PREFIX_PATH=$HOME/.local/opt/fltk-1.4.5` and use
`~/.local/opt/bin/cmake`. Never install packages system-wide.

## Tests never touch real user data

Tests run with a private 0700 `TMPDIR` and private `HOME`,
`XDG_CONFIG_HOME`, `XDG_DATA_HOME`, `XDG_CACHE_HOME` and
`XDG_STATE_HOME` inside it; the test runner sets these itself. Fixtures
come from a helper that creates a fresh exclusive directory per test and
never deletes it. Tests do not delete directory trees, do not follow
symlinks out of their fixture, and never point links at `/`, `$HOME` or
real host paths. Product code that deletes must refuse to act outside the
path it was asked to act on. If anything deletes files unexpectedly, stop
and tell the user; never retry the command or run it another way.
