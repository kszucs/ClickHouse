option(ENABLE_OPENDAL "Enable a generic IObjectStorage backend on top of Apache OpenDAL (supports many services, including the Hugging Face Hub with Xet chunk dedup)" 0)

if (NOT ENABLE_OPENDAL)
    message(STATUS "Not using OpenDAL")
    return()
endif()

set(OPENDAL_SOURCE_DIR "${ClickHouse_SOURCE_DIR}/contrib/opendal")

# Other OpenDAL services (s3, gcs, azblob, ...) can be added the same way by extending
# this list, provided the C++ binding forwards a matching feature to the core crate.
#
# "memory" and "fs" are always on: they are pure Rust with no network dependencies, and
# gtest_opendal_object_storage.cpp runs the IObjectStorage contract against both, which
# is what keeps that suite runnable anywhere.
set(OPENDAL_FEATURES "services-hf,services-memory,services-fs" CACHE STRING "" FORCE)
set(OPENDAL_ENABLE_TESTING OFF CACHE BOOL "" FORCE)

add_subdirectory("${OPENDAL_SOURCE_DIR}/bindings/cpp" "${ClickHouse_BINARY_DIR}/contrib/opendal-cpp")

# Treat the vendored binding's headers as system includes (like every other contrib
# dependency) so its own warnings don't get promoted to errors by our -Werror build.
get_target_property(_opendal_cpp_includes opendal_cpp INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(opendal_cpp PROPERTIES INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_opendal_cpp_includes}")

if (OS_DARWIN)
    # The vendored binding's own CMakeLists.txt only adds these frameworks to its own test
    # binary, not to the opendal_cpp library itself - needed here for the Rust side's
    # TLS/keychain access, proxy detection, and system info (security-framework,
    # system-configuration, sysinfo crates) on macOS.
    target_link_libraries(opendal_cpp INTERFACE
        "-framework CoreFoundation"
        "-framework Security"
        "-framework SystemConfiguration"
        "-framework IOKit")
endif()

add_library(ch_contrib::opendal ALIAS opendal_cpp)
