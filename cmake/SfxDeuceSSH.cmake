# Private, offline, hash-pinned SSH dependency. The upstream archives stay exact;
# generated source/headers and compiler output live only in the build tree.
if(CMAKE_VERSION VERSION_LESS 3.22)
    message(FATAL_ERROR "The Windows SSH dependency requires CMake 3.22 or newer")
endif()
if(NOT MSVC OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "The Alpha SSH host requires the approved MSVC Win32 toolchain")
endif()
include("${CMAKE_CURRENT_LIST_DIR}/SfxSshSources.cmake")

find_program(SFX_OPENSSL_PERL_EXECUTABLE perl.exe
    HINTS "${CMAKE_SOURCE_DIR}/build/alpha-ssh-tools/perl-5.42.2.1/perl/bin"
    REQUIRED)
find_program(SFX_OPENSSL_NASM_EXECUTABLE nasm.exe
    HINTS "${CMAKE_SOURCE_DIR}/build/alpha-ssh-tools/nasm-3.02/nasm-3.02"
    REQUIRED)
find_program(SFX_POWERSHELL_EXECUTABLE NAMES pwsh.exe powershell.exe REQUIRED)
set(SFX_OPENSSL_BUILD_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/BuildSfxOpenSSL.ps1")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${SFX_OPENSSL_BUILD_SCRIPT}" "${SFX_OPENSSL_ARCHIVE}" "${SFX_DEUCESSH_ARCHIVE}")
file(SHA256 "${SFX_OPENSSL_BUILD_SCRIPT}" SFX_OPENSSL_SCRIPT_SHA256)
string(SHA256 SFX_OPENSSL_BUILD_KEY "${SFX_OPENSSL_SCRIPT_SHA256}:${SFX_OPENSSL_SHA256}")
string(SUBSTRING "${SFX_OPENSSL_BUILD_KEY}" 0 12 SFX_OPENSSL_BUILD_KEY)
# OpenSSL's no-makedepend build cannot infer header/option changes. A changed
# recipe gets an entirely new output/SDK directory, never stale object reuse.
set(SFX_OPENSSL_BUILD "${SFX_SSH_THIRD_PARTY}/openssl-build-x86-${SFX_OPENSSL_BUILD_KEY}")
set(SFX_OPENSSL_SDK "${SFX_SSH_THIRD_PARTY}/openssl-sdk-${SFX_OPENSSL_BUILD_KEY}")
set(SFX_OPENSSL_COMMAND "${SFX_POWERSHELL_EXECUTABLE}" -NoProfile -NonInteractive
    -ExecutionPolicy Bypass -File "${SFX_OPENSSL_BUILD_SCRIPT}"
    -SourceDirectory "${SFX_OPENSSL_SOURCE}" -BuildDirectory "${SFX_OPENSSL_BUILD}"
    -PerlExecutable "${SFX_OPENSSL_PERL_EXECUTABLE}"
    -NasmExecutable "${SFX_OPENSSL_NASM_EXECUTABLE}"
    -VisualStudioDirectory "${CMAKE_GENERATOR_INSTANCE}")
set(SFX_OPENSSL_STAMP "${SFX_OPENSSL_BUILD}/sfx-configured-${SFX_OPENSSL_SCRIPT_SHA256}")
if(NOT EXISTS "${SFX_OPENSSL_STAMP}")
    execute_process(COMMAND ${SFX_OPENSSL_COMMAND} -Mode Configure
        RESULT_VARIABLE SFX_OPENSSL_CONFIGURE_RESULT)
    if(NOT SFX_OPENSSL_CONFIGURE_RESULT EQUAL 0)
        message(FATAL_ERROR "Pinned OpenSSL configuration failed; inspect ${SFX_OPENSSL_BUILD}")
    endif()
    file(COPY "${SFX_OPENSSL_SOURCE}/include/openssl" DESTINATION "${SFX_OPENSSL_SDK}/include"
        FILES_MATCHING PATTERN "*.h")
    file(COPY "${SFX_OPENSSL_BUILD}/include/openssl" DESTINATION "${SFX_OPENSSL_SDK}/include"
        FILES_MATCHING PATTERN "*.h")
    file(WRITE "${SFX_OPENSSL_STAMP}" "${SFX_OPENSSL_SHA256}\n")
