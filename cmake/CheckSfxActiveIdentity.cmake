if(NOT DEFINED SFX_SOURCE_ROOT)
    message(FATAL_ERROR "SFX_SOURCE_ROOT is required")
endif()

string(CONCAT retired_upper "S" "F" "N" "G")
string(CONCAT retired_lower "s" "f" "n" "g")

file(GLOB_RECURSE active_files LIST_DIRECTORIES false
    "${SFX_SOURCE_ROOT}/src/*"
    "${SFX_SOURCE_ROOT}/test/*"
    "${SFX_SOURCE_ROOT}/cmake/*"
)
list(APPEND active_files "${SFX_SOURCE_ROOT}/CMakeLists.txt")

set(violations "")
foreach(active_file IN LISTS active_files)
    if(active_file STREQUAL "${CMAKE_CURRENT_LIST_FILE}")
        continue()
    endif()
    if(active_file MATCHES "/test/conformance/fixtures/ogsf/")
        continue()
    endif()
    if(NOT active_file MATCHES "\\.(c|cc|cpp|cxx|h|hh|hpp|hxx|cmake|md|ps1|cmd|txt)$" AND
       NOT active_file MATCHES "/CMakeLists\\.txt$")
        continue()
    endif()
    file(READ "${active_file}" content)
    if(content MATCHES "${retired_upper}|${retired_lower}")
        list(APPEND violations "${active_file}")
    endif()
endforeach()

set(retired_active_files
    "src/adapters/storage/src/sqlite_account_migrations.cpp"
    "src/adapters/storage/src/sqlite_authentication_migrations.cpp"
    "src/adapters/storage/src/sqlite_phase3a_migrations.cpp"
    "src/adapters/storage/src/sqlite_r5_transition.cpp"
    "src/adapters/storage/src/sqlite_r5_transition.hpp"
    "src/adapters/storage/src/sqlite_r5_transition_runtime.cpp"
    "src/adapters/storage/src/sqlite_r5_transition_runtime.hpp"
    "test/integration/r5_transition_tests.cpp"
    "test/platform/win32_r5_transition_tests.cpp"
    "cmake/CheckR5ActiveSql.cmake"
)
foreach(relative_file IN LISTS retired_active_files)
    if(EXISTS "${SFX_SOURCE_ROOT}/${relative_file}")
        list(APPEND violations "${SFX_SOURCE_ROOT}/${relative_file}")
    endif()
endforeach()

if(violations)
    list(REMOVE_DUPLICATES violations)
    list(JOIN violations "\n  " formatted)
    message(FATAL_ERROR
        "retired product identity or compatibility input remains active:\n  ${formatted}")
endif()

message(STATUS "active production and retained v1 test/build inputs are SFX-only")
