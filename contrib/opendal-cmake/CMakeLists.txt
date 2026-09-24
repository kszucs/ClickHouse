option(ENABLE_OPENDAL "Enable a generic IObjectStorage backend on top of Apache OpenDAL (supports many services, including the Hugging Face Hub with Xet chunk dedup)" 0)

if (NOT ENABLE_OPENDAL)
    message(STATUS "Not using OpenDAL")
    return()
endif()

set(OPENDAL_SOURCE_DIR "${ClickHouse_SOURCE_DIR}/contrib/opendal/bindings/cpp")
set(OPENDAL_CARGO_TARGET_DIR "${CMAKE_CURRENT_BINARY_DIR}/target")
set(OPENDAL_BRIDGE_DIR "${OPENDAL_CARGO_TARGET_DIR}/cxxbridge")

# Features of the `opendal` crate. Other services (s3, gcs, azblob, ...) are enabled the same way.
# "memory" and "fs" need no network, which keeps gtest_opendal_object_storage.cpp runnable anywhere.
set(OPENDAL_FEATURES "opendal/services-hf,opendal/services-memory,opendal/services-fs")

if (CMAKE_BUILD_TYPE_UC STREQUAL "DEBUG")
    set(OPENDAL_RUST_LIB "${OPENDAL_CARGO_TARGET_DIR}/debug/libopendal_cpp.a")
else()
    set(OPENDAL_CARGO_RELEASE_FLAG "--release")
    set(OPENDAL_RUST_LIB "${OPENDAL_CARGO_TARGET_DIR}/release/libopendal_cpp.a")
endif()

# The `cxx` crate compiles its C++ runtime (`cxx.cc`, which defines `rust::Error`) through the
# `cc` crate, so it must see the same libc++ as the rest of ClickHouse: our libc++ adds a stack
# trace to `std::exception`, which changes the layout of every class derived from it.
#
# The binding still builds with a plain `cargo build`, which downloads crates, rather than
# through corrosion and the vendored crates in contrib/rust_vendor.
add_custom_target(_opendal_cargo_build
    COMMAND ${CMAKE_COMMAND} -E env
        "CARGO_TARGET_DIR=${OPENDAL_CARGO_TARGET_DIR}"
        "CXX=${CMAKE_CXX_COMPILER}"
        "CXXFLAGS=${RUST_CXXFLAGS}"
        cargo build --manifest-path "${OPENDAL_SOURCE_DIR}/Cargo.toml" --features "${OPENDAL_FEATURES}" ${OPENDAL_CARGO_RELEASE_FLAG}
    BYPRODUCTS "${OPENDAL_BRIDGE_DIR}/opendal-cpp/src/lib.rs.cc" "${OPENDAL_BRIDGE_DIR}/opendal-cpp/src/lib.rs.h" "${OPENDAL_RUST_LIB}"
    USES_TERMINAL
    VERBATIM
    COMMENT "Building the OpenDAL C++ binding with cargo")

add_library(_opendal STATIC
    "${OPENDAL_SOURCE_DIR}/src/error.cpp"
    "${OPENDAL_SOURCE_DIR}/src/layer.cpp"
    "${OPENDAL_SOURCE_DIR}/src/lister.cpp"
    "${OPENDAL_SOURCE_DIR}/src/operator.cpp"
    "${OPENDAL_SOURCE_DIR}/src/reader.cpp"
    "${OPENDAL_SOURCE_DIR}/src/writer.cpp"
    "${OPENDAL_SOURCE_DIR}/src/utils/ffi_converter.cpp"
    "${OPENDAL_BRIDGE_DIR}/opendal-cpp/src/lib.rs.cc")
add_dependencies(_opendal _opendal_cargo_build)

target_include_directories(_opendal SYSTEM PUBLIC "${OPENDAL_SOURCE_DIR}/include")
target_include_directories(_opendal SYSTEM PRIVATE
    "${OPENDAL_SOURCE_DIR}/src"
    "${OPENDAL_BRIDGE_DIR}"
    "${OPENDAL_BRIDGE_DIR}/opendal-cpp/src")
target_link_libraries(_opendal PUBLIC "${OPENDAL_RUST_LIB}")

if (OS_DARWIN)
    # Needed by the Rust side for TLS/keychain access, proxy detection, and system info
    # (security-framework, system-configuration, sysinfo crates) on macOS.
    target_link_libraries(_opendal PUBLIC
        "-framework CoreFoundation"
        "-framework Security"
        "-framework SystemConfiguration"
        "-framework IOKit")
endif()

add_library(ch_contrib::opendal ALIAS _opendal)