endif()
add_custom_command(OUTPUT "${SFX_OPENSSL_BUILD}/libcrypto.lib" "${SFX_OPENSSL_BUILD}/libssl.lib"
    COMMAND ${SFX_OPENSSL_COMMAND} -Mode Build
    DEPENDS "${SFX_OPENSSL_STAMP}" "${SFX_OPENSSL_ARCHIVE}" "${SFX_OPENSSL_BUILD_SCRIPT}"
    COMMENT "Building pinned OpenSSL 3.5.9 private static crypto/TLS (MSVC Win32 /MT)"
    VERBATIM)
add_custom_target(sfx_openssl_crypto_build DEPENDS "${SFX_OPENSSL_BUILD}/libcrypto.lib" "${SFX_OPENSSL_BUILD}/libssl.lib")
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OPENSSL_MSVC_STATIC_RT TRUE)
set(OPENSSL_INCLUDE_DIR "${SFX_OPENSSL_SDK}/include" CACHE PATH "Pinned private OpenSSL headers" FORCE)
set(LIB_EAY_RELEASE "${SFX_OPENSSL_BUILD}/libcrypto.lib" CACHE FILEPATH "Pinned private libcrypto" FORCE)
set(LIB_EAY_DEBUG "${SFX_OPENSSL_BUILD}/libcrypto.lib" CACHE FILEPATH "Pinned private libcrypto" FORCE)
# FindOpenSSL does not create an imported target until its library exists.
# Our library is a build-time output, so declare that exact output up front.
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${SFX_OPENSSL_BUILD}/libcrypto.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${SFX_OPENSSL_SDK}/include")
add_dependencies(OpenSSL::Crypto sfx_openssl_crypto_build)
set(SSL_EAY_RELEASE "${SFX_OPENSSL_BUILD}/libssl.lib" CACHE FILEPATH "Pinned private libssl" FORCE)
set(SSL_EAY_DEBUG "${SFX_OPENSSL_BUILD}/libssl.lib" CACHE FILEPATH "Pinned private libssl" FORCE)
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${SFX_OPENSSL_BUILD}/libssl.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${SFX_OPENSSL_SDK}/include"
    INTERFACE_LINK_LIBRARIES "OpenSSL::Crypto;ws2_32;crypt32")
add_dependencies(OpenSSL::SSL sfx_openssl_crypto_build)
find_package(OpenSSL 3.5.9 EXACT REQUIRED)
set(DEUCESSH_CRYPTO_BACKEND OpenSSL CACHE STRING "SFX SSH backend" FORCE)
set(DEUCESSH_BUILD_STATIC ON CACHE BOOL "SFX static SSH library" FORCE)
set(DEUCESSH_BUILD_SHARED OFF CACHE BOOL "No SSH DLL boundary" FORCE)
set(DEUCESSH_BUILD_TESTS OFF CACHE BOOL "Upstream tests qualified separately" FORCE)
add_subdirectory("${SFX_DEUCESSH_OVERLAY}" "${SFX_SSH_THIRD_PARTY}/deucessh" EXCLUDE_FROM_ALL)
add_dependencies(deucessh_static sfx_openssl_crypto_build)
target_compile_definitions(deucessh_static PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(deucessh_static INTERFACE OpenSSL::Crypto ws2_32 advapi32 crypt32 user32 gdi32)
add_executable(sfx_ssh_bounds_tests EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/test/platform/ssh_bounds_tests.c")
set_property(TARGET sfx_ssh_bounds_tests PROPERTY C_STANDARD 17)
set_property(TARGET sfx_ssh_bounds_tests PROPERTY C_STANDARD_REQUIRED ON)
target_include_directories(sfx_ssh_bounds_tests BEFORE PRIVATE "${SFX_DEUCESSH_OVERLAY}/win32" "${SFX_DEUCESSH_OVERLAY}" "${SFX_OPENSSL_SDK}/include")
target_compile_options(sfx_ssh_bounds_tests PRIVATE /experimental:c11atomics)
target_compile_definitions(sfx_ssh_bounds_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
target_link_libraries(sfx_ssh_bounds_tests PRIVATE deucessh_static)
