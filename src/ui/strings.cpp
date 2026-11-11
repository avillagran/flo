#include "ui/strings.h"

namespace flo::ui {

// flo_catalogs.cpp, generated from i18n/*.json at build time.
void register_embedded_catalogs();

// English carries the plural forms of counted strings; every other
// language of Omarchy's registry has its own catalog (i18n/<code>.json).
void register_catalogs() { register_embedded_catalogs(); }

} // namespace flo::ui
