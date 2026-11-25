foreach(required_variable IN ITEMS
    SFX_CONFORMANCE_EXECUTABLE
    SFX_CONFORMANCE_FIXTURE_ROOT
    SFX_CONFORMANCE_SCRATCH)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

function(require_failed_result case_name expected_text)
    set(options ${ARGN})
    execute_process(
        COMMAND "${SFX_CONFORMANCE_EXECUTABLE}" ${options}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error
    )
    string(CONCAT combined "${standard_output}" "${standard_error}")
    if(result EQUAL 0)
        message(FATAL_ERROR "${case_name}: malformed fixture invocation succeeded")
    endif()
    string(FIND "${combined}" "SFX foundation conformance test failure:" diagnostic_position)
    if(diagnostic_position EQUAL -1)
        message(FATAL_ERROR "${case_name}: visible failure diagnostic missing: ${combined}")
    endif()
    string(FIND "${combined}" "${expected_text}" expected_position)
    if(expected_position EQUAL -1)
        message(FATAL_ERROR "${case_name}: expected diagnostic missing: ${combined}")
    endif()
endfunction()

function(copy_fixture_set case_name output_variable)
    set(case_root "${SFX_CONFORMANCE_SCRATCH}/${case_name}")
    file(MAKE_DIRECTORY "${case_root}")
    file(COPY "${SFX_CONFORMANCE_FIXTURE_ROOT}/" DESTINATION "${case_root}")
    set(${output_variable} "${case_root}" PARENT_SCOPE)
endfunction()

file(REMOVE_RECURSE "${SFX_CONFORMANCE_SCRATCH}")
file(MAKE_DIRECTORY "${SFX_CONFORMANCE_SCRATCH}")

require_failed_result(
    no_argument
    "exactly one fixture-root argument is required")

copy_fixture_set(missing missing_root)
file(REMOVE "${missing_root}/ogsf/startup-ready.observed.txt")
require_failed_result(
    missing
    "required fixture missing"
    "${missing_root}")

copy_fixture_set(altered altered_root)
file(APPEND "${altered_root}/sfx/caller-info.ANS" "ALTERED")
require_failed_result(
    altered
    "fixture byte-size mismatch"
    "${altered_root}")

copy_fixture_set(incomplete incomplete_root)
file(WRITE "${incomplete_root}/sfx/caller-info.ASC" "incomplete")
require_failed_result(
    incomplete
    "fixture byte-size mismatch"
    "${incomplete_root}")

copy_fixture_set(unexpected unexpected_root)
file(WRITE "${unexpected_root}/unexpected.txt" "unexpected")
require_failed_result(
    unexpected
    "unexpected fixture path"
    "${unexpected_root}")

file(REMOVE_RECURSE "${SFX_CONFORMANCE_SCRATCH}")
message(STATUS "SFX conformance fixture failure tests passed")
