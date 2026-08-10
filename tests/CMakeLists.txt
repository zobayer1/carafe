include(FetchContent)

# Prefer a system GoogleTest, otherwise fetch the pinned release.
# FIND_PACKAGE_ARGS needs CMake >= 3.24.
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.17.0
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS NAMES GTest
)

# Keep GoogleTest out of our install rules and, on Windows, matched to our CRT.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

include(GoogleTest)

set(CARAFE_TEST_SOURCES
    http/headers_test.cpp
    http/line_reader_test.cpp
    http/request_parser_test.cpp
    version_test.cpp
)

add_executable(carafe_tests ${CARAFE_TEST_SOURCES})

# Tests reach into src/ deliberately: internal headers are not in include/ and
# do not arrive through carafe::carafe's PUBLIC include dirs.
target_include_directories(carafe_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(carafe_tests
    PRIVATE
        carafe::carafe
        GTest::gtest
        GTest::gmock
        GTest::gtest_main
)

# Warnings only: sanitizer and coverage flags already arrive PUBLICly through
# carafe::carafe, and applying them again would just duplicate the flags.
carafe_target_warnings(carafe_tests)

# Register every TEST() with CTest so `ctest` reports them individually.
gtest_discover_tests(carafe_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit
)
