# Flo

**F**ile manager, **L**ight, l**O**w memory.

Flo is a lightweight file manager for Omarchy. It follows the patterns and
standards of [Folio](https://github.com/omacom/folio), Omarchy's own file
manager: the same keys, the same shared thumbnail cache, GTK bookmarks and
recent files, the freedesktop trash, and nothing running in the background.
Flo's goal is to do that with as little memory as possible, using
[FLTK](https://www.fltk.org) instead of Qt Quick.

## From Rust to C++

Flo is the C++ rewrite of
[avillagran/files-rust](https://github.com/avillagran/files-rust), the
original project, a Rust file manager built on fltk-rs. The Rust version
proved that a full file manager fits in about 19 MiB, but each Rust project
carried gigabytes of build output and long release builds. Flo keeps the
goal and drops the weight: plain C++17 on FLTK 1.4, built with CMake into a
build folder of a few megabytes.

Its toolkit moved the same way:
[avillagran/lib-fltk-omarchy](https://github.com/avillagran/lib-fltk-omarchy)
(C++), from
[lib-fltk-omarchy-rust](https://github.com/avillagran/lib-fltk-omarchy-rust).

## Status

Early. The non-UI core is in place; the window is being built on top of it.

- **flo-core** (`src/core/`, no FLTK): folder listing off the UI thread with
  Folio's sort modes and natural order, live reload, `/` search, the shared
  freedesktop thumbnail cache with sandboxed `flo-thumbnailer` workers, file
  operations that never overwrite (copy, move, paste, duplicate, rename, new
  folder, compress, extract) with undo and redo, the freedesktop trash,
  GNOME/KDE clipboard formats, places (home folders, GTK bookmarks, Recents,
  drives with mount/unmount/eject), tailnet machines over SFTP and Dropbox
  status, `org.freedesktop.FileManager1`, and settings.
- **CLI** for scripts and agents, no display needed:
  `flo list|info|thumbnail|trash|restore [--json] PATH`.
- **GUI**: not built yet.

See [docs/CORE.md](docs/CORE.md), [docs/THUMBNAILS.md](docs/THUMBNAILS.md)
and [docs/FOLIO-PARITY.md](docs/FOLIO-PARITY.md).

## Building

Requires CMake 3.16+, a C++17 compiler, GLib/GIO, libpng and libjpeg-turbo,
plus `bsdtar` (libarchive) at runtime for compress and extract. FLTK 1.4 is
a build-time dependency only: the `flo` binary links `libfltk.a` statically
by default, so the fltk package isn't needed at runtime
(`-DFLO_SHARED_FLTK=ON` links the shared library instead).

```sh
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

Tests run in a fresh private directory under `build/test-runs/` with their
own `HOME`, `TMPDIR` and XDG directories; they never touch your files.

## License

MIT. See [LICENSE](LICENSE).
