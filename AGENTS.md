# Flo instructions

Flo is the C++ successor of **Files** (`~/Work/files`, GitHub
`files-rust`), the Rust file manager for Omarchy, and keeps Files'
identity: **Flo must be Files.** Files is the reference for the look,
layout, keys, wording and feature set (its README and `docs/*.md`, its
screenshots in `docs/evidence/`, the binary at `~/.local/bin/files`, its
toolkit `~/Work/lib-fltk-omarchy`), and for safety rules (undo through the
trash, conflict handling, sandboxed child processes). Never edit Files or
its Rust toolkit from here.

Folio (github.com/omacom/folio, MIT) is a reference only for desktop
standards and plumbing: freedesktop thumbnails and trash, GTK bookmarks,
Recents, `org.freedesktop.FileManager1`, GVfs/SFTP and the tailnet, MTP.
Where Folio and Files differ in look, keys or wording, Files wins.

`docs/FILES-PARITY.md` tracks Files' features in Flo. Compare visually with
`tools/side-by-side.sh` (Files and Flo in the same states, each in a
private Xvfb session).

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
