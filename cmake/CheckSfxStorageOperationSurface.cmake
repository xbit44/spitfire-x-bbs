foreach(required_variable IN ITEMS
    SFX_STORAGE_IMPLEMENTATION_SOURCE
    SFX_ACCOUNT_REPOSITORY_SOURCE)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
    if(NOT EXISTS "${${required_variable}}")
        message(FATAL_ERROR "${required_variable} does not exist: ${${required_variable}}")
    endif()
endforeach()

file(READ "${SFX_STORAGE_IMPLEMENTATION_SOURCE}" storage_source)
file(READ "${SFX_ACCOUNT_REPOSITORY_SOURCE}" repository_source)

set(required_operations
    start checkpoint shutdown health reader_pool_health
    first_run_state stage_initial_sysop complete_first_run create_account
    commit_registration rename_account update_profile set_purge_allowed
    replace_credential change_account_state tombstone_account
    publish_security_policy publish_retention_policy record_session_started
    persist_session_progress finalize_session find_account_by_name
    find_account_by_id load_credential list_accounts inactivity_candidates
    session_history audit_history security_levels security_level
    security_policy retention_policy replace_account_password
    rehash_account_credential issue_temporary_password
    transition_temporary_password complete_temporary_password temporary_password
    replace_system_password rehash_system_credential system_password
    record_successful_login_ip successful_login_ips store_rate_limit_snapshot
    remove_rate_limit_snapshot clear_rate_limit active_rate_limit_snapshots
    authentication_audit_history transfer_preference update_transfer_preference
    enqueue_failed_login_notice claim_failed_login_notices
    acknowledge_failed_login_notices recover_abandoned_failed_login_claims
    create_backup validate_backup restore_backup database_path
)
list(LENGTH required_operations operation_count)
if(NOT operation_count EQUAL 58)
    message(FATAL_ERROR "storage operation inventory is ${operation_count}, expected 58")
endif()

foreach(operation IN LISTS required_operations)
    string(REGEX MATCHALL "SqliteStorage::${operation}\\(" definitions "${storage_source}")
    list(LENGTH definitions definition_count)
    if(NOT definition_count EQUAL 1)
        message(FATAL_ERROR
            "operation definition missing or duplicated: ${operation} (${definition_count})")
    endif()
    string(REGEX MATCHALL "impl_->${operation}\\(" forwards "${storage_source}")
    list(LENGTH forwards forward_count)
    if(NOT forward_count EQUAL 1)
        message(FATAL_ERROR
            "operation implementation forward missing or duplicated: ${operation} (${forward_count})")
    endif()
endforeach()

string(CONCAT retired_prefix "s" "f" "n" "g" "_")
string(FIND "${repository_source}" "${retired_prefix}" retired_position)
if(NOT retired_position EQUAL -1)
    message(FATAL_ERROR "retired table identity remains in active prepared-query source")
endif()

set(required_tables
    sfx_security_levels
    sfx_security_policy
    sfx_account_retention_policy
    sfx_accounts
    sfx_account_name_reservations
    sfx_account_profiles
    sfx_account_credentials
    sfx_account_statistics
    sfx_account_sessions
    sfx_account_audit
    sfx_installation_state
    sfx_system_credentials
    sfx_account_password_resets
    sfx_successful_login_ip_history
    sfx_authentication_rate_state
    sfx_authentication_audit
    sfx_account_transfer_preferences
    sfx_failed_login_notice_claims
    sfx_failed_login_notice_enqueues
    sfx_failed_login_notices
    sfx_registration_commit_receipts
    sfx_caller_number_state
    sfx_caller_journals
    sfx_caller_journal_publication
    sfx_account_menu_preferences
    sfx_caller_daily_seconds
    sfx_chat_configuration
    sfx_chat_usage_exclusions
    sfx_door_definitions
    sfx_door_entries
    sfx_door_menu_requirements
    sfx_door_menus
    sfx_door_policy
    sfx_file_areas
    sfx_file_policy
    sfx_security_calling_limits
    sfx_security_chat_limits
)
list(LENGTH required_tables table_count)
if(NOT table_count EQUAL 37)
    message(FATAL_ERROR "prepared-query table inventory is ${table_count}, expected 37")
endif()

# Header filenames such as sqlite_sfx_baseline.hpp are not SQL identities.
# Exclude only preprocessor include lines; unexpected identities in the actual
# repository body still fail the exact inventory comparison below.
string(REGEX REPLACE "(^|\n)[ \t]*#[ \t]*include[^\n]*" "\n" repository_body "${repository_source}")
string(REGEX MATCHALL "sfx_[a-z0-9_]+" observed_tables "${repository_body}")
list(REMOVE_DUPLICATES observed_tables)
list(SORT observed_tables)
list(SORT required_tables)
if(NOT "${observed_tables}" STREQUAL "${required_tables}")
    message(FATAL_ERROR
        "prepared-query table identity set mismatch\n"
        "  expected: ${required_tables}\n"
        "  observed: ${observed_tables}")
endif()

message(STATUS
    "SFX storage operation surface audit passed: 58 forwards, 37 prepared-query table identities")
