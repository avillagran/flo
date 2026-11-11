# Turns i18n/*.json into a C++ source that registers each catalog with
# fo::i18n::add_catalog(). The JSON stays in read-only data: the toolkit
# parses only English, the base language and the regional one when the
# language is set, so the other catalogs are never paged in.
#
#   cmake -DSOURCE_DIR=<i18n dir> -DOUTPUT=<file.cpp> -P embed-catalogs.cmake
file(GLOB catalogs "${SOURCE_DIR}/*.json")
list(SORT catalogs)
set(body "// Generated from i18n/*.json by cmake/embed-catalogs.cmake. Do not edit.\n")
string(APPEND body "#include \"fltk-omarchy/i18n.h\"\n\nnamespace flo::ui {\n\nnamespace {\n")
set(calls "")
set(index 0)
foreach(path IN LISTS catalogs)
  get_filename_component(code "${path}" NAME_WE)
  if(code STREQUAL "strings")
    continue() # the list of source strings, not a catalog
  endif()
  file(READ "${path}" json)
  string(FIND "${json}" ")FLOI18N\"" clash)
  if(NOT clash EQUAL -1)
    message(FATAL_ERROR "${path} contains the raw string delimiter")
  endif()
  string(APPEND body "const char kCatalog${index}[] = R\"FLOI18N(${json})FLOI18N\";\n")
  string(APPEND calls "    fo::i18n::add_catalog(\"${code}\", kCatalog${index});\n")
  math(EXPR index "${index} + 1")
endforeach()
string(APPEND body "} // namespace\n\nvoid register_embedded_catalogs() {\n${calls}}\n\n} // namespace flo::ui\n")
# Rewrite only when something changed, so an unchanged build stays put.
set(old "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" old)
endif()
if(NOT old STREQUAL body)
  file(WRITE "${OUTPUT}" "${body}")
endif()
