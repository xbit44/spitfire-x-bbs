function(sfx_configure_portable_target target_name)
    if("cxx_std_17" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
        target_compile_features(${target_name} PUBLIC cxx_std_17)
    else()
        # Some locked-down MSVC/Ninja hosts cannot complete CMake's feature
        # probe even though the selected v143 compiler is known and checked.
        # Preserve the same C++17 requirement through target properties.
        set_target_properties(${target_name} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED YES
            CXX_EXTENSIONS NO)
    endif()
    if(MSVC)
        target_compile_options(${target_name} PRIVATE
            /W4 /WX /permissive- /EHsc /utf-8 /Zc:__cplusplus /sdl /Brepro
        )
    else()
        target_compile_options(${target_name} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
        )
    endif()
endfunction()

function(sfx_configure_test_executable target_name)
    sfx_configure_portable_target(${target_name})
    if(MSVC)
        target_compile_definitions(${target_name} PRIVATE
            _WIN32_WINNT=0x0601
            WINVER=0x0601
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
        )
        target_link_options(${target_name} PRIVATE
            /SUBSYSTEM:CONSOLE,6.01
            /DYNAMICBASE
            /NXCOMPAT
            /Brepro
        )
    endif()
endfunction()

function(sfx_configure_win32_product target_name)
    sfx_configure_portable_target(${target_name})
    if(MSVC)
        target_compile_definitions(${target_name} PRIVATE
            _WIN32_WINNT=0x0601
            WINVER=0x0601
            WIN32_LEAN_AND_MEAN
            NOMINMAX
            UNICODE
            _UNICODE
        )
        target_link_options(${target_name} PRIVATE
            /SUBSYSTEM:WINDOWS,6.01
            /DYNAMICBASE
            /NXCOMPAT
            /Brepro
        )
    endif()
endfunction()
