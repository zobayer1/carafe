# 3.24 for FetchContent's FIND_PACKAGE_ARGS, 3.25 for CMakePresets schema v6.
cmake_minimum_required(VERSION 3.25)

project(carafe
    VERSION 0.1.0
    DESCRIPTION "A small HTTP microframework for C++"
    HOMEPAGE_URL "https://github.com/zobayer1/carafe"
    LANGUAGES CXX
)

# ---------------------------------------------------------------------------
# Guardrails
# ---------------------------------------------------------------------------
if(PROJECT_SOURCE_DIR STREQUAL PROJECT_BINARY_DIR)
    message(FATAL_ERROR
        "In-source builds are not allowed. Run: cmake -B build -S .")
endif()

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

include(CompilerWarnings)
include(Sanitizers)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(CARAFE_BUILD_TESTS      "Build the unit tests"              ${PROJECT_IS_TOP_LEVEL})
option(CARAFE_BUILD_EXAMPLES   "Build the example programs"        ${PROJECT_IS_TOP_LEVEL})
option(CARAFE_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(CARAFE_ENABLE_SANITIZERS  "Enable AddressSanitizer + UBSan"   OFF)
option(CARAFE_ENABLE_COVERAGE    "Enable gcov coverage instrumentation" OFF)

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_EXTENSIONS OFF)            # -std=c++17, never -std=gnu++17
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # for clangd / clang-tidy
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Single-config generators (Make, Ninja) have no build type by default.
get_property(_is_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _is_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY
        STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Put every binary in one predictable place.
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")

# Use ccache when available; it makes rebuilds dramatically faster.
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache found: ${CCACHE_PROGRAM}")
endif()

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(src)

if(CARAFE_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

if(CARAFE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Configuration summary
# ---------------------------------------------------------------------------
if(PROJECT_IS_TOP_LEVEL)
    message(STATUS "")
    message(STATUS "${PROJECT_NAME} ${PROJECT_VERSION}")
    message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
    message(STATUS "  compiler ........... ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
    message(STATUS "  tests .............. ${CARAFE_BUILD_TESTS}")
    message(STATUS "  examples ........... ${CARAFE_BUILD_EXAMPLES}")
    message(STATUS "  warnings as errors . ${CARAFE_WARNINGS_AS_ERRORS}")
    message(STATUS "  sanitizers ......... ${CARAFE_ENABLE_SANITIZERS}")
    message(STATUS "  coverage ........... ${CARAFE_ENABLE_COVERAGE}")
    message(STATUS "")
endif()
