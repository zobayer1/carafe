# Expand include/carafe/version.hpp.in -> build/generated/carafe/version.hpp
configure_file(
    "${PROJECT_SOURCE_DIR}/include/carafe/version.hpp.in"
    "${PROJECT_BINARY_DIR}/generated/carafe/version.hpp"
    @ONLY
)

# Headers are listed only so IDEs show them in the project tree.
set(CARAFE_HEADERS
    "${PROJECT_BINARY_DIR}/generated/carafe/version.hpp"
    "${PROJECT_SOURCE_DIR}/include/carafe/app.hpp"
    "${PROJECT_SOURCE_DIR}/include/carafe/http/headers.hpp"
    "${PROJECT_SOURCE_DIR}/include/carafe/http/request.hpp"
    "${PROJECT_SOURCE_DIR}/include/carafe/http/response.hpp"
    http/ascii.hpp
    http/line_reader.hpp
    http/request_parser.hpp
    http/request_reader.hpp
    net/listener.hpp
    net/socket.hpp
    server/connection.hpp
    server/serve.hpp
)

set(CARAFE_SOURCES
    app.cpp
    http/headers.cpp
    http/line_reader.cpp
    http/request_parser.cpp
    http/request_reader.cpp
    http/response.cpp
    net/listener.cpp
    net/socket.cpp
    server/connection.cpp
    server/serve.cpp
    version.cpp
)

add_library(carafe STATIC ${CARAFE_SOURCES} ${CARAFE_HEADERS})
add_library(carafe::carafe ALIAS carafe)

# BUILD_INTERFACE only: consumed from inside this build tree. Exporting would
# need install(TARGETS ... EXPORT) and an $<INSTALL_INTERFACE:include> entry.
target_include_directories(carafe
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<BUILD_INTERFACE:${PROJECT_BINARY_DIR}/generated>
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

target_compile_features(carafe PUBLIC cxx_std_17)

find_package(Threads REQUIRED)
target_link_libraries(carafe PUBLIC Threads::Threads)

# Warnings are PRIVATE, so every target asks for them itself. Sanitizer and
# coverage flags are PUBLIC and reach anything linking carafe::carafe.
carafe_target_warnings(carafe)
carafe_target_sanitizers(carafe)
carafe_target_coverage(carafe)
