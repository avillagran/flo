// File clipboard payloads, interoperable with GNOME and KDE apps. The UI
// does the actual clipboard I/O; these build and parse the bytes.
#pragma once

#include <string>
#include <vector>

namespace flo::clipboard {

// "x-special/gnome-copied-files": "copy" or "cut", then one URI per line.
std::string gnome_copied_files(const std::vector<std::string> &paths, bool cut);
// "text/uri-list": URIs separated by CRLF (RFC 2483).
std::string uri_list(const std::vector<std::string> &paths);
// "application/x-kde-cutselection": "1" for cut, else absent.
inline const char *kde_cut_selection() { return "1"; }
// "text/plain;charset=utf-8": the paths, one per line (Folio's text).
std::string plain_text(const std::vector<std::string> &paths);

// The MIME types to offer, in order of preference.
std::vector<std::string> offered_types(bool cut);

// Parses gnome-copied-files. Returns local paths only; `cut` tells whether
// the files were cut.
std::vector<std::string> parse_gnome_copied_files(const std::string &data, bool &cut);
// Parses text/uri-list (comments and non-local URIs skipped).
std::vector<std::string> parse_uri_list(const std::string &data);
// Best effort from whatever was offered: gnome format first, then
// uri-list, then plain text holding absolute paths or file URIs.
std::vector<std::string> parse_any(const std::string &mime, const std::string &data, bool &cut);

} // namespace flo::clipboard
