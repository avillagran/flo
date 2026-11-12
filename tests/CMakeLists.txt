# Core tests: a zero-dependency harness (tests/harness.h).
file(GLOB FLO_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
# The calculating mascot is pure and needs no toolkit.
add_executable(flo-tests harness.cpp ${FLO_TEST_SOURCES} ${PROJECT_SOURCE_DIR}/src/ui/omi.cpp)
target_link_libraries(flo-tests PRIVATE flo-thumbgen flo-core)
target_compile_definitions(flo-tests PRIVATE FLO_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
add_dependencies(flo-tests flo-thumbnailer flo)

# Every run gets a fresh private 0700 directory under the build tree with
# its own TMPDIR, HOME and XDG_* (never deleted); see run-isolated.sh.
add_test(NAME flo-core-tests
         COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run-isolated.sh ${CMAKE_BINARY_DIR}/test-runs
                 $<TARGET_FILE:flo-tests>)
set_tests_properties(flo-core-tests PROPERTIES
  ENVIRONMENT "FLO_THUMBNAILER=$<TARGET_FILE:flo-thumbnailer>;FLO_BIN=$<TARGET_FILE:flo>"
  TIMEOUT 300)
