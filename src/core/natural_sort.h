// Natural, case-insensitive name ordering ("file2" before "file10"), the
// way Folio's numeric QCollator orders names.
#pragma once

#include <string>

namespace flo {

// < 0, 0, > 0. Digit runs compare by value (leading zeros only break ties),
// letters compare case-insensitively (Unicode simple lowercase), and equal
// names under those rules fall back to byte order so the order is total.
int natural_compare(const std::string &a, const std::string &b);

inline bool natural_less(const std::string &a, const std::string &b) { return natural_compare(a, b) < 0; }

// Case-insensitive substring match for search; `needle_folded` must come
// from fold_case().
std::string fold_case(const std::string &s);
bool contains_folded(const std::string &haystack, const std::string &needle_folded);

} // namespace flo
