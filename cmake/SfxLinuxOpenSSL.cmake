# Private static Linux OpenSSL build recipe.
find_program(SFX_OPENSSL_PERL_EXECUTABLE NAMES perl REQUIRED)
find_program(SFX_OPENSSL_MAKE_EXECUTABLE NAMES gmake make REQUIRED)
file(SHA256 "${CMAKE_CURRENT_LIST_FILE}" SFX_OPENSSL_RECIPE_SHA256)
string(SHA256 SFX_OPENSSL_BUILD_KEY
    "${SFX_OPENSSL_RECIPE_SHA256}:${SFX_OPENSSL_SHA256}:${CMAKE_C_COMPILER}:${CMAKE_C_COMPILER_VERSION}")
string(SUBSTRING "${SFX_OPENSSL_BUILD_KEY}" 0 12 SFX_OPENSSL_BUILD_KEY)
set(SFX_OPENSSL_BUILD "${SFX_SSH_THIRD_PARTY}/openssl-linux-${SFX_OPENSSL_BUILD_KEY}")
file(MAKE_DIRECTORY "${SFX_OPENSSL_BUILD}")
set(SFX_OPENSSL_STAMP "${SFX_OPENSSL_BUILD}/sfx-configured")
if(NOT EXISTS "${SFX_OPENSSL_STAMP}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
            "${SFX_OPENSSL_PERL_EXECUTABLE}" "${SFX_OPENSSL_SOURCE}/Configure"
            linux-x86_64 no-shared no-tests no-module no-legacy
            "--prefix=${SFX_OPENSSL_BUILD}/sdk" "--openssldir=${SFX_OPENSSL_BUILD}/sdk/ssl"
        WORKING_DIRECTORY "${SFX_OPENSSL_BUILD}"
        RESULT_VARIABLE SFX_OPENSSL_CONFIGURE_RESULT
        OUTPUT_FILE "${SFX_OPENSSL_BUILD}/configure.log"
        ERROR_FILE "${SFX_OPENSSL_BUILD}/configure.stderr")
    if(NOT SFX_OPENSSL_CONFIGURE_RESULT EQUAL 0)
        message(FATAL_ERROR "Linux OpenSSL configure failed; inspect ${SFX_OPENSSL_BUILD}")
    endif()
    execute_process(COMMAND "${SFX_OPENSSL_MAKE_EXECUTABLE}" build_generated
        WORKING_DIRECTORY "${SFX_OPENSSL_BUILD}"
        RESULT_VARIABLE SFX_OPENSSL_HEADERS_RESULT
        OUTPUT_FILE "${SFX_OPENSSL_BUILD}/headers.log"
        ERROR_FILE "${SFX_OPENSSL_BUILD}/headers.stderr")
    if(NOT SFX_OPENSSL_HEADERS_RESULT EQUAL 0)
        message(FATAL_ERROR "Linux OpenSSL headers failed; inspect ${SFX_OPENSSL_BUILD}")
    endif()
    file(WRITE "${SFX_OPENSSL_STAMP}" "${SFX_OPENSSL_SHA256}\n")
endif()
add_custom_command(OUTPUT "${SFX_OPENSSL_BUILD}/libcrypto.a" "${SFX_OPENSSL_BUILD}/libssl.a"
    COMMAND "${SFX_OPENSSL_MAKE_EXECUTABLE}" -j4 build_libs
    WORKING_DIRECTORY "${SFX_OPENSSL_BUILD}"
    DEPENDS "${SFX_OPENSSL_STAMP}" "${SFX_OPENSSL_ARCHIVE}" "${CMAKE_CURRENT_LIST_FILE}"
    COMMENT "Building pinned private OpenSSL 3.5.9 for Linux AMD64"
    VERBATIM)
add_custom_target(sfx_openssl_crypto_build DEPENDS
    "${SFX_OPENSSL_BUILD}/libcrypto.a" "${SFX_OPENSSL_BUILD}/libssl.a")
set(SFX_OPENSSL_SDK "${SFX_OPENSSL_BUILD}/sdk")
file(MAKE_DIRECTORY "${SFX_OPENSSL_SDK}/include")
file(COPY "${SFX_OPENSSL_SOURCE}/include/openssl" DESTINATION "${SFX_OPENSSL_SDK}/include"
    FILES_MATCHING PATTERN "*.h")
file(COPY "${SFX_OPENSSL_BUILD}/include/openssl" DESTINATION "${SFX_OPENSSL_SDK}/include"
    FILES_MATCHING PATTERN "*.h")
set(OPENSSL_USE_STATIC_LIBS TRUE)
set(OPENSSL_INCLUDE_DIR "${SFX_OPENSSL_SDK}/include" CACHE PATH "Private OpenSSL headers" FORCE)
set(OPENSSL_CRYPTO_LIBRARY "${SFX_OPENSSL_BUILD}/libcrypto.a" CACHE FILEPATH "Private crypto" FORCE)
set(OPENSSL_SSL_LIBRARY "${SFX_OPENSSL_BUILD}/libssl.a" CACHE FILEPATH "Private TLS" FORCE)
add_library(OpenSSL::Crypto STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::Crypto PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_CRYPTO_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "Threads::Threads;${CMAKE_DL_LIBS}")
add_dependencies(OpenSSL::Crypto sfx_openssl_crypto_build)
add_library(OpenSSL::SSL STATIC IMPORTED GLOBAL)
set_target_properties(OpenSSL::SSL PROPERTIES
    IMPORTED_LOCATION "${OPENSSL_SSL_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${OPENSSL_INCLUDE_DIR}"
    INTERFACE_LINK_LIBRARIES "OpenSSL::Crypto")
add_dependencies(OpenSSL::SSL sfx_openssl_crypto_build)
