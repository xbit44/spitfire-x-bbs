foreach(required_variable IN ITEMS
    SFX_STORAGE_IMPLEMENTATION_SOURCE
    SFX_ACCOUNT_REPOSITORY_SOURCE
    SFX_STORAGE_OPERATION_AUDIT
    SFX_STORAGE_OPERATION_AUDIT_SCRATCH)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

file(READ "${SFX_STORAGE_IMPLEMENTATION_SOURCE}" original_storage)
file(READ "${SFX_ACCOUNT_REPOSITORY_SOURCE}" original_repository)
file(REMOVE_RECURSE "${SFX_STORAGE_OPERATION_AUDIT_SCRATCH}")
file(MAKE_DIRECTORY "${SFX_STORAGE_OPERATION_AUDIT_SCRATCH}")

function(require_audit_failure case_name storage_content repository_content expected_text)
    set(case_root "${SFX_STORAGE_OPERATION_AUDIT_SCRATCH}/${case_name}")
    file(MAKE_DIRECTORY "${case_root}")
    set(storage_file "${case_root}/sqlite_storage.cpp")
    set(repository_file "${case_root}/sqlite_account_repository.cpp")
    file(WRITE "${storage_file}" "${storage_content}")
    file(WRITE "${repository_file}" "${repository_content}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}"
            "-DSFX_STORAGE_IMPLEMENTATION_SOURCE=${storage_file}"
            "-DSFX_ACCOUNT_REPOSITORY_SOURCE=${repository_file}"
            -P "${SFX_STORAGE_OPERATION_AUDIT}"
        RESULT_VARIABLE result
        OUTPUT_VARIABLE standard_output
        ERROR_VARIABLE standard_error
    )
    string(CONCAT combined "${standard_output}" "${standard_error}")
    if(result EQUAL 0)
        message(FATAL_ERROR "${case_name}: malformed operation surface passed")
    endif()
    string(FIND "${combined}" "${expected_text}" expected_position)
    if(expected_position EQUAL -1)
        message(FATAL_ERROR
            "${case_name}: expected failure diagnostic missing: ${combined}")
    endif()
endfunction()

set(missing_operation "${original_storage}")
string(REPLACE
    "application::StorageStatus SqliteStorage::checkpoint() { return impl_->checkpoint(); }"
    ""
    missing_operation
    "${missing_operation}")
if(missing_operation STREQUAL original_storage)
    message(FATAL_ERROR "missing-operation negative mutation did not apply")
endif()
require_audit_failure(
    missing_operation
    "${missing_operation}"
    "${original_repository}"
    "operation definition missing or duplicated: checkpoint")

set(altered_operation "${original_storage}")
string(REPLACE
    "SqliteStorage::checkpoint"
    "SqliteStorage::checkpoint_changed"
    altered_operation
    "${altered_operation}")
if(altered_operation STREQUAL original_storage)
    message(FATAL_ERROR "altered-operation negative mutation did not apply")
endif()
require_audit_failure(
    altered_operation
    "${altered_operation}"
    "${original_repository}"
    "operation definition missing or duplicated: checkpoint")

set(missing_table "${original_repository}")
string(REPLACE "sfx_security_levels" "" missing_table "${missing_table}")
if(missing_table STREQUAL original_repository)
    message(FATAL_ERROR "missing-table negative mutation did not apply")
endif()
require_audit_failure(
    missing_table
    "${original_storage}"
    "${missing_table}"
    "prepared-query table identity set mismatch")

set(altered_table "${original_repository}")
string(REPLACE "sfx_accounts" "sfx_members" altered_table "${altered_table}")
if(altered_table STREQUAL original_repository)
    message(FATAL_ERROR "altered-table negative mutation did not apply")
endif()
require_audit_failure(
    altered_table
    "${original_storage}"
    "${altered_table}"
    "prepared-query table identity set mismatch")

require_audit_failure(
    unexpected_table
    "${original_storage}"
    "${original_repository}\nconst char* unexpected_query = \"SELECT * FROM sfx_unreviewed_table\";\n"
    "prepared-query table identity set mismatch")
require_audit_failure(
    duplicate_forward
    "${original_storage}\nvoid duplicate_checkpoint() { impl_->checkpoint(); }\n"
    "${original_repository}"
    "operation implementation forward missing or duplicated: checkpoint")

file(REMOVE_RECURSE "${SFX_STORAGE_OPERATION_AUDIT_SCRATCH}")
message(STATUS "SFX storage operation surface negative tests passed: missing/altered operations and tables, unexpected table and duplicate forward")
