// file:// URIs and percent-encoding, matching GLib.
#pragma once

#include <string>

namespace flo {

// GLib's g_filename_to_uri() escaping for an absolute path: ASCII
// alphanumerics and !$&'()*+,-./:=@_~ stay literal, every other byte becomes
// %XX in upper case. This is the URI whose MD5 names a shared thumbnail.
std::string path_to_uri(const std::string &absolute_path);

// "file:///a%20b" -> "/a b". Returns "" for anything but a local file URI
// (no host other than empty or "localhost"), or an invalid escape / NUL.
std::string uri_to_path(const std::string &uri);

// The escaping used for a .trashinfo Path= value (same character set as a
// URI path).
std::string percent_encode_path(const std::string &path);

// Decodes %XX; returns false on a malformed escape or an encoded NUL.
bool percent_decode(const std::string &in, std::string &out);

// The scheme of a URI ("sftp" for "sftp://host/"), or "" for none.
std::string uri_scheme(const std::string &uri);

} // namespace flo
