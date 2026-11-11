cmake_minimum_required(VERSION 3.16)
project(flo VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Build out of tree: cmake -S . -B build")
endif()

add_compile_options(-Wall -Wextra)
add_compile_definitions(_GNU_SOURCE FLO_VERSION="${PROJECT_VERSION}")

find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0 gio-unix-2.0)
# Decoders only go into the thumbnail worker process, never into `flo`.
pkg_check_modules(PNG REQUIRED IMPORTED_TARGET libpng)
pkg_check_modules(JPEG REQUIRED IMPORTED_TARGET libjpeg)

option(FLO_BUILD_TESTS "Build the core tests" ON)

# ---------------------------------------------------------------------------
# flo-core: the non-UI core (no FLTK, no toolkit). See docs/CORE.md.
# ---------------------------------------------------------------------------
add_library(flo-core STATIC
  src/core/clipboard.cpp
  src/core/dbus_service.cpp
  src/core/entry.cpp
  src/core/event_queue.cpp
  src/core/fileops.cpp
  src/core/folder_views.cpp
  src/core/fsutil.cpp
  src/core/gmain_thread.cpp
  src/core/json.cpp
  src/core/lister.cpp
  src/core/mounter.cpp
  src/core/name_filter.cpp
  src/core/natural_sort.cpp
  src/core/network.cpp
  src/core/places.cpp
  src/core/process.cpp
  src/core/recents.cpp
  src/core/search.cpp
  src/core/session.cpp
  src/core/settings.cpp
  src/core/properties.cpp
  src/core/starred.cpp
  src/core/thumbnail_cache.cpp
  src/core/thumbnail_service.cpp
  src/core/thumbnailers.cpp
  src/core/trash.cpp
  src/core/tree_size.cpp
  src/core/undo.cpp
  src/core/uri.cpp
  src/core/watcher.cpp
  src/core/xdg.cpp
)
target_include_directories(flo-core PUBLIC src)
target_link_libraries(flo-core PUBLIC PkgConfig::GIO Threads::Threads)

# ---------------------------------------------------------------------------
# flo-thumbnailer: the sandboxed thumbnail worker process. It decodes JPEG
# and PNG itself and runs the installed .thumbnailer entries for the rest.
# ---------------------------------------------------------------------------
add_library(flo-thumbgen STATIC
  src/thumbnailer/decode.cpp
  src/thumbnailer/generate.cpp
  src/thumbnailer/png_write.cpp
)
target_link_libraries(flo-thumbgen PUBLIC flo-core PkgConfig::PNG PkgConfig::JPEG)

add_executable(flo-thumbnailer src/thumbnailer/main.cpp)
target_link_libraries(flo-thumbnailer PRIVATE flo-thumbgen)

# ---------------------------------------------------------------------------
# flo: the application: the CLI (src/cli.cpp), the window and the preview
# process (src/ui/, when FLO_BUILD_GUI is on).
# ---------------------------------------------------------------------------
add_executable(flo src/main.cpp src/cli.cpp)
target_link_libraries(flo PRIVATE flo-core)

# ===========================================================================
# FLTK for the GUI. The `flo` binary links FLTK statically by default
# (libfltk.a), so FLTK is a build-time dependency only and users don't need
# the fltk package at runtime. -DFLO_SHARED_FLTK=ON links libfltk.so instead.
# flo-core and flo-thumbnailer never link FLTK.
# ===========================================================================
option(FLO_SHARED_FLTK "Link the flo binary against shared FLTK instead of libfltk.a" OFF)
option(FLO_BUILD_GUI "Build the FLTK GUI (src/ui/ and the toolkit submodule)" ON)

# ===========================================================================
# The GUI: src/ui/ on top of the lib-fltk-omarchy toolkit (a submodule in
# third_party/). flo-core keeps building without any of this.
# ===========================================================================
if(FLO_BUILD_GUI)
  find_package(FLTK 1.4 CONFIG REQUIRED)
  if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lib-fltk-omarchy/CMakeLists.txt)
    message(FATAL_ERROR "The toolkit submodule is missing: git submodule update --init")
  endif()
  set(FO_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(FO_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(FLTK_OMARCHY_SHARED_FLTK ${FLO_SHARED_FLTK} CACHE BOOL "" FORCE)
  add_subdirectory(third_party/lib-fltk-omarchy EXCLUDE_FROM_ALL)
  file(GLOB FLO_UI_SOURCES CONFIGURE_DEPENDS src/ui/*.cpp)
  target_sources(flo PRIVATE ${FLO_UI_SOURCES})
  # Translation catalogs (i18n/<language>.json), embedded as read-only data.
  file(GLOB FLO_CATALOGS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/i18n/*.json)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/flo_catalogs.cpp
    COMMAND ${CMAKE_COMMAND} -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/i18n
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/flo_catalogs.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-catalogs.cmake
    DEPENDS ${FLO_CATALOGS} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed-catalogs.cmake
    COMMENT "Embedding translation catalogs")
  target_sources(flo PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/flo_catalogs.cpp)
  target_link_libraries(flo PRIVATE fltk-omarchy::fltk-omarchy)
  target_compile_definitions(flo PRIVATE FLO_HAVE_GUI=1)
endif()

install(TARGETS flo flo-thumbnailer RUNTIME DESTINATION bin)

if(FLO_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
