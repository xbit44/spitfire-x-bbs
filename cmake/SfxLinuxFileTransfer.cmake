# Separate GPL helper. No SFX library links; keep corresponding source and
# inherited notices with its distribution companion.
set(sfx_file_helper_root "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sfx_file_transfer_linux/20261108")
set(sfx_file_helper_dirs src/sbbs3 src/xpdev src/hash src/smblib)
set(sfx_file_helper_names sfx_linux_main xmodem zmodem nopen date_str telnet
    crc16 crc32 datewrap dirwrap filewrap genwrap ini_file netwrap os_info sockwrap
    str_list threadwrap xpdatetime xpprintf)
set(sfx_file_helper_sources)
foreach(name IN LISTS sfx_file_helper_names)
    set(found FALSE)
    foreach(dir IN LISTS sfx_file_helper_dirs)
        if(EXISTS "${sfx_file_helper_root}/source/${dir}/${name}.c")
            list(APPEND sfx_file_helper_sources "${sfx_file_helper_root}/source/${dir}/${name}.c")
            set(found TRUE)
            break()
        endif()
    endforeach()
    if(NOT found)
        message(FATAL_ERROR "Pinned Linux file helper source missing: ${name}")
    endif()
endforeach()
add_executable(SFXFileTransfer ${sfx_file_helper_sources})
target_include_directories(SFXFileTransfer PRIVATE "${sfx_file_helper_root}")
foreach(dir IN LISTS sfx_file_helper_dirs)
    target_include_directories(SFXFileTransfer PRIVATE "${sfx_file_helper_root}/source/${dir}")
endforeach()
target_compile_definitions(SFXFileTransfer PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
    NEEDS_STRLCPY NEEDS_STRLCAT HAS_INTTYPES_H HAS_STDINT_H NDEBUG)
target_compile_features(SFXFileTransfer PRIVATE c_std_11)
target_compile_options(SFXFileTransfer PRIVATE -Werror=implicit-function-declaration
    -Werror=return-type -ffunction-sections -fdata-sections)
target_link_options(SFXFileTransfer PRIVATE -Wl,--gc-sections)
target_link_libraries(SFXFileTransfer PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)

# QWK exposes the accepted one-packet protocol separately. General-file mode,
# inherited payload capability rules and its pinned image remain unchanged.
set(sfx_qwk_helper_sources ${sfx_file_helper_sources})
list(REMOVE_ITEM sfx_qwk_helper_sources "${sfx_file_helper_root}/source/src/sbbs3/sfx_linux_main.c")
list(APPEND sfx_qwk_helper_sources "${sfx_file_helper_root}/source/src/sbbs3/sfx_linux_qwk_main.c")
add_executable(SFXQWKTransfer ${sfx_qwk_helper_sources})
target_include_directories(SFXQWKTransfer PRIVATE "${sfx_file_helper_root}")
foreach(dir IN LISTS sfx_file_helper_dirs)
    target_include_directories(SFXQWKTransfer PRIVATE "${sfx_file_helper_root}/source/${dir}")
endforeach()
target_compile_definitions(SFXQWKTransfer PRIVATE _GNU_SOURCE _FILE_OFFSET_BITS=64
    NEEDS_STRLCPY NEEDS_STRLCAT HAS_INTTYPES_H HAS_STDINT_H NDEBUG)
target_compile_features(SFXQWKTransfer PRIVATE c_std_11)
target_compile_options(SFXQWKTransfer PRIVATE -Werror=implicit-function-declaration
    -Werror=return-type -ffunction-sections -fdata-sections)
target_link_options(SFXQWKTransfer PRIVATE -Wl,--gc-sections)
target_link_libraries(SFXQWKTransfer PRIVATE Threads::Threads ${CMAKE_DL_LIBS} m)
