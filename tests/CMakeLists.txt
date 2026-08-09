include(FetchContent)

# Use a system-installed GoogleTest if one is present (find_package(GTest)),
# otherwise download and build the pinned release. FIND_PACKAGE_ARGS needs
# CMake >= 3.24.
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
    version_test.cpp
    http/request_parser_test.cpp
)

add_executable(carafe_tests ${CARAFE_TEST_SOURCES})

# Tests reach into src/ deliberately: the parser is internal, so it is not in
# include/ and does not arrive through carafe::carafe's PUBLIC include dirs.
target_include_directories(carafe_tests PRIVATE "${PROJECT_SOURCE_DIR}/src")

target_link_libraries(carafe_tests
    PRIVATE
        carafe::carafe
        GTest::gtest
        GTest::gmock
        GTest::gtest_main
)

# Only warnings: they are PRIVATE on each target and do not propagate. The
# sanitizer and coverage flags arrive through carafe::carafe, which carries
# them PUBLICly -- applying them again here would just duplicate the flags.
carafe_target_warnings(carafe_tests)

# Register every TEST() with CTest so `ctest` reports them individually.
gtest_discover_tests(carafe_tests
    DISCOVERY_MODE PRE_TEST
    PROPERTIES LABELS unit
)
