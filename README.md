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

Early. See [docs/](docs/) as it grows.

## Building

Requires CMake 3.16+, a C++17 compiler and FLTK 1.4 (`pacman -S fltk` on
Arch).

```sh
git submodule update --init
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build
```

## License

MIT. See [LICENSE](LICENSE).
