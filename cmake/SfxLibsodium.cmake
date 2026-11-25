set(SFX_LIBSODIUM_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/libsodium/1.0.22")
set(SFX_LIBSODIUM_INCLUDE "${SFX_LIBSODIUM_ROOT}/include")

if(MSVC)
    set(SFX_LIBSODIUM_MSVC_PROJECT
        "${SFX_LIBSODIUM_ROOT}/source/builds/msvc/vs2022/libsodium/libsodium.vcxproj")
    set(SFX_LIBSODIUM_MSVC_BUILD
        "${CMAKE_CURRENT_BINARY_DIR}/third-party/libsodium-1.0.22-msvc")
    set(SFX_LIBSODIUM_MSVC_LIBRARY
        "${SFX_LIBSODIUM_MSVC_BUILD}/lib/libsodium.lib")
    add_custom_command(
        OUTPUT "${SFX_LIBSODIUM_MSVC_LIBRARY}"
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "${SFX_LIBSODIUM_MSVC_BUILD}/lib"
            "${SFX_LIBSODIUM_MSVC_BUILD}/obj"
        COMMAND "${CMAKE_VS_MSBUILD_COMMAND}" "${SFX_LIBSODIUM_MSVC_PROJECT}"
            /nologo
            /m
            /t:Build
            /p:Configuration=ReleaseLIB
            /p:Platform=Win32
            /p:PlatformToolset=v143
            /p:OutDir=${SFX_LIBSODIUM_MSVC_BUILD}/lib/
            /p:IntDir=${SFX_LIBSODIUM_MSVC_BUILD}/obj/
            /p:WindowsTargetPlatformVersion=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}
        DEPENDS
            "${SFX_LIBSODIUM_ROOT}/artifacts/libsodium-1.0.22.tar.gz"
            "${SFX_LIBSODIUM_MSVC_PROJECT}"
            "${CMAKE_CURRENT_LIST_FILE}"
        COMMENT "Building pinned libsodium 1.0.22 from unmodified source (v143 Win32 static /MT)"
        VERBATIM
    )
    add_custom_target(sfx_libsodium_external DEPENDS "${SFX_LIBSODIUM_MSVC_LIBRARY}")
    add_library(sfx_libsodium STATIC IMPORTED GLOBAL)
    set_target_properties(sfx_libsodium PROPERTIES
        IMPORTED_LOCATION "${SFX_LIBSODIUM_MSVC_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${SFX_LIBSODIUM_INCLUDE}"
        INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC
    )
    add_dependencies(sfx_libsodium sfx_libsodium_external)
    target_link_libraries(sfx_libsodium INTERFACE advapi32)
else()
    include(ExternalProject)
    find_program(SFX_MAKE_EXECUTABLE make REQUIRED)
    set(SFX_LIBSODIUM_BUILD "${CMAKE_CURRENT_BINARY_DIR}/third-party/libsodium-1.0.22")
    ExternalProject_Add(sfx_libsodium_external
        SOURCE_DIR "${SFX_LIBSODIUM_ROOT}/source"
        BINARY_DIR "${SFX_LIBSODIUM_BUILD}"
        CONFIGURE_COMMAND
            ${CMAKE_COMMAND} -E env "CC=${CMAKE_C_COMPILER}"
            "${SFX_LIBSODIUM_ROOT}/source/configure"
            --disable-shared
            --enable-static
            --enable-minimal
            --disable-dependency-tracking
        BUILD_COMMAND ${SFX_MAKE_EXECUTABLE} -j2
        INSTALL_COMMAND ""
        UPDATE_COMMAND ""
        BUILD_BYPRODUCTS "${SFX_LIBSODIUM_BUILD}/src/libsodium/.libs/libsodium.a"
    )
    add_library(sfx_libsodium STATIC IMPORTED GLOBAL)
    set_target_properties(sfx_libsodium PROPERTIES
        IMPORTED_LOCATION "${SFX_LIBSODIUM_BUILD}/src/libsodium/.libs/libsodium.a"
        INTERFACE_INCLUDE_DIRECTORIES "${SFX_LIBSODIUM_INCLUDE}"
        INTERFACE_COMPILE_DEFINITIONS SODIUM_STATIC
    )
    add_dependencies(sfx_libsodium sfx_libsodium_external)
endif()

add_library(sfx::libsodium ALIAS sfx_libsodium)
