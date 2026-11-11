# Thumbnails

Flo shares the desktop's thumbnail cache the way Folio does, following the
freedesktop [Thumbnail Managing Standard](https://specifications.freedesktop.org/thumbnail-spec/latest/).
Thumbnails other apps made are used as they are; the ones Flo makes are
usable by them.

## Cache layout

- Root: `$XDG_CACHE_HOME/thumbnails` (`~/.cache/thumbnails`). Tests inject a
  root inside their fixture; the worker takes `FLO_THUMBNAIL_CACHE`.
- File name: `MD5(uri).png`, where the URI is the absolute `file://` URI with
  GLib's `g_filename_to_uri` escaping (ASCII alphanumerics and
  `!$&'()*+,-./:=@_~` literal, every other byte `%XX` upper case). Symlinks
  are not resolved, like GIO.
- Buckets by requested size: `normal` up to 128 px, `large` up to 256,
  `x-large` up to 512, `xx-large` up to 1024 (and above).
- A lookup tries the requested bucket, then bigger ones (scaled down by the
  UI); smaller ones are never used.
- Failure markers: `fail/flo/MD5(uri).png`, a 1x1 PNG with the same keys.
  Only a decoder or thumbnailer that ran and failed leaves one, so a
  thumbnailer installed later still gets its turn. Other apps' `fail/`
  folders are never read.

## Writing

- Keys in plain `tEXt` chunks (never `zTXt`): `Thumb::URI`, `Thumb::MTime`
  (whole seconds), `Thumb::Size`, `Software=flo`.
- RGB when opaque, RGBA otherwise, never upscaled.
- Directories are created 0700; an existing one that is a symlink, belongs
  to someone else or is group/world-writable is refused.
- The PNG goes to `.<name>.flo-<pid>-<n>.tmp` (`O_CREAT|O_EXCL|O_NOFOLLOW`,
  mode exactly 0600) and is renamed over the target, so no app reads half a
  thumbnail.
- If the file changed (mtime or size) while it was being thumbnailed, the
  result is not stored.
- Files inside the cache are never thumbnailed (checked lexically and
  through the real path of their folder).

## Reading (untrusted)

`read_png_meta` refuses symlinks, non-regular files, files not owned by the
user and files over a per-bucket cap (1, 2, 8, 16 MiB). It walks chunk
headers only: the PNG signature, a first 13-byte `IHDR` with dimensions from
1 to the bucket size, at most 4096 chunks and a reachable `IEND`. Only
`tEXt` chunks of at most 16 KiB are read, with their CRC checked; a repeated
`Thumb::` key rejects the file. Nothing is inflated in the UI process. A
thumbnail counts only when `Thumb::URI` equals the URI, `Thumb::MTime`
equals the file's mtime in seconds and `Thumb::Size`, when present, equals
its size.

## Generation: the worker process

`ThumbnailService` (in the UI process) answers cache hits, valid failure
markers and unsupported types itself, and sends the rest to `flo-thumbnailer`
worker processes over a socket pair (protocol in
`src/core/thumbnail_protocol.h`). The UI process only ever reads the final
small PNGs.

- 2 to 4 workers (by CPU count), started on demand, exiting after 20 s idle.
- Requests are served newest first (the tiles just scrolled into view).
  Requests not yet sent can be cancelled; a request in flight finishes and is
  cached, but its callback is dropped.
- A worker that takes over 30 s is killed; a worker that dies or hangs on a
  file makes the next worker leave a failure marker for it, so it is not
  retried forever.
- The worker lowers its own soft address-space limit to 1 GiB and disables
  core dumps. JPEG is decoded with libjpeg-turbo using DCT-domain scaling
  (1/2, 1/4, 1/8) to the smallest size at least as big as the bucket, with
  EXIF orientation applied and CMYK converted; PNG with libpng, rows streamed
  into a box filter (interlaced PNGs are buffered, up to 50 megapixels).
  Images over 400 megapixels are refused.
- Everything else goes through the installed `.thumbnailer` entries from
  `$XDG_DATA_DIRS/thumbnailers` (lowest priority first) and
  `$XDG_DATA_HOME/thumbnailers`: a file with the same name in a
  higher-priority directory replaces the lower one; `TryExec` must be found;
  `Exec` is split by the desktop-entry rules and `%i %u %o %s %%` expanded;
  no shell. Each run gets its own process group, a 15 s wall timeout that
  kills the group, `RLIMIT_AS` 2 GiB (glycin and ffmpeg map a lot),
  `RLIMIT_CPU` 20 s, `RLIMIT_FSIZE` 64 MiB, 512 descriptors and no core
  dumps. Its output (a regular file of at most 32 MiB, in a private 0700
  scratch folder under `$XDG_RUNTIME_DIR`) is decoded and re-encoded with
  Flo's keys, then removed.
- MIME types come from GIO's content type guess by name, sniffing the first
  KiB only when the name is ambiguous.

Installed on the development machine: glycin (HEIF/AVIF, JPEG XL, SVG and
the common raster formats), ffmpegthumbnailer (video and audio covers),
papers and evince (PDF, comics, DjVu, TIFF), gsf-office, xournalpp.

## Measurements (NODE, 2026-11-11, RelWithDebInfo)

| Process | Peak RSS | PSS |
|---|---|---|
| `flo-thumbnailer --once` (2000x1500 PNG to 256 px) | 11.2 MiB | — |
| `flo-thumbnailer` worker idle after one request | 6.9 MiB RSS | 1.0 MiB |

## CLI

`flo thumbnail [--json] [--size N] FILE...` makes or finds thumbnails and
prints their paths (exit 1 if any failed).
