set(portable_roots
    "${SFX_SOURCE_ROOT}/src/core"
    "${SFX_SOURCE_ROOT}/src/application"
)

set(forbidden_patterns
    "[<\"]windows\\.h[>\"]"
    "[<\"]winsock2\\.h[>\"]"
    "\\b_WIN32\\b"
    "\\bHANDLE\\b"
    "\\bHWND\\b"
    "\\bDWORD\\b"
    "\\bSOCKET\\b"
)

foreach(portable_root IN LISTS portable_roots)
    file(GLOB_RECURSE portable_sources
        "${portable_root}/*.h"
        "${portable_root}/*.hpp"
        "${portable_root}/*.c"
        "${portable_root}/*.cpp"
    )
    foreach(source_file IN LISTS portable_sources)
        file(READ "${source_file}" source_text)
        foreach(pattern IN LISTS forbidden_patterns)
            if(source_text MATCHES "${pattern}")
                message(FATAL_ERROR "Portable boundary violation in ${source_file}: ${pattern}")
            endif()
        endforeach()
    endforeach()
endforeach()

message(STATUS "SFX portable boundary check passed")

