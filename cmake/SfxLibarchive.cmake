include(ExternalProject)

if(CMAKE_VERSION VERSION_LESS 3.18)
    message(FATAL_ERROR "The pinned archive source build requires CMake 3.18 or newer")
endif()

set(SFX_ZLIB_VERSION "1.3.2")
set(SFX_ZLIB_SHA256 "bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16")
set(SFX_ZLIB_ARCHIVE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/zlib/${SFX_ZLIB_VERSION}/artifacts/zlib-${SFX_ZLIB_VERSION}.tar.gz")
set(SFX_LIBARCHIVE_VERSION "3.8.9")
set(SFX_LIBARCHIVE_SHA256 "7b20200203d08866b62373dd51417798427ec67852928d71f142bfe578e9ea75")
set(SFX_LIBARCHIVE_ARCHIVE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libarchive/${SFX_LIBARCHIVE_VERSION}/artifacts/libarchive-${SFX_LIBARCHIVE_VERSION}.zip")

foreach(component IN ITEMS ZLIB LIBARCHIVE)
    if(NOT EXISTS "${SFX_${component}_ARCHIVE}")
        message(FATAL_ERROR "Pinned ${component} source archive is missing")
    endif()
    file(SHA256 "${SFX_${component}_ARCHIVE}" actual_sha256)
    if(NOT actual_sha256 STREQUAL SFX_${component}_SHA256)
        message(FATAL_ERROR
            "Pinned ${component} source hash mismatch: ${actual_sha256}")
    endif()
endforeach()

set(SFX_ARCHIVE_SOURCE_ROOT "${CMAKE_CURRENT_BINARY_DIR}/third-party/archive-sources")
set(SFX_ZLIB_SOURCE
    "${SFX_ARCHIVE_SOURCE_ROOT}/zlib/zlib-${SFX_ZLIB_VERSION}")
set(SFX_LIBARCHIVE_SOURCE
    "${SFX_ARCHIVE_SOURCE_ROOT}/libarchive/libarchive-${SFX_LIBARCHIVE_VERSION}")

function(sfx_extract_archive_once archive expected_marker source_parent)
    if(NOT EXISTS "${expected_marker}")
        file(REMOVE_RECURSE "${source_parent}")
        file(MAKE_DIRECTORY "${source_parent}")
        file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${source_parent}")
    endif()
    if(NOT EXISTS "${expected_marker}")
        message(FATAL_ERROR "Pinned source extraction did not produce ${expected_marker}")
    endif()
endfunction()

sfx_extract_archive_once(
    "${SFX_ZLIB_ARCHIVE}"
    "${SFX_ZLIB_SOURCE}/CMakeLists.txt"
    "${SFX_ARCHIVE_SOURCE_ROOT}/zlib")
sfx_extract_archive_once(
    "${SFX_LIBARCHIVE_ARCHIVE}"
    "${SFX_LIBARCHIVE_SOURCE}/CMakeLists.txt"
    "${SFX_ARCHIVE_SOURCE_ROOT}/libarchive")

set(SFX_ZLIB_BUILD "${CMAKE_CURRENT_BINARY_DIR}/third-party/zlib-build")
set(SFX_ZLIB_INSTALL "${CMAKE_CURRENT_BINARY_DIR}/third-party/zlib-install")
set(SFX_LIBARCHIVE_BUILD "${CMAKE_CURRENT_BINARY_DIR}/third-party/libarchive-build")
set(SFX_LIBARCHIVE_INSTALL "${CMAKE_CURRENT_BINARY_DIR}/third-party/libarchive-install")
file(MAKE_DIRECTORY "${SFX_ZLIB_INSTALL}/include")
file(MAKE_DIRECTORY "${SFX_LIBARCHIVE_INSTALL}/include")
if(MSVC)
    set(SFX_ZLIB_LIBRARY "${SFX_ZLIB_INSTALL}/lib/zs.lib")
    set(SFX_LIBARCHIVE_LIBRARY "${SFX_LIBARCHIVE_INSTALL}/lib/archive.lib")
else()
    set(SFX_ZLIB_LIBRARY "${SFX_ZLIB_INSTALL}/lib/libz.a")
    set(SFX_LIBARCHIVE_LIBRARY "${SFX_LIBARCHIVE_INSTALL}/lib/libarchive.a")
endif()

set(SFX_ARCHIVE_COMMON_CMAKE_ARGS
    "-DCMAKE_BUILD_TYPE=Release"
    "-DCMAKE_C_COMPILER:FILEPATH=${CMAKE_C_COMPILER}"
    "-DCMAKE_CXX_COMPILER:FILEPATH=${CMAKE_CXX_COMPILER}"
    "-DCMAKE_POSITION_INDEPENDENT_CODE=OFF")
if(MSVC)
    list(APPEND SFX_ARCHIVE_COMMON_CMAKE_ARGS
        "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded"
        "-DCMAKE_C_FLAGS=/D_WIN32_WINNT=0x0601 /DWINVER=0x0601"
        "-DCMAKE_SYSTEM_VERSION=${CMAKE_SYSTEM_VERSION}")
endif()

set(SFX_ARCHIVE_ENABLE_ICONV OFF)
if(NOT WIN32)
    # POSIX needs iconv for the archive engine's explicit UTF-8 ZIP headers.
    # Windows keeps libarchive's native conversion and its existing static inputs.
    find_package(Iconv REQUIRED)
    set(SFX_ARCHIVE_ENABLE_ICONV ON)
endif()

ExternalProject_Add(sfx_zlib_external
    SOURCE_DIR "${SFX_ZLIB_SOURCE}"
    BINARY_DIR "${SFX_ZLIB_BUILD}"
    INSTALL_DIR "${SFX_ZLIB_INSTALL}"
    CMAKE_ARGS
        ${SFX_ARCHIVE_COMMON_CMAKE_ARGS}
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        "-DCMAKE_INSTALL_LIBDIR=lib"
        "-DZLIB_BUILD_SHARED=OFF"
        "-DZLIB_BUILD_STATIC=ON"
        "-DZLIB_BUILD_TESTING=OFF"
        "-DZLIB_INSTALL=ON"
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
        --target zlibstatic
    # Keep installation in the producing step so CMake 3.18 can declare the
    # imported archive as a build byproduct for Ninja as well as MSBuild.
    COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
        --target install
    BUILD_BYPRODUCTS "${SFX_ZLIB_LIBRARY}"
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
)

ExternalProject_Add(sfx_libarchive_external
    SOURCE_DIR "${SFX_LIBARCHIVE_SOURCE}"
    BINARY_DIR "${SFX_LIBARCHIVE_BUILD}"
    INSTALL_DIR "${SFX_LIBARCHIVE_INSTALL}"
    CMAKE_ARGS
        ${SFX_ARCHIVE_COMMON_CMAKE_ARGS}
        "-DCMAKE_INSTALL_PREFIX=<INSTALL_DIR>"
        "-DCMAKE_INSTALL_LIBDIR=lib"
        "-DCMAKE_PREFIX_PATH=${SFX_ZLIB_INSTALL}"
        "-DZLIB_ROOT=${SFX_ZLIB_INSTALL}"
        "-DZLIB_USE_STATIC_LIBS=ON"
        "-DZLIB_LIBRARY=${SFX_ZLIB_LIBRARY}"
        "-DBUILD_SHARED_LIBS=OFF"
        "-DENABLE_WERROR=OFF"
        "-DENABLE_MBEDTLS=OFF"
        "-DENABLE_NETTLE=OFF"
        "-DENABLE_OPENSSL=OFF"
        "-DENABLE_LIBB2=OFF"
        "-DENABLE_LZ4=OFF"
        "-DENABLE_LZO=OFF"
        "-DENABLE_LZMA=OFF"
        "-DENABLE_ZSTD=OFF"
        "-DENABLE_ZLIB=ON"
        "-DENABLE_BZip2=OFF"
        "-DENABLE_LIBXML2=OFF"
        "-DENABLE_EXPAT=OFF"
        "-DENABLE_WIN32_XMLLITE=OFF"
        "-DENABLE_PCREPOSIX=OFF"
        "-DENABLE_PCRE2POSIX=OFF"
        "-DENABLE_LIBGCC=OFF"
        "-DPOSIX_REGEX_LIB=NONE"
        "-DENABLE_CNG=OFF"
        "-DENABLE_TAR=OFF"
        "-DENABLE_CPIO=OFF"
        "-DENABLE_CAT=OFF"
        "-DENABLE_UNZIP=OFF"
        "-DENABLE_XATTR=OFF"
        "-DENABLE_ACL=OFF"
        "-DENABLE_ICONV=${SFX_ARCHIVE_ENABLE_ICONV}"
        "-DENABLE_TEST=OFF"
        "-DENABLE_COVERAGE=OFF"
        "-DENABLE_INSTALL=ON"
        "-DDONT_FAIL_ON_CRC_ERROR=OFF"
    BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
        --target archive_static
    COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --config Release
        --target install
    BUILD_BYPRODUCTS "${SFX_LIBARCHIVE_LIBRARY}"
    INSTALL_COMMAND ""
    UPDATE_COMMAND ""
    DEPENDS sfx_zlib_external
)

add_library(sfx_zlib STATIC IMPORTED GLOBAL)
set_target_properties(sfx_zlib PROPERTIES
    IMPORTED_LOCATION "${SFX_ZLIB_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SFX_ZLIB_INSTALL}/include")
add_dependencies(sfx_zlib sfx_zlib_external)

add_library(sfx_libarchive STATIC IMPORTED GLOBAL)
set_target_properties(sfx_libarchive PROPERTIES
    IMPORTED_LOCATION "${SFX_LIBARCHIVE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${SFX_LIBARCHIVE_INSTALL}/include"
    INTERFACE_COMPILE_DEFINITIONS LIBARCHIVE_STATIC
    INTERFACE_LINK_LIBRARIES sfx_zlib)
add_dependencies(sfx_libarchive sfx_libarchive_external)
if(WIN32)
    target_link_libraries(sfx_libarchive INTERFACE bcrypt)
else()
    target_link_libraries(sfx_libarchive INTERFACE Iconv::Iconv)
endif()
add_library(sfx::libarchive ALIAS sfx_libarchive)
