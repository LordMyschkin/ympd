cmake_minimum_required(VERSION 3.16)

# Build the upstream sources and their pinned dependencies in the same CMake
# tree as ympd. The vendored directories are ordinary source, not submodules.
set(ABSL_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ABSL_BUILD_TEST_HELPERS OFF CACHE BOOL "" FORCE)
set(ABSL_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_INSTALL OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(subprojects/absl EXCLUDE_FROM_ALL)
add_subdirectory(subprojects/yaml-cpp EXCLUDE_FROM_ALL)

set(ASHUFFLE_VERSION "v3.14.10")
set(VERSION "${ASHUFFLE_VERSION}")
configure_file(src/version.cc.in "${CMAKE_CURRENT_BINARY_DIR}/version.cc" @ONLY)

add_executable(ashuffle
    src/args.cc
    src/ashuffle.cc
    src/getpass.cc
    src/load.cc
    src/log.cc
    src/rule.cc
    src/shuffle.cc
    src/mpd_client.cc
    src/main.cc
    "${CMAKE_CURRENT_BINARY_DIR}/version.cc"
)
target_compile_features(ashuffle PRIVATE cxx_std_17)
target_include_directories(ashuffle PRIVATE src "${LIBMPDCLIENT_INCLUDE_DIR}")
target_link_libraries(ashuffle PRIVATE
    "${LIBMPDCLIENT_LIBRARY}"
    yaml-cpp::yaml-cpp
    absl::hash
    absl::status
    absl::statusor
    absl::str_format
    absl::strings
    absl::time
)
set_target_properties(ashuffle PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)
