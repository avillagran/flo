#include "ui/strings.h"

#include "fltk-omarchy/i18n.h"

namespace flo::ui {

namespace {
// English plural forms for the strings Flo counts with fo::trn().
const char kEnglish[] = R"({
  "Copied %n items": { "one": "Copied %n item", "other": "Copied %n items" },
  "Cut %n items": { "one": "Cut %n item", "other": "Cut %n items" },
  "Copied %n paths": { "one": "Copied %n path", "other": "Copied %n paths" },
  "Restore %n Items": { "one": "Restore %n Item", "other": "Restore %n Items" },
  "Delete %n Items": { "one": "Delete %n Item", "other": "Delete %n Items" },
  "%n items will be deleted permanently. This can't be undone.": {
    "one": "%n item will be deleted permanently. This can't be undone.",
    "other": "%n items will be deleted permanently. This can't be undone." }
})";
} // namespace

void register_catalogs() { fo::i18n::add_catalog("en", kEnglish); }

} // namespace flo::ui
