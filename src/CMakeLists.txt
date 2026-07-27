# Expand include/carafe/version.hpp.in -> build/generated/carafe/version.hpp
configure_file(
    "${PROJECT_SOURCE_DIR}/include/carafe/version.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/carafe/version.hpp"
    @ONLY
)

# Headers are listed only so IDEs show them in the project tree.
set(CARAFE_HEADERS
    "${PROJECT_BINARY_DIR}/generated/carafe/version.hpp"
)

set(CARAFE_SOURCES
    version.cpp
)

add_library(carafe STATIC ${CARAFE_SOURCES} ${CARAFE_HEADERS})
add_library(carafe::carafe ALIAS carafe)

# BUILD_INTERFACE only: the target is consumed from inside this build tree.
# Installing the library for other projects to find_package() would need
# install(TARGETS ... EXPORT) rules and an $<INSTALL_INTERFACE:include> entry
# here -- neither exists yet, and nothing in this repo needs them.
target_include_directories(carafe
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
)

target_compile_features(carafe PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(carafe PUBLIC Threads::Threads)

# Warnings are PRIVATE, so every target has to ask for them itself. Sanitizer
# and coverage flags are PUBLIC and reach anything that links carafe::carafe --
# see cmake/Sanitizers.cmake.
carafe_target_warnings(carafe)
carafe_target_sanitizers(carafe)
carafe_target_coverage(carafe)
