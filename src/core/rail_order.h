// The order of the rail's places and devices, after Files' sidebar_order.rs:
// rows are named by stable keys (never used as paths), the saved order
// lists keys, and a row without a saved slot keeps its place after the
// saved ones. Bookmarks are not ordered here: their order is the GTK
// bookmarks file's (places::move_bookmark).
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace flo::rail_order {

constexpr std::size_t kMaxEntries = 128;
constexpr std::size_t kMaxBytes = 64 * 1024;

// "section:" and the path's bytes in lower-case hex, so no two paths share
// a key and a key never contains a separator.
std::string path_key(const std::string &section, const std::string &path);
// A place: its path, then a 00 byte and its label, so two roles on the
// same folder stay apart (Files' place_key).
std::string place_key(const std::string &path, const std::string &label);
// A device, by its name (stable whether or not it is mounted).
std::string device_key(const std::string &name);

// The order to show `keys` in: saved keys first in their saved order, the
// rest after them in the order given.
std::vector<std::size_t> ordered_indices(const std::vector<std::string> &keys,
                                         const std::vector<std::string> &saved);

// The saved order after moving `key` to `index` among `visible` (the rows
// of one section as shown). Keys of absent rows and of other sections keep
// their slots. False when the request is stale, duplicated or too large.
bool moved(const std::vector<std::string> &saved, const std::vector<std::string> &visible, const std::string &key,
           std::size_t index, std::vector<std::string> &out);

// The settings value: keys joined by commas. parse() drops anything that
// is not a well-formed key and stops at the limits.
std::string serialize(const std::vector<std::string> &order);
std::vector<std::string> parse(const std::string &text);

} // namespace flo::rail_order
