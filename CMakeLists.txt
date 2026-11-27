cmake_minimum_required(VERSION 3.16)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(spitfire_x_bbs VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    if(NOT MSVC_TOOLSET_VERSION STREQUAL "143")
        message(FATAL_ERROR "SFX Windows builds require the MSVC v143 toolset")
    endif()
    if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
        message(FATAL_ERROR "SFX Windows builds require the Win32/x86 target")
    endif()
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CTest)
include(cmake/SfxTargetSettings.cmake)
find_package(Threads REQUIRED)
include(cmake/SfxLibsodium.cmake)
include(cmake/SfxLibarchive.cmake)

option(SFX_ALPHA_TEST_AUTOMATION
    "Enable explicit repository-local Alpha 0.0.1 Control Panel automation in this non-shipping build"
    OFF)
set(SFX_ALPHA_TEST_INSTALL_ROOT "" CACHE STRING
    "Compile-time Alpha 0.0.1 installation root for a BUILD_TESTING review build")
if((SFX_ALPHA_TEST_AUTOMATION OR NOT SFX_ALPHA_TEST_INSTALL_ROOT STREQUAL "")
        AND NOT BUILD_TESTING)
    message(FATAL_ERROR "Alpha test automation/root injection requires BUILD_TESTING=ON")
endif()
if(NOT SFX_ALPHA_TEST_INSTALL_ROOT STREQUAL "" AND
        NOT IS_ABSOLUTE "${SFX_ALPHA_TEST_INSTALL_ROOT}")
    message(FATAL_ERROR "SFX_ALPHA_TEST_INSTALL_ROOT must be absolute")
endif()

add_library(sfx_sqlite3 STATIC third_party/sqlite/3.53.4/sqlite3.c)
set_target_properties(sfx_sqlite3 PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_include_directories(sfx_sqlite3 PUBLIC third_party/sqlite/3.53.4)
target_link_libraries(sfx_sqlite3 PRIVATE Threads::Threads ${CMAKE_DL_LIBS})
target_compile_definitions(sfx_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_ENABLE_API_ARMOR
    SQLITE_SECURE_DELETE=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
)
if(MSVC)
    target_compile_definitions(sfx_sqlite3 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601)
    target_compile_options(sfx_sqlite3 PRIVATE /W3 /wd4996 /utf-8 /Brepro)
else()
    target_compile_options(sfx_sqlite3 PRIVATE -Wall -Wextra)
endif()

add_library(sfx_archive STATIC
    src/archive/src/archive_engine.cpp
    src/archive/src/qwk_archive.cpp
    src/archive/src/file_zip.cpp
    src/archive/src/file_contents.cpp
    src/archive/src/qwk_packet_builder.cpp
    src/archive/src/backup_service.cpp)
add_library(sfx::archive ALIAS sfx_archive)
target_include_directories(sfx_archive PUBLIC src/archive/include)
target_link_libraries(sfx_archive PUBLIC sfx::application PRIVATE sfx::libarchive sfx::libsodium)
if(WIN32)
    target_compile_definitions(sfx_archive PRIVATE
        _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX
        UNICODE _UNICODE)
endif()
sfx_configure_portable_target(sfx_archive)

add_library(sfx_core STATIC
    src/core/src/node.cpp
    src/core/src/security.cpp
    src/core/src/session.cpp
)
add_library(sfx::core ALIAS sfx_core)
target_include_directories(sfx_core PUBLIC src/core/include)
sfx_configure_portable_target(sfx_core)

add_library(sfx_application STATIC
    src/application/src/ftn.cpp
    src/application/src/ftn_setup.cpp
    src/application/src/ftn_nodelist.cpp
    src/application/src/ftn_stored_message.cpp
    src/application/src/ftn_codec.cpp
    src/application/src/ftn_wire.cpp
    src/application/src/ftn_mail.cpp
    src/application/src/ftn_external.cpp
    src/application/src/ftn_echo_import.cpp
    src/application/src/ftn_hatch_catalog.cpp
    src/application/src/ftn_configuration_transport.cpp
    src/application/src/ftn_retention.cpp
    src/application/src/chat.cpp
    src/application/src/chat_notice.cpp
    src/application/src/chat_session.cpp
    src/application/src/control_panel.cpp
    src/application/src/account_storage.cpp
    src/application/src/caller_journal.cpp
    src/application/src/durable_journal.cpp
    src/application/src/new_caller_names.cpp
    src/application/src/accounting.cpp
    src/application/src/caller_lifetime.cpp
    src/application/src/caller_assignment.cpp
    src/application/src/hostname_lookup.cpp
    src/application/src/authentication_security.cpp
    src/application/src/authentication_service.cpp
    src/application/src/session_revocation.cpp
    src/application/src/local_account_administration.cpp
    src/application/src/authentication_storage.cpp
    src/application/src/commands.cpp
    src/application/src/configuration.cpp
    src/application/src/display.cpp
    src/application/src/menu.cpp
    src/application/src/menu_session.cpp
    src/application/src/sysop_session.cpp
    src/application/src/caller_help.cpp
    src/application/src/classic_information.cpp
    src/application/src/classic_information_storage.cpp
    src/application/src/message.cpp
    src/application/src/file.cpp
    src/application/src/door.cpp
    src/application/src/door_execution.cpp
    src/application/src/door_drop.cpp
    src/application/src/door_session.cpp
    src/application/src/door_bulletin.cpp
    src/application/src/door_bulletin_session.cpp
    src/application/src/door_bulletin_preview.cpp
    src/application/src/local_door_bulletin_administration.cpp
    src/application/src/telgate.cpp
    src/application/src/rlogin.cpp
    src/application/src/local_door_administration.cpp
    src/application/src/file_scan.cpp
    src/application/src/file_presentation.cpp
    src/application/src/file_session.cpp
    src/application/src/qwk.cpp
    src/application/src/qwk_session.cpp
    src/application/src/message_source.cpp
    src/application/src/message_receive.cpp
    src/application/src/message_editor.cpp
    src/application/src/newsletter_editor.cpp
    src/application/src/message_arrival.cpp
    src/application/src/message_compose.cpp
    src/application/src/message_selection.cpp
    src/application/src/message_reader.cpp
    src/application/src/message_presentation.cpp
    src/application/src/message_session.cpp
    src/application/src/other_board_text.cpp
    src/application/src/local_classic_information.cpp
    src/application/src/local_message_administration.cpp
    src/application/src/local_file_administration.cpp
    src/application/src/file_management.cpp
    src/application/src/local_qwk_administration.cpp
    src/application/src/caller_utilities.cpp
    src/application/src/calling_time.cpp
    src/application/src/events.cpp
    src/application/src/external_telnet_client.cpp
    src/application/src/lifecycle.cpp
    src/application/src/local_control_protocol.cpp
    src/application/src/logging.cpp
    src/application/src/operational_log.cpp
    src/application/src/event_sounds.cpp
    src/application/src/administration_reporting.cpp
    src/application/src/operational_backlog.cpp
    src/application/src/node_pool.cpp
    src/application/src/operational_configuration.cpp
    src/application/src/botgate.cpp
    src/application/src/botgate_rules.cpp
    src/application/src/scheduler.cpp
    src/application/src/external_events.cpp
    src/application/src/phase3a.cpp
    src/application/src/phase3a_private_adapters.cpp
    src/application/src/phase3a_storage.cpp
    src/application/src/phase3a_registration_workflow.cpp
    src/application/src/phase3a_workflow.cpp
    src/application/src/terminal.cpp
    src/application/src/terminal_stream.cpp
)
add_library(sfx::application ALIAS sfx_application)
target_include_directories(sfx_application PUBLIC src/application/include)
target_link_libraries(sfx_application PUBLIC sfx::core)
target_link_libraries(sfx_application PRIVATE Threads::Threads)
sfx_configure_portable_target(sfx_application)

add_library(sfx_portable_credential_codec STATIC src/platform/common/src/portable_credential_codec.cpp)
target_include_directories(sfx_portable_credential_codec PUBLIC src/platform/common/include)
target_link_libraries(sfx_portable_credential_codec PUBLIC sfx::application PRIVATE sfx::libsodium)
sfx_configure_portable_target(sfx_portable_credential_codec)
add_library(sfx_ftn_external_files STATIC src/platform/common/ftn_external_files.cpp)
target_include_directories(sfx_ftn_external_files PUBLIC src/platform/common)
target_link_libraries(sfx_ftn_external_files PUBLIC sfx::application PRIVATE sfx::libsodium)
if(WIN32)
    target_link_libraries(sfx_ftn_external_files PUBLIC sfx_platform_win32_storage PRIVATE advapi32)
endif()
sfx_configure_portable_target(sfx_ftn_external_files)
add_executable(SFXBinkD src/apps/ftn_external/main.cpp)
target_link_libraries(SFXBinkD PRIVATE sfx_ftn_external_files)
sfx_configure_portable_target(SFXBinkD)
if(BUILD_TESTING)
    add_executable(sfx_ftn_external_files_tests test/platform/ftn_external_files_tests.cpp)
    target_link_libraries(sfx_ftn_external_files_tests PRIVATE sfx_ftn_external_files)
    sfx_configure_test_executable(sfx_ftn_external_files_tests)
    add_executable(sfx_portable_credential_tests test/unit/portable_credential_tests.cpp)
    target_link_libraries(sfx_portable_credential_tests PRIVATE sfx_portable_credential_codec sfx::libsodium)
    sfx_configure_test_executable(sfx_portable_credential_tests)
    add_test(NAME sfx.unit.portable_credentials COMMAND sfx_portable_credential_tests)
endif()

add_library(sfx_authentication STATIC
    src/adapters/authentication/src/phase3a_authentication_adapters.cpp
    src/adapters/authentication/src/sodium_authentication.cpp
)
add_library(sfx::authentication ALIAS sfx_authentication)
target_include_directories(sfx_authentication PUBLIC src/adapters/authentication/include)
# Ship the already pinned local blocklist inside the protected binaries. This
# adds no runtime download or writable-data dependency; verify again on load.
set(sfx_password_list "${CMAKE_CURRENT_SOURCE_DIR}/third_party/seclists/2026.1/xato-net-10-million-passwords-10000.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sfx_password_list}")
file(SHA256 "${sfx_password_list}" sfx_password_list_sha256)
if(NOT sfx_password_list_sha256 STREQUAL "c63d5e4ccc31344d662583cc39ca4bd5bd20517ff1d24501f0c4e0c22d9b722a")
    message(FATAL_ERROR "Pinned SecLists 2026.1 password data hash mismatch")
endif()
file(READ "${sfx_password_list}" sfx_password_list_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1,\n" sfx_password_list_initializer "${sfx_password_list_hex}")
file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/generated/sfx_pinned_password_list.hpp"
    CONTENT "// Generated from the hash-verified pinned SecLists artifact.\n#pragma once\nnamespace sfx::adapters::authentication::embedded {\ninline constexpr unsigned char password_list[] = {\n${sfx_password_list_initializer}};\n}\n")
target_include_directories(sfx_authentication PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(sfx_authentication
    PUBLIC sfx::application
    PRIVATE sfx::libsodium Threads::Threads
)
sfx_configure_portable_target(sfx_authentication)

add_library(sfx_storage_sqlite STATIC
    src/adapters/storage/src/control_panel_statistics.cpp
    src/adapters/storage/src/sqlite_sfx_baseline.cpp
    src/adapters/storage/src/sqlite_registration_fingerprint.cpp
    src/adapters/storage/src/sqlite_account_repository.cpp
    src/adapters/storage/src/sqlite_connection.cpp
    src/adapters/storage/src/sqlite_storage.cpp
    src/adapters/storage/src/sqlite_writer.cpp
)
add_library(sfx::storage_sqlite ALIAS sfx_storage_sqlite)
target_include_directories(sfx_storage_sqlite
    PUBLIC src/adapters/storage/include
    PRIVATE src/adapters/storage/src
)
target_link_libraries(sfx_storage_sqlite
    PUBLIC sfx::application
    PRIVATE sfx_sqlite3 sfx::libsodium Threads::Threads
)
sfx_configure_portable_target(sfx_storage_sqlite)
if(MSVC)
    # The retained SQLite repository includes all transactional FTN ledgers.
    # Its Win32 template sections exceed the standard COFF object limit.
    set_source_files_properties(src/adapters/storage/src/sqlite_account_repository.cpp
        PROPERTIES COMPILE_OPTIONS /bigobj)
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(cmake/SfxLinuxSSH.cmake)
    include(cmake/SfxLinuxFileTransfer.cmake)
    include(cmake/SfxLinuxTimeZone.cmake)
    include(cmake/SfxBotGateTLS.cmake)
    add_library(sfx_linux_host_files STATIC src/platform/linux/src/private_files.cpp src/platform/linux/src/child_process.cpp
        src/platform/linux/src/installation_layout.cpp
        src/platform/linux/src/durable_journal_files.cpp src/platform/linux/src/trusted_display_asset_reader.cpp
        src/platform/linux/src/other_board_text_file.cpp src/platform/linux/src/scheduler_files.cpp)
    target_include_directories(sfx_linux_host_files PUBLIC src/platform/linux/include src/adapters/storage/include src/archive/include)
    target_link_libraries(sfx_linux_host_files PUBLIC sfx::application PRIVATE sfx::libsodium)
    sfx_configure_portable_target(sfx_linux_host_files)
    target_link_libraries(sfx_ftn_external_files PRIVATE sfx_linux_host_files)
    if(BUILD_TESTING)
        target_link_libraries(sfx_ftn_external_files_tests PRIVATE sfx_linux_host_files)
        add_executable(sfx_linux_layout_tests test/platform/linux_layout_tests.cpp)
        target_link_libraries(sfx_linux_layout_tests PRIVATE sfx_linux_host_files)
        sfx_configure_test_executable(sfx_linux_layout_tests)
    endif()
    add_library(sfx_linux_local_control STATIC src/platform/linux/src/local_control.cpp)
    target_include_directories(sfx_linux_local_control PRIVATE src/platform/common/include)
    target_link_libraries(sfx_linux_local_control PUBLIC sfx_linux_host_files Threads::Threads)
    sfx_configure_portable_target(sfx_linux_local_control)
    add_library(sfx_linux_time_zone STATIC src/platform/linux/src/bbs_time_zone_provider.cpp)
    target_include_directories(sfx_linux_time_zone PRIVATE "${SFX_CLDR_ROOT}")
    target_link_libraries(sfx_linux_time_zone PUBLIC sfx_linux_host_files PRIVATE sfx_date_tz)
    sfx_configure_portable_target(sfx_linux_time_zone)
    add_library(sfx_linux_runtime_journal STATIC src/apps/runtime/alpha_operational_journal.cpp
        src/apps/runtime/alpha_durable_journal.cpp)
    target_include_directories(sfx_linux_runtime_journal PUBLIC src/apps/runtime src/platform/common/include)
    target_link_libraries(sfx_linux_runtime_journal PUBLIC sfx_linux_host_files sfx_linux_time_zone sfx::storage_sqlite Threads::Threads)
    sfx_configure_portable_target(sfx_linux_runtime_journal)
    add_library(sfx_linux_runtime_jobs STATIC src/apps/runtime/linux_runtime_jobs.cpp)
    target_link_libraries(sfx_linux_runtime_jobs PUBLIC sfx_linux_runtime_journal)
    sfx_configure_portable_target(sfx_linux_runtime_jobs)
    if(BUILD_TESTING)
        add_executable(sfx_linux_runtime_jobs_tests test/platform/linux_runtime_jobs_tests.cpp)
        target_link_libraries(sfx_linux_runtime_jobs_tests PRIVATE sfx_linux_runtime_jobs)
        target_link_options(sfx_linux_runtime_jobs_tests PRIVATE "-Wl,--wrap=fsync")
        sfx_configure_test_executable(sfx_linux_runtime_jobs_tests)
    endif()
    add_library(sfx_linux_botgate STATIC src/platform/linux/src/botgate_rule_files.cpp src/apps/runtime/linux_botgate_feed.cpp src/apps/runtime/linux_verified_domain.cpp src/apps/runtime/linux_hostname_lookup.cpp)
    target_link_libraries(sfx_linux_botgate PUBLIC sfx_linux_runtime_journal sfx_botgate_https)
    sfx_configure_portable_target(sfx_linux_botgate)
    add_executable(SFXHostLookup src/apps/runtime/linux_host_lookup.cpp)
    target_link_libraries(SFXHostLookup PRIVATE sfx::application)
    sfx_configure_portable_target(SFXHostLookup)
    add_library(sfx_linux_ssh STATIC src/apps/runtime/alpha_ssh_stream.cpp
        src/apps/runtime/linux_ssh_host_key.cpp)
    target_include_directories(sfx_linux_ssh PUBLIC src/apps/runtime
        src/platform/common/include)
    target_link_libraries(sfx_linux_ssh PUBLIC sfx::authentication PRIVATE deucessh::static sfx_linux_host_files)
    sfx_configure_portable_target(sfx_linux_ssh)
    add_library(sfx_linux_caller_session STATIC src/apps/runtime/linux_caller_session.cpp
        src/apps/runtime/linux_caller_registry.cpp
        src/apps/runtime/linux_caller_listener.cpp)
    target_link_libraries(sfx_linux_caller_session PUBLIC sfx_linux_ssh sfx_linux_runtime_journal)
    sfx_configure_portable_target(sfx_linux_caller_session)
    add_library(sfx_linux_runtime_store STATIC src/apps/runtime/linux_runtime_store.cpp src/apps/runtime/linux_security_publication.cpp)
    add_library(sfx_linux_portable_keys STATIC src/platform/linux/src/portable_key_store.cpp)
    target_link_libraries(sfx_linux_portable_keys PUBLIC sfx_linux_host_files sfx_portable_credential_codec PRIVATE sfx::libsodium)
    sfx_configure_portable_target(sfx_linux_portable_keys)
    add_library(sfx_linux_file_payload STATIC src/platform/linux/src/file_payload_store.cpp src/platform/linux/src/portable_file_bindings.cpp)
    target_sources(sfx_linux_file_payload PRIVATE src/platform/linux/src/door_bulletin_sources.cpp)
    target_link_libraries(sfx_linux_file_payload PUBLIC sfx_linux_host_files sfx_linux_portable_keys PRIVATE sfx::libsodium)
    if(BUILD_TESTING)
        add_executable(sfx_linux_door_bulletin_source_tests test/platform/door_bulletin_source_tests.cpp)
        target_link_libraries(sfx_linux_door_bulletin_source_tests PRIVATE sfx_linux_file_payload)
        sfx_configure_test_executable(sfx_linux_door_bulletin_source_tests)
    endif()
    sfx_configure_portable_target(sfx_linux_file_payload)
    add_library(sfx_linux_file_backup STATIC src/platform/linux/src/file_backup.cpp src/platform/linux/src/backup_collections.cpp src/platform/linux/src/core_restore.cpp)
    target_sources(sfx_linux_file_backup PRIVATE src/platform/linux/src/ftn_history_recovery.cpp)
    target_link_libraries(sfx_linux_file_backup PUBLIC sfx_linux_file_payload sfx::archive sfx::storage_sqlite PRIVATE sfx_sqlite3 sfx::libsodium)
    target_link_libraries(sfx_linux_file_backup PRIVATE sfx_linux_ftn_session)
    sfx_configure_portable_target(sfx_linux_file_backup)
    if(BUILD_TESTING)
        add_executable(sfx_linux_file_backup_tests test/platform/linux_file_backup_tests.cpp)
        target_link_libraries(sfx_linux_file_backup_tests PRIVATE sfx_linux_file_backup)
        sfx_configure_test_executable(sfx_linux_file_backup_tests)
    endif()
    add_library(sfx_linux_file_intake STATIC src/platform/linux/src/file_intake.cpp)
    target_link_libraries(sfx_linux_file_intake PUBLIC sfx_linux_file_payload sfx::archive)
    sfx_configure_portable_target(sfx_linux_file_intake)
    add_library(sfx_linux_file_manager STATIC src/platform/linux/src/file_manager.cpp)
    target_link_libraries(sfx_linux_file_manager PUBLIC sfx_linux_file_payload sfx::archive)
    sfx_configure_portable_target(sfx_linux_file_manager)
    target_link_libraries(sfx_linux_file_intake PUBLIC sfx_linux_file_manager)
    add_library(sfx_linux_file_configuration STATIC src/platform/linux/src/file_configuration.cpp)
    target_link_libraries(sfx_linux_file_configuration PUBLIC sfx_linux_file_manager PRIVATE sfx::libsodium)
    sfx_configure_portable_target(sfx_linux_file_configuration)
    add_library(sfx_linux_file_administration STATIC src/platform/linux/src/file_administration.cpp src/platform/linux/src/file_owner_view.cpp)
    target_link_libraries(sfx_linux_file_administration PUBLIC sfx_linux_file_manager sfx_linux_file_intake PRIVATE Threads::Threads)
    sfx_configure_portable_target(sfx_linux_file_administration)
    add_library(sfx_linux_file_scanner STATIC src/platform/linux/src/file_scanner.cpp)
    target_link_libraries(sfx_linux_file_scanner PUBLIC sfx_linux_file_payload sfx::application PRIVATE sfx::archive sfx::libsodium Threads::Threads)
    sfx_configure_portable_target(sfx_linux_file_scanner)
    add_library(sfx_linux_file_transfer STATIC src/apps/runtime/linux_file_transfer.cpp)
    target_include_directories(sfx_linux_file_transfer PUBLIC src/apps/runtime)
    target_link_libraries(sfx_linux_file_transfer PUBLIC sfx_linux_file_payload sfx::application PRIVATE sfx::libsodium Threads::Threads)
    sfx_configure_portable_target(sfx_linux_file_transfer)
    add_library(sfx_linux_qwk_spool STATIC src/apps/runtime/linux_qwk_reply_spool.cpp)
    target_include_directories(sfx_linux_qwk_spool PUBLIC src/apps/runtime)
    target_link_libraries(sfx_linux_qwk_spool PUBLIC sfx_linux_host_files sfx::application PRIVATE sfx::libsodium)
    sfx_configure_portable_target(sfx_linux_qwk_spool)
    add_library(sfx_linux_qwk_transfer STATIC src/apps/runtime/linux_qwk_transfer.cpp)
    target_include_directories(sfx_linux_qwk_transfer PUBLIC src/apps/runtime)
    target_link_libraries(sfx_linux_qwk_transfer PUBLIC sfx_linux_qwk_spool sfx::archive PRIVATE sfx::libsodium Threads::Threads)
    sfx_configure_portable_target(sfx_linux_qwk_transfer)
    add_library(sfx_linux_caller_files STATIC src/apps/runtime/linux_caller_files.cpp src/apps/runtime/linux_file_viewer.cpp)
    target_link_libraries(sfx_linux_caller_files PUBLIC sfx_linux_file_transfer sfx_linux_file_intake sfx_linux_file_scanner)
    sfx_configure_portable_target(sfx_linux_caller_files)
    target_include_directories(sfx_linux_runtime_store PUBLIC src/apps/runtime)
    target_link_libraries(sfx_linux_runtime_store PUBLIC sfx_linux_time_zone sfx_linux_portable_keys sfx_linux_file_payload sfx_linux_file_intake sfx::storage_sqlite sfx::authentication PRIVATE sfx::libsodium)
    sfx_configure_portable_target(sfx_linux_runtime_store)
    if(BUILD_TESTING)
        add_executable(sfx_linux_botgate_tests test/platform/linux_botgate_tests.cpp)
        target_link_libraries(sfx_linux_botgate_tests PRIVATE sfx_linux_botgate)
        sfx_configure_test_executable(sfx_linux_botgate_tests)
        add_executable(sfx_linux_botgate_tls_tests test/platform/linux_botgate_tls_tests.cpp)
        target_link_libraries(sfx_linux_botgate_tls_tests PRIVATE sfx_botgate_https sfx::application OpenSSL::SSL Threads::Threads)
        sfx_configure_test_executable(sfx_linux_botgate_tls_tests)
        add_executable(sfx_linux_botgate_signal_tests test/platform/linux_botgate_signal_tests.cpp)
        target_link_libraries(sfx_linux_botgate_signal_tests PRIVATE sfx_botgate_https CURL::libcurl_static Threads::Threads)
        target_link_options(sfx_linux_botgate_signal_tests PRIVATE "-Wl,--wrap=curl_easy_perform" "-Wl,--wrap=curl_easy_cleanup")
        sfx_configure_test_executable(sfx_linux_botgate_signal_tests)
        add_executable(sfx_linux_host_lookup_fixture test/platform/linux_host_lookup_fixture.cpp)
        sfx_configure_test_executable(sfx_linux_host_lookup_fixture)
        add_executable(sfx_linux_verified_domain_tests test/platform/linux_verified_domain_tests.cpp)
        target_link_libraries(sfx_linux_verified_domain_tests PRIVATE sfx_linux_botgate)
        sfx_configure_test_executable(sfx_linux_verified_domain_tests)
        add_executable(sfx_linux_file_payload_tests test/platform/linux_file_payload_tests.cpp)
        add_executable(sfx_linux_file_transfer_fixture test/platform/linux_file_transfer_fixture.cpp)
        add_executable(sfx_linux_local_time_tests test/unit/qwk_local_time_tests.cpp)
        target_link_libraries(sfx_linux_local_time_tests PRIVATE sfx::application)
        sfx_configure_test_executable(sfx_linux_local_time_tests)
        target_link_libraries(sfx_linux_file_transfer_fixture PRIVATE sfx_linux_file_transfer)
        sfx_configure_test_executable(sfx_linux_file_transfer_fixture)
        add_executable(sfx_linux_file_intake_tests test/platform/linux_file_intake_tests.cpp)
        add_executable(sfx_linux_file_manager_tests test/platform/linux_file_manager_tests.cpp)
        target_include_directories(sfx_linux_file_manager_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_file_manager_tests PRIVATE sfx_linux_file_manager sfx_linux_file_intake sfx::storage_sqlite sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_file_manager_tests)
        add_executable(sfx_linux_file_import_custody_tests test/platform/linux_file_import_custody_tests.cpp)
        target_include_directories(sfx_linux_file_import_custody_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_file_import_custody_tests PRIVATE sfx_linux_file_manager sfx_linux_file_intake sfx::storage_sqlite sfx::libarchive)
        target_link_options(sfx_linux_file_import_custody_tests PRIVATE "-Wl,--wrap=statx")
        sfx_configure_test_executable(sfx_linux_file_import_custody_tests)
        add_executable(sfx_linux_sysop_log_tests test/platform/linux_sysop_log_tests.cpp)
        target_link_libraries(sfx_linux_sysop_log_tests PRIVATE sfx_linux_host_files sfx::application sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_sysop_log_tests)
        add_executable(sfx_linux_qwk_spool_tests test/platform/linux_qwk_spool_tests.cpp)
        target_include_directories(sfx_linux_qwk_spool_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_qwk_spool_tests PRIVATE sfx_linux_qwk_spool sfx::storage_sqlite)
        sfx_configure_test_executable(sfx_linux_qwk_spool_tests)
        add_executable(sfx_linux_qwk_transfer_fixture test/platform/linux_qwk_transfer_fixture.cpp)
        target_link_libraries(sfx_linux_qwk_transfer_fixture PRIVATE sfx_linux_qwk_transfer)
        sfx_configure_test_executable(sfx_linux_qwk_transfer_fixture)
        add_executable(sfx_linux_file_configuration_tests test/platform/linux_file_configuration_tests.cpp)
        target_include_directories(sfx_linux_file_configuration_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_file_configuration_tests PRIVATE sfx_linux_file_configuration sfx_linux_file_intake sfx::storage_sqlite sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_file_configuration_tests)
        add_executable(sfx_linux_file_administration_tests test/platform/linux_file_administration_tests.cpp)
        target_include_directories(sfx_linux_file_administration_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_file_administration_tests PRIVATE sfx_linux_file_administration sfx::storage_sqlite sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_file_administration_tests)
        add_executable(sfx_linux_file_owner_view_tests test/platform/linux_file_owner_view_tests.cpp)
        target_include_directories(sfx_linux_file_owner_view_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_file_owner_view_tests PRIVATE sfx_linux_file_administration sfx::storage_sqlite sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_file_owner_view_tests)
        add_executable(sfx_linux_file_scanner_tests test/platform/linux_file_scanner_tests.cpp)
        target_link_libraries(sfx_linux_file_scanner_tests PRIVATE sfx_linux_file_scanner)
        sfx_configure_test_executable(sfx_linux_file_scanner_tests)
        add_executable(sfx_linux_clamav_update_tests test/platform/linux_clamav_update_tests.cpp)
        target_link_libraries(sfx_linux_clamav_update_tests PRIVATE sfx_linux_file_scanner)
        target_link_options(sfx_linux_clamav_update_tests PRIVATE -Wl,--wrap=fsync)
        sfx_configure_test_executable(sfx_linux_clamav_update_tests)
        add_executable(sfx_linux_child_thread_policy_tests test/platform/linux_child_thread_policy_tests.cpp)
        target_link_libraries(sfx_linux_child_thread_policy_tests PRIVATE sfx_linux_host_files Threads::Threads)
        sfx_configure_test_executable(sfx_linux_child_thread_policy_tests)
        target_include_directories(sfx_linux_file_intake_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_file_intake_tests PRIVATE sfx_linux_file_intake sfx::storage_sqlite)
        target_link_options(sfx_linux_file_intake_tests PRIVATE "-Wl,--wrap=statx")
        sfx_configure_test_executable(sfx_linux_file_intake_tests)
        target_link_libraries(sfx_linux_file_payload_tests PRIVATE sfx_linux_file_payload sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_file_payload_tests)
        add_executable(sfx_linux_file_binding_tests test/platform/linux_file_binding_tests.cpp)
        target_link_libraries(sfx_linux_file_binding_tests PRIVATE sfx_linux_file_payload)
        target_link_options(sfx_linux_file_binding_tests PRIVATE "-Wl,--wrap=statx")
        sfx_configure_test_executable(sfx_linux_file_binding_tests)
        add_executable(sfx_linux_runtime_store_tests test/platform/linux_runtime_store_tests.cpp)
        add_executable(sfx_linux_runtime_fixture test/platform/linux_runtime_fixture.cpp)
        target_link_libraries(sfx_linux_runtime_fixture PRIVATE sfx_linux_runtime_store)
        sfx_configure_test_executable(sfx_linux_runtime_fixture)
        add_executable(sfx_linux_portable_key_tests test/platform/linux_portable_key_tests.cpp)
        target_link_libraries(sfx_linux_portable_key_tests PRIVATE sfx_linux_portable_keys)
        sfx_configure_test_executable(sfx_linux_portable_key_tests)
        target_link_libraries(sfx_linux_runtime_store_tests PRIVATE sfx_linux_runtime_store)
        sfx_configure_test_executable(sfx_linux_runtime_store_tests)
        add_executable(sfx_linux_local_control_tests test/platform/linux_local_control_tests.cpp)
        target_link_libraries(sfx_linux_local_control_tests PRIVATE sfx_linux_local_control sfx_linux_runtime_store)
        sfx_configure_test_executable(sfx_linux_local_control_tests)
        add_executable(sfx_linux_runtime_log_probe test/platform/linux_runtime_log_probe.cpp)
        target_link_libraries(sfx_linux_runtime_log_probe PRIVATE sfx_linux_local_control)
        sfx_configure_test_executable(sfx_linux_runtime_log_probe)
        add_executable(sfx_linux_caller_session_tests test/platform/linux_caller_session_tests.cpp)
        add_executable(sfx_linux_listener_reconfigure_tests test/platform/linux_listener_reconfigure_tests.cpp)
        add_executable(sfx_linux_local_logon_tests test/platform/linux_local_logon_tests.cpp)
        target_link_libraries(sfx_linux_local_logon_tests PRIVATE sfx_linux_caller_session)
        sfx_configure_test_executable(sfx_linux_local_logon_tests)
        add_executable(sfx_linux_live_configuration_tests test/platform/linux_live_configuration_tests.cpp)
        target_link_libraries(sfx_linux_live_configuration_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_live_configuration_tests)
        target_link_libraries(sfx_linux_listener_reconfigure_tests PRIVATE sfx_linux_caller_session)
        sfx_configure_test_executable(sfx_linux_listener_reconfigure_tests)
        add_executable(sfx_linux_caller_registry_tests test/platform/linux_caller_registry_tests.cpp)
        target_link_libraries(sfx_linux_caller_registry_tests PRIVATE sfx_linux_caller_session)
        sfx_configure_test_executable(sfx_linux_caller_registry_tests)
        target_link_libraries(sfx_linux_caller_session_tests PRIVATE sfx_linux_caller_session sfx_linux_runtime_door)
        sfx_configure_test_executable(sfx_linux_caller_session_tests)
        add_executable(sfx_linux_network_tests test/platform/linux_network_tests.cpp)
        target_link_libraries(sfx_linux_network_tests PRIVATE sfx_linux_ssh)
        sfx_configure_test_executable(sfx_linux_network_tests)
        add_test(NAME sfx.platform.linux_network COMMAND sfx_linux_network_tests)
        add_executable(sfx_linux_private_files_tests test/platform/linux_private_files_tests.cpp)
        target_link_libraries(sfx_linux_private_files_tests PRIVATE sfx_linux_host_files)
        sfx_configure_test_executable(sfx_linux_private_files_tests)
        add_executable(sfx_linux_private_publication_tests test/platform/linux_private_publication_tests.cpp)
        target_link_libraries(sfx_linux_private_publication_tests PRIVATE sfx_linux_host_files)
        target_link_options(sfx_linux_private_publication_tests PRIVATE "-Wl,--wrap=syscall" "-Wl,--wrap=unlinkat" "-Wl,--wrap=fsync")
        sfx_configure_test_executable(sfx_linux_private_publication_tests)
        add_executable(sfx_linux_journal_tests test/platform/linux_journal_tests.cpp)
        add_executable(sfx_linux_display_files_tests test/platform/linux_display_files_tests.cpp)
        target_link_libraries(sfx_linux_display_files_tests PRIVATE sfx_linux_host_files)
        sfx_configure_test_executable(sfx_linux_display_files_tests)
        target_link_libraries(sfx_linux_journal_tests PRIVATE sfx_linux_host_files)
        sfx_configure_test_executable(sfx_linux_journal_tests)
        add_executable(sfx_linux_runtime_journal_tests test/platform/linux_runtime_journal_tests.cpp)
        target_link_libraries(sfx_linux_runtime_journal_tests PRIVATE sfx_linux_runtime_journal)
        sfx_configure_test_executable(sfx_linux_runtime_journal_tests)
        add_executable(sfx_linux_backup_snapshot_tests test/platform/linux_backup_snapshot_tests.cpp)
        target_link_libraries(sfx_linux_backup_snapshot_tests PRIVATE sfx_linux_runtime_journal)
        sfx_configure_test_executable(sfx_linux_backup_snapshot_tests)
        add_executable(sfx_linux_time_zone_tests test/platform/linux_time_zone_tests.cpp)
        target_include_directories(sfx_linux_time_zone_tests PRIVATE src/apps/runtime)
        target_link_libraries(sfx_linux_time_zone_tests PRIVATE sfx_linux_time_zone)
        sfx_configure_test_executable(sfx_linux_time_zone_tests)
        add_executable(sfx_linux_ssh_key_tests test/platform/linux_ssh_key_tests.cpp)
        target_link_libraries(sfx_linux_ssh_key_tests PRIVATE sfx_linux_ssh sfx_linux_host_files)
        sfx_configure_test_executable(sfx_linux_ssh_key_tests)
    endif()
    add_library(sfx_local_door_linux STATIC src/platform/linux/src/local_door.cpp)
    target_include_directories(sfx_local_door_linux PUBLIC src/platform/linux/include)
    target_link_libraries(sfx_local_door_linux PUBLIC sfx::application PRIVATE sfx::libsodium sfx_linux_host_files Threads::Threads util)
    sfx_configure_portable_target(sfx_local_door_linux)
    add_library(sfx_linux_ftn_transport STATIC src/platform/linux/src/ftn_crypto.cpp src/platform/linux/src/ftn_native_transport.cpp)
    target_link_libraries(sfx_linux_ftn_transport PUBLIC sfx::application sfx_linux_host_files PRIVATE OpenSSL::SSL)
    sfx_configure_portable_target(sfx_linux_ftn_transport)
    if(BUILD_TESTING)
        add_executable(sfx_linux_ftn_crypto_tests test/platform/linux_ftn_crypto_tests.cpp)
        target_link_libraries(sfx_linux_ftn_crypto_tests PRIVATE sfx_linux_ftn_transport)
        sfx_configure_test_executable(sfx_linux_ftn_crypto_tests)
        add_executable(sfx_linux_ftn_native_transport_tests test/platform/linux_ftn_native_transport_tests.cpp)
        target_link_libraries(sfx_linux_ftn_native_transport_tests PRIVATE sfx_linux_ftn_transport OpenSSL::SSL Threads::Threads)
        sfx_configure_test_executable(sfx_linux_ftn_native_transport_tests)
    endif()
    add_library(sfx_linux_ftn_session STATIC src/platform/linux/src/ftn_installation.cpp src/platform/linux/src/ftn_native_custody.cpp src/platform/linux/src/ftn_native_session.cpp src/platform/linux/src/ftn_secrets.cpp)
    target_link_libraries(sfx_linux_ftn_session PUBLIC sfx_linux_ftn_transport sfx_linux_file_payload sfx_linux_portable_keys PRIVATE sfx::libsodium)
    sfx_configure_portable_target(sfx_linux_ftn_session)
    add_library(sfx_linux_ftn_processing STATIC src/platform/linux/src/ftn_native_toss.cpp src/platform/linux/src/ftn_processed_retention.cpp src/platform/linux/src/ftn_stored_scan.cpp)
    target_link_libraries(sfx_linux_ftn_processing PUBLIC sfx_linux_ftn_session PRIVATE sfx::libsodium sfx::libarchive)
    sfx_configure_portable_target(sfx_linux_ftn_processing)
    if(BUILD_TESTING)
        add_executable(sfx_linux_ftn_native_session_tests test/platform/linux_ftn_native_session_tests.cpp)
        target_link_libraries(sfx_linux_ftn_native_session_tests PRIVATE sfx_linux_ftn_session sfx::storage_sqlite sfx_test_support sfx::libsodium OpenSSL::SSL Threads::Threads)
        sfx_configure_test_executable(sfx_linux_ftn_native_session_tests)
        add_executable(sfx_linux_ftn_native_toss_tests test/platform/linux_ftn_native_toss_tests.cpp)
        add_executable(sfx_linux_ftn_retention_native_tests test/platform/linux_ftn_retention_native_tests.cpp test/support/src/sqlite_write_fault_vfs.cpp)
        target_include_directories(sfx_linux_ftn_retention_native_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_retention_native_tests PRIVATE sfx_linux_ftn_processing sfx::storage_sqlite sfx::libsodium)
        target_link_options(sfx_linux_ftn_retention_native_tests PRIVATE -Wl,--wrap=fsync -Wl,--wrap=close -Wl,--wrap=statx)
        sfx_configure_test_executable(sfx_linux_ftn_retention_native_tests)
        add_executable(sfx_linux_ftn_native_publication_tests test/platform/linux_ftn_native_publication_tests.cpp)
        target_include_directories(sfx_linux_ftn_native_publication_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_native_publication_tests PRIVATE sfx_linux_ftn_processing sfx_linux_file_manager sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_ftn_native_publication_tests)
        add_executable(sfx_linux_ftn_stored_scan_tests test/platform/linux_ftn_stored_scan_tests.cpp)
        target_include_directories(sfx_linux_ftn_stored_scan_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_stored_scan_tests PRIVATE sfx_linux_ftn_processing sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_ftn_stored_scan_tests)
        target_include_directories(sfx_linux_ftn_native_toss_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_native_toss_tests PRIVATE sfx_linux_ftn_processing sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_ftn_native_toss_tests)
        add_executable(sfx_linux_ftn_native_tic_tests test/platform/linux_ftn_native_tic_tests.cpp)
        target_include_directories(sfx_linux_ftn_native_tic_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_native_tic_tests PRIVATE sfx_linux_ftn_processing sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        sfx_configure_test_executable(sfx_linux_ftn_native_tic_tests)
    endif()
    add_library(sfx_linux_gateway STATIC src/apps/runtime/linux_rlogin.cpp src/apps/runtime/linux_gateway_resolver.cpp)
    add_library(sfx_linux_ftn_engine STATIC src/apps/runtime/linux_ftn_engine.cpp)
    target_include_directories(sfx_linux_ftn_engine PUBLIC src/apps/runtime)
    target_link_libraries(sfx_linux_ftn_engine PUBLIC sfx_linux_ftn_processing sfx_linux_time_zone sfx_linux_gateway Threads::Threads sfx_ftn_external_files sfx::libsodium)
    sfx_configure_portable_target(sfx_linux_ftn_engine)
    add_library(sfx_linux_backup_job STATIC src/apps/runtime/linux_backup_job.cpp)
    target_link_libraries(sfx_linux_backup_job PUBLIC sfx_linux_runtime_jobs sfx_linux_file_backup sfx_linux_ftn_engine)
    sfx_configure_portable_target(sfx_linux_backup_job)
    if(BUILD_TESTING)
        add_executable(sfx_linux_ftn_engine_tests test/platform/linux_ftn_engine_tests.cpp)
        target_include_directories(sfx_linux_ftn_engine_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_engine_tests PRIVATE sfx_linux_ftn_engine sfx::storage_sqlite sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_ftn_engine_tests)
        add_executable(sfx_linux_ftn_external_host test/platform/linux_ftn_external_host.cpp)
        target_include_directories(sfx_linux_ftn_external_host PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_external_host PRIVATE sfx_linux_ftn_engine sfx::storage_sqlite sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_ftn_external_host)
    endif()
    target_include_directories(sfx_linux_gateway PUBLIC src/apps/runtime PRIVATE src/platform/common/include)
    target_link_libraries(sfx_linux_gateway PUBLIC sfx::application sfx_linux_host_files PRIVATE OpenSSL::Crypto)
    sfx_configure_portable_target(sfx_linux_gateway)
    if(BUILD_TESTING)
        add_executable(sfx_linux_gateway_resolver_tests test/platform/linux_gateway_resolver_tests.cpp)
        target_link_libraries(sfx_linux_gateway_resolver_tests PRIVATE sfx_linux_gateway)
        sfx_configure_test_executable(sfx_linux_gateway_resolver_tests)
    endif()
    add_library(sfx_linux_caller_engine STATIC src/apps/runtime/linux_caller_engine.cpp)
    target_link_libraries(sfx_linux_caller_engine PUBLIC sfx_linux_runtime_store sfx_linux_caller_session sfx_linux_runtime_door sfx_linux_botgate sfx_linux_gateway sfx_linux_caller_files sfx_linux_file_administration)
    target_link_libraries(sfx_linux_caller_engine PRIVATE sfx::libsodium sfx_linux_qwk_transfer sfx_linux_ftn_engine)
    sfx_configure_portable_target(sfx_linux_caller_engine)
    add_library(sfx_linux_runtime_host STATIC src/apps/runtime/linux_runtime_host.cpp src/apps/runtime/linux_runtime_controls.cpp src/apps/runtime/linux_runtime_observations.cpp src/apps/runtime/linux_node_controls.cpp src/apps/runtime/linux_clamav_worker.cpp src/apps/runtime/external_event_runner.cpp)
    target_sources(sfx_linux_runtime_host PRIVATE src/apps/runtime/linux_system_monitor.cpp)
    target_link_libraries(sfx_linux_runtime_host PUBLIC sfx_linux_caller_engine sfx_linux_file_administration sfx_linux_file_configuration sfx_linux_backup_job)
    sfx_configure_portable_target(sfx_linux_runtime_host)
    add_library(sfx_linux_health STATIC src/platform/linux/src/bbs_health.cpp)
    target_include_directories(sfx_linux_health PRIVATE src/adapters/storage/src)
    target_link_libraries(sfx_linux_health PUBLIC sfx_linux_file_payload sfx_linux_ftn_session sfx::archive sfx::storage_sqlite PRIVATE sfx_sqlite3)
    sfx_configure_portable_target(sfx_linux_health)
    target_link_libraries(sfx_linux_runtime_host PUBLIC sfx_linux_health)
    if(BUILD_TESTING)
        add_executable(sfx_linux_management_catalog_tests test/platform/linux_management_catalog_tests.cpp)
        target_link_libraries(sfx_linux_management_catalog_tests PRIVATE sfx_linux_runtime_host)
        add_executable(sfx_beta12_linux_botgate_tests EXCLUDE_FROM_ALL test/platform/beta12_linux_botgate_tests.cpp)
        target_link_libraries(sfx_beta12_linux_botgate_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_beta12_linux_botgate_tests)
        add_executable(sfx_linux_local_logon_host_tests test/platform/linux_local_logon_host_tests.cpp)
        add_executable(sfx_linux_runtime_controls_tests test/platform/linux_runtime_controls_tests.cpp)
        target_link_libraries(sfx_linux_runtime_controls_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_runtime_controls_tests)
        target_link_libraries(sfx_linux_local_logon_host_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_local_logon_host_tests)
        add_executable(sfx_linux_local_logon_resolver_fixture test/platform/linux_local_logon_resolver_fixture.cpp)
        sfx_configure_test_executable(sfx_linux_local_logon_resolver_fixture)
        add_executable(sfx_linux_capability_fixture test/platform/linux_capability_fixture.cpp)
        sfx_configure_test_executable(sfx_linux_capability_fixture)
        add_executable(sfx_linux_runtime_backup_tests test/platform/linux_runtime_backup_tests.cpp)
        target_link_libraries(sfx_linux_runtime_backup_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_runtime_backup_tests)
        add_executable(sfx_linux_ftn_serial_tests test/integration/ftn_serial_tests.cpp)
        target_sources(sfx_linux_ftn_serial_tests PRIVATE test/support/src/sqlite_write_fault_vfs.cpp)
        target_include_directories(sfx_linux_ftn_serial_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_linux_ftn_serial_tests PRIVATE sfx::storage_sqlite)
        sfx_configure_test_executable(sfx_linux_ftn_serial_tests)
        add_executable(sfx_linux_core_restore_tests test/platform/linux_core_restore_tests.cpp)
        target_link_libraries(sfx_linux_core_restore_tests PRIVATE sfx_linux_runtime_host)
        target_link_libraries(sfx_linux_core_restore_tests PRIVATE sfx_linux_health)
        sfx_configure_test_executable(sfx_linux_core_restore_tests)
        add_executable(sfx_beta1_linux_ftn_history_tests EXCLUDE_FROM_ALL test/platform/beta1_linux_ftn_history_tests.cpp)
        target_include_directories(sfx_beta1_linux_ftn_history_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_beta1_linux_ftn_history_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_beta1_linux_ftn_history_tests)
        add_executable(sfx_linux_ftn_runtime_fixture test/platform/linux_ftn_runtime_fixture.cpp)
        target_link_libraries(sfx_linux_ftn_runtime_fixture PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_ftn_runtime_fixture)
        sfx_configure_test_executable(sfx_linux_management_catalog_tests)
        add_executable(sfx_linux_account_host_tests test/platform/linux_account_host_tests.cpp)
        target_link_libraries(sfx_linux_account_host_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_account_host_tests)
        add_executable(sfx_linux_compaction_tests test/platform/linux_compaction_tests.cpp)
        add_executable(sfx_linux_offline_tests test/platform/linux_offline_tests.cpp)
        target_link_libraries(sfx_linux_offline_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_offline_tests)
        target_link_libraries(sfx_linux_compaction_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_compaction_tests)
        add_executable(sfx_linux_other_board_tests test/platform/linux_other_board_tests.cpp)
        target_link_libraries(sfx_linux_other_board_tests PRIVATE sfx_linux_host_files)
        sfx_configure_test_executable(sfx_linux_other_board_tests)
    endif()
    add_executable(SFXRuntime src/apps/runtime/linux_main.cpp)
    target_link_libraries(SFXRuntime PRIVATE sfx_linux_runtime_host sfx_linux_local_control)
    sfx_configure_portable_target(SFXRuntime)
    include(cmake/SfxLinuxWeb.cmake)
    add_library(sfx_linux_ftn_source STATIC src/apps/runtime/linux_web_ftn_source.cpp src/apps/runtime/linux_web_ftn_download.cpp)
    target_include_directories(sfx_linux_ftn_source PUBLIC src/apps/runtime PRIVATE "${CMAKE_BINARY_DIR}/generated/botgate")
    target_link_libraries(sfx_linux_ftn_source PUBLIC sfx_linux_host_files PRIVATE CURL::libcurl_static OpenSSL::SSL)
    sfx_configure_portable_target(sfx_linux_ftn_source)

    add_library(sfx_linux_web_forms STATIC src/apps/runtime/linux_web_operational.cpp src/apps/runtime/linux_web_messages.cpp src/apps/runtime/linux_web_file_catalog.cpp src/apps/runtime/linux_web_access.cpp src/apps/runtime/linux_web_doors.cpp src/apps/runtime/linux_web_hosting.cpp src/apps/runtime/linux_browser_terminal.cpp src/apps/runtime/linux_web_caller_actions.cpp src/apps/runtime/linux_external_client.cpp src/apps/runtime/linux_web_ftn_transfer.cpp src/apps/runtime/linux_web_ftn_records.cpp src/apps/runtime/linux_web_ftn_drafts.cpp src/apps/runtime/linux_web_ftn_view.cpp src/apps/runtime/linux_web_ftn_import.cpp)
    target_link_libraries(sfx_linux_web_forms PUBLIC sfx_linux_runtime_host sfx_linux_web_dependencies PRIVATE sfx_linux_ftn_source sfx::libsodium)
    target_sources(sfx_linux_web_forms PRIVATE src/apps/runtime/linux_web_ftn_held.cpp)
    target_sources(sfx_linux_web_forms PRIVATE src/apps/runtime/linux_web_binkp.cpp)
    target_sources(sfx_linux_web_forms PRIVATE src/apps/runtime/linux_web_help.cpp)
    target_sources(sfx_linux_web_forms PRIVATE src/apps/runtime/linux_installed_components.cpp)
    target_sources(sfx_linux_web_forms PRIVATE src/apps/runtime/linux_web_sounds.cpp)
    target_sources(sfx_linux_web_forms PRIVATE src/apps/runtime/linux_web_file_manager.cpp)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_linux_dos.py" SFX_DOS_SETUP_HELPER)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_linux_dos.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux_dos_pop22_amd64.json")
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux_dos_pop22_amd64.json" SFX_DOS_SETUP_PROFILE)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_linux_dos.py" SFX_DOS_SETUP_HELPER_SHA256)
    file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/scripts/linux_dos_pop22_amd64.json" SFX_DOS_SETUP_PROFILE_SHA256)
    string(SHA256 SFX_DOS_SETUP_IDENTITY "${SFX_DOS_SETUP_HELPER_SHA256}:${SFX_DOS_SETUP_PROFILE_SHA256}")
    configure_file(cmake/LinuxDosSetupResources.hpp.in generated/linux/LinuxDosSetupResources.hpp @ONLY)
    target_include_directories(sfx_linux_web_forms PRIVATE "${CMAKE_BINARY_DIR}/generated/linux")
    target_sources(sfx_linux_web_forms PRIVATE src/apps/runtime/linux_web_dos_setup.cpp)
    target_link_libraries(sfx_linux_web_forms PRIVATE sfx_local_door_linux)
    sfx_configure_portable_target(sfx_linux_web_forms)
    if(BUILD_TESTING)
        add_executable(sfx_linux_web_forms_tests test/platform/linux_web_forms_tests.cpp)
        target_link_libraries(sfx_linux_web_forms_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_web_forms_tests)
        add_executable(sfx_linux_dos_setup_fixture test/platform/linux_dos_setup_fixture.cpp)
        target_link_libraries(sfx_linux_dos_setup_fixture PRIVATE Threads::Threads)
        sfx_configure_test_executable(sfx_linux_dos_setup_fixture)
        add_executable(sfx_linux_dos_setup_tests test/platform/linux_dos_setup_tests.cpp)
        target_link_libraries(sfx_linux_dos_setup_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_dos_setup_tests)
        add_executable(sfx_linux_dos_wizard_game_runner test/platform/linux_dos_wizard_game_runner.cpp)
        target_link_libraries(sfx_linux_dos_wizard_game_runner PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_dos_wizard_game_runner)
    endif()
    add_executable(SFXWeb src/apps/runtime/linux_web.cpp)
    if(BUILD_TESTING)
        add_executable(sfx_linux_web_sessions_tests test/platform/linux_web_sessions_tests.cpp)
        target_link_libraries(sfx_linux_web_sessions_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_sessions_tests)
        add_executable(sfx_linux_browser_terminal_tests test/platform/linux_browser_terminal_tests.cpp)
        target_link_libraries(sfx_linux_browser_terminal_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_browser_terminal_tests)
        add_executable(sfx_linux_web_access_tests test/platform/linux_web_access_tests.cpp)
        target_link_libraries(sfx_linux_web_access_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_access_tests)
        add_executable(sfx_linux_web_ftn_source_tests test/platform/linux_web_ftn_source_tests.cpp)
        target_link_libraries(sfx_linux_web_ftn_source_tests PRIVATE sfx_linux_ftn_source)
        sfx_configure_test_executable(sfx_linux_web_ftn_source_tests)
        add_executable(sfx_linux_web_ftn_records_tests test/platform/linux_web_ftn_records_tests.cpp)
        add_executable(sfx_linux_web_ftn_held_tests test/platform/linux_web_ftn_held_tests.cpp)
        target_link_libraries(sfx_linux_web_ftn_held_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_web_ftn_held_tests)
        add_executable(sfx_linux_web_ftn_held_fixture test/platform/linux_web_ftn_held_fixture.cpp)
        target_link_libraries(sfx_linux_web_ftn_held_fixture PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_ftn_held_fixture)
        add_executable(sfx_linux_web_ftn_retention_fixture test/platform/linux_web_ftn_retention_fixture.cpp)
        target_link_libraries(sfx_linux_web_ftn_retention_fixture PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_ftn_retention_fixture)
        target_link_libraries(sfx_linux_web_ftn_records_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_web_ftn_records_tests)
        add_executable(sfx_linux_web_ftn_drafts_tests test/platform/linux_web_ftn_drafts_tests.cpp)
        target_link_libraries(sfx_linux_web_ftn_drafts_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_web_ftn_drafts_tests)
        add_executable(sfx_linux_web_ftn_transfer_tests test/platform/linux_web_ftn_transfer_tests.cpp)
        target_link_libraries(sfx_linux_web_ftn_transfer_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_web_ftn_transfer_tests)
        add_executable(sfx_linux_web_caller_actions_tests test/platform/linux_web_caller_actions_tests.cpp)
        add_executable(sfx_linux_web_binkp_tests test/platform/linux_web_binkp_tests.cpp)
        add_executable(sfx_linux_web_file_manager_tests test/platform/linux_web_file_manager_tests.cpp)
        add_executable(sfx_linux_web_sounds_tests test/platform/linux_web_sounds_tests.cpp)
        target_link_libraries(sfx_linux_web_sounds_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_sounds_tests)
        target_link_libraries(sfx_linux_web_file_manager_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_file_manager_tests)
        add_executable(sfx_linux_web_file_manager_fixture test/platform/linux_web_file_manager_fixture.cpp)
        target_link_libraries(sfx_linux_web_file_manager_fixture PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_file_manager_fixture)
        target_link_libraries(sfx_linux_web_binkp_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_binkp_tests)
        add_executable(sfx_linux_web_help_tests test/platform/linux_web_help_tests.cpp)
        target_link_libraries(sfx_linux_web_help_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_web_help_tests)
        add_executable(sfx_linux_system_monitor_tests test/platform/linux_system_monitor_tests.cpp)
        target_link_libraries(sfx_linux_system_monitor_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_system_monitor_tests)
        target_link_libraries(sfx_linux_web_caller_actions_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_web_caller_actions_tests)
        add_executable(sfx_linux_web_hosting_tests test/platform/linux_web_hosting_tests.cpp)
        add_executable(sfx_linux_external_client_tests test/platform/linux_external_client_tests.cpp)
        target_link_libraries(sfx_linux_external_client_tests PRIVATE sfx_linux_web_forms sfx::libsodium)
        sfx_configure_test_executable(sfx_linux_external_client_tests)
        target_link_libraries(sfx_linux_web_hosting_tests PRIVATE sfx_linux_web_forms)
        sfx_configure_test_executable(sfx_linux_web_hosting_tests)
        add_executable(sfx_linux_web_auth_tests test/platform/linux_web_auth_tests.cpp)
        target_link_libraries(sfx_linux_web_auth_tests PRIVATE sfx_linux_runtime_host)
        sfx_configure_test_executable(sfx_linux_web_auth_tests)
    endif()
    target_link_libraries(SFXWeb PRIVATE sfx_linux_web_forms sfx_linux_ftn_source sfx::libsodium)
    sfx_configure_portable_target(SFXWeb)
    # Product target IDs remain stable; only Linux physical output spelling
    # follows the fresh lowercase installation contract.
    foreach(product IN ITEMS SFXRuntime SFXWeb SFXHostLookup SFXBinkD SFXFileTransfer SFXQWKTransfer)
        string(TOLOWER "${product}" native_output)
        set_target_properties(${product} PROPERTIES OUTPUT_NAME "${native_output}")
    endforeach()
    add_library(sfx_linux_runtime_door STATIC src/apps/runtime/linux_local_door.cpp)
    target_include_directories(sfx_linux_runtime_door PUBLIC src/apps/runtime)
    target_link_libraries(sfx_linux_runtime_door PUBLIC sfx_local_door_linux)
    sfx_configure_portable_target(sfx_linux_runtime_door)
    if(BUILD_TESTING)
        add_executable(sfx_linux_door_fixture test/platform/linux_door_fixture.cpp)
        sfx_configure_test_executable(sfx_linux_door_fixture)
        add_executable(sfx_linux_door_tests test/platform/linux_door_tests.cpp)
        target_link_libraries(sfx_linux_door_tests PRIVATE sfx_local_door_linux)
        sfx_configure_test_executable(sfx_linux_door_tests)
        # Explicit unprivileged runner supplies a prepared DOSEMU2 prefix and
        # assembled DOS probe; an unavailable emulator is not a passing test.
        add_executable(sfx_linux_dos_door_tests test/platform/linux_dos_door_tests.cpp)
        target_link_libraries(sfx_linux_dos_door_tests PRIVATE sfx_local_door_linux)
        sfx_configure_test_executable(sfx_linux_dos_door_tests)
        add_executable(sfx_linux_dos_game_runner test/platform/linux_dos_game_runner.cpp)
        target_link_libraries(sfx_linux_dos_game_runner PRIVATE sfx_local_door_linux)
        sfx_configure_test_executable(sfx_linux_dos_game_runner)
    endif()
endif()

if(BUILD_TESTING AND (WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux"))
    add_executable(sfx_shared_telnet_stream_tests test/platform/shared_telnet_stream_tests.cpp)
    target_include_directories(sfx_shared_telnet_stream_tests PRIVATE src/apps/runtime src/platform/common/include)
    target_link_libraries(sfx_shared_telnet_stream_tests PRIVATE sfx::application Threads::Threads)
    if(WIN32)
        target_link_libraries(sfx_shared_telnet_stream_tests PRIVATE ws2_32)
    endif()
    sfx_configure_test_executable(sfx_shared_telnet_stream_tests)
endif()

if(WIN32)
    include(cmake/SfxDeuceSSH.cmake)
    include(cmake/SfxBotGateTLS.cmake)
    add_library(sfx_ftn_download_win32 STATIC src/platform/win32/src/ftn_echo_list_download.cpp)
    target_include_directories(sfx_ftn_download_win32 PUBLIC src/platform/win32/include PRIVATE "${CMAKE_BINARY_DIR}/generated/botgate")
    target_link_libraries(sfx_ftn_download_win32 PUBLIC sfx::application PRIVATE CURL::libcurl_static OpenSSL::SSL)
    target_compile_definitions(sfx_ftn_download_win32 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    sfx_configure_portable_target(sfx_ftn_download_win32)
    add_library(sfx_ftn_crypto_win32 STATIC src/platform/win32/src/ftn_crypto.cpp)
    target_include_directories(sfx_ftn_crypto_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_ftn_crypto_win32 PUBLIC sfx::application PRIVATE OpenSSL::Crypto bcrypt ws2_32 crypt32)
    target_compile_definitions(sfx_ftn_crypto_win32 PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601 WINVER=0x0601)
    sfx_configure_portable_target(sfx_ftn_crypto_win32)
    add_executable(SFXBotGateTLSCheck EXCLUDE_FROM_ALL test/platform/botgate_tls_check.cpp test/platform/botgate_tls_check.rc)
    target_link_libraries(SFXBotGateTLSCheck PRIVATE sfx_botgate_https sfx::application OpenSSL::SSL ws2_32 winhttp)
    target_compile_definitions(SFXBotGateTLSCheck PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
    set_target_properties(SFXBotGateTLSCheck PROPERTIES OUTPUT_NAME "SFX-BotGate-TLS-Check")
    sfx_configure_test_executable(SFXBotGateTLSCheck)

    add_executable(SFXZip src/apps/archive_tools/zip_main.cpp src/apps/archive_tools/archive_tools.rc)
    target_link_libraries(SFXZip PRIVATE sfx::archive)
    set_target_properties(SFXZip PROPERTIES OUTPUT_NAME "zip")
    sfx_configure_test_executable(SFXZip)

    add_executable(SFXUnzip src/apps/archive_tools/unzip_main.cpp src/apps/archive_tools/archive_tools.rc)
    target_link_libraries(SFXUnzip PRIVATE sfx::archive)
    set_target_properties(SFXUnzip PROPERTIES OUTPUT_NAME "unzip")
    sfx_configure_test_executable(SFXUnzip)

    add_library(sfx_service_file_owner_win32 STATIC src/platform/win32/src/service_file_owner.cpp)
    target_include_directories(sfx_service_file_owner_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_service_file_owner_win32 PRIVATE advapi32)
    target_compile_definitions(sfx_service_file_owner_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_service_file_owner_win32)
    add_library(sfx_platform_win32_phase3a STATIC
        src/platform/win32/src/portable_credential_migration.cpp
        src/platform/win32/src/portable_key_store.cpp
        src/platform/win32/src/rlogin_secrets.cpp
        src/platform/win32/src/service_secrets.cpp
        src/platform/win32/src/ftn_secrets.cpp
        src/platform/win32/src/bbs_time_zone_provider.cpp
        src/platform/win32/src/bbs_log_timestamp.cpp
    )
    target_include_directories(sfx_platform_win32_phase3a
        PUBLIC src/platform/win32/include
    )
    target_link_libraries(sfx_platform_win32_phase3a
        PUBLIC sfx::application sfx_portable_credential_codec
        PRIVATE advapi32 crypt32 sfx::libsodium sfx_service_file_owner_win32
    )
    target_compile_definitions(sfx_platform_win32_phase3a PRIVATE
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    sfx_configure_portable_target(sfx_platform_win32_phase3a)
    if(BUILD_TESTING)
        add_executable(sfx_portable_key_store_tests EXCLUDE_FROM_ALL test/platform/portable_key_store_tests.cpp)
        target_link_libraries(sfx_portable_key_store_tests PRIVATE sfx_platform_win32_phase3a)
        sfx_configure_test_executable(sfx_portable_key_store_tests)
        add_executable(sfx_portable_migration_tests EXCLUDE_FROM_ALL test/platform/portable_migration_tests.cpp)
        target_link_libraries(sfx_portable_migration_tests PRIVATE sfx_platform_win32_phase3a)
        sfx_configure_test_executable(sfx_portable_migration_tests)
        add_executable(sfx_ftn_secret_tests test/integration/ftn_secret_tests.cpp)
        add_executable(sfx_ftn_crypto_tests test/integration/ftn_crypto_tests.cpp)
        target_link_libraries(sfx_ftn_crypto_tests PRIVATE sfx_ftn_crypto_win32)
        sfx_configure_test_executable(sfx_ftn_crypto_tests)
        target_link_libraries(sfx_ftn_secret_tests PRIVATE sfx_platform_win32_phase3a)
        sfx_configure_test_executable(sfx_ftn_secret_tests)
    endif()

    add_library(sfx_platform_win32_display STATIC
        src/platform/win32/src/trusted_display_asset_reader.cpp
    )
    target_include_directories(sfx_platform_win32_display
        PUBLIC src/platform/win32/include
    )
    target_link_libraries(sfx_platform_win32_display PUBLIC sfx::application)
    target_compile_definitions(sfx_platform_win32_display PRIVATE
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    sfx_configure_portable_target(sfx_platform_win32_display)

    add_library(sfx_platform_win32_storage STATIC
        src/platform/win32/src/secure_identifier_generator.cpp
        src/platform/win32/src/storage_dacl_policy.cpp
        src/platform/win32/src/backup_read_access.cpp
        src/platform/win32/src/storage_access.cpp
        src/platform/win32/src/storage_path_policy.cpp
    )
    target_include_directories(sfx_platform_win32_storage PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_platform_win32_storage PUBLIC sfx::storage_sqlite)
    target_link_libraries(sfx_platform_win32_storage PRIVATE advapi32 authz bcrypt sfx_service_file_owner_win32)
    target_compile_definitions(sfx_platform_win32_storage PRIVATE
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    sfx_configure_portable_target(sfx_platform_win32_storage)

    add_library(sfx_installation_win32 STATIC src/platform/win32/src/installation_security.cpp src/platform/win32/src/door_component.cpp src/platform/win32/src/starter_doors.cpp src/platform/win32/src/service_mode.cpp)
    target_link_libraries(sfx_installation_win32 PRIVATE shell32 sfx_platform_win32_storage)
    target_sources(sfx_installation_win32 PRIVATE src/platform/win32/src/portable_installation.cpp)
    target_include_directories(sfx_installation_win32 PUBLIC src/platform/win32/include)
    target_compile_definitions(sfx_installation_win32 PRIVATE
        WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

    add_library(sfx_file_payload_win32 STATIC src/platform/win32/src/file_payload_store.cpp src/platform/win32/src/file_receive_budget.cpp)
    target_sources(sfx_file_payload_win32 PRIVATE src/platform/win32/src/door_bulletin_sources.cpp)
    target_include_directories(sfx_file_payload_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_payload_win32 PUBLIC sfx::application sfx_installation_win32 sfx_platform_win32_storage PRIVATE sfx::libsodium)
    target_sources(sfx_file_payload_win32 PRIVATE src/platform/win32/src/portable_file_bindings.cpp)
    target_link_libraries(sfx_file_payload_win32 PRIVATE sfx_platform_win32_phase3a)
    target_link_libraries(sfx_file_payload_win32 PUBLIC sfx_platform_win32_display)
    add_executable(sfx_door_bulletin_source_tests EXCLUDE_FROM_ALL test/platform/door_bulletin_source_tests.cpp)
    target_link_libraries(sfx_door_bulletin_source_tests PRIVATE sfx_file_payload_win32 advapi32)
    target_compile_definitions(sfx_door_bulletin_source_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_test_executable(sfx_door_bulletin_source_tests)
    add_executable(sfx_portable_file_binding_tests EXCLUDE_FROM_ALL test/platform/portable_file_binding_tests.cpp)
    target_link_libraries(sfx_portable_file_binding_tests PRIVATE sfx_file_payload_win32 sfx_platform_win32_phase3a)
    sfx_configure_test_executable(sfx_portable_file_binding_tests)
    target_compile_definitions(sfx_file_payload_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_payload_win32)
    add_library(sfx_ftn_native_win32 STATIC src/platform/win32/src/ftn_stored_scan.cpp src/platform/win32/src/ftn_native_custody.cpp src/platform/win32/src/ftn_native_transport.cpp src/platform/win32/src/ftn_native_session.cpp src/platform/win32/src/ftn_native_toss.cpp)
    target_sources(sfx_ftn_native_win32 PRIVATE src/platform/win32/src/ftn_processed_retention.cpp)
    target_include_directories(sfx_ftn_native_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_ftn_native_win32 PUBLIC sfx_file_payload_win32 PRIVATE sfx_ftn_crypto_win32 sfx_platform_win32_phase3a sfx::libsodium sfx::libarchive bcrypt ws2_32 crypt32 OpenSSL::SSL OpenSSL::Crypto)
    target_compile_definitions(sfx_ftn_native_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_ftn_native_win32)
    add_library(sfx_file_scanner_win32 STATIC src/platform/win32/src/file_scanner.cpp src/platform/win32/src/clamav_resources.cpp src/platform/win32/src/clamav_host.cpp)
    target_include_directories(sfx_file_scanner_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_scanner_win32 PUBLIC sfx_file_payload_win32)
    target_compile_definitions(sfx_file_scanner_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_scanner_win32)
    add_library(sfx_clamav_component_win32 STATIC src/platform/win32/src/clamav_installation.cpp src/platform/win32/src/clamav_update.cpp)
    target_include_directories(sfx_clamav_component_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_clamav_component_win32 PUBLIC sfx_file_scanner_win32 sfx::archive)
    target_compile_definitions(sfx_clamav_component_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_clamav_component_win32)
    add_library(sfx_file_intake_win32 STATIC src/platform/win32/src/file_intake.cpp)
    target_include_directories(sfx_file_intake_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_intake_win32 PUBLIC sfx_file_payload_win32 sfx::archive)
    target_compile_definitions(sfx_file_intake_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_intake_win32)
    add_library(sfx_file_manager_win32 STATIC src/platform/win32/src/file_manager.cpp)
    target_include_directories(sfx_file_manager_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_manager_win32 PUBLIC sfx_file_payload_win32 sfx::archive)
    target_compile_definitions(sfx_file_manager_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_manager_win32)
    target_link_libraries(sfx_installation_win32 PRIVATE advapi32 authz bcrypt)
    sfx_configure_portable_target(sfx_installation_win32)

    add_library(sfx_alpha_win32 STATIC
        src/platform/win32/src/alpha_runtime_platform.cpp
        src/platform/win32/src/service_secret_bridge.cpp
        src/platform/win32/src/bbs_health.cpp
        src/platform/win32/src/service_mode_control.cpp
        src/platform/win32/src/portable_preparation.cpp
        src/platform/win32/src/backup_collections.cpp
        src/platform/win32/src/durable_journal_files.cpp
        src/platform/win32/src/control_panel_metrics.cpp
        src/platform/win32/src/botgate_rule_files.cpp
        src/platform/win32/src/other_board_text_file.cpp
    )
    target_include_directories(sfx_alpha_win32 PUBLIC src/platform/win32/include)
    target_include_directories(sfx_alpha_win32 PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
    target_link_libraries(sfx_alpha_win32
        PUBLIC sfx::application sfx_platform_win32_storage sfx_installation_win32
        PRIVATE sfx::archive sfx_platform_win32_phase3a sfx_file_payload_win32 sfx_clamav_component_win32 advapi32 authz bcrypt iphlpapi pdh ws2_32 comctl32 shell32)
    add_executable(sfx_section16_health_tests EXCLUDE_FROM_ALL test/platform/section16_health_tests.cpp)
    add_executable(sfx_portable_preparation_tests EXCLUDE_FROM_ALL test/platform/portable_preparation_tests.cpp)
    add_executable(sfx_rc8_installed_fixture EXCLUDE_FROM_ALL test/platform/rc8_installed_fixture.cpp)
    target_link_libraries(sfx_rc8_installed_fixture PRIVATE sfx_alpha_win32 sfx::storage_sqlite sfx_platform_win32_phase3a)
    sfx_configure_test_executable(sfx_rc8_installed_fixture)
    add_executable(sfx_section16_installed_fixture EXCLUDE_FROM_ALL test/platform/section16_installed_fixture.cpp)
    target_link_libraries(sfx_section16_installed_fixture PRIVATE sfx_alpha_win32 sfx::storage_sqlite sfx_platform_win32_phase3a)
    sfx_configure_test_executable(sfx_section16_installed_fixture)
    target_link_libraries(sfx_portable_preparation_tests PRIVATE sfx_alpha_win32 sfx_portable_credential_codec)
    sfx_configure_test_executable(sfx_portable_preparation_tests)
    target_include_directories(sfx_section16_health_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_section16_health_tests PRIVATE sfx_alpha_win32)
    sfx_configure_test_executable(sfx_section16_health_tests)
    add_executable(sfx_portable_file_journal_tests EXCLUDE_FROM_ALL test/platform/portable_file_journal_tests.cpp)
    target_include_directories(sfx_portable_file_journal_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_portable_file_journal_tests PRIVATE sfx_file_manager_win32 sfx_file_intake_win32 sfx::storage_sqlite sfx_platform_win32_phase3a)
    sfx_configure_test_executable(sfx_portable_file_journal_tests)
    target_compile_definitions(sfx_alpha_win32 PRIVATE
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    if(NOT SFX_ALPHA_TEST_INSTALL_ROOT STREQUAL "")
        file(TO_CMAKE_PATH "${SFX_ALPHA_TEST_INSTALL_ROOT}" SFX_ALPHA_TEST_ROOT_NORMALIZED)
        target_compile_definitions(sfx_alpha_win32 PRIVATE
            "SFX_ALPHA_TEST_INSTALL_ROOT=L\"${SFX_ALPHA_TEST_ROOT_NORMALIZED}\"")
    endif()
    sfx_configure_portable_target(sfx_alpha_win32)

    add_library(sfx_file_backup_win32 STATIC src/platform/win32/src/file_backup.cpp)
    target_include_directories(sfx_file_backup_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_backup_win32 PUBLIC sfx_file_payload_win32 sfx_alpha_win32 sfx::archive PRIVATE sfx_sqlite3)
    target_compile_definitions(sfx_file_backup_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_backup_win32)

    add_library(sfx_file_configuration_win32 STATIC src/platform/win32/src/file_configuration.cpp)
    target_include_directories(sfx_file_configuration_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_configuration_win32 PUBLIC sfx_file_payload_win32 sfx_file_manager_win32 PRIVATE sfx::libsodium)
    target_compile_definitions(sfx_file_configuration_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_configuration_win32)
    add_library(sfx_file_administration_win32 STATIC src/platform/win32/src/file_administration.cpp src/platform/win32/src/file_owner_view.cpp)
    target_include_directories(sfx_file_administration_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_administration_win32 PUBLIC sfx_file_manager_win32 sfx_file_intake_win32 sfx_ftn_native_win32)
    target_compile_definitions(sfx_file_administration_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_administration_win32)

    add_library(sfx_account_administration_win32 STATIC
        src/platform/win32/src/offline_account_administration.cpp
        src/platform/win32/src/offline_file_administration.cpp)
    target_include_directories(sfx_account_administration_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_account_administration_win32 PUBLIC sfx_alpha_win32 sfx::authentication sfx_platform_win32_phase3a sfx::archive PRIVATE sfx_file_configuration_win32 sfx_file_administration_win32 sfx_file_backup_win32)
    target_compile_definitions(sfx_account_administration_win32 PRIVATE
        WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_account_administration_win32)
    add_library(sfx_file_administration_client_win32 STATIC src/platform/win32/src/file_administration_client.cpp)
    target_include_directories(sfx_file_administration_client_win32 PUBLIC src/platform/win32/include)
    target_link_libraries(sfx_file_administration_client_win32 PUBLIC sfx_account_administration_win32 sfx_alpha_win32 user32)
    target_compile_definitions(sfx_file_administration_client_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_administration_client_win32)
    add_library(sfx_file_manager_client_model_win32 STATIC src/apps/file_manager/client_worker.cpp)
    target_include_directories(sfx_file_manager_client_model_win32 PUBLIC src/apps/file_manager)
    target_link_libraries(sfx_file_manager_client_model_win32 PUBLIC sfx_file_administration_client_win32)
    target_compile_definitions(sfx_file_manager_client_model_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_manager_client_model_win32)
    add_library(sfx_file_manager_widgets_win32 STATIC src/apps/file_manager/dialogs.cpp src/apps/file_manager/preview_window.cpp)
    target_include_directories(sfx_file_manager_widgets_win32 PUBLIC src/apps/file_manager)
    target_link_libraries(sfx_file_manager_widgets_win32 PUBLIC sfx_file_manager_client_model_win32 sfx_desktop_theme_win32 comctl32 shell32 ole32 uuid gdi32)
    target_compile_definitions(sfx_file_manager_widgets_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    sfx_configure_portable_target(sfx_file_manager_widgets_win32)

    add_library(sfx_alpha_runtime_host STATIC
        src/apps/runtime/alpha_ftn_engine.cpp
        src/apps/runtime/alpha_rlogin.cpp
        src/apps/runtime/alpha_rlogin_resolver.cpp
        src/apps/runtime/alpha_message_receive.cpp
        src/apps/runtime/alpha_qwk_transfer.cpp
        src/apps/runtime/alpha_file_transfer.cpp
        src/apps/runtime/alpha_caller_files.cpp
        src/apps/runtime/alpha_file_viewer.cpp
        src/apps/runtime/alpha_qwk_reply_spool.cpp
        src/apps/runtime/alpha_ssh_stream.cpp
        src/apps/runtime/alpha_botgate_feed.cpp
        src/apps/runtime/alpha_verified_domain.cpp
        src/apps/runtime/alpha_telnet_listener.cpp
        src/apps/runtime/alpha_local_door.cpp
        src/apps/runtime/alpha_hostname_lookup.cpp
        src/apps/runtime/alpha_operational_journal.cpp
        src/apps/runtime/alpha_durable_journal.cpp
        src/apps/runtime/alpha_runtime_host.cpp
        src/apps/runtime/external_event_runner.cpp
    )
    target_include_directories(sfx_alpha_runtime_host PUBLIC src/apps/runtime src/platform/common/include)
    target_link_libraries(sfx_alpha_runtime_host PRIVATE deucessh::static bcrypt shell32 sfx_ftn_native_win32 sfx_ftn_external_files sfx::libsodium)
    target_link_libraries(sfx_alpha_runtime_host
        PUBLIC sfx::authentication sfx::storage_sqlite sfx_alpha_win32
            sfx_platform_win32_phase3a
        PRIVATE sfx::archive sfx::libsodium sfx_platform_win32_display sfx_botgate_https sfx_file_intake_win32 sfx_file_payload_win32 sfx_file_scanner_win32 sfx_clamav_component_win32 sfx_file_configuration_win32 sfx_file_administration_win32 sfx_file_backup_win32 ws2_32 winhttp)
    target_compile_definitions(sfx_alpha_runtime_host PRIVATE
        _WIN32_WINNT=0x0601
        WINVER=0x0601
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
    )
    sfx_configure_portable_target(sfx_alpha_runtime_host)

    add_executable(SFXRuntime WIN32
        src/apps/runtime/main.cpp
        src/apps/runtime/runtime.rc
        src/apps/runtime/runtime.manifest
    )
    target_link_libraries(SFXRuntime PRIVATE sfx_alpha_runtime_host shell32)
    set_target_properties(SFXRuntime PROPERTIES OUTPUT_NAME "SFXRuntime")
    sfx_configure_win32_product(SFXRuntime)

    add_library(sfx_desktop_theme_win32 STATIC src/platform/win32/src/desktop_theme.cpp
        src/platform/win32/src/editor_placement.cpp)
    target_include_directories(sfx_desktop_theme_win32 PUBLIC src/platform/win32/include)
    target_compile_definitions(sfx_desktop_theme_win32 PRIVATE
        WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(sfx_desktop_theme_win32 PRIVATE advapi32 comctl32 uxtheme)
    sfx_configure_portable_target(sfx_desktop_theme_win32)

    add_executable(SFXControlPanel WIN32
        src/apps/control_panel/main.cpp
        src/apps/control_panel/control_panel.rc
        src/apps/control_panel/control_panel.manifest
    )
    add_library(sfx_event_sound_win32 STATIC src/platform/win32/src/event_sound_player.cpp)
    target_include_directories(sfx_event_sound_win32 PUBLIC src/platform/win32/include)
    target_compile_definitions(sfx_event_sound_win32 PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    target_link_libraries(sfx_event_sound_win32 PRIVATE winmm)
    sfx_configure_portable_target(sfx_event_sound_win32)
    target_include_directories(SFXControlPanel PRIVATE src/apps/control_panel)
    target_link_libraries(SFXControlPanel PRIVATE
        sfx_alpha_win32 sfx_desktop_theme_win32 sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
    if(SFX_ALPHA_TEST_AUTOMATION)
        target_compile_definitions(SFXControlPanel PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
        target_compile_definitions(SFXRuntime PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
        target_compile_definitions(sfx_alpha_runtime_host PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
    endif()
    set_target_properties(SFXControlPanel PROPERTIES OUTPUT_NAME "SFXControlPanel")
    sfx_configure_win32_product(SFXControlPanel)

    add_executable(SFXConfig WIN32
        src/apps/config_ui/main.cpp
        src/apps/config_ui/door_bulletin_window.cpp
        src/apps/config_ui/config_ui.rc
        src/apps/config_ui/config_ui.manifest
    )
    target_include_directories(SFXConfig PRIVATE src/apps/config_ui)
    target_link_libraries(SFXConfig PRIVATE
        sfx_alpha_win32 sfx_desktop_theme_win32 sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive sfx_account_administration_win32
        sfx_clamav_component_win32 comctl32 comdlg32 shell32 advapi32 ole32 uuid)
    if(SFX_ALPHA_TEST_AUTOMATION)
        target_compile_definitions(SFXConfig PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
    endif()
    set_target_properties(SFXConfig PROPERTIES OUTPUT_NAME "SFXConfig")
    sfx_configure_win32_product(SFXConfig)
    target_link_libraries(SFXConfig PRIVATE sfx_ftn_download_win32)
    target_link_libraries(SFXConfig PRIVATE sfx_file_manager_widgets_win32)

    add_executable(SFXFileManager WIN32
        src/apps/file_manager/main.cpp
        src/apps/file_manager/window.cpp
        src/apps/file_manager/file_manager.rc
        src/apps/file_manager/file_manager.manifest)
    target_include_directories(SFXFileManager PRIVATE src/apps/file_manager)
    target_link_libraries(SFXFileManager PRIVATE sfx_file_manager_widgets_win32 sfx_platform_win32_phase3a bcrypt)
    sfx_configure_win32_product(SFXFileManager)
    add_executable(SFXOldTestCleanup EXCLUDE_FROM_ALL src/apps/setup/old_test_cleanup_main.cpp)
    target_link_libraries(SFXOldTestCleanup PRIVATE sfx_account_administration_win32 sfx::archive ole32)
    set_target_properties(SFXOldTestCleanup PROPERTIES OUTPUT_NAME "SFX-OldTestCleanup")
    sfx_configure_test_executable(SFXOldTestCleanup)
    add_executable(sfx_old_test_cleanup_seed EXCLUDE_FROM_ALL test/platform/old_test_cleanup_seed.cpp)
    target_compile_definitions(sfx_old_test_cleanup_seed PRIVATE SFX_RC16_QUALIFICATION=1)
    target_link_libraries(sfx_old_test_cleanup_seed PRIVATE sfx_account_administration_win32 sfx::archive ole32)
    sfx_configure_test_executable(sfx_old_test_cleanup_seed)

    add_executable(SFXMaintenance WIN32 src/apps/setup/maintenance_main.cpp
        src/apps/setup/clamav_maintenance.cpp src/apps/setup/clamav_download.cpp
        src/apps/setup/maintenance.rc src/apps/setup/setup.manifest)
    target_link_libraries(SFXMaintenance PRIVATE sfx_alpha_win32 sfx_clamav_component_win32
        sfx_file_payload_win32 shell32 advapi32 winhttp)
    sfx_configure_win32_product(SFXMaintenance)
    function(sfx_clamav_https target)
        target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated/botgate")
        target_link_libraries(${target} PRIVATE CURL::libcurl_static OpenSSL::SSL)
    endfunction()
    sfx_clamav_https(SFXMaintenance)

    set(SFX_SETUP_PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/setup-payload/$<CONFIG>")
    # External matched DOS component. Its GPL source/notices remain separate;
    # neither Runtime nor Config links this executable or the VDD.
    set(SFX_DOS_DRIVER "${CMAKE_CURRENT_SOURCE_DIR}/build/sfxsys-r2/V-T/SFXSYS/SYSTEM32/SFXEXEC.DLL" CACHE FILEPATH "Exact reviewed SFXEXEC core")
    set(SFX_DOS_LAUNCHER "${CMAKE_CURRENT_SOURCE_DIR}/build/sfxexec-dos-compile-normal-r1/drive/SRC/DOSXTRN/SFXDOOR.EXE" CACHE FILEPATH "Matched 16-bit DOS launcher")
    set(SFX_DOS_PAIR "${CMAKE_CURRENT_BINARY_DIR}/dos-pair")
    file(MAKE_DIRECTORY "${SFX_DOS_PAIR}")
    add_custom_command(OUTPUT "${SFX_DOS_PAIR}/pair_hashes.hpp"
        COMMAND ${CMAKE_COMMAND} "-DRUNTIME=$<TARGET_FILE:SFXRuntime>" "-DDRIVER=${SFX_DOS_DRIVER}" "-DLAUNCHER=${SFX_DOS_LAUNCHER}"
            "-DOUTPUT=${SFX_DOS_PAIR}/pair_hashes.hpp" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateDoorPair.cmake"
        DEPENDS SFXRuntime "${SFX_DOS_DRIVER}" "${SFX_DOS_LAUNCHER}" cmake/GenerateDoorPair.cmake VERBATIM)
    add_executable(SFXDOSHost components/sfxexec/host/bridge.cpp "${SFX_DOS_PAIR}/pair_hashes.hpp")
    target_include_directories(SFXDOSHost PRIVATE components/sfxexec/host src/core/include "${SFX_DOS_PAIR}")
    target_compile_definitions(SFXDOSHost PRIVATE SFX_NORMAL_HOST=1 _WIN32_WINNT=0x0601 WINVER=0x0601)
    target_compile_options(SFXDOSHost PRIVATE /W4 /O2 /Gy /arch:IA32 /Brepro)
    target_link_options(SFXDOSHost PRIVATE /SUBSYSTEM:CONSOLE,6.01 /DYNAMICBASE /NXCOMPAT /Brepro /OPT:REF)
    target_link_libraries(SFXDOSHost PRIVATE ws2_32 advapi32 iphlpapi bcrypt)
    # External GPL executable, never linked into SFX. Its fixed reproducible
    # source/build materials are shipped separately with the owner media.
    set(SFX_MESSAGE_HELPER "${CMAKE_CURRENT_SOURCE_DIR}/build/section8b-helper-empty-batch/r4/sexyz.exe")
    set(SFX_MESSAGE_HELPER_STAMP "${CMAKE_CURRENT_SOURCE_DIR}/build/section8b-helper-empty-batch/r4-built.stamp")
    file(GLOB_RECURSE SFX_MESSAGE_HELPER_INPUTS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/section8b-helper-empty-batch/*")
    # Keep the MSBuild output directory outside the script-owned unique run.
    add_custom_command(OUTPUT "${SFX_MESSAGE_HELPER_STAMP}"
        COMMAND pwsh.exe -NoProfile -File
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsureMessageHelper.ps1"
            -Repository "${CMAKE_CURRENT_SOURCE_DIR}" -Stamp "${SFX_MESSAGE_HELPER_STAMP}"
        DEPENDS ${SFX_MESSAGE_HELPER_INPUTS}
            "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/evidence/section8b-helper-empty-batch-20261009/r1/source-materials.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/evidence/section8b-helper-empty-batch-20261009/r1/DR_EXACT_HELPER_REVIEW.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsureMessageHelper.ps1"
        VERBATIM)
    add_custom_target(SFXMessageHelper DEPENDS "${SFX_MESSAGE_HELPER_STAMP}")
    set(SFX_PACKET_HELPER "${CMAKE_CURRENT_SOURCE_DIR}/build/rc10-helper-packet/r2/SFXQWKTransfer.exe")
    set(SFX_PACKET_HELPER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/packet-helper-bound.stamp")
    file(GLOB_RECURSE SFX_PACKET_HELPER_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/rc10-packet-helper/*")
    add_custom_command(OUTPUT "${SFX_PACKET_HELPER_STAMP}"
        COMMAND pwsh.exe -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsurePacketHelper.ps1"
            -Repository "${CMAKE_CURRENT_SOURCE_DIR}" -Stamp "${SFX_PACKET_HELPER_STAMP}"
        DEPENDS ${SFX_PACKET_HELPER_INPUTS} "${SFX_PACKET_HELPER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/rc10-helper-packet/r2/manifest.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/rc10-helper-packet-tests/r4/qualification.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsurePacketHelper.ps1"
        VERBATIM)
    add_custom_target(SFXPacketHelper DEPENDS "${SFX_PACKET_HELPER_STAMP}")
    set(SFX_FILE_HELPER "${CMAKE_CURRENT_SOURCE_DIR}/build/section10-helper-file/r4/SFXFileTransfer.exe")
    set(SFX_FILE_HELPER_STAMP "${CMAKE_CURRENT_BINARY_DIR}/file-helper-bound.stamp")
    file(GLOB_RECURSE SFX_FILE_HELPER_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/section10-file-helper/*")
    add_custom_command(OUTPUT "${SFX_FILE_HELPER_STAMP}"
        COMMAND pwsh.exe -NoProfile -File "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsureFileHelper.ps1"
            -Repository "${CMAKE_CURRENT_SOURCE_DIR}" -Stamp "${SFX_FILE_HELPER_STAMP}"
        DEPENDS ${SFX_FILE_HELPER_INPUTS} "${SFX_FILE_HELPER}"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/section10-helper-file/r4/manifest.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/section10-g-startup-r2/result.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/section10-helper-tests/r5/result.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/build/section10-helper-zregression/r3/result.json"
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/EnsureFileHelper.ps1"
        VERBATIM)
    add_custom_target(SFXFileHelper DEPENDS "${SFX_FILE_HELPER_STAMP}")
    set(SFX_STOCK_MENU_ASSETS)
    foreach(base main main999 file msg your chat bullet1 batch)
        list(APPEND SFX_STOCK_MENU_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/${base}.ans"
            "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/${base}.asc")
    endforeach()
    list(APPEND SFX_STOCK_MENU_ASSETS "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/bulletin.ans"
        "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/bulletin.asc" "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/bullet2.asc"
        "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/qwk.ans" "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/qwk.asc"
        "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/sysop.ans" "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/sysop.asc"
        "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/chat-emote.txt"
        "${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/spitfire-x.ans")
    file(GLOB_RECURSE SFX_SYSOP_DOCS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/docs/sysop/*")
    file(GLOB_RECURSE SFX_CORE_NOTICE_INPUTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/distribution/ThirdParty/*")
    add_custom_command(OUTPUT "${SFX_SETUP_PAYLOAD}/payload.rc"
        COMMAND ${CMAKE_COMMAND}
            "-DCONTROL_PANEL=$<TARGET_FILE:SFXControlPanel>"
            "-DRUNTIME=$<TARGET_FILE:SFXRuntime>"
            "-DZIP=$<TARGET_FILE:SFXZip>"
            "-DUNZIP=$<TARGET_FILE:SFXUnzip>"
            "-DCONFIG=$<TARGET_FILE:SFXConfig>"
            "-DMESSAGE_HELPER=${SFX_MESSAGE_HELPER}"
            "-DPACKET_HELPER=${SFX_PACKET_HELPER}"
            "-DFILE_HELPER=${SFX_FILE_HELPER}"
            "-DFILE_MANAGER=$<TARGET_FILE:SFXFileManager>"
            "-DMAINTENANCE=$<TARGET_FILE:SFXMaintenance>"
            "-DDOS_HOST=$<TARGET_FILE:SFXDOSHost>" "-DDOS_LAUNCHER=${SFX_DOS_LAUNCHER}" "-DDOS_DRIVER=${SFX_DOS_DRIVER}"
            "-DDOS_INI=${CMAKE_CURRENT_SOURCE_DIR}/distribution/sfxexec.ini"
            "-DBOTGATE_ART=${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/bg-prelog.ans"
            "-DSTOCK_MENU_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/distribution"
            "-DSTARTER_DOOR_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/doors/door-workshop/outputs"
            "-DOUTPUT_DIRECTORY=${SFX_SETUP_PAYLOAD}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSetupPayload.cmake"
        DEPENDS SFXControlPanel SFXRuntime SFXZip SFXUnzip SFXConfig SFXFileManager SFXMaintenance SFXDOSHost "${SFX_DOS_LAUNCHER}" "${SFX_DOS_DRIVER}" distribution/sfxexec.ini SFXMessageHelper SFXPacketHelper SFXFileHelper cmake/GenerateSetupPayload.cmake distribution/Display/bg-prelog.ans
            distribution/stock-menu-manifest.json ${SFX_STOCK_MENU_ASSETS}
            cmake/GenerateStarterDoors.cmake cmake/GenerateSysopDocs.cmake ${SFX_SYSOP_DOCS}
            distribution/ThirdParty/manifest.json distribution/windows/THIRD_PARTY.txt ${SFX_CORE_NOTICE_INPUTS}
            distribution/windows/sysop-docs-manifest.json
            distribution/windows/README.txt src/apps/product_version.h
            doors/door-workshop/outputs/SFXGuess-a-Number-1.0.1-Win32-Win7-test-r1.zip
            doors/door-workshop/outputs/newswire-x-rss/0.1.0-rc1/NewsWire-X-RSS-0.1.0-rc1-Win32-install-ca20261105-r1.zip
        VERBATIM)
    add_executable(SFXSetup WIN32
        src/apps/setup/main.cpp src/apps/setup/setup_engine.cpp src/apps/setup/clamav_download.cpp
        src/apps/setup/setup.rc src/apps/setup/setup.manifest
        "${SFX_SETUP_PAYLOAD}/payload.rc")
    target_link_libraries(SFXSetup PRIVATE
        sfx_alpha_win32 sfx_platform_win32_phase3a sfx_file_payload_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version comdlg32 winhttp)
    target_include_directories(SFXSetup PRIVATE "${SFX_SETUP_PAYLOAD}")
    set_target_properties(SFXSetup PROPERTIES OUTPUT_NAME "SFX-BBS-Setup")
    sfx_configure_win32_product(SFXSetup)
    sfx_clamav_https(SFXSetup)

    if(BUILD_TESTING)
        add_executable(sfx_section9_install_qualification EXCLUDE_FROM_ALL
            test/platform/section9_install_qualification.cpp src/apps/setup/setup_engine.cpp src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_section9_install_qualification PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        add_executable(sfx_ftn_setup_tests EXCLUDE_FROM_ALL
            test/platform/ftn_setup_tests.cpp src/apps/setup/setup_engine.cpp src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_ftn_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_ftn_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_ftn_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_setup_tests)
        sfx_clamav_https(sfx_ftn_setup_tests)
        target_link_libraries(sfx_section9_install_qualification PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        add_executable(sfx_clamav_download_tests EXCLUDE_FROM_ALL test/integration/clamav_download_tests.cpp src/apps/setup/clamav_download.cpp)
        target_include_directories(sfx_clamav_download_tests PRIVATE src/apps/setup)
        target_link_libraries(sfx_clamav_download_tests PRIVATE sfx_file_payload_win32 sfx_clamav_component_win32 winhttp)
        target_compile_definitions(sfx_clamav_download_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_download_tests)
        sfx_clamav_https(sfx_clamav_download_tests)
        add_executable(sfx_clamav_https_tests EXCLUDE_FROM_ALL test/platform/clamav_https_tests.cpp src/apps/setup/clamav_download.cpp)
        target_include_directories(sfx_clamav_https_tests PRIVATE src/apps/setup)
        target_link_libraries(sfx_clamav_https_tests PRIVATE sfx_file_payload_win32 sfx_clamav_component_win32)
        target_compile_definitions(sfx_clamav_https_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_https_tests)
        sfx_clamav_https(sfx_clamav_https_tests)
        target_compile_definitions(sfx_section9_install_qualification PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section9_install_qualification)
        sfx_clamav_https(sfx_section9_install_qualification)
        add_executable(sfx_section9_file_backup_tests EXCLUDE_FROM_ALL test/platform/section9_file_backup_tests.cpp)
        target_link_libraries(sfx_section9_file_backup_tests PRIVATE sfx_account_administration_win32 sfx_file_backup_win32)
        target_compile_definitions(sfx_section9_file_backup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section9_file_backup_tests)
        add_executable(sfx_section16_backup_restore_tests EXCLUDE_FROM_ALL test/platform/section16_backup_restore_tests.cpp)
        target_link_libraries(sfx_section16_backup_restore_tests PRIVATE sfx_account_administration_win32 sfx_file_backup_win32)
        target_compile_definitions(sfx_section16_backup_restore_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section16_backup_restore_tests)
        add_executable(sfx_file_backup_chain_tests EXCLUDE_FROM_ALL test/platform/file_backup_chain_tests.cpp)
        target_link_libraries(sfx_file_backup_chain_tests PRIVATE sfx_alpha_win32 sfx::archive)
        target_compile_definitions(sfx_file_backup_chain_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_backup_chain_tests)
        set(SFX_QUALIFICATION_MEDIA "${CMAKE_CURRENT_SOURCE_DIR}/build/section4-owner/Release" CACHE PATH "Matched shipping images for isolated Setup qualification")
        set(SFX_QUALIFICATION_PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/qualification-payload")
        add_custom_command(OUTPUT "${SFX_QUALIFICATION_PAYLOAD}/payload.rc"
            COMMAND ${CMAKE_COMMAND}
                "-DCONTROL_PANEL=${SFX_QUALIFICATION_MEDIA}/SFXControlPanel.exe"
                "-DRUNTIME=${SFX_QUALIFICATION_MEDIA}/SFXRuntime.exe"
                "-DZIP=${SFX_QUALIFICATION_MEDIA}/zip.exe"
                "-DUNZIP=${SFX_QUALIFICATION_MEDIA}/unzip.exe"
                "-DCONFIG=${SFX_QUALIFICATION_MEDIA}/SFXConfig.exe"
                "-DMESSAGE_HELPER=${SFX_MESSAGE_HELPER}"
                "-DPACKET_HELPER=${SFX_PACKET_HELPER}"
                "-DBOTGATE_ART=${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/bg-prelog.ans"
                "-DSTOCK_MENU_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/distribution"
                "-DOUTPUT_DIRECTORY=${SFX_QUALIFICATION_PAYLOAD}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSetupPayload.cmake"
            DEPENDS SFXMessageHelper SFXPacketHelper "${SFX_QUALIFICATION_MEDIA}/SFXControlPanel.exe"
                "${SFX_QUALIFICATION_MEDIA}/SFXRuntime.exe" "${SFX_QUALIFICATION_MEDIA}/SFXConfig.exe"
                "${SFX_QUALIFICATION_MEDIA}/zip.exe" "${SFX_QUALIFICATION_MEDIA}/unzip.exe"
                distribution/Display/bg-prelog.ans cmake/GenerateSetupPayload.cmake
                distribution/stock-menu-manifest.json ${SFX_STOCK_MENU_ASSETS}
            VERBATIM)
        add_executable(sfx_qualification_setup EXCLUDE_FROM_ALL
            test/platform/qualification_setup.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/setup.manifest "${SFX_QUALIFICATION_PAYLOAD}/payload.rc")
        target_include_directories(sfx_qualification_setup PRIVATE src/apps/setup "${SFX_QUALIFICATION_PAYLOAD}")
        target_link_libraries(sfx_qualification_setup PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_qualification_setup)
        add_executable(sfx_rc10_installed_runtime EXCLUDE_FROM_ALL test/platform/rc10_installed_runtime.cpp)
        target_include_directories(sfx_rc10_installed_runtime PRIVATE src/apps/setup)
        target_link_libraries(sfx_rc10_installed_runtime PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_rc10_installed_runtime)
        add_executable(sfx_beta2_installed_runtime EXCLUDE_FROM_ALL test/platform/beta2_installed_runtime.cpp)
        target_include_directories(sfx_beta2_installed_runtime PRIVATE src/apps/setup)
        target_link_libraries(sfx_beta2_installed_runtime PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_beta2_installed_runtime)
        add_executable(sfx_beta3_installed_runtime EXCLUDE_FROM_ALL test/platform/beta3_installed_runtime.cpp)
        target_include_directories(sfx_beta3_installed_runtime PRIVATE src/apps/setup)
        target_link_libraries(sfx_beta3_installed_runtime PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_beta3_installed_runtime)
        add_executable(sfx_beta4_installed_runtime EXCLUDE_FROM_ALL test/platform/beta4_installed_runtime.cpp)
        target_include_directories(sfx_beta4_installed_runtime PRIVATE src/apps/setup)
        target_link_libraries(sfx_beta4_installed_runtime PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_beta4_installed_runtime)
        add_executable(sfx_qwk_profile_caller_host EXCLUDE_FROM_ALL test/platform/qwk_profile_caller_host.cpp)
        target_link_libraries(sfx_qwk_profile_caller_host PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_qwk_profile_caller_host)
        add_executable(sfx_beta2_bootstrap_docs_tests EXCLUDE_FROM_ALL test/platform/beta2_bootstrap_docs_tests.cpp)
        target_link_libraries(sfx_beta2_bootstrap_docs_tests PRIVATE sfx_alpha_win32)
        target_compile_definitions(sfx_beta2_bootstrap_docs_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta2_bootstrap_docs_tests)
        add_executable(sfx_rc12_installed_runtime EXCLUDE_FROM_ALL test/platform/rc12_installed_runtime.cpp)
        target_include_directories(sfx_rc12_installed_runtime PRIVATE src/apps/setup)
        target_link_libraries(sfx_rc12_installed_runtime PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_rc12_installed_runtime)
        add_executable(sfx_rc10_qwk_fixture EXCLUDE_FROM_ALL test/platform/rc10_qwk_fixture.cpp)
        target_include_directories(sfx_rc10_qwk_fixture PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_rc10_qwk_fixture PRIVATE sfx_alpha_runtime_host sfx_ftn_native_win32 sfx::archive sfx::libarchive sfx::libsodium)
        sfx_configure_test_executable(sfx_rc10_qwk_fixture)
        add_executable(sfx_section8d_install_qualification EXCLUDE_FROM_ALL test/platform/section8d_install_qualification.cpp src/apps/setup/setup_engine.cpp src/apps/setup/setup.manifest "${SFX_QUALIFICATION_PAYLOAD}/payload.rc")
        target_include_directories(sfx_section8d_install_qualification PRIVATE src/apps/setup "${SFX_QUALIFICATION_PAYLOAD}")
        target_link_libraries(sfx_section8d_install_qualification PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_section8d_install_qualification)
        add_executable(sfx_section8c_qualification EXCLUDE_FROM_ALL
            test/platform/section8c_qualification.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/setup.manifest "${SFX_QUALIFICATION_PAYLOAD}/payload.rc")
        target_include_directories(sfx_section8c_qualification PRIVATE src/apps/setup "${SFX_QUALIFICATION_PAYLOAD}")
        target_link_libraries(sfx_section8c_qualification PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_section8c_qualification)
        set(SFX_SECTION8C_LEGACY_MEDIA "" CACHE PATH "Optional exact pre-8C media for isolated migration qualification only")
        if(NOT SFX_SECTION8C_LEGACY_MEDIA STREQUAL "")
            set(SFX_SECTION8C_LEGACY_PAYLOAD "${CMAKE_CURRENT_BINARY_DIR}/section8c-legacy-payload")
            add_custom_command(OUTPUT "${SFX_SECTION8C_LEGACY_PAYLOAD}/payload.rc"
                COMMAND ${CMAKE_COMMAND}
                    "-DCONTROL_PANEL=${SFX_SECTION8C_LEGACY_MEDIA}/SFXControlPanel.exe"
                    "-DRUNTIME=${SFX_SECTION8C_LEGACY_MEDIA}/SFXRuntime.exe"
                    "-DZIP=${SFX_SECTION8C_LEGACY_MEDIA}/zip.exe"
                    "-DUNZIP=${SFX_SECTION8C_LEGACY_MEDIA}/unzip.exe"
                    "-DCONFIG=${SFX_SECTION8C_LEGACY_MEDIA}/SFXConfig.exe"
                    "-DMESSAGE_HELPER=${SFX_MESSAGE_HELPER}"
                    "-DBOTGATE_ART=${CMAKE_CURRENT_SOURCE_DIR}/distribution/Display/bg-prelog.ans"
                    "-DSTOCK_MENU_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/distribution"
                    "-DOUTPUT_DIRECTORY=${SFX_SECTION8C_LEGACY_PAYLOAD}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateSetupPayload.cmake"
                DEPENDS SFXMessageHelper cmake/GenerateSetupPayload.cmake ${SFX_STOCK_MENU_ASSETS}
                    "${SFX_SECTION8C_LEGACY_MEDIA}/SFXRuntime.exe" "${SFX_SECTION8C_LEGACY_MEDIA}/SFXConfig.exe"
                    "${SFX_SECTION8C_LEGACY_MEDIA}/SFXControlPanel.exe" "${SFX_SECTION8C_LEGACY_MEDIA}/zip.exe"
                    "${SFX_SECTION8C_LEGACY_MEDIA}/unzip.exe" VERBATIM)
            add_executable(sfx_section8c_legacy EXCLUDE_FROM_ALL test/platform/section8c_qualification.cpp
                src/apps/setup/setup_engine.cpp src/apps/setup/setup.manifest "${SFX_SECTION8C_LEGACY_PAYLOAD}/payload.rc")
            target_include_directories(sfx_section8c_legacy PRIVATE src/apps/setup "${SFX_SECTION8C_LEGACY_PAYLOAD}")
            target_compile_definitions(sfx_section8c_legacy PRIVATE SFX_SECTION8C_LEGACY_SEED=1)
            target_link_libraries(sfx_section8c_legacy PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
            sfx_configure_test_executable(sfx_section8c_legacy)
        endif()
        add_executable(sfx_backup_redesign_qualification EXCLUDE_FROM_ALL
            test/platform/backup_redesign_qualification.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/setup.manifest "${SFX_QUALIFICATION_PAYLOAD}/payload.rc")
        target_include_directories(sfx_backup_redesign_qualification PRIVATE src/apps/setup "${SFX_QUALIFICATION_PAYLOAD}")
        target_link_libraries(sfx_backup_redesign_qualification PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_backup_redesign_qualification)
        add_executable(sfx_backup_two_folder_qualification EXCLUDE_FROM_ALL
            test/platform/backup_two_folder_qualification.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/setup.manifest "${SFX_QUALIFICATION_PAYLOAD}/payload.rc")
        target_include_directories(sfx_backup_two_folder_qualification PRIVATE src/apps/setup "${SFX_QUALIFICATION_PAYLOAD}")
        target_link_libraries(sfx_backup_two_folder_qualification PRIVATE sfx_alpha_win32 sfx_alpha_runtime_host sfx_account_administration_win32 sfx::archive shell32 ole32 uuid version)
        sfx_configure_test_executable(sfx_backup_two_folder_qualification)
        add_executable(sfx_message_receive_host_tests EXCLUDE_FROM_ALL test/platform/message_receive_host_tests.cpp)
        add_executable(sfx_message_receive_completion_probe EXCLUDE_FROM_ALL test/platform/message_receive_completion_probe.cpp)
        target_link_libraries(sfx_message_receive_completion_probe PRIVATE sfx_alpha_runtime_host ws2_32)
        sfx_configure_test_executable(sfx_message_receive_completion_probe)
        add_executable(sfx_qwk_transfer_host_tests EXCLUDE_FROM_ALL test/platform/qwk_transfer_host_tests.cpp src/apps/runtime/alpha_qwk_transfer.cpp src/apps/runtime/alpha_qwk_reply_spool.cpp src/apps/runtime/alpha_message_receive.cpp)
        target_include_directories(sfx_qwk_transfer_host_tests PRIVATE src/apps/runtime)
        target_link_libraries(sfx_qwk_transfer_host_tests PRIVATE sfx::archive sfx::libarchive sfx_installation_win32 sfx_platform_win32_storage sfx::libsodium ws2_32)
        target_compile_definitions(sfx_qwk_transfer_host_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_qwk_transfer_host_tests)
        add_executable(sfx_section16_packet_cleanup_tests EXCLUDE_FROM_ALL test/platform/section16_packet_cleanup_tests.cpp src/apps/runtime/alpha_qwk_transfer.cpp src/apps/runtime/alpha_qwk_reply_spool.cpp src/apps/runtime/alpha_message_receive.cpp)
        target_include_directories(sfx_section16_packet_cleanup_tests PRIVATE src/apps/runtime)
        target_link_libraries(sfx_section16_packet_cleanup_tests PRIVATE sfx::archive sfx::libarchive sfx_installation_win32 sfx_platform_win32_storage sfx::libsodium ws2_32)
        target_compile_definitions(sfx_section16_packet_cleanup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section16_packet_cleanup_tests)
        add_executable(sfx_file_transfer_host_tests EXCLUDE_FROM_ALL test/platform/file_transfer_host_tests.cpp src/apps/runtime/alpha_file_transfer.cpp src/apps/runtime/alpha_caller_files.cpp src/apps/runtime/alpha_message_receive.cpp)
        target_include_directories(sfx_file_transfer_host_tests PRIVATE src/apps/runtime src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_transfer_host_tests PRIVATE sfx_file_intake_win32 sfx_file_payload_win32 sfx::storage_sqlite sfx::archive sfx::libarchive sfx_installation_win32 sfx_platform_win32_storage sfx::libsodium ws2_32)
        target_compile_definitions(sfx_file_transfer_host_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_transfer_host_tests)
        add_executable(sfx_section16_file_cleanup_tests EXCLUDE_FROM_ALL test/platform/section16_file_cleanup_tests.cpp test/support/src/sqlite_write_fault_vfs.cpp)
        target_include_directories(sfx_section16_file_cleanup_tests PRIVATE src/apps/runtime src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_section16_file_cleanup_tests PRIVATE sfx_alpha_runtime_host ws2_32)
        target_compile_definitions(sfx_section16_file_cleanup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section16_file_cleanup_tests)
        add_executable(sfx_file_private_collision_tests EXCLUDE_FROM_ALL test/platform/file_private_collision_tests.cpp src/apps/runtime/alpha_file_transfer.cpp src/apps/runtime/alpha_caller_files.cpp src/apps/runtime/alpha_message_receive.cpp src/apps/runtime/alpha_file_viewer.cpp)
        target_include_directories(sfx_file_private_collision_tests PRIVATE src/apps/runtime src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_private_collision_tests PRIVATE sfx_file_manager_win32 sfx_file_intake_win32 sfx_file_payload_win32 sfx::storage_sqlite sfx::archive sfx::libarchive sfx_installation_win32 sfx_platform_win32_storage sfx::libsodium ws2_32)
        target_compile_definitions(sfx_file_private_collision_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_private_collision_tests)
        add_executable(sfx_qwk_installation_receipt_tests test/platform/qwk_installation_receipt_tests.cpp)
        target_link_libraries(sfx_qwk_installation_receipt_tests PRIVATE sfx_installation_win32)
        target_compile_definitions(sfx_qwk_installation_receipt_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_qwk_installation_receipt_tests)
        target_link_libraries(sfx_message_receive_host_tests PRIVATE sfx_alpha_runtime_host ws2_32)
        sfx_configure_test_executable(sfx_message_receive_host_tests)
    endif()

    add_executable(sfx_installer_contract_tests EXCLUDE_FROM_ALL test/platform/installer_contract_tests.cpp)
    add_executable(sfx_setup_backup_followup_tests EXCLUDE_FROM_ALL test/platform/setup_backup_followup_probe.cpp)
    add_executable(sfx_setup_backup_followup_contracts EXCLUDE_FROM_ALL test/platform/setup_backup_followup_contracts.cpp)
    target_link_libraries(sfx_setup_backup_followup_contracts PRIVATE sfx_archive sfx_installation_win32)
    sfx_configure_test_executable(sfx_setup_backup_followup_contracts)
    target_link_libraries(sfx_setup_backup_followup_tests PRIVATE sfx_archive)
    target_compile_definitions(sfx_setup_backup_followup_tests PRIVATE SFX_FOLLOWUP_UPDATED SODIUM_STATIC)
    target_include_directories(sfx_setup_backup_followup_tests PRIVATE third_party/libsodium/1.0.22/include)
    sfx_configure_test_executable(sfx_setup_backup_followup_tests)
    target_link_libraries(sfx_installer_contract_tests PRIVATE sfx_installation_win32)
    sfx_configure_test_executable(sfx_installer_contract_tests)

    # Native alternate-volume proof using the actual private ancestor verifier.
    add_executable(sfx_alternate_drive_tests EXCLUDE_FROM_ALL test/platform/alternate_drive_tests.cpp)
    target_include_directories(sfx_alternate_drive_tests PRIVATE src/platform/win32/include)
    target_link_libraries(sfx_alternate_drive_tests PRIVATE advapi32 authz bcrypt)
    sfx_configure_test_executable(sfx_alternate_drive_tests)

    add_executable(sfx_setup_completion_tests EXCLUDE_FROM_ALL test/platform/setup_completion_tests.cpp)
    target_link_libraries(sfx_setup_completion_tests PRIVATE
        sfx_installation_win32 sfx::archive shell32 ole32 user32)
    sfx_configure_test_executable(sfx_setup_completion_tests)
    add_executable(sfx_setup_planning_task_tests EXCLUDE_FROM_ALL test/platform/setup_planning_task_tests.cpp)
    target_link_libraries(sfx_setup_planning_task_tests PRIVATE sfx_installation_win32 sfx::archive)
    sfx_configure_test_executable(sfx_setup_planning_task_tests)

    add_executable(sfx_section4_artwork_upgrade_tests EXCLUDE_FROM_ALL
        test/platform/section4_artwork_upgrade_tests.cpp)
    target_link_libraries(sfx_section4_artwork_upgrade_tests PRIVATE
        sfx_alpha_win32 sfx::archive shell32 ole32 uuid version)
    sfx_configure_test_executable(sfx_section4_artwork_upgrade_tests)

    add_executable(sfx_setup_source_rename_tests EXCLUDE_FROM_ALL test/platform/setup_source_rename_tests.cpp)
    target_link_libraries(sfx_setup_source_rename_tests PRIVATE sfx_installation_win32)
    sfx_configure_test_executable(sfx_setup_source_rename_tests)

    add_executable(sfx_section3_installed_archive_tests EXCLUDE_FROM_ALL
        test/platform/section3_installed_archive_tests.cpp)
    target_link_libraries(sfx_section3_installed_archive_tests PRIVATE sfx_alpha_win32 sfx::archive)
    sfx_configure_test_executable(sfx_section3_installed_archive_tests)
endif()

if(BUILD_TESTING)
    add_library(sfx_test_support STATIC test/support/src/fakes.cpp)
    target_include_directories(sfx_test_support PUBLIC test/support/include)
    target_link_libraries(sfx_test_support PUBLIC sfx::application)
    sfx_configure_portable_target(sfx_test_support)

    add_executable(sfx_unit_tests test/unit/foundation_tests.cpp)
    target_link_libraries(sfx_unit_tests PRIVATE sfx_test_support)
    sfx_configure_test_executable(sfx_unit_tests)
    add_test(NAME sfx.unit.foundation COMMAND sfx_unit_tests)

    add_executable(sfx_archive_engine_tests test/integration/archive_engine_tests.cpp)
    if(WIN32)
        add_executable(sfx_restore_artifact_tests EXCLUDE_FROM_ALL test/platform/restore_artifact_tests.cpp)
        add_executable(sfx_backup_read_access_tests EXCLUDE_FROM_ALL test/platform/backup_read_access_tests.cpp)
        target_link_libraries(sfx_backup_read_access_tests PRIVATE sfx_platform_win32_storage advapi32 authz)
        sfx_configure_test_executable(sfx_backup_read_access_tests)
        target_include_directories(sfx_restore_artifact_tests PRIVATE src/apps/setup)
        target_link_libraries(sfx_restore_artifact_tests PRIVATE sfx_file_backup_win32 sfx::archive)
        sfx_configure_test_executable(sfx_restore_artifact_tests)
    endif()
    add_executable(sfx_file_contents_tests test/integration/file_contents_tests.cpp)
    target_link_libraries(sfx_file_contents_tests PRIVATE sfx::archive)
    sfx_configure_test_executable(sfx_file_contents_tests)
    target_link_libraries(sfx_archive_engine_tests PRIVATE
        sfx::archive sfx::libarchive)
    sfx_configure_test_executable(sfx_archive_engine_tests)
    add_test(NAME sfx.integration.archive_engine COMMAND sfx_archive_engine_tests
        ${CMAKE_CURRENT_BINARY_DIR}/test-output/archive-engine)

    add_executable(sfx_node_pool_tests test/unit/node_pool_tests.cpp)
    target_link_libraries(sfx_node_pool_tests PRIVATE sfx::application Threads::Threads)
    sfx_configure_test_executable(sfx_node_pool_tests)
    add_test(NAME sfx.unit.node_pool COMMAND sfx_node_pool_tests)
    set_tests_properties(sfx.unit.node_pool PROPERTIES TIMEOUT 60)

    add_executable(sfx_caller_lifetime_tests test/unit/caller_lifetime_tests.cpp)
    target_link_libraries(sfx_caller_lifetime_tests PRIVATE sfx::application sfx_test_support Threads::Threads)
    sfx_configure_test_executable(sfx_caller_lifetime_tests)
    add_test(NAME sfx.unit.caller_lifetime COMMAND sfx_caller_lifetime_tests)
    set_tests_properties(sfx.unit.caller_lifetime PROPERTIES TIMEOUT 60)

    add_executable(sfx_caller_color_tests test/unit/caller_color_tests.cpp)
    target_link_libraries(sfx_caller_color_tests PRIVATE sfx::application sfx_test_support)
    sfx_configure_test_executable(sfx_caller_color_tests)
    add_test(NAME sfx.unit.caller_color COMMAND sfx_caller_color_tests)
    add_executable(sfx_menu_tests test/unit/menu_tests.cpp)
    add_executable(sfx_sysop_session_tests test/unit/sysop_session_tests.cpp)
    add_executable(sfx_newsletter_editor_tests test/unit/newsletter_editor_tests.cpp)
    add_executable(sfx_rc11_terminal_tests test/unit/rc11_terminal_tests.cpp)
    target_include_directories(sfx_rc11_terminal_tests PRIVATE src/apps/runtime)
    target_link_libraries(sfx_rc11_terminal_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_rc11_terminal_tests)
    add_test(NAME sfx.unit.rc11_terminal COMMAND sfx_rc11_terminal_tests)
    target_link_libraries(sfx_newsletter_editor_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_newsletter_editor_tests)
    add_test(NAME sfx.unit.newsletter_editor COMMAND sfx_newsletter_editor_tests "${CMAKE_SOURCE_DIR}/test/fixtures/newsletter-source-sample.asc")
    if(WIN32 OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(sfx_newsletter_file_tests test/platform/newsletter_file_tests.cpp)
        target_include_directories(sfx_newsletter_file_tests PRIVATE src/apps/runtime)
        target_link_libraries(sfx_newsletter_file_tests PRIVATE sfx::application sfx::libsodium)
        if(WIN32)
            target_link_libraries(sfx_newsletter_file_tests PRIVATE sfx_installation_win32 sfx_platform_win32_display)
            target_compile_definitions(sfx_newsletter_file_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
        else()
            target_link_libraries(sfx_newsletter_file_tests PRIVATE sfx_linux_host_files)
        endif()
        sfx_configure_test_executable(sfx_newsletter_file_tests)
    endif()
    target_link_libraries(sfx_sysop_session_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_sysop_session_tests)
    add_test(NAME sfx.unit.sysop_session COMMAND sfx_sysop_session_tests)
    set_tests_properties(sfx.unit.sysop_session PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    if(WIN32)
        add_executable(sfx_sysop_log_tests test/platform/sysop_log_tests.cpp)
        add_executable(sfx_service_secret_tests test/platform/service_secret_tests.cpp)
        target_link_libraries(sfx_service_secret_tests PRIVATE sfx_platform_win32_phase3a sfx_platform_win32_storage)
        sfx_configure_test_executable(sfx_service_secret_tests)
        add_executable(sfx_service_pipe_tests test/platform/service_pipe_tests.cpp)
        target_link_libraries(sfx_service_pipe_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_service_pipe_tests)
        target_link_libraries(sfx_sysop_log_tests PRIVATE sfx::application)
        target_compile_definitions(sfx_sysop_log_tests PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
        sfx_configure_test_executable(sfx_sysop_log_tests)
    endif()
    target_link_libraries(sfx_menu_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_menu_tests)
    add_test(NAME sfx.unit.menu COMMAND sfx_menu_tests)
    add_executable(sfx_menu_display_tests test/unit/menu_display_tests.cpp)
    target_link_libraries(sfx_menu_display_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_menu_display_tests)
    add_test(NAME sfx.unit.menu_display COMMAND sfx_menu_display_tests)
    add_executable(sfx_menu_session_tests test/unit/menu_session_tests.cpp)
    add_executable(sfx_beta6_polish_prompt_tests test/unit/beta6_polish_prompt_tests.cpp)
    target_link_libraries(sfx_beta6_polish_prompt_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_beta6_polish_prompt_tests)
    add_test(NAME sfx.unit.beta6_polish_prompt COMMAND sfx_beta6_polish_prompt_tests)
    add_executable(sfx_caller_utilities_tests test/unit/caller_utilities_tests.cpp)
    add_executable(sfx_classic_information_tests test/unit/classic_information_tests.cpp)
    add_executable(sfx_message_receive_completion_tests test/unit/message_receive_completion_tests.cpp)
    target_link_libraries(sfx_message_receive_completion_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_message_receive_completion_tests)
    add_test(NAME sfx.unit.message_receive_completion COMMAND sfx_message_receive_completion_tests)
    add_executable(sfx_message_tests test/unit/message_tests.cpp)
    add_executable(sfx_file_tests test/unit/file_tests.cpp)
    add_executable(sfx_ftn_tests test/unit/ftn_tests.cpp)
    target_link_libraries(sfx_ftn_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_ftn_tests)
    add_test(NAME sfx.unit.ftn COMMAND sfx_ftn_tests)
    add_executable(sfx_ftn_import_retry_tests test/unit/ftn_import_retry_tests.cpp)
    target_link_libraries(sfx_ftn_import_retry_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_ftn_import_retry_tests)
    add_test(NAME sfx.unit.ftn_import_retry COMMAND sfx_ftn_import_retry_tests)
    add_executable(sfx_ftn_received_compat_tests test/unit/ftn_received_compat_tests.cpp)
    target_link_libraries(sfx_ftn_received_compat_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_ftn_received_compat_tests)
    add_test(NAME sfx.unit.ftn_received_compat COMMAND sfx_ftn_received_compat_tests)
    add_executable(sfx_telgate_tests test/unit/telgate_tests.cpp)
    target_link_libraries(sfx_telgate_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_telgate_tests)
    add_test(NAME sfx.unit.telgate COMMAND sfx_telgate_tests)
    add_executable(sfx_ftn_setup_model_tests test/unit/ftn_setup_model_tests.cpp)
    target_link_libraries(sfx_ftn_setup_model_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_ftn_setup_model_tests)
    add_test(NAME sfx.unit.ftn_setup_model COMMAND sfx_ftn_setup_model_tests)
    add_executable(sfx_ftn_wire_tests test/unit/ftn_wire_tests.cpp)
    target_link_libraries(sfx_ftn_wire_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_ftn_wire_tests)
    add_test(NAME sfx.unit.ftn_wire COMMAND sfx_ftn_wire_tests)
    add_executable(sfx_ftn_mail_tests test/unit/ftn_mail_tests.cpp)
    target_link_libraries(sfx_ftn_mail_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_ftn_mail_tests)
    add_test(NAME sfx.unit.ftn_mail COMMAND sfx_ftn_mail_tests)
    add_executable(sfx_ftn_external_tests test/unit/ftn_external_tests.cpp)
    target_include_directories(sfx_ftn_external_tests PRIVATE src/archive/include)
    target_link_libraries(sfx_ftn_external_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_ftn_external_tests)
    add_test(NAME sfx.unit.ftn_external COMMAND sfx_ftn_external_tests)
    add_executable(sfx_ftn_schema_probe test/integration/ftn_schema_probe.cpp)
    target_link_libraries(sfx_ftn_schema_probe PRIVATE sfx_sqlite3)
    sfx_configure_test_executable(sfx_ftn_schema_probe)
    add_test(NAME sfx.integration.ftn_schema_probe COMMAND sfx_ftn_schema_probe)
    add_executable(sfx_chat_tests test/unit/chat_tests.cpp)
    target_link_libraries(sfx_chat_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_chat_tests)
    add_test(NAME sfx.unit.chat COMMAND sfx_chat_tests)
    set_tests_properties(sfx.unit.chat PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    add_executable(sfx_door_tests test/unit/door_tests.cpp)
    add_executable(sfx_door_drop_tests test/unit/door_drop_tests.cpp)
    target_link_libraries(sfx_door_drop_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_door_drop_tests)
    if(WIN32)
        add_executable(sfx_telgate_native_tests test/platform/telgate_native_tests.cpp)
        target_link_libraries(sfx_telgate_native_tests PRIVATE sfx_alpha_runtime_host sfx_platform_win32_phase3a ws2_32)
        target_compile_definitions(sfx_telgate_native_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_telgate_native_tests)
        add_executable(sfx_rlogin_native_tests test/platform/rlogin_native_tests.cpp)
        add_executable(sfx_event_sounds_journal_tests test/integration/event_sounds_journal_tests.cpp)
        target_link_libraries(sfx_event_sounds_journal_tests PRIVATE sfx_alpha_runtime_host sfx_platform_win32_phase3a)
        target_compile_definitions(sfx_event_sounds_journal_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_event_sounds_journal_tests)
        target_link_libraries(sfx_rlogin_native_tests PRIVATE sfx_alpha_runtime_host sfx_platform_win32_phase3a ws2_32)
        target_compile_definitions(sfx_rlogin_native_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rlogin_native_tests)
        add_test(NAME sfx.platform.rlogin COMMAND sfx_rlogin_native_tests)
        add_executable(sfx_rlogin_cancel_probe test/platform/rlogin_cancel_probe.cpp)
        target_link_libraries(sfx_rlogin_cancel_probe PRIVATE sfx_alpha_runtime_host ws2_32)
        target_compile_definitions(sfx_rlogin_cancel_probe PRIVATE SFX_RLOGIN_OWNED_SECRETS=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rlogin_cancel_probe)
    endif()
    target_link_libraries(sfx_door_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_door_tests)
    add_test(NAME sfx.unit.door COMMAND sfx_door_tests)
    add_executable(sfx_door_session_tests test/unit/door_session_tests.cpp)
    target_link_libraries(sfx_door_session_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_door_session_tests)
    add_test(NAME sfx.unit.door_session COMMAND sfx_door_session_tests)
    target_link_libraries(sfx_file_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_file_tests)
    add_test(NAME sfx.unit.file COMMAND sfx_file_tests)
    add_executable(sfx_file_presentation_tests test/unit/file_presentation_tests.cpp)
    target_link_libraries(sfx_file_presentation_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_file_presentation_tests)
    add_test(NAME sfx.unit.file_presentation COMMAND sfx_file_presentation_tests)
    add_executable(sfx_file_transfer_completion_tests test/unit/file_transfer_completion_tests.cpp)
    target_link_libraries(sfx_file_transfer_completion_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_file_transfer_completion_tests)
    add_test(NAME sfx.unit.file_transfer_completion COMMAND sfx_file_transfer_completion_tests)
    add_executable(sfx_file_scan_tests test/unit/file_scan_tests.cpp)
    target_link_libraries(sfx_file_scan_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_file_scan_tests)
    add_test(NAME sfx.unit.file_scan COMMAND sfx_file_scan_tests)
    add_executable(sfx_file_zip_tests test/unit/file_zip_tests.cpp)
    target_link_libraries(sfx_file_zip_tests PRIVATE sfx::archive sfx::libarchive)
    sfx_configure_test_executable(sfx_file_zip_tests)
    add_test(NAME sfx.unit.file_zip COMMAND sfx_file_zip_tests)
    add_executable(sfx_qwk_tests test/unit/qwk_tests.cpp)
    target_link_libraries(sfx_qwk_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_qwk_tests)
    add_test(NAME sfx.unit.qwk COMMAND sfx_qwk_tests)
    add_executable(sfx_qwk_archive_tests test/unit/qwk_archive_tests.cpp)
    add_executable(sfx_qwk_preparation_activity_tests test/unit/qwk_preparation_activity_tests.cpp)
    target_link_libraries(sfx_qwk_preparation_activity_tests PRIVATE sfx::archive Threads::Threads)
    sfx_configure_test_executable(sfx_qwk_preparation_activity_tests)
    add_test(NAME sfx.unit.qwk_preparation_activity COMMAND sfx_qwk_preparation_activity_tests)
    target_link_libraries(sfx_qwk_archive_tests PRIVATE sfx::archive sfx::libarchive)
    sfx_configure_test_executable(sfx_qwk_archive_tests)
    add_test(NAME sfx.unit.qwk_archive COMMAND sfx_qwk_archive_tests)
    add_executable(sfx_qwk_transfer_completion_tests test/unit/qwk_transfer_completion_tests.cpp)
    target_link_libraries(sfx_qwk_transfer_completion_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_qwk_transfer_completion_tests)
    add_test(NAME sfx.unit.qwk_transfer_completion COMMAND sfx_qwk_transfer_completion_tests)
    add_executable(sfx_qwk_policy_protocol_tests test/unit/qwk_policy_protocol_tests.cpp)
    target_link_libraries(sfx_qwk_policy_protocol_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_qwk_policy_protocol_tests)
    target_include_directories(sfx_message_tests PRIVATE test/support/include)
    target_link_libraries(sfx_message_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_message_tests)
    add_test(NAME sfx.unit.message COMMAND sfx_message_tests)
    add_executable(sfx_message_source_tests test/unit/message_source_tests.cpp)
    target_include_directories(sfx_message_source_tests PRIVATE test/support/include)
    target_link_libraries(sfx_message_source_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_message_source_tests)
    add_test(NAME sfx.unit.message_source COMMAND sfx_message_source_tests)
    target_link_libraries(sfx_classic_information_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_classic_information_tests)
    add_test(NAME sfx.unit.classic_information COMMAND sfx_classic_information_tests)
    target_link_libraries(sfx_caller_utilities_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_caller_utilities_tests)
    add_test(NAME sfx.unit.caller_utilities COMMAND sfx_caller_utilities_tests)
    target_link_libraries(sfx_menu_session_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_menu_session_tests)
    add_test(NAME sfx.unit.menu_session COMMAND sfx_menu_session_tests)

    add_executable(sfx_operational_configuration_tests
        test/unit/operational_configuration_tests.cpp
    )
    target_link_libraries(sfx_operational_configuration_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_operational_configuration_tests)
    add_test(NAME sfx.unit.operational_configuration
        COMMAND sfx_operational_configuration_tests)

    add_executable(sfx_caller_menu_notice_tests test/unit/caller_menu_notice_tests.cpp)
    add_executable(sfx_botgate_tests test/unit/botgate_tests.cpp)
    target_link_libraries(sfx_botgate_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_botgate_tests)
    add_test(NAME sfx.unit.botgate COMMAND sfx_botgate_tests)
    target_link_libraries(sfx_caller_menu_notice_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_caller_menu_notice_tests)
    add_test(NAME sfx.unit.caller_menu_notice COMMAND sfx_caller_menu_notice_tests)

    add_executable(sfx_scheduler_tests test/unit/scheduler_tests.cpp)
    target_link_libraries(sfx_scheduler_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_scheduler_tests)
    add_test(NAME sfx.unit.scheduler COMMAND sfx_scheduler_tests)

    add_executable(sfx_local_control_protocol_tests
        test/unit/local_control_protocol_tests.cpp
    )
    target_link_libraries(sfx_local_control_protocol_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_local_control_protocol_tests)
    add_test(NAME sfx.unit.local_control_protocol
        COMMAND sfx_local_control_protocol_tests)

    add_executable(sfx_operational_log_tests test/unit/operational_log_tests.cpp)
    add_executable(sfx_caller_journal_tests test/unit/caller_journal_tests.cpp)
    if(WIN32)
        add_executable(sfx_clamav_visibility_tests test/platform/clamav_visibility_tests.cpp)
        target_include_directories(sfx_clamav_visibility_tests PRIVATE src/apps/runtime)
        target_link_libraries(sfx_clamav_visibility_tests PRIVATE sfx_alpha_runtime_host sfx_alpha_win32)
        target_compile_definitions(sfx_clamav_visibility_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_visibility_tests)
    endif()
    add_executable(sfx_durable_journal_tests test/unit/durable_journal_tests.cpp)
    target_link_libraries(sfx_durable_journal_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_durable_journal_tests)
    add_test(NAME sfx.unit.durable_journal COMMAND sfx_durable_journal_tests)
    add_executable(sfx_caller_journal_storage_tests test/integration/caller_journal_storage_tests.cpp)
    target_include_directories(sfx_caller_journal_storage_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
    target_link_libraries(sfx_caller_journal_storage_tests PRIVATE sfx::storage_sqlite)
    sfx_configure_test_executable(sfx_caller_journal_storage_tests)
    add_test(NAME sfx.integration.caller_journal_storage COMMAND sfx_caller_journal_storage_tests)
    add_executable(sfx_menu_storage_tests test/integration/menu_storage_tests.cpp)
    target_include_directories(sfx_menu_storage_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
    target_link_libraries(sfx_menu_storage_tests PRIVATE sfx::storage_sqlite)
    sfx_configure_test_executable(sfx_menu_storage_tests)
    add_test(NAME sfx.integration.menu_storage COMMAND sfx_menu_storage_tests)
    add_executable(sfx_classic_information_storage_tests test/integration/classic_information_storage_tests.cpp)
    add_executable(sfx_message_storage_tests test/integration/message_storage_tests.cpp test/support/src/sqlite_write_fault_vfs.cpp)
    add_executable(sfx_sysop_message_tests test/integration/sysop_message_tests.cpp test/support/src/sqlite_write_fault_vfs.cpp)
    target_include_directories(sfx_sysop_message_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_sysop_message_tests PRIVATE sfx::storage_sqlite)
    sfx_configure_test_executable(sfx_sysop_message_tests)
    add_executable(sfx_file_storage_tests test/integration/file_storage_tests.cpp)
    add_executable(sfx_door_storage_tests test/integration/door_storage_tests.cpp)
    add_executable(sfx_door_bulletin_model_tests test/unit/door_bulletin_model_tests.cpp)
    add_executable(sfx_door_bulletin_storage_tests test/integration/door_bulletin_storage_tests.cpp)
    target_include_directories(sfx_door_bulletin_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_door_bulletin_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_door_bulletin_storage_tests)
    target_link_libraries(sfx_door_bulletin_model_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_door_bulletin_model_tests)
    add_test(NAME sfx.unit.door_bulletin_model COMMAND sfx_door_bulletin_model_tests)
    add_executable(sfx_section15_starter_storage_tests test/integration/section15_starter_storage_tests.cpp)
    target_include_directories(sfx_section15_starter_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_section15_starter_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_section15_starter_storage_tests)
    add_executable(sfx_network_setup_storage_tests test/integration/network_setup_storage_tests.cpp)
    target_include_directories(sfx_network_setup_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_network_setup_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_network_setup_storage_tests)
    add_executable(sfx_ftn_storage_tests test/integration/ftn_storage_tests.cpp)
    target_include_directories(sfx_ftn_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_ftn_storage_tests)
    add_executable(sfx_ftn_mail_storage_tests test/integration/ftn_mail_storage_tests.cpp)
    target_include_directories(sfx_ftn_mail_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_mail_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_ftn_mail_storage_tests)
    add_executable(sfx_ftn_work_storage_tests test/integration/ftn_work_storage_tests.cpp)
    add_executable(sfx_ftn_external_storage_tests test/integration/ftn_external_storage_tests.cpp)
    add_executable(sfx_ftn_external_copy_tests EXCLUDE_FROM_ALL test/platform/ftn_external_copy_tests.cpp)
    target_include_directories(sfx_ftn_external_copy_tests PRIVATE src/platform/common test/support/include)
    target_link_libraries(sfx_ftn_external_copy_tests PRIVATE sfx_ftn_external_files sfx::storage_sqlite)
    if(WIN32)
        target_link_libraries(sfx_ftn_external_copy_tests PRIVATE sfx_alpha_win32)
        target_compile_definitions(sfx_ftn_external_copy_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    else()
        target_link_libraries(sfx_ftn_external_copy_tests PRIVATE sfx_linux_file_payload sfx_linux_ftn_processing)
    endif()
    sfx_configure_test_executable(sfx_ftn_external_copy_tests)
    target_include_directories(sfx_ftn_external_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_external_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_ftn_external_storage_tests)
    add_executable(sfx_ftn_external_publication_tests test/integration/ftn_external_publication_tests.cpp)
    target_include_directories(sfx_ftn_external_publication_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_external_publication_tests PRIVATE sfx::storage_sqlite sfx::libsodium sfx_ftn_external_files)
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(sfx_ftn_external_publication_tests PRIVATE sfx_linux_host_files)
    endif()
    sfx_configure_test_executable(sfx_ftn_external_publication_tests)
    add_executable(sfx_sysop_storage_tests test/integration/sysop_storage_tests.cpp)
    target_include_directories(sfx_sysop_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_sysop_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_sysop_storage_tests)
    add_executable(sfx_ftn_retention_storage_tests test/integration/ftn_retention_storage_tests.cpp)
    target_include_directories(sfx_ftn_retention_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_retention_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_ftn_retention_storage_tests)
    target_include_directories(sfx_ftn_work_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_work_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_ftn_work_storage_tests)
    add_executable(sfx_ftn_import_storage_tests test/integration/ftn_import_storage_tests.cpp)
    target_include_directories(sfx_ftn_import_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_import_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_ftn_import_storage_tests)
    add_executable(sfx_ftn_held_storage_tests test/integration/ftn_held_storage_tests.cpp)
    target_include_directories(sfx_ftn_held_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_ftn_held_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_ftn_held_storage_tests)
    add_executable(sfx_chat_storage_tests test/integration/chat_storage_tests.cpp)
    target_include_directories(sfx_chat_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_chat_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_chat_storage_tests)
    target_include_directories(sfx_door_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_door_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_door_storage_tests)
    add_executable(sfx_file_configuration_protocol_tests test/unit/file_configuration_protocol_tests.cpp)
    add_executable(sfx_file_management_protocol_tests test/unit/file_management_protocol_tests.cpp)
    target_link_libraries(sfx_file_management_protocol_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_file_management_protocol_tests)
    add_test(NAME sfx.unit.file_management_protocol COMMAND sfx_file_management_protocol_tests)
    target_link_libraries(sfx_file_configuration_protocol_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_file_configuration_protocol_tests)
    add_test(NAME sfx.unit.file_configuration_protocol COMMAND sfx_file_configuration_protocol_tests)
    target_include_directories(sfx_file_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_file_storage_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_file_storage_tests)
    add_executable(sfx_file_session_tests test/integration/file_session_tests.cpp)
    target_include_directories(sfx_file_session_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_file_session_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
    sfx_configure_test_executable(sfx_file_session_tests)

    if(WIN32)
        add_executable(sfx_file_reserve_tests test/integration/file_reserve_tests.cpp)
        target_link_libraries(sfx_file_reserve_tests PRIVATE sfx_file_payload_win32)
        target_compile_definitions(sfx_file_reserve_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_reserve_tests)
        add_executable(sfx_file_reserve_lifetime_tests test/integration/file_reserve_lifetime_tests.cpp)
        target_link_libraries(sfx_file_reserve_lifetime_tests PRIVATE sfx_file_payload_win32)
        target_compile_definitions(sfx_file_reserve_lifetime_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_reserve_lifetime_tests)
        add_executable(sfx_rc10_owner_tests test/integration/rc10_owner_tests.cpp)
        target_include_directories(sfx_rc10_owner_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_rc10_owner_tests PRIVATE sfx_file_configuration_win32 sfx_file_manager_win32 sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_rc10_owner_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc10_owner_tests)
        add_executable(sfx_file_configuration_tests test/integration/file_configuration_tests.cpp)
        add_executable(sfx_file_administration_tests test/integration/file_administration_tests.cpp)
        target_include_directories(sfx_file_administration_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_administration_tests PRIVATE sfx_file_administration_win32 sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_file_administration_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_administration_tests)
        add_executable(sfx_file_owner_view_tests test/integration/file_owner_view_tests.cpp)
        target_include_directories(sfx_file_owner_view_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_owner_view_tests PRIVATE sfx_file_administration_win32 sfx_file_intake_win32 sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_file_owner_view_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_owner_view_tests)
        add_executable(sfx_file_manager_model_tests test/platform/file_manager_model_tests.cpp)
        target_link_libraries(sfx_file_manager_model_tests PRIVATE sfx_file_manager_client_model_win32)
        target_compile_definitions(sfx_file_manager_model_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_manager_model_tests)
        add_executable(sfx_file_manager_window_tests test/platform/file_manager_window_tests.cpp src/apps/file_manager/file_manager.rc src/apps/file_manager/file_manager.manifest)
        target_link_libraries(sfx_file_manager_window_tests PRIVATE sfx_file_manager_widgets_win32 sfx_platform_win32_phase3a bcrypt)
        target_compile_definitions(sfx_file_manager_window_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_manager_window_tests)
        target_include_directories(sfx_file_configuration_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_configuration_tests PRIVATE sfx_file_configuration_win32 sfx_file_manager_win32 sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_file_configuration_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_configuration_tests)
        add_executable(sfx_file_payload_tests test/integration/file_payload_tests.cpp)
        add_executable(sfx_file_viewer_tests test/platform/file_viewer_tests.cpp src/apps/runtime/alpha_file_viewer.cpp)
        target_include_directories(sfx_file_viewer_tests PRIVATE src/apps/runtime src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_viewer_tests PRIVATE sfx_file_manager_win32 sfx_file_payload_win32 sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_file_viewer_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_viewer_tests)
        target_link_libraries(sfx_file_payload_tests PRIVATE sfx_file_payload_win32 sfx::libsodium)
        target_compile_definitions(sfx_file_payload_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_payload_tests)
        add_executable(sfx_ftn_native_custody_tests test/integration/ftn_native_custody_tests.cpp)
        target_include_directories(sfx_ftn_native_custody_tests PRIVATE test/support/include)
        target_link_libraries(sfx_ftn_native_custody_tests PRIVATE sfx_ftn_native_win32 sfx::storage_sqlite sfx::libsodium)
        target_compile_definitions(sfx_ftn_native_custody_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_native_custody_tests)
        add_executable(sfx_ftn_native_transport_tests test/integration/ftn_native_transport_tests.cpp)
        target_link_libraries(sfx_ftn_native_transport_tests PRIVATE sfx_ftn_native_win32 OpenSSL::SSL OpenSSL::Crypto ws2_32)
        target_compile_definitions(sfx_ftn_native_transport_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_native_transport_tests)
        add_executable(sfx_ftn_native_session_tests test/integration/ftn_native_session_tests.cpp)
        target_include_directories(sfx_ftn_native_session_tests PRIVATE test/support/include)
        target_link_libraries(sfx_ftn_native_session_tests PRIVATE sfx_portable_credential_codec)
        target_link_libraries(sfx_ftn_native_session_tests PRIVATE sfx_ftn_native_win32 sfx::storage_sqlite sfx::libsodium OpenSSL::SSL OpenSSL::Crypto)
        target_compile_definitions(sfx_ftn_native_session_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_native_session_tests)
        add_executable(sfx_ftn_native_peer_host test/integration/ftn_native_peer_host.cpp)
        target_include_directories(sfx_ftn_native_peer_host PRIVATE test/support/include)
        target_link_libraries(sfx_ftn_native_peer_host PRIVATE sfx_ftn_native_win32 sfx::storage_sqlite sfx::libsodium)
        target_compile_definitions(sfx_ftn_native_peer_host PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_native_peer_host)
        add_executable(sfx_ftn_receipt_contract_tests test/platform/ftn_receipt_contract_tests.cpp)
        add_executable(sfx_ftn_engine_tests test/integration/ftn_engine_tests.cpp)
        add_executable(sfx_beta7_desktop_shortcut_tests EXCLUDE_FROM_ALL test/platform/beta7_desktop_shortcut_tests.cpp)
        target_link_libraries(sfx_beta7_desktop_shortcut_tests PRIVATE sfx_alpha_win32 shell32 ole32 uuid)
        sfx_configure_test_executable(sfx_beta7_desktop_shortcut_tests)
        add_executable(sfx_beta7_clamav_startup_tests EXCLUDE_FROM_ALL test/platform/beta7_clamav_startup_tests.cpp)
        target_include_directories(sfx_beta7_clamav_startup_tests PRIVATE src/apps/runtime)
        target_link_libraries(sfx_beta7_clamav_startup_tests PRIVATE sfx_alpha_runtime_host sfx_clamav_component_win32 sfx::archive)
        sfx_configure_test_executable(sfx_beta7_clamav_startup_tests)
        add_executable(sfx_beta7_startup_fixture_child EXCLUDE_FROM_ALL test/platform/beta7_startup_fixture_child.cpp)
        target_link_libraries(sfx_beta7_startup_fixture_child PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_beta7_startup_fixture_child)
        add_executable(sfx_beta7_startup_tests EXCLUDE_FROM_ALL test/platform/beta7_startup_tests.cpp src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_beta7_startup_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_beta7_startup_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
        add_dependencies(sfx_beta7_startup_tests SFXRuntime sfx_beta7_startup_fixture_child)
        sfx_configure_test_executable(sfx_beta7_startup_tests)
        add_executable(sfx_beta7_startup_ui_review WIN32 EXCLUDE_FROM_ALL test/platform/beta7_startup_ui_review.cpp src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_beta7_startup_ui_review PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_beta7_startup_ui_review PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
        add_dependencies(sfx_beta7_startup_ui_review SFXRuntime sfx_beta7_startup_fixture_child)
        sfx_configure_win32_product(sfx_beta7_startup_ui_review)
        add_executable(sfx_ftn_native_toss_tests test/integration/ftn_native_toss_tests.cpp)
        add_executable(sfx_ftn_retention_native_tests test/integration/ftn_retention_native_tests.cpp test/support/src/sqlite_write_fault_vfs.cpp)
        target_include_directories(sfx_ftn_retention_native_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_retention_native_tests PRIVATE sfx_ftn_native_win32 sfx_platform_win32_phase3a sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        target_compile_definitions(sfx_ftn_retention_native_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_retention_native_tests)
        # Separate test-only file-payload object supplies a real Windows
        # protected-handle fault; shipping libraries never define this macro.
        add_executable(sfx_ftn_retention_close_tests test/integration/ftn_retention_native_tests.cpp test/support/src/sqlite_write_fault_vfs.cpp src/platform/win32/src/file_payload_store.cpp)
        target_include_directories(sfx_ftn_retention_close_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_retention_close_tests PRIVATE sfx_ftn_native_win32 sfx_platform_win32_phase3a sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        target_compile_definitions(sfx_ftn_retention_close_tests PRIVATE SFX_FTN_EXPIRY_CLOSE_QUALIFICATION=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_retention_close_tests)
        add_executable(sfx_ftn_native_tic_tests test/integration/ftn_native_tic_tests.cpp)
        add_executable(sfx_ftn_native_publication_tests test/integration/ftn_native_publication_tests.cpp)
        target_include_directories(sfx_ftn_native_publication_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_native_publication_tests PRIVATE sfx_ftn_native_win32 sfx_file_manager_win32 sfx_platform_win32_phase3a sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        target_compile_definitions(sfx_ftn_native_publication_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_native_publication_tests)
        add_executable(sfx_portable_ftn_copy_tests EXCLUDE_FROM_ALL test/platform/portable_ftn_copy_tests.cpp)
        target_include_directories(sfx_portable_ftn_copy_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_portable_ftn_copy_tests PRIVATE sfx_ftn_native_win32 sfx_file_manager_win32 sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        target_compile_definitions(sfx_portable_ftn_copy_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_portable_ftn_copy_tests)
        target_include_directories(sfx_ftn_native_tic_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_native_tic_tests PRIVATE sfx_ftn_native_win32 sfx::storage_sqlite sfx::libsodium)
        target_compile_definitions(sfx_ftn_native_tic_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_native_tic_tests)
        add_executable(sfx_ftn_areafix_storage_tests test/integration/ftn_areafix_storage_tests.cpp)
        add_executable(sfx_ftn_file_origin_tests test/integration/ftn_file_origin_tests.cpp)
        add_executable(sfx_ftn_file_publication_tests test/integration/ftn_file_publication_tests.cpp)
        target_include_directories(sfx_ftn_file_publication_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_file_publication_tests PRIVATE sfx::storage_sqlite sfx::libsodium)
        sfx_configure_test_executable(sfx_ftn_file_publication_tests)
        target_include_directories(sfx_ftn_file_origin_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_file_origin_tests PRIVATE sfx::storage_sqlite)
        sfx_configure_test_executable(sfx_ftn_file_origin_tests)
        add_executable(sfx_ftn_file_native_tests test/integration/ftn_file_native_tests.cpp)
        target_include_directories(sfx_ftn_file_native_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_file_native_tests PRIVATE sfx::storage_sqlite sfx_file_manager_win32)
        target_compile_definitions(sfx_ftn_file_native_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_file_native_tests)
        add_executable(sfx_ftn_owned_copy_tests test/integration/ftn_owned_copy_tests.cpp)
        target_link_libraries(sfx_ftn_owned_copy_tests PRIVATE sfx_file_payload_win32)
        target_compile_definitions(sfx_ftn_owned_copy_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_owned_copy_tests)
        target_include_directories(sfx_ftn_areafix_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_areafix_storage_tests PRIVATE sfx::storage_sqlite)
        sfx_configure_test_executable(sfx_ftn_areafix_storage_tests)
        add_executable(sfx_ftn_serial_tests test/integration/ftn_serial_tests.cpp)
        target_sources(sfx_ftn_serial_tests PRIVATE test/support/src/sqlite_write_fault_vfs.cpp)
        target_include_directories(sfx_ftn_serial_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_serial_tests PRIVATE sfx::storage_sqlite)
        sfx_configure_test_executable(sfx_ftn_serial_tests)
        add_executable(sfx_ftn_restore_pin_tests test/integration/ftn_restore_pin_tests.cpp)
        target_link_libraries(sfx_ftn_restore_pin_tests PRIVATE sfx_account_administration_win32 sfx::storage_sqlite)
        target_compile_definitions(sfx_ftn_restore_pin_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_restore_pin_tests)
        target_include_directories(sfx_ftn_native_toss_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_native_toss_tests PRIVATE sfx_portable_credential_codec)
        target_link_libraries(sfx_ftn_native_toss_tests PRIVATE sfx_ftn_native_win32 sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        target_compile_definitions(sfx_ftn_native_toss_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_native_toss_tests)
        add_executable(sfx_ftn_rescan_recovery_tests test/integration/ftn_rescan_recovery_tests.cpp)
        target_include_directories(sfx_ftn_rescan_recovery_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_rescan_recovery_tests PRIVATE sfx_ftn_native_win32 sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        target_compile_definitions(sfx_ftn_rescan_recovery_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_rescan_recovery_tests)
        add_executable(sfx_ftn_received_migration_tests test/integration/ftn_received_migration_tests.cpp)
        target_include_directories(sfx_ftn_received_migration_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_received_migration_tests PRIVATE sfx_ftn_native_win32 sfx::storage_sqlite sfx::libsodium sfx::libarchive)
        target_compile_definitions(sfx_ftn_received_migration_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_received_migration_tests)
        add_executable(sfx_ftn_backup_tests test/integration/ftn_backup_tests.cpp)
        target_link_libraries(sfx_ftn_backup_tests PRIVATE sfx::archive sfx_installation_win32)
        target_compile_definitions(sfx_ftn_backup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_backup_tests)
        target_include_directories(sfx_ftn_engine_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_engine_tests PRIVATE sfx_alpha_runtime_host sfx_ftn_native_win32)
        target_compile_definitions(sfx_ftn_engine_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_engine_tests)
        add_executable(sfx_ftn_external_host test/integration/ftn_external_host.cpp)
        target_include_directories(sfx_ftn_external_host PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_ftn_external_host PRIVATE sfx_alpha_runtime_host sfx_ftn_native_win32 sfx_ftn_external_files sfx::libsodium)
        target_compile_definitions(sfx_ftn_external_host PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_external_host)
        add_executable(sfx_network_setup_engine_tests test/integration/network_setup_engine_tests.cpp)
        target_include_directories(sfx_network_setup_engine_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_network_setup_engine_tests PRIVATE sfx_alpha_runtime_host sfx_ftn_native_win32)
        target_compile_definitions(sfx_network_setup_engine_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_network_setup_engine_tests)

        add_executable(sfx_rc9_owner_tests test/integration/rc9_owner_tests.cpp)
        target_include_directories(sfx_rc9_owner_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_rc9_owner_tests PRIVATE sfx_alpha_runtime_host sfx_ftn_native_win32 sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_rc9_owner_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc9_owner_tests)
        target_link_libraries(sfx_ftn_receipt_contract_tests PRIVATE sfx_installation_win32 advapi32 authz bcrypt)
        target_compile_definitions(sfx_ftn_receipt_contract_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_ftn_receipt_contract_tests)
        add_executable(sfx_file_scanner_tests test/integration/file_scanner_tests.cpp)
        add_executable(sfx_clamav_package_tests test/integration/clamav_package_tests.cpp)
        add_executable(sfx_clamav_host_tests test/integration/clamav_host_tests.cpp)
        target_link_libraries(sfx_clamav_host_tests PRIVATE sfx_file_scanner_win32)
        target_compile_definitions(sfx_clamav_host_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_host_tests)
        add_executable(sfx_clamav_noav_tests test/integration/clamav_noav_tests.cpp src/apps/setup/clamav_download.cpp)
        target_include_directories(sfx_clamav_noav_tests PRIVATE src/apps/setup)
        target_link_libraries(sfx_clamav_noav_tests PRIVATE sfx_clamav_component_win32)
        target_compile_definitions(sfx_clamav_noav_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_noav_tests)
        sfx_clamav_https(sfx_clamav_noav_tests)
        add_executable(sfx_clamav_noav_config_tests test/platform/clamav_noav_config_tests.cpp src/apps/config_ui/door_bulletin_window.cpp src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_link_libraries(sfx_clamav_noav_config_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_manager_widgets_win32 comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        target_compile_definitions(sfx_clamav_noav_config_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_noav_config_tests)
        add_executable(sfx_beta12_config_async_tests EXCLUDE_FROM_ALL
            test/platform/beta12_config_async_tests.cpp src/apps/config_ui/door_bulletin_window.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_link_libraries(sfx_beta12_config_async_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive sfx_account_administration_win32 sfx_clamav_component_win32 sfx_ftn_download_win32 sfx_file_manager_widgets_win32 comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        target_compile_definitions(sfx_beta12_config_async_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta12_config_async_tests)
        target_link_libraries(sfx_clamav_package_tests PRIVATE sfx_clamav_component_win32)
        target_compile_definitions(sfx_clamav_package_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_package_tests)
        target_link_libraries(sfx_file_scanner_tests PRIVATE sfx_file_scanner_win32)
        target_compile_definitions(sfx_file_scanner_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_scanner_tests)
        add_executable(sfx_clamav_resources_tests test/integration/clamav_resources_tests.cpp)
        target_link_libraries(sfx_clamav_resources_tests PRIVATE sfx_file_scanner_win32)
        target_compile_definitions(sfx_clamav_resources_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_clamav_resources_tests)
        add_executable(sfx_file_intake_tests test/integration/file_intake_tests.cpp)
        target_include_directories(sfx_file_intake_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_intake_tests PRIVATE sfx_file_manager_win32 sfx_file_intake_win32 sfx::storage_sqlite)
        target_compile_definitions(sfx_file_intake_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_intake_tests)
        add_executable(sfx_file_acceptance_tests test/platform/file_acceptance_followup_tests.cpp)
        target_include_directories(sfx_file_acceptance_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_acceptance_tests PRIVATE sfx_file_manager_win32 sfx_file_intake_win32 sfx::storage_sqlite)
        target_compile_definitions(sfx_file_acceptance_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_acceptance_tests)
        add_executable(sfx_file_manager_tests test/integration/file_manager_tests.cpp)
        target_include_directories(sfx_file_manager_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_file_manager_tests PRIVATE sfx_file_manager_win32 sfx::storage_sqlite sfx::libarchive)
        target_compile_definitions(sfx_file_manager_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_file_manager_tests)
    endif()
    add_executable(sfx_qwk_storage_tests test/integration/qwk_storage_tests.cpp)
    add_executable(sfx_beta2_qwk_caller_tests test/integration/beta2_qwk_caller_tests.cpp)
    target_include_directories(sfx_beta2_qwk_caller_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_beta2_qwk_caller_tests PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
    sfx_configure_test_executable(sfx_beta2_qwk_caller_tests)
    add_executable(sfx_qwk_candidate_tests test/integration/qwk_candidate_tests.cpp)
    target_include_directories(sfx_qwk_candidate_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_qwk_candidate_tests PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
    sfx_configure_test_executable(sfx_qwk_candidate_tests)
    add_executable(sfx_qwk_activity_storage_tests EXCLUDE_FROM_ALL test/integration/qwk_activity_storage_tests.cpp)
    target_include_directories(sfx_qwk_activity_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_qwk_activity_storage_tests PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium Threads::Threads)
    sfx_configure_test_executable(sfx_qwk_activity_storage_tests)
    add_executable(sfx_beta2_message_seed EXCLUDE_FROM_ALL test/platform/beta2_message_seed.cpp)
    target_include_directories(sfx_beta2_message_seed PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_beta2_message_seed PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
    if(WIN32)
        target_link_libraries(sfx_beta2_message_seed PRIVATE sfx_installation_win32 sfx_account_administration_win32)
        target_compile_definitions(sfx_beta2_message_seed PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    endif()
    sfx_configure_test_executable(sfx_beta2_message_seed)
    add_executable(sfx_beta3_message_seed EXCLUDE_FROM_ALL test/platform/beta3_message_seed.cpp)
    target_include_directories(sfx_beta3_message_seed PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_beta3_message_seed PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
    if(WIN32)
        target_link_libraries(sfx_beta3_message_seed PRIVATE sfx_installation_win32 sfx_account_administration_win32)
        target_compile_definitions(sfx_beta3_message_seed PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    endif()
    sfx_configure_test_executable(sfx_beta3_message_seed)
    add_executable(sfx_beta4_message_seed EXCLUDE_FROM_ALL test/platform/beta4_message_seed.cpp)
    target_include_directories(sfx_beta4_message_seed PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_beta4_message_seed PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
    if(WIN32)
        target_link_libraries(sfx_beta4_message_seed PRIVATE sfx_installation_win32 sfx_account_administration_win32)
        target_compile_definitions(sfx_beta4_message_seed PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    endif()
    sfx_configure_test_executable(sfx_beta4_message_seed)
    if(WIN32)
        set(SFX_QWK_PROFILE_SOURCE_DIR "" CACHE PATH "Optional diagnostic-only instrumented QWK sources")
        if(SFX_QWK_PROFILE_SOURCE_DIR)
            add_executable(sfx_qwk_preparation_profile EXCLUDE_FROM_ALL test/platform/qwk_preparation_profile.cpp
                "${SFX_QWK_PROFILE_SOURCE_DIR}/qwk_packet_builder_profile.cpp"
                "${SFX_QWK_PROFILE_SOURCE_DIR}/qwk_archive_profile.cpp")
            target_include_directories(sfx_qwk_preparation_profile PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
            target_link_libraries(sfx_qwk_preparation_profile PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
            target_compile_definitions(sfx_qwk_preparation_profile PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
            sfx_configure_test_executable(sfx_qwk_preparation_profile)
        endif()
    endif()
    target_include_directories(sfx_qwk_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_qwk_storage_tests PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium)
    sfx_configure_test_executable(sfx_qwk_storage_tests)
    add_test(NAME sfx.integration.qwk_storage COMMAND sfx_qwk_storage_tests "${CMAKE_CURRENT_BINARY_DIR}/qwk-fixtures")
    if(WIN32)
        add_executable(sfx_qwk_spool_tests test/platform/qwk_spool_tests.cpp src/apps/runtime/alpha_qwk_reply_spool.cpp)
        target_include_directories(sfx_qwk_spool_tests PRIVATE src/apps/runtime src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_qwk_spool_tests PRIVATE sfx::storage_sqlite sfx_installation_win32 sfx_platform_win32_storage sfx::libsodium)
        target_compile_definitions(sfx_qwk_spool_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_qwk_spool_tests)
        # Leave room for UUID/case names within the unpinned spool factory path limit.
        add_test(NAME sfx.platform.qwk_spool COMMAND sfx_qwk_spool_tests "${CMAKE_CURRENT_BINARY_DIR}/qwk-spool")
        add_executable(sfx_qwk_report_tests EXCLUDE_FROM_ALL test/integration/qwk_report_tests.cpp src/apps/runtime/alpha_qwk_reply_spool.cpp)
        target_include_directories(sfx_qwk_report_tests PRIVATE src/apps/runtime src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_qwk_report_tests PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium sfx_installation_win32 sfx_platform_win32_storage psapi)
        target_compile_definitions(sfx_qwk_report_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_qwk_report_tests)
        add_executable(sfx_qwk_report_installed_fixture EXCLUDE_FROM_ALL test/platform/qwk_report_installed_fixture.cpp)
        target_include_directories(sfx_qwk_report_installed_fixture PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
        target_link_libraries(sfx_qwk_report_installed_fixture PRIVATE sfx::storage_sqlite sfx::archive sfx::libarchive sfx::libsodium sfx_installation_win32 sfx_platform_win32_storage sfx_account_administration_win32 sfx_alpha_win32)
        target_compile_definitions(sfx_qwk_report_installed_fixture PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_qwk_report_installed_fixture)
        add_executable(sfx_qwk_local_time_tests test/unit/qwk_local_time_tests.cpp)
        target_link_libraries(sfx_qwk_local_time_tests PRIVATE sfx::application)
        target_compile_definitions(sfx_qwk_local_time_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_qwk_local_time_tests)
    endif()
    target_include_directories(sfx_message_storage_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_message_storage_tests PRIVATE sfx::storage_sqlite)
    sfx_configure_test_executable(sfx_message_storage_tests)
    add_test(NAME sfx.integration.message_storage COMMAND sfx_message_storage_tests "${CMAKE_CURRENT_BINARY_DIR}/test-output/section8a")
    # This suite runs many real SQLite migrations, backups and fault-recovery cases.
    set_tests_properties(sfx.integration.message_storage PROPERTIES TIMEOUT 600)
    add_executable(sfx_optional_subject_tests test/integration/optional_subject_tests.cpp test/support/src/sqlite_write_fault_vfs.cpp)
    target_include_directories(sfx_optional_subject_tests PRIVATE src/adapters/storage/src test/support/include third_party/sqlite/3.53.4)
    target_link_libraries(sfx_optional_subject_tests PRIVATE sfx::storage_sqlite)
    sfx_configure_test_executable(sfx_optional_subject_tests)
    add_test(NAME sfx.integration.optional_subject COMMAND sfx_optional_subject_tests "${CMAKE_CURRENT_BINARY_DIR}/test-output/optional-subject")
    target_include_directories(sfx_classic_information_storage_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
    target_link_libraries(sfx_classic_information_storage_tests PRIVATE sfx::storage_sqlite)
    sfx_configure_test_executable(sfx_classic_information_storage_tests)
    add_test(NAME sfx.integration.classic_information_storage COMMAND sfx_classic_information_storage_tests "${CMAKE_BINARY_DIR}/test-output/classic-information")
    target_link_libraries(sfx_caller_journal_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_caller_journal_tests)
    add_test(NAME sfx.unit.caller_journal COMMAND sfx_caller_journal_tests)
    target_link_libraries(sfx_operational_log_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_operational_log_tests)
    add_test(NAME sfx.unit.operational_log COMMAND sfx_operational_log_tests)
    add_executable(sfx_event_sounds_tests test/unit/event_sounds_tests.cpp)
    target_link_libraries(sfx_event_sounds_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_event_sounds_tests)
    add_test(NAME sfx.unit.event_sounds COMMAND sfx_event_sounds_tests)
    add_executable(sfx_hostname_lookup_tests test/unit/hostname_lookup_tests.cpp)
    target_link_libraries(sfx_hostname_lookup_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_hostname_lookup_tests)
    add_test(NAME sfx.unit.hostname_lookup COMMAND sfx_hostname_lookup_tests)

    add_executable(sfx_control_panel_tests
        test/unit/control_panel_tests.cpp
    )
    add_executable(sfx_modem_lights_tests test/unit/modem_lights_tests.cpp)
    add_executable(sfx_session_revocation_tests test/unit/session_revocation_tests.cpp)
    add_executable(sfx_local_account_administration_tests test/unit/local_account_administration_tests.cpp)
    target_link_libraries(sfx_local_account_administration_tests PRIVATE sfx::application sfx_test_support)
    sfx_configure_test_executable(sfx_local_account_administration_tests)
    add_test(NAME sfx.unit.local_account_administration COMMAND sfx_local_account_administration_tests)
    target_link_libraries(sfx_session_revocation_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_session_revocation_tests)
    add_test(NAME sfx.unit.session_revocation COMMAND sfx_session_revocation_tests)
    target_link_libraries(sfx_modem_lights_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_modem_lights_tests)
    add_test(NAME sfx.unit.modem_lights COMMAND sfx_modem_lights_tests)
    add_executable(sfx_operational_backlog_tests test/unit/operational_backlog_tests.cpp)
    target_link_libraries(sfx_operational_backlog_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_operational_backlog_tests)
    add_test(NAME sfx.unit.operational_backlog COMMAND sfx_operational_backlog_tests)
    target_link_libraries(sfx_control_panel_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_control_panel_tests)
    add_test(NAME sfx.unit.control_panel COMMAND sfx_control_panel_tests
        ${CMAKE_CURRENT_SOURCE_DIR}/src/apps/control_panel/assets/owner/sfx-node-view.ans
        ${CMAKE_CURRENT_SOURCE_DIR}/src/apps/control_panel/assets/owner/sf-x-ss.ans
        ${CMAKE_CURRENT_SOURCE_DIR}/src/apps/control_panel/assets/owner/moebius-cp437.f16)

    add_executable(sfx_authentication_security_tests
        test/unit/authentication_security_tests.cpp
    )
    target_link_libraries(sfx_authentication_security_tests PRIVATE
        sfx::authentication
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_authentication_security_tests)
    add_test(NAME sfx.unit.authentication_security COMMAND sfx_authentication_security_tests)

    add_executable(sfx_conformance_tests test/conformance/foundation_conformance_tests.cpp)
    target_link_libraries(sfx_conformance_tests PRIVATE
        sfx_test_support
        sfx::libsodium
    )
    sfx_configure_test_executable(sfx_conformance_tests)
    add_test(NAME sfx.conformance.foundation COMMAND
        sfx_conformance_tests
        "${CMAKE_CURRENT_SOURCE_DIR}/test/conformance/fixtures"
    )
    add_test(NAME sfx.conformance.fixture_failures COMMAND
        ${CMAKE_COMMAND}
        "-DSFX_CONFORMANCE_EXECUTABLE=$<TARGET_FILE:sfx_conformance_tests>"
        "-DSFX_CONFORMANCE_FIXTURE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}/test/conformance/fixtures"
        "-DSFX_CONFORMANCE_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/test-output/conformance-fixture-failures"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckConformanceFixtureFailures.cmake"
    )

    add_executable(sfx_storage_infrastructure_tests
        test/integration/storage_infrastructure_tests.cpp
    )
    target_include_directories(sfx_storage_infrastructure_tests PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_storage_infrastructure_tests PRIVATE
        sfx::storage_sqlite
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_storage_infrastructure_tests)
    add_test(
        NAME sfx.integration.storage_infrastructure
        COMMAND sfx_storage_infrastructure_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/stage5a-storage-tests
    )

    add_executable(sfx_storage_account_tests
        test/integration/storage_account_tests.cpp
    )
    target_include_directories(sfx_storage_account_tests PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_storage_account_tests PRIVATE
        sfx::storage_sqlite
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_storage_account_tests)
    target_compile_definitions(sfx_storage_account_tests PRIVATE
        SFX_ACCOUNT_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
    add_test(NAME sfx.integration.account_security_assignment
        COMMAND sfx_storage_account_tests "${CMAKE_CURRENT_BINARY_DIR}/test-output" --assignment-only)
    add_test(NAME sfx.integration.local_security_catalog
        COMMAND sfx_storage_account_tests "${CMAKE_CURRENT_BINARY_DIR}/test-output" --catalog-only)
    add_test(
        NAME sfx.integration.storage_accounts
        COMMAND sfx_storage_account_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/stage5b-storage-accounts
    )

    add_executable(sfx_sqlite_pager_metrics_tests
        test/integration/sqlite_pager_metrics_tests.cpp
    )
    add_executable(sfx_runtime_shared_services_tests test/integration/runtime_shared_services_tests.cpp)
    add_executable(sfx_runtime_control_dispatch_tests test/unit/runtime_control_dispatch_tests.cpp)
    add_executable(sfx_caller_runtime_accounting_tests test/unit/caller_runtime_accounting_tests.cpp)
    target_include_directories(sfx_caller_runtime_accounting_tests PRIVATE src/apps/runtime)
    target_link_libraries(sfx_caller_runtime_accounting_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_caller_runtime_accounting_tests)
    add_test(NAME sfx.unit.caller_runtime_accounting COMMAND sfx_caller_runtime_accounting_tests)
    target_include_directories(sfx_runtime_control_dispatch_tests PRIVATE src/apps/runtime)
    target_link_libraries(sfx_runtime_control_dispatch_tests PRIVATE sfx::application)
    sfx_configure_test_executable(sfx_runtime_control_dispatch_tests)
    add_test(NAME sfx.unit.runtime_control_dispatch COMMAND sfx_runtime_control_dispatch_tests)
    target_include_directories(sfx_runtime_shared_services_tests PRIVATE third_party/sqlite/3.53.4)
    target_link_libraries(sfx_runtime_shared_services_tests PRIVATE sfx::storage_sqlite sfx_test_support Threads::Threads)
    sfx_configure_test_executable(sfx_runtime_shared_services_tests)
    add_test(NAME sfx.integration.runtime_shared_services COMMAND sfx_runtime_shared_services_tests
        ${CMAKE_CURRENT_BINARY_DIR}/test-output/stage2-shared-services)
    set_tests_properties(sfx.integration.runtime_shared_services PROPERTIES TIMEOUT 60)
    target_link_libraries(sfx_sqlite_pager_metrics_tests PRIVATE
        sfx::storage_sqlite
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_sqlite_pager_metrics_tests)
    add_test(
        NAME sfx.integration.sqlite_pager_metrics
        COMMAND sfx_sqlite_pager_metrics_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/alpha-slice-b-sqlite-pager
    )

    add_executable(sfx_authentication_integration_tests
        test/integration/authentication_integration_tests.cpp
    )
    target_link_libraries(sfx_authentication_integration_tests PRIVATE
        sfx::authentication
        sfx::storage_sqlite
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_authentication_integration_tests)
    target_compile_definitions(sfx_authentication_integration_tests PRIVATE
        SFX_AUTH_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
    add_test(
        NAME sfx.integration.authentication
        COMMAND sfx_authentication_integration_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/stage5c-authentication
            ${CMAKE_CURRENT_SOURCE_DIR}/third_party/seclists/2026.1/xato-net-10-million-passwords-10000.txt
    )

    add_executable(sfx_phase3a_storage_tests
        test/integration/phase3a_storage_tests.cpp
    )
    target_include_directories(sfx_phase3a_storage_tests PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_phase3a_storage_tests PRIVATE
        sfx::storage_sqlite
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_phase3a_storage_tests)
    add_test(
        NAME sfx.integration.phase3a_storage
        COMMAND sfx_phase3a_storage_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/phase3a-window1-storage
    )

    add_executable(sfx_phase3a_registration_storage_tests
        test/integration/phase3a_registration_storage_tests.cpp
    )
    target_include_directories(sfx_phase3a_registration_storage_tests PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_phase3a_registration_storage_tests PRIVATE
        sfx::storage_sqlite
        sfx::libsodium
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_phase3a_registration_storage_tests)
    add_test(
        NAME sfx.integration.phase3a_registration_storage
        COMMAND sfx_phase3a_registration_storage_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/phase3a-window5-registration
    )

    add_executable(sfx_phase3a_window6c_storage_tests
        test/integration/phase3a_window6c_storage_tests.cpp
        test/support/src/sqlite_write_fault_vfs.cpp
    )
    target_include_directories(sfx_phase3a_window6c_storage_tests PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_phase3a_window6c_storage_tests PRIVATE
        sfx::authentication
        sfx::storage_sqlite
        sfx_test_support
        Threads::Threads
    )
    sfx_configure_test_executable(sfx_phase3a_window6c_storage_tests)
    add_test(
        NAME sfx.integration.phase3a_window6c_storage
        COMMAND sfx_phase3a_window6c_storage_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/phase3a-window6c-storage
    )

    add_executable(sfx_baseline_b1_tests
        test/integration/sfx_baseline_b1_tests.cpp
    )
    target_include_directories(sfx_baseline_b1_tests PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_baseline_b1_tests PRIVATE
        sfx::storage_sqlite
        sfx::libsodium
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_baseline_b1_tests)
    add_test(
        NAME sfx.integration.baseline_b1
        COMMAND sfx_baseline_b1_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/baseline-b1
    )

    add_executable(sfx_baseline_crash_tests
        test/integration/sfx_baseline_crash_tests.cpp
    )
    target_include_directories(sfx_baseline_crash_tests PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_baseline_crash_tests PRIVATE
        sfx::storage_sqlite
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_baseline_crash_tests)
    add_test(
        NAME sfx.integration.baseline_crash
        COMMAND sfx_baseline_crash_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/baseline-crash
    )

    add_executable(sfx_phase3a_window2_tests
        test/unit/phase3a_window2_tests.cpp
    )
    target_link_libraries(sfx_phase3a_window2_tests PRIVATE sfx_test_support)
    sfx_configure_test_executable(sfx_phase3a_window2_tests)
    add_test(NAME sfx.unit.phase3a_window2 COMMAND sfx_phase3a_window2_tests)

    add_executable(sfx_phase3a_window3_tests
        test/unit/phase3a_window3_tests.cpp
    )
    target_link_libraries(sfx_phase3a_window3_tests PRIVATE sfx_test_support)
    sfx_configure_test_executable(sfx_phase3a_window3_tests)
    add_test(NAME sfx.unit.phase3a_window3 COMMAND sfx_phase3a_window3_tests)

    add_executable(sfx_phase3a_window4_tests
        test/unit/phase3a_window4_tests.cpp
    )
    target_link_libraries(sfx_phase3a_window4_tests PRIVATE sfx_test_support)
    sfx_configure_test_executable(sfx_phase3a_window4_tests)
    add_test(NAME sfx.unit.phase3a_window4 COMMAND sfx_phase3a_window4_tests)

    add_executable(sfx_phase3a_window6a_tests
        test/unit/phase3a_window6a_tests.cpp
    )
    target_link_libraries(sfx_phase3a_window6a_tests PRIVATE sfx_test_support)
    sfx_configure_test_executable(sfx_phase3a_window6a_tests)
    add_test(NAME sfx.unit.phase3a_window6a COMMAND sfx_phase3a_window6a_tests)

    add_executable(sfx_phase3a_window6b_tests
        test/unit/phase3a_window6b_tests.cpp
    )
    target_link_libraries(sfx_phase3a_window6b_tests
        PRIVATE sfx_test_support Threads::Threads)
    sfx_configure_test_executable(sfx_phase3a_window6b_tests)
    add_test(NAME sfx.unit.phase3a_window6b COMMAND sfx_phase3a_window6b_tests)

    add_executable(sfx_phase3a_window6c_tests
        test/unit/phase3a_window6c_tests.cpp
    )
    target_link_libraries(sfx_phase3a_window6c_tests PRIVATE
        sfx::authentication
        sfx::storage_sqlite
        sfx_test_support
        Threads::Threads
    )
    sfx_configure_test_executable(sfx_phase3a_window6c_tests)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL 11)
        # These test executables replace both new and delete with matched malloc/free.
        # GCC can report a mismatch at the inlined call sites.
        target_compile_options(sfx_node_pool_tests PRIVATE -Wno-mismatched-new-delete)
        target_compile_options(sfx_phase3a_window6c_tests PRIVATE -Wno-mismatched-new-delete)
    endif()
    add_test(NAME sfx.unit.phase3a_window6c COMMAND sfx_phase3a_window6c_tests)

    add_executable(sfx_authentication_kdf_tests
        test/integration/authentication_kdf_tests.cpp
    )
    target_link_libraries(sfx_authentication_kdf_tests PRIVATE sfx::authentication)
    sfx_configure_test_executable(sfx_authentication_kdf_tests)
    add_test(NAME sfx.integration.authentication_kdf COMMAND sfx_authentication_kdf_tests)

    add_executable(sfx_authentication_rate_measurements
        test/integration/authentication_rate_measurements.cpp
    )
    target_link_libraries(sfx_authentication_rate_measurements PRIVATE sfx::authentication)
    sfx_configure_test_executable(sfx_authentication_rate_measurements)
    add_test(NAME sfx.integration.authentication_rate_measurements
        COMMAND sfx_authentication_rate_measurements)

    add_executable(sfx_storage_reader_pool_measurements
        test/integration/storage_reader_pool_measurements.cpp
    )
    target_include_directories(sfx_storage_reader_pool_measurements PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_storage_reader_pool_measurements PRIVATE
        sfx::storage_sqlite
        sfx_test_support
    )
    sfx_configure_test_executable(sfx_storage_reader_pool_measurements)
    add_test(
        NAME sfx.integration.storage_reader_pool
        COMMAND sfx_storage_reader_pool_measurements
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/stage5b-reader-pool
    )

    add_executable(sfx_storage_constrained_measurements
        test/integration/storage_constrained_measurements.cpp
    )
    target_include_directories(sfx_storage_constrained_measurements PRIVATE
        src/adapters/storage/src
        third_party/sqlite/3.53.4
    )
    target_link_libraries(sfx_storage_constrained_measurements PRIVATE
        sfx::storage_sqlite
    )
    sfx_configure_test_executable(sfx_storage_constrained_measurements)
    add_test(
        NAME sfx.integration.storage_constrained
        COMMAND sfx_storage_constrained_measurements
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/stage5a-storage-constrained
    )

    if(WIN32)
        add_executable(sfx_win32_storage_path_tests
            test/platform/win32_storage_path_tests.cpp
        )
        target_link_libraries(sfx_win32_storage_path_tests PRIVATE sfx_platform_win32_storage)
        sfx_configure_test_executable(sfx_win32_storage_path_tests)
        add_test(
            NAME sfx.platform.win32_storage_path
            COMMAND sfx_win32_storage_path_tests
                ${CMAKE_CURRENT_BINARY_DIR}/test-output/win32-storage-path-test
        )

        add_executable(sfx_win32_storage_dacl_tests
            test/platform/win32_storage_dacl_tests.cpp
        )
        target_link_libraries(sfx_win32_storage_dacl_tests PRIVATE sfx_platform_win32_storage)
        sfx_configure_test_executable(sfx_win32_storage_dacl_tests)
        add_test(
            NAME sfx.platform.win32_storage_dacl
            COMMAND sfx_win32_storage_dacl_tests
                ${CMAKE_CURRENT_BINARY_DIR}/test-output/win32-storage-dacl-test
        )

        add_executable(sfx_win32_trusted_display_path_tests
            test/platform/win32_trusted_display_path_tests.cpp
        )
        target_link_libraries(sfx_win32_trusted_display_path_tests
            PRIVATE sfx_platform_win32_display
        )
        sfx_configure_test_executable(
            sfx_win32_trusted_display_path_tests)
        add_test(
            NAME sfx.platform.win32_trusted_display_path
            COMMAND sfx_win32_trusted_display_path_tests
                ${CMAKE_CURRENT_BINARY_DIR}/test-output
        )

        add_executable(sfx_win32_bbs_time_zone_tests
            test/platform/win32_bbs_time_zone_tests.cpp
        )
        target_link_libraries(sfx_win32_bbs_time_zone_tests PRIVATE
            sfx_platform_win32_phase3a
            sfx_test_support
        )
        sfx_configure_test_executable(sfx_win32_bbs_time_zone_tests)
        add_test(
            NAME sfx.platform.win32_bbs_time_zone
            COMMAND sfx_win32_bbs_time_zone_tests
        )

        add_executable(sfx_alpha_runtime_platform_tests
            test/platform/alpha_runtime_platform_tests.cpp
        )
        target_link_libraries(sfx_alpha_runtime_platform_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_alpha_runtime_platform_tests)
        add_executable(sfx_cr3_configuration_platform_tests
            test/platform/cr3_configuration_platform_tests.cpp)
        target_link_libraries(sfx_cr3_configuration_platform_tests PRIVATE sfx_account_administration_win32)
        sfx_configure_test_executable(sfx_cr3_configuration_platform_tests)
        add_test(NAME sfx.platform.cr3_configuration
            COMMAND sfx_cr3_configuration_platform_tests
                ${CMAKE_CURRENT_BINARY_DIR}/test-output/cr3-configuration-platform)
        add_executable(sfx_desktop_theme_tests test/platform/desktop_theme_tests.cpp)
        target_link_libraries(sfx_desktop_theme_tests PRIVATE sfx_desktop_theme_win32)
        sfx_configure_test_executable(sfx_desktop_theme_tests)
        add_test(NAME sfx.platform.desktop_theme COMMAND sfx_desktop_theme_tests)
        add_executable(sfx_editor_placement_tests test/platform/editor_placement_tests.cpp)
        target_link_libraries(sfx_editor_placement_tests PRIVATE sfx_desktop_theme_win32 user32)
        sfx_configure_test_executable(sfx_editor_placement_tests)
        add_test(NAME sfx.platform.editor_placement COMMAND sfx_editor_placement_tests)
        add_executable(sfx_control_panel_metrics_tests test/platform/control_panel_metrics_tests.cpp)
        target_link_libraries(sfx_control_panel_metrics_tests PRIVATE sfx_alpha_win32)
        target_compile_definitions(sfx_control_panel_metrics_tests PRIVATE
            SFX_METRIC_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        sfx_configure_test_executable(sfx_control_panel_metrics_tests)
        add_test(NAME sfx.platform.control_panel_metrics COMMAND sfx_control_panel_metrics_tests)
        # Explicit bounded disk workload and offscreen shipping-painter review;
        # never run automatically on an installation or as an idle monitor task.
        add_executable(sfx_system_monitor_disk_probe test/platform/system_monitor_disk_probe.cpp)
        target_link_libraries(sfx_system_monitor_disk_probe PRIVATE sfx_alpha_win32 pdh)
        sfx_configure_test_executable(sfx_system_monitor_disk_probe)
        add_executable(sfx_system_monitor_disk_render test/platform/system_monitor_disk_render.cpp)
        target_include_directories(sfx_system_monitor_disk_render PRIVATE src/apps/control_panel)
        target_compile_definitions(sfx_system_monitor_disk_render PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
        target_link_libraries(sfx_system_monitor_disk_render PRIVATE sfx_alpha_win32
            sfx_desktop_theme_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32
            advapi32 windowscodecs ole32 shlwapi)
        sfx_configure_test_executable(sfx_system_monitor_disk_render)
        target_link_libraries(sfx_system_monitor_disk_render PRIVATE sfx_event_sound_win32)
        add_executable(sfx_alpha_bin_owner_tests test/platform/alpha_bin_owner_tests.cpp)
        target_link_libraries(sfx_alpha_bin_owner_tests PRIVATE sfx_alpha_win32)
        target_compile_definitions(sfx_alpha_bin_owner_tests PRIVATE
            SFX_BIN_OWNER_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        sfx_configure_test_executable(sfx_alpha_bin_owner_tests)
        # Explicit two-phase fixture: only seed requires a Windows elevation
        # prompt. Run verify unelevated; do not silently skip it in CTest.
        add_test(
            NAME sfx.platform.alpha_runtime
            COMMAND sfx_alpha_runtime_platform_tests
                ${CMAKE_CURRENT_BINARY_DIR}/test-output/alpha-runtime-platform
        )

        add_executable(sfx_alpha_runtime_host_tests
            test/integration/alpha_runtime_host_tests.cpp
        )
        add_executable(sfx_alpha_operational_journal_tests test/platform/alpha_operational_journal_tests.cpp)
        add_executable(sfx_durable_journal_files_tests test/platform/durable_journal_files_tests.cpp)
        add_executable(sfx_other_board_text_file_tests test/platform/other_board_text_file_tests.cpp)
        add_executable(sfx_other_board_config_offline_tests EXCLUDE_FROM_ALL test/platform/other_board_config_offline_tests.cpp)
        target_link_libraries(sfx_other_board_config_offline_tests PRIVATE sfx_account_administration_win32)
        sfx_configure_test_executable(sfx_other_board_config_offline_tests)
        target_link_libraries(sfx_other_board_text_file_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_other_board_text_file_tests)
        add_test(NAME sfx.platform.other_board_text_file COMMAND sfx_other_board_text_file_tests "${CMAKE_BINARY_DIR}/test-output/other-bbs-text")
        add_executable(sfx_section4_logging_tests test/platform/section4_logging_tests.cpp)
        target_link_libraries(sfx_section4_logging_tests PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_section4_logging_tests PRIVATE
            _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section4_logging_tests)
        add_executable(sfx_section4_feed_tests test/platform/section4_feed_tests.cpp)
        add_executable(sfx_section4_rule_files_tests test/platform/section4_rule_files_tests.cpp)
        target_link_libraries(sfx_section4_rule_files_tests PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_section4_rule_files_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        sfx_configure_test_executable(sfx_section4_rule_files_tests)
        target_link_libraries(sfx_section4_feed_tests PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_section4_feed_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        sfx_configure_test_executable(sfx_section4_feed_tests)
        add_executable(sfx_section4_domain_tests test/platform/section4_domain_tests.cpp)
        target_link_libraries(sfx_section4_domain_tests PRIVATE ws2_32 user32)
        target_compile_definitions(sfx_section4_domain_tests PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
        sfx_configure_test_executable(sfx_section4_domain_tests)
        target_link_libraries(sfx_durable_journal_files_tests PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_durable_journal_files_tests PRIVATE
            _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_durable_journal_files_tests)
        add_test(NAME sfx.platform.durable_journal_files COMMAND sfx_durable_journal_files_tests)
        # This test deliberately accepts only a disposable immediate build child.
        # Supply one even when the configured binary tree is nested more deeply.
        set(sfx_journal_work "${CMAKE_CURRENT_BINARY_DIR}/build/native-journal-work")
        file(MAKE_DIRECTORY "${sfx_journal_work}")
        set_tests_properties(sfx.platform.durable_journal_files PROPERTIES
            WORKING_DIRECTORY "${sfx_journal_work}")

        add_executable(sfx_r1_icon_resources_tests test/platform/r1_icon_resources_tests.cpp)
        target_include_directories(sfx_r1_icon_resources_tests PRIVATE src/platform/win32/include)
        target_link_libraries(sfx_r1_icon_resources_tests PRIVATE user32 gdi32 ole32 windowscodecs shlwapi)
        target_compile_definitions(sfx_r1_icon_resources_tests PRIVATE UNICODE _UNICODE WINVER=0x0601 _WIN32_WINNT=0x0601)
        sfx_configure_test_executable(sfx_r1_icon_resources_tests)
        add_executable(sfx_alpha_r1_fresh_install_state_tests EXCLUDE_FROM_ALL
            test/platform/alpha_r1_fresh_install_state_tests.cpp)
        target_link_libraries(sfx_alpha_r1_fresh_install_state_tests PRIVATE sfx_alpha_win32)
        target_compile_definitions(sfx_alpha_r1_fresh_install_state_tests PRIVATE
            _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_alpha_r1_fresh_install_state_tests)
        add_executable(sfx_alpha_hostname_native_tests test/platform/alpha_hostname_native_tests.cpp)
        target_link_libraries(sfx_alpha_hostname_native_tests PRIVATE sfx::application ws2_32 user32 Threads::Threads)
        target_compile_definitions(sfx_alpha_hostname_native_tests PRIVATE
            _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_alpha_hostname_native_tests)
        add_test(NAME sfx.platform.alpha_hostname_native COMMAND sfx_alpha_hostname_native_tests)
        target_link_libraries(sfx_alpha_operational_journal_tests PRIVATE sfx_alpha_runtime_host Threads::Threads)
        target_compile_definitions(sfx_alpha_operational_journal_tests PRIVATE
            SFX_LOG_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        sfx_configure_test_executable(sfx_alpha_operational_journal_tests)
        add_test(NAME sfx.platform.alpha_operational_journal COMMAND sfx_alpha_operational_journal_tests
            ${CMAKE_CURRENT_BINARY_DIR}/test-output/l1-journal)
        add_executable(sfx_alpha_ssh_fixture_host test/integration/alpha_ssh_fixture_host.cpp)
        add_executable(sfx_chat_live_host test/integration/chat_live_host.cpp)
        target_link_libraries(sfx_chat_live_host PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_chat_live_host PRIVATE SFX_R1_JOURNAL_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        sfx_configure_test_executable(sfx_chat_live_host)
        add_executable(sfx_chat_ssh_peer test/platform/section12_chat_ssh_peer.c)
        target_link_libraries(sfx_chat_ssh_peer PRIVATE deucessh::static)
        target_compile_definitions(sfx_chat_ssh_peer PRIVATE DSSH_STATIC _CRT_SECURE_NO_WARNINGS)
        target_compile_options(sfx_chat_ssh_peer PRIVATE /experimental:c11atomics)
        set_target_properties(sfx_chat_ssh_peer PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
        sfx_configure_test_executable(sfx_chat_ssh_peer)
        add_executable(sfx_chat_notice_ui_tests test/platform/chat_notice_ui_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_chat_notice_ui_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_chat_notice_ui_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32
            sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
        sfx_configure_test_executable(sfx_chat_notice_ui_tests)
        target_link_libraries(sfx_chat_notice_ui_tests PRIVATE sfx_event_sound_win32)
        add_executable(sfx_event_sounds_ui_tests test/platform/event_sounds_ui_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_event_sounds_ui_tests PRIVATE src/apps/control_panel)
        target_compile_definitions(sfx_event_sounds_ui_tests PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
        target_link_libraries(sfx_event_sounds_ui_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32
            sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
        sfx_configure_test_executable(sfx_event_sounds_ui_tests)
        add_executable(sfx_event_sounds_picker_tests test/platform/event_sounds_picker_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_event_sounds_picker_tests PRIVATE src/apps/control_panel)
        target_compile_definitions(sfx_event_sounds_picker_tests PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
        target_link_libraries(sfx_event_sounds_picker_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32
            sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 oleaut32 shlwapi uiautomationcore)
        sfx_configure_test_executable(sfx_event_sounds_picker_tests)
        add_executable(sfx_event_sounds_owner_tests test/platform/event_sounds_owner_tests.cpp src/apps/control_panel/control_panel.rc)
        target_include_directories(sfx_event_sounds_owner_tests PRIVATE src/apps/control_panel)
        target_compile_definitions(sfx_event_sounds_owner_tests PRIVATE SFX_ALPHA_TEST_AUTOMATION=1)
        target_link_libraries(sfx_event_sounds_owner_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32
            sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 oleaut32 shlwapi uiautomationcore)
        sfx_configure_test_executable(sfx_event_sounds_owner_tests)
        add_executable(sfx_alpha_r1_journal_host test/integration/alpha_r1_journal_host.cpp)
        add_executable(sfx_modem_ui_review test/platform/modem_ui_review.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_modem_ui_review PRIVATE src/apps/control_panel)
        target_compile_definitions(sfx_modem_ui_review PRIVATE SFX_ALPHA_TEST_AUTOMATION=1
            SFX_UI_REVIEW_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        target_link_libraries(sfx_modem_ui_review PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32
            sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
        target_link_libraries(sfx_modem_ui_review PRIVATE sfx_event_sound_win32)
        sfx_configure_test_executable(sfx_modem_ui_review)
        add_test(NAME sfx.platform.configuration_relay_lifetime COMMAND sfx_modem_ui_review --relay-only)
        add_executable(sfx_config_theme_review test/platform/config_theme_review.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_config_theme_review PRIVATE src/apps/config_ui)
        target_compile_definitions(sfx_config_theme_review PRIVATE
            SFX_UI_REVIEW_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        target_link_libraries(sfx_config_theme_review PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_config_theme_review)
        add_executable(sfx_section9_config_model_tests test/platform/section9_config_model_tests.cpp
            src/apps/config_ui/door_bulletin_window.cpp src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_section9_config_model_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_section9_config_model_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive sfx_file_manager_widgets_win32 comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_section9_config_model_tests)
        add_executable(sfx_chat_ui_tests test/platform/chat_ui_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_chat_ui_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_chat_ui_tests PRIVATE sfx_clamav_component_win32 sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_chat_ui_tests)
        add_executable(sfx_ftn_ui_tests test/platform/ftn_ui_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_ftn_ui_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_ftn_ui_tests PRIVATE sfx_ftn_download_win32 sfx_clamav_component_win32 sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_ftn_ui_tests)
        add_executable(sfx_owner_ftn_save_tests test/platform/owner_ftn_save_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_owner_ftn_save_tests PRIVATE src/apps/config_ui)
        get_target_property(_sfx_owner_ftn_ui_links sfx_ftn_ui_tests LINK_LIBRARIES)
        target_link_libraries(sfx_owner_ftn_save_tests PRIVATE ${_sfx_owner_ftn_ui_links})
        sfx_configure_test_executable(sfx_owner_ftn_save_tests)
        add_executable(sfx_checkbox_render_tests test/platform/checkbox_render_tests.cpp)
        target_link_libraries(sfx_checkbox_render_tests PRIVATE sfx_desktop_theme_win32 comctl32 gdiplus)
        target_compile_definitions(sfx_checkbox_render_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_checkbox_render_tests)
        add_test(NAME sfx.platform.checkbox_font COMMAND sfx_checkbox_render_tests --auto)
        add_executable(sfx_checkbox_native_tests test/platform/checkbox_native_tests.cpp src/apps/config_ui/config_ui.manifest)
        target_link_libraries(sfx_checkbox_native_tests PRIVATE sfx_desktop_theme_win32 comctl32 gdiplus)
        target_compile_definitions(sfx_checkbox_native_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_checkbox_native_tests)

        add_executable(sfx_ftn_import_ui_tests EXCLUDE_FROM_ALL test/platform/ftn_import_ui_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_ftn_import_ui_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_ftn_import_ui_tests PRIVATE ${_sfx_owner_ftn_ui_links})
        sfx_configure_test_executable(sfx_ftn_import_ui_tests)
        add_executable(sfx_network_setup_ui_tests test/platform/network_setup_ui_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_network_setup_ui_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_network_setup_ui_tests PRIVATE ${_sfx_owner_ftn_ui_links})
        sfx_configure_test_executable(sfx_network_setup_ui_tests)
        add_executable(sfx_ftn_download_tests test/platform/ftn_download_tests.cpp)
        target_link_libraries(sfx_ftn_download_tests PRIVATE sfx_ftn_download_win32)
        sfx_configure_test_executable(sfx_ftn_download_tests)
        add_executable(sfx_ftn_picker_tests test/platform/ftn_picker_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_ftn_picker_tests PRIVATE src/apps/config_ui)
        get_target_property(_sfx_ftn_ui_links sfx_ftn_ui_tests LINK_LIBRARIES)
        target_link_libraries(sfx_ftn_picker_tests PRIVATE ${_sfx_ftn_ui_links} oleaut32 uiautomationcore)
        sfx_configure_test_executable(sfx_ftn_picker_tests)
        add_executable(sfx_ftn_panel_tests test/platform/ftn_panel_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_panel_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_ftn_panel_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32
            sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
        sfx_configure_test_executable(sfx_ftn_panel_tests)
        add_executable(sfx_ftn_external_visible_config EXCLUDE_FROM_ALL test/platform/ftn_external_visible_config.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_ftn_external_visible_config PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_ftn_external_visible_config PRIVATE ${_sfx_ftn_ui_links})
        sfx_configure_test_executable(sfx_ftn_external_visible_config)
        add_executable(sfx_ftn_external_visible_panel EXCLUDE_FROM_ALL test/platform/ftn_external_visible_panel.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_external_visible_panel PRIVATE src/apps/control_panel)
        get_target_property(_sfx_external_panel_links sfx_ftn_panel_tests LINK_LIBRARIES)
        target_link_libraries(sfx_ftn_external_visible_panel PRIVATE ${_sfx_external_panel_links})
        sfx_configure_test_executable(sfx_ftn_external_visible_panel)
        add_executable(sfx_ftn_pre_setup_backup_tests test/platform/ftn_pre_setup_backup_tests.cpp)
        target_link_libraries(sfx_ftn_pre_setup_backup_tests PRIVATE sfx_alpha_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_pre_setup_backup_tests)
        add_executable(sfx_ftn_retention_resume_tests test/platform/ftn_retention_resume_tests.cpp)
        target_link_libraries(sfx_ftn_retention_resume_tests PRIVATE sfx_alpha_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_retention_resume_tests)
        add_executable(sfx_ftn_installed_positive_fixture test/platform/ftn_installed_positive_fixture.cpp)
        target_link_libraries(sfx_ftn_installed_positive_fixture PRIVATE sfx_alpha_win32 sfx_ftn_native_win32 sfx_file_administration_client_win32 sfx_file_payload_win32 sfx::storage_sqlite sfx::archive sfx::libsodium)
        sfx_configure_test_executable(sfx_ftn_installed_positive_fixture)
        add_executable(sfx_ftn_installed_tests test/platform/ftn_installed_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_installed_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_ftn_installed_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32
            sfx_event_sound_win32 sfx_platform_win32_phase3a comctl32 comdlg32 shell32 advapi32 windowscodecs ole32 shlwapi)
        sfx_configure_test_executable(sfx_ftn_installed_tests)
        add_executable(sfx_ftn_installed_ui_tests test/platform/ftn_installed_ui_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_installed_ui_tests PRIVATE src/apps/control_panel)
        get_target_property(_sfx_ftn_installed_links sfx_ftn_installed_tests LINK_LIBRARIES)
        add_executable(sfx_ftn_installed_hatch_tests test/platform/ftn_installed_hatch_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_installed_hatch_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_ftn_installed_hatch_tests PRIVATE ${_sfx_ftn_installed_links} sfx_file_administration_client_win32 sfx_file_payload_win32)
        sfx_configure_test_executable(sfx_ftn_installed_hatch_tests)
        target_link_libraries(sfx_ftn_installed_ui_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_installed_ui_tests)
        add_executable(sfx_owner_ftn_installed_ui_tests test/platform/owner_ftn_installed_ui_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_owner_ftn_installed_ui_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_owner_ftn_installed_ui_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
        sfx_configure_test_executable(sfx_owner_ftn_installed_ui_tests)
        add_executable(sfx_rc10_installed_config_tests test/platform/rc10_installed_config_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_rc10_installed_config_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_rc10_installed_config_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
        sfx_configure_test_executable(sfx_rc10_installed_config_tests)
        add_executable(sfx_rc11_navigation_tests test/platform/rc11_navigation_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_rc11_navigation_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_rc11_navigation_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
        sfx_configure_test_executable(sfx_rc11_navigation_tests)
        add_executable(sfx_hosting_navigation_tests EXCLUDE_FROM_ALL test/platform/hosting_navigation_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_hosting_navigation_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_hosting_navigation_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
        sfx_configure_test_executable(sfx_hosting_navigation_tests)
        # Fixed new normal-Setup installation for the approved network-flow release.
        add_executable(sfx_network_setup_normal_setup_tests EXCLUDE_FROM_ALL
            test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_network_setup_normal_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_network_setup_normal_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_network_setup_normal_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_network_setup_normal_setup_tests)
        sfx_clamav_https(sfx_network_setup_normal_setup_tests)
        # Read-only retained RC12 payload, independently checked before use.
        set(_network_predecessor_payload "${CMAKE_CURRENT_SOURCE_DIR}/build/section14-windows-r1/setup-payload/Release")
        if(EXISTS "${_network_predecessor_payload}/payload.rc")
        add_executable(sfx_network_setup_predecessor_setup_tests EXCLUDE_FROM_ALL
            test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${_network_predecessor_payload}/payload.rc")
        target_include_directories(sfx_network_setup_predecessor_setup_tests PRIVATE src/apps/setup "${_network_predecessor_payload}")
        target_link_libraries(sfx_network_setup_predecessor_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_network_setup_predecessor_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_network_setup_predecessor_setup_tests)
        sfx_clamav_https(sfx_network_setup_predecessor_setup_tests)
        endif()
        foreach(_network_driver bootstrap installed navigation)
            add_executable(sfx_network_setup_${_network_driver}_tests EXCLUDE_FROM_ALL
                test/platform/network_setup_${_network_driver}_tests.cpp
                src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
            target_include_directories(sfx_network_setup_${_network_driver}_tests PRIVATE src/apps/control_panel src/adapters/storage/src third_party/sqlite/3.53.4)
            target_link_libraries(sfx_network_setup_${_network_driver}_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
            sfx_configure_test_executable(sfx_network_setup_${_network_driver}_tests)
        endforeach()
        foreach(_rc14_setup current predecessor)
            if(_rc14_setup STREQUAL "current")
                set(_rc14_payload "${SFX_SETUP_PAYLOAD}")
            else()
                set(_rc14_payload "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/evidence/owner-rc14-20261102/predecessor-payload")
            endif()
            if(_rc14_setup STREQUAL "current" OR EXISTS "${_rc14_payload}/payload.rc")
                add_executable(sfx_rc14_${_rc14_setup}_setup_tests EXCLUDE_FROM_ALL
                    test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
                    src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
                    src/apps/setup/setup.manifest "${_rc14_payload}/payload.rc")
                target_include_directories(sfx_rc14_${_rc14_setup}_setup_tests PRIVATE src/apps/setup "${_rc14_payload}")
                target_link_libraries(sfx_rc14_${_rc14_setup}_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
                target_compile_definitions(sfx_rc14_${_rc14_setup}_setup_tests PRIVATE SFX_RC14_QUALIFICATION=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
                sfx_configure_test_executable(sfx_rc14_${_rc14_setup}_setup_tests)
                sfx_clamav_https(sfx_rc14_${_rc14_setup}_setup_tests)
            endif()
        endforeach()
        foreach(_rc14_driver bootstrap import_installed navigation)
            if(_rc14_driver STREQUAL "navigation")
                set(_rc14_source test/platform/network_setup_navigation_tests.cpp)
            else()
                set(_rc14_source test/platform/rc14_${_rc14_driver}_tests.cpp)
            endif()
            add_executable(sfx_rc14_${_rc14_driver}_tests EXCLUDE_FROM_ALL ${_rc14_source}
                src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
            target_include_directories(sfx_rc14_${_rc14_driver}_tests PRIVATE src/apps/control_panel src/adapters/storage/src third_party/sqlite/3.53.4)
            target_compile_definitions(sfx_rc14_${_rc14_driver}_tests PRIVATE SFX_RC14_QUALIFICATION=1)
            target_link_libraries(sfx_rc14_${_rc14_driver}_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
            sfx_configure_test_executable(sfx_rc14_${_rc14_driver}_tests)
        endforeach()
        foreach(_rc15_setup current predecessor)
            if(_rc15_setup STREQUAL "current")
                set(_rc15_payload "${SFX_SETUP_PAYLOAD}")
            else()
                set(_rc15_payload "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/evidence/owner-rc15-20261102/predecessor-payload")
            endif()
            if(_rc15_setup STREQUAL "current" OR EXISTS "${_rc15_payload}/payload.rc")
                add_executable(sfx_rc15_${_rc15_setup}_setup_tests EXCLUDE_FROM_ALL
                    test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
                    src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
                    src/apps/setup/setup.manifest "${_rc15_payload}/payload.rc")
                target_include_directories(sfx_rc15_${_rc15_setup}_setup_tests PRIVATE src/apps/setup "${_rc15_payload}")
                target_link_libraries(sfx_rc15_${_rc15_setup}_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
                target_compile_definitions(sfx_rc15_${_rc15_setup}_setup_tests PRIVATE SFX_RC15_QUALIFICATION=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
                sfx_configure_test_executable(sfx_rc15_${_rc15_setup}_setup_tests)
                sfx_clamav_https(sfx_rc15_${_rc15_setup}_setup_tests)
            endif()
        endforeach()
        foreach(_rc15_driver bootstrap import_installed navigation binkp_installed)
            if(_rc15_driver STREQUAL "navigation")
                set(_rc15_source test/platform/network_setup_navigation_tests.cpp)
            else()
                set(_rc15_source test/platform/rc15_${_rc15_driver}_tests.cpp)
            endif()
            add_executable(sfx_rc15_${_rc15_driver}_tests EXCLUDE_FROM_ALL ${_rc15_source}
                src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
            target_include_directories(sfx_rc15_${_rc15_driver}_tests PRIVATE src/apps/control_panel src/adapters/storage/src third_party/sqlite/3.53.4)
            target_compile_definitions(sfx_rc15_${_rc15_driver}_tests PRIVATE SFX_RC15_QUALIFICATION=1)
            target_link_libraries(sfx_rc15_${_rc15_driver}_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
            sfx_configure_test_executable(sfx_rc15_${_rc15_driver}_tests)
        endforeach()
        foreach(_rc16_setup current predecessor)
            if(_rc16_setup STREQUAL "current")
                set(_rc16_payload "${SFX_SETUP_PAYLOAD}")
            else()
                set(_rc16_payload "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/evidence/owner-rc16-20261103/predecessor-payload")
            endif()
            if(_rc16_setup STREQUAL "current" OR EXISTS "${_rc16_payload}/payload.rc")
                add_executable(sfx_rc16_${_rc16_setup}_setup_tests EXCLUDE_FROM_ALL
                    test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
                    src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
                    src/apps/setup/setup.manifest "${_rc16_payload}/payload.rc")
                target_include_directories(sfx_rc16_${_rc16_setup}_setup_tests PRIVATE src/apps/setup "${_rc16_payload}")
                target_link_libraries(sfx_rc16_${_rc16_setup}_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
                target_compile_definitions(sfx_rc16_${_rc16_setup}_setup_tests PRIVATE SFX_RC16_QUALIFICATION=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
                sfx_configure_test_executable(sfx_rc16_${_rc16_setup}_setup_tests)
                sfx_clamav_https(sfx_rc16_${_rc16_setup}_setup_tests)
            endif()
        endforeach()
        foreach(_rc16_driver bootstrap import_installed navigation binkp_installed)
            if(_rc16_driver STREQUAL "navigation")
                set(_rc16_source test/platform/network_setup_navigation_tests.cpp)
            else()
                set(_rc16_source test/platform/rc16_${_rc16_driver}_tests.cpp)
            endif()
            add_executable(sfx_rc16_${_rc16_driver}_tests EXCLUDE_FROM_ALL ${_rc16_source}
                src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
            target_include_directories(sfx_rc16_${_rc16_driver}_tests PRIVATE src/apps/control_panel src/adapters/storage/src third_party/sqlite/3.53.4)
            target_compile_definitions(sfx_rc16_${_rc16_driver}_tests PRIVATE SFX_RC16_QUALIFICATION=1)
            target_link_libraries(sfx_rc16_${_rc16_driver}_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
            sfx_configure_test_executable(sfx_rc16_${_rc16_driver}_tests)
        endforeach()
        foreach(_rc17_setup current predecessor)
            if(_rc17_setup STREQUAL "current")
                set(_rc17_payload "${SFX_SETUP_PAYLOAD}")
            else()
                set(_rc17_payload "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/evidence/owner-rc17-20261103/predecessor-payload")
            endif()
            if(_rc17_setup STREQUAL "current" OR EXISTS "${_rc17_payload}/payload.rc")
                add_executable(sfx_rc17_${_rc17_setup}_setup_tests EXCLUDE_FROM_ALL
                    test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
                    src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
                    src/apps/setup/setup.manifest "${_rc17_payload}/payload.rc")
                target_include_directories(sfx_rc17_${_rc17_setup}_setup_tests PRIVATE src/apps/setup "${_rc17_payload}")
                target_link_libraries(sfx_rc17_${_rc17_setup}_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
                target_compile_definitions(sfx_rc17_${_rc17_setup}_setup_tests PRIVATE SFX_RC17_QUALIFICATION=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
                sfx_configure_test_executable(sfx_rc17_${_rc17_setup}_setup_tests)
                sfx_clamav_https(sfx_rc17_${_rc17_setup}_setup_tests)
            endif()
        endforeach()
        foreach(_rc17_driver bootstrap import_installed navigation binkp_installed)
            if(_rc17_driver STREQUAL "navigation")
                set(_rc17_source test/platform/network_setup_navigation_tests.cpp)
            else()
                set(_rc17_source test/platform/rc17_${_rc17_driver}_tests.cpp)
            endif()
            add_executable(sfx_rc17_${_rc17_driver}_tests EXCLUDE_FROM_ALL ${_rc17_source}
                src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
            target_include_directories(sfx_rc17_${_rc17_driver}_tests PRIVATE src/apps/control_panel src/adapters/storage/src third_party/sqlite/3.53.4)
            target_compile_definitions(sfx_rc17_${_rc17_driver}_tests PRIVATE SFX_RC17_QUALIFICATION=1)
            target_link_libraries(sfx_rc17_${_rc17_driver}_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
            sfx_configure_test_executable(sfx_rc17_${_rc17_driver}_tests)
        endforeach()
        foreach(_rc18_setup current predecessor)
            if(_rc18_setup STREQUAL "current")
                set(_rc18_payload "${SFX_SETUP_PAYLOAD}")
            else()
                set(_rc18_payload "${CMAKE_CURRENT_SOURCE_DIR}/research/architecture/evidence/owner-rc18-20261103/predecessor-payload")
            endif()
            if(_rc18_setup STREQUAL "current" OR EXISTS "${_rc18_payload}/payload.rc")
                add_executable(sfx_rc18_${_rc18_setup}_setup_tests EXCLUDE_FROM_ALL
                    test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
                    src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
                    src/apps/setup/setup.manifest "${_rc18_payload}/payload.rc")
                target_include_directories(sfx_rc18_${_rc18_setup}_setup_tests PRIVATE src/apps/setup "${_rc18_payload}")
                target_link_libraries(sfx_rc18_${_rc18_setup}_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
                target_compile_definitions(sfx_rc18_${_rc18_setup}_setup_tests PRIVATE SFX_RC18_QUALIFICATION=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
                sfx_configure_test_executable(sfx_rc18_${_rc18_setup}_setup_tests)
                sfx_clamav_https(sfx_rc18_${_rc18_setup}_setup_tests)
            endif()
        endforeach()
        foreach(_rc18_driver bootstrap import_installed navigation binkp_installed caller_installed)
            if(_rc18_driver STREQUAL "navigation")
                set(_rc18_source test/platform/network_setup_navigation_tests.cpp)
            else()
                set(_rc18_source test/platform/rc18_${_rc18_driver}_tests.cpp)
            endif()
            add_executable(sfx_rc18_${_rc18_driver}_tests EXCLUDE_FROM_ALL ${_rc18_source}
                src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
            target_include_directories(sfx_rc18_${_rc18_driver}_tests PRIVATE src/apps/control_panel src/adapters/storage/src third_party/sqlite/3.53.4)
            target_compile_definitions(sfx_rc18_${_rc18_driver}_tests PRIVATE SFX_RC18_QUALIFICATION=1)
            target_link_libraries(sfx_rc18_${_rc18_driver}_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
            sfx_configure_test_executable(sfx_rc18_${_rc18_driver}_tests)
        endforeach()
        add_executable(sfx_rc17_owner_tests EXCLUDE_FROM_ALL test/integration/rc17_owner_tests.cpp)
        target_include_directories(sfx_rc17_owner_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_rc17_owner_tests PRIVATE sfx_alpha_runtime_host sfx_ftn_native_win32 sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_rc17_owner_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc17_owner_tests)
        add_executable(sfx_rc18_timezone_tests EXCLUDE_FROM_ALL test/integration/rc18_timezone_tests.cpp)
        target_include_directories(sfx_rc18_timezone_tests PRIVATE test/support/include src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_rc18_timezone_tests PRIVATE sfx_alpha_runtime_host sfx_ftn_native_win32 sfx::archive sfx::libarchive sfx::libsodium)
        target_compile_definitions(sfx_rc18_timezone_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc18_timezone_tests)
        add_executable(sfx_door_native_fixture EXCLUDE_FROM_ALL test/platform/door_native_fixture.cpp)
        target_link_libraries(sfx_door_native_fixture PRIVATE ws2_32 user32)
        sfx_configure_test_executable(sfx_door_native_fixture)
        add_executable(sfx_door_native_tests EXCLUDE_FROM_ALL test/platform/door_native_tests.cpp)
        target_link_libraries(sfx_door_native_tests PRIVATE sfx_alpha_runtime_host ws2_32)
        sfx_configure_test_executable(sfx_door_native_tests)
        add_executable(sfx_dos_host_protocol_tests EXCLUDE_FROM_ALL test/unit/dos_host_protocol_tests.cpp)
        target_include_directories(sfx_dos_host_protocol_tests PRIVATE src/core/include)
        sfx_configure_test_executable(sfx_dos_host_protocol_tests)
        add_executable(sfx_rc19_setup_tests EXCLUDE_FROM_ALL
            test/platform/network_setup_normal_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_rc19_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_rc19_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_rc19_setup_tests PRIVATE SFX_RC19_QUALIFICATION=1 WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc19_setup_tests)
        sfx_clamav_https(sfx_rc19_setup_tests)
        add_executable(sfx_section14a_setup_tests EXCLUDE_FROM_ALL
            test/platform/section14a_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_section14a_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_section14a_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_section14a_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section14a_setup_tests)
        sfx_clamav_https(sfx_section14a_setup_tests)
        add_executable(sfx_beta1_setup_tests EXCLUDE_FROM_ALL
            test/platform/beta1_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_beta1_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_beta1_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_beta1_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta1_setup_tests)
        add_executable(sfx_beta2_setup_tests EXCLUDE_FROM_ALL
            test/platform/beta2_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_beta2_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_beta2_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_beta2_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta2_setup_tests)
        sfx_clamav_https(sfx_beta2_setup_tests)
        add_executable(sfx_beta3_setup_tests EXCLUDE_FROM_ALL
            test/platform/beta3_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_beta3_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_beta3_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_beta3_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta3_setup_tests)
        sfx_clamav_https(sfx_beta3_setup_tests)
        add_executable(sfx_beta4_setup_tests EXCLUDE_FROM_ALL
            test/platform/beta4_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_beta4_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_beta4_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_beta4_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta4_setup_tests)
        sfx_clamav_https(sfx_beta4_setup_tests)
        set(SFX_SETUP_PLANNING_PAYLOAD "" CACHE PATH "Frozen extracted media for read-only planning measurements")
        if(SFX_SETUP_PLANNING_PAYLOAD)
            add_executable(sfx_setup_planning_diagnostic EXCLUDE_FROM_ALL
                test/platform/setup_planning_diagnostic.cpp src/apps/setup/setup_engine.cpp
                src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
                src/apps/setup/setup.manifest "${SFX_SETUP_PLANNING_PAYLOAD}/payload.rc")
            target_include_directories(sfx_setup_planning_diagnostic PRIVATE src/apps/setup "${SFX_SETUP_PLANNING_PAYLOAD}")
            target_link_libraries(sfx_setup_planning_diagnostic PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
            target_compile_definitions(sfx_setup_planning_diagnostic PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
            sfx_configure_test_executable(sfx_setup_planning_diagnostic)
            sfx_clamav_https(sfx_setup_planning_diagnostic)
        endif()
        add_executable(sfx_backup_permissions_setup_tests EXCLUDE_FROM_ALL
            test/platform/backup_permissions_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_backup_permissions_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_backup_permissions_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_backup_permissions_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_backup_permissions_setup_tests)
        sfx_clamav_https(sfx_backup_permissions_setup_tests)
        sfx_clamav_https(sfx_beta1_setup_tests)
        add_executable(sfx_beta1_ftn_history_tests EXCLUDE_FROM_ALL test/platform/beta1_ftn_history_tests.cpp)
        target_include_directories(sfx_beta1_ftn_history_tests PRIVATE src/adapters/storage/src third_party/sqlite/3.53.4)
        target_link_libraries(sfx_beta1_ftn_history_tests PRIVATE sfx_account_administration_win32 sfx_ftn_native_win32 sfx::libsodium)
        target_compile_definitions(sfx_beta1_ftn_history_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta1_ftn_history_tests)
        add_executable(sfx_beta1_setup_docs_tests EXCLUDE_FROM_ALL
            test/platform/beta1_setup_docs_tests.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_beta1_setup_docs_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_beta1_setup_docs_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_beta1_setup_docs_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta1_setup_docs_tests)
        sfx_clamav_https(sfx_beta1_setup_docs_tests)
        add_executable(sfx_beta1_setup_caption_tests EXCLUDE_FROM_ALL
            test/platform/beta1_setup_caption_tests.cpp src/apps/setup/setup.rc
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_beta1_setup_caption_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_beta1_setup_caption_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_beta1_setup_caption_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_beta1_setup_caption_tests)
        sfx_clamav_https(sfx_beta1_setup_caption_tests)
        add_executable(sfx_rc10_stock_menu_tests EXCLUDE_FROM_ALL
            test/platform/rc10_stock_menu_tests.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_rc10_stock_menu_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_rc10_stock_menu_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_rc10_stock_menu_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc10_stock_menu_tests)
        sfx_clamav_https(sfx_rc10_stock_menu_tests)
        add_executable(sfx_rc10_setup_tests EXCLUDE_FROM_ALL
            test/platform/rc10_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_rc10_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_rc10_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_rc10_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc10_setup_tests)
        sfx_clamav_https(sfx_rc10_setup_tests)
        add_executable(sfx_rc11_setup_tests EXCLUDE_FROM_ALL
            test/platform/rc11_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_rc11_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_rc11_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_rc11_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc11_setup_tests)
        sfx_clamav_https(sfx_rc11_setup_tests)
        add_executable(sfx_rc12_setup_tests EXCLUDE_FROM_ALL
            test/platform/rc12_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_rc12_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_rc12_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_rc12_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_rc12_setup_tests)
        sfx_clamav_https(sfx_rc12_setup_tests)
        add_executable(sfx_section15_setup_tests EXCLUDE_FROM_ALL
            test/platform/section15_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_section15_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_section15_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_section15_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_section15_setup_tests)
        sfx_clamav_https(sfx_section15_setup_tests)
        add_executable(sfx_section15_legacy_tests EXCLUDE_FROM_ALL test/platform/section15_legacy_tests.cpp)
        target_link_libraries(sfx_section15_legacy_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx::archive ws2_32)
        sfx_configure_test_executable(sfx_section15_legacy_tests)
        add_executable(sfx_section15_service_security_tests EXCLUDE_FROM_ALL test/platform/section15_service_security_tests.cpp)
        target_link_libraries(sfx_section15_service_security_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_section15_service_security_tests)
        add_executable(sfx_config_service_setup_tests EXCLUDE_FROM_ALL
            test/platform/config_service_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_config_service_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_config_service_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_config_service_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_config_service_setup_tests)
        sfx_clamav_https(sfx_config_service_setup_tests)
        add_executable(sfx_config_service_legacy_tests EXCLUDE_FROM_ALL test/platform/config_service_legacy_tests.cpp)
        target_link_libraries(sfx_config_service_legacy_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx::archive ws2_32)
        sfx_configure_test_executable(sfx_config_service_legacy_tests)
        add_executable(sfx_config_service_security_tests EXCLUDE_FROM_ALL test/platform/config_service_security_tests.cpp)
        target_link_libraries(sfx_config_service_security_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_config_service_security_tests)
        add_executable(sfx_config_service_fault_tests EXCLUDE_FROM_ALL test/platform/config_service_fault_tests.cpp)
        target_link_libraries(sfx_config_service_fault_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_config_service_fault_tests)
        add_executable(sfx_service_button_fault_tests EXCLUDE_FROM_ALL test/platform/service_button_fault_tests.cpp)
        target_link_libraries(sfx_service_button_fault_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_service_button_fault_tests)
        add_executable(sfx_hosting_fault_tests EXCLUDE_FROM_ALL test/platform/hosting_fault_tests.cpp)
        target_link_libraries(sfx_hosting_fault_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_hosting_fault_tests)
        add_executable(sfx_hosting_security_tests EXCLUDE_FROM_ALL test/platform/hosting_security_tests.cpp)
        target_link_libraries(sfx_hosting_security_tests PRIVATE sfx_alpha_win32)
        sfx_configure_test_executable(sfx_hosting_security_tests)
        add_executable(sfx_hosting_setup_tests EXCLUDE_FROM_ALL test/platform/hosting_setup_tests.cpp src/apps/setup/setup_engine.cpp
            src/apps/setup/clamav_download.cpp src/apps/setup/clamav_maintenance.cpp
            src/apps/setup/setup.manifest "${SFX_SETUP_PAYLOAD}/payload.rc")
        target_include_directories(sfx_hosting_setup_tests PRIVATE src/apps/setup "${SFX_SETUP_PAYLOAD}")
        target_link_libraries(sfx_hosting_setup_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx_clamav_component_win32 sfx_file_backup_win32 sfx::archive shell32 ole32 uuid version winhttp)
        target_compile_definitions(sfx_hosting_setup_tests PRIVATE WINVER=0x0601 _WIN32_WINNT=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_hosting_setup_tests)
        sfx_clamav_https(sfx_hosting_setup_tests)
        add_executable(sfx_section14a_runtime_tests EXCLUDE_FROM_ALL test/platform/section14a_runtime_tests.cpp)
        target_link_libraries(sfx_section14a_runtime_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx::archive ws2_32)
        sfx_configure_test_executable(sfx_section14a_runtime_tests)
        add_executable(sfx_rc19_bootstrap_tests EXCLUDE_FROM_ALL test/platform/rc18_bootstrap_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_rc19_bootstrap_tests PRIVATE src/apps/control_panel src/adapters/storage/src third_party/sqlite/3.53.4)
        target_compile_definitions(sfx_rc19_bootstrap_tests PRIVATE SFX_RC19_QUALIFICATION=1)
        target_link_libraries(sfx_rc19_bootstrap_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
        sfx_configure_test_executable(sfx_rc19_bootstrap_tests)
        add_executable(sfx_rc12_idle_tests test/platform/rc12_idle_tests.cpp
            ${_sfx_control_panel_test_resources})
        target_include_directories(sfx_rc12_idle_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_rc12_idle_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
        sfx_configure_test_executable(sfx_rc12_idle_tests)
        add_executable(sfx_rc12_sessions_host test/platform/rc12_sessions_host.cpp)
        target_include_directories(sfx_rc12_sessions_host PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_rc12_sessions_host PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx_ftn_crypto_win32 sfx::archive)
        sfx_configure_test_executable(sfx_rc12_sessions_host)
        add_executable(sfx_owner_ftn_bootstrap_tests test/platform/owner_ftn_bootstrap_tests.cpp)
        target_link_libraries(sfx_owner_ftn_bootstrap_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx::archive)
        sfx_configure_test_executable(sfx_owner_ftn_bootstrap_tests)
        add_executable(sfx_ftn_shipping_fileman_tests test/platform/ftn_shipping_fileman_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_shipping_fileman_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_ftn_shipping_fileman_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_shipping_fileman_tests)
        add_executable(sfx_ftn_shipping_retention_ui_tests test/platform/ftn_shipping_retention_ui_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_shipping_retention_ui_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_ftn_shipping_retention_ui_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_shipping_retention_ui_tests)
        add_executable(sfx_ftn_installed_restore_tests test/platform/ftn_installed_restore_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_installed_restore_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_ftn_installed_restore_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_installed_restore_tests)
        add_executable(sfx_ftn_current_restore_tests test/platform/ftn_current_restore_tests.cpp)
        target_link_libraries(sfx_ftn_current_restore_tests PRIVATE sfx_alpha_win32 sfx_account_administration_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_current_restore_tests)
        add_executable(sfx_ftn_current_config_view_tests test/platform/ftn_current_config_view_tests.cpp
            src/apps/control_panel/control_panel.rc src/apps/control_panel/control_panel.manifest)
        target_include_directories(sfx_ftn_current_config_view_tests PRIVATE src/apps/control_panel)
        target_link_libraries(sfx_ftn_current_config_view_tests PRIVATE ${_sfx_ftn_installed_links} sfx_account_administration_win32 sfx::archive)
        sfx_configure_test_executable(sfx_ftn_current_config_view_tests)
        add_executable(sfx_ftn_fileman_tests test/platform/ftn_fileman_tests.cpp
            src/apps/file_manager/file_manager.rc src/apps/file_manager/file_manager.manifest)
        target_include_directories(sfx_ftn_fileman_tests PRIVATE src/apps/file_manager)
        target_link_libraries(sfx_ftn_fileman_tests PRIVATE sfx_file_manager_widgets_win32 sfx_platform_win32_phase3a bcrypt)
        sfx_configure_test_executable(sfx_ftn_fileman_tests)
        add_executable(sfx_door_config_tests test/platform/door_config_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_door_config_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_door_config_tests PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_door_config_tests)
        add_executable(sfx_owner_scroll_tests test/platform/config_scroll_owner.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_owner_scroll_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_owner_scroll_tests PRIVATE sfx_clamav_component_win32 sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_owner_scroll_tests)
        add_executable(sfx_rc8_config_tests test/platform/rc8_config_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_rc8_config_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_rc8_config_tests PRIVATE sfx_ftn_download_win32 sfx_clamav_component_win32 sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_rc8_config_tests)
        add_executable(sfx_door_round2_tests test/platform/door_round2_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_door_round2_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_door_round2_tests PRIVATE sfx_clamav_component_win32 sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_door_round2_tests)
        add_executable(sfx_door_round3_tests test/platform/door_round3_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_door_round3_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_door_round3_tests PRIVATE sfx_clamav_component_win32 sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_door_round3_tests)
        add_executable(sfx_door_actions_tests test/platform/door_actions_tests.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_door_actions_tests PRIVATE src/apps/config_ui)
        target_link_libraries(sfx_door_actions_tests PRIVATE sfx_clamav_component_win32 sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_door_actions_tests)
        add_executable(sfx_section8c_config_review EXCLUDE_FROM_ALL test/platform/section8c_config_review.cpp
            src/apps/config_ui/config_ui.rc src/apps/config_ui/config_ui.manifest)
        target_include_directories(sfx_section8c_config_review PRIVATE src/apps/config_ui)
        target_compile_definitions(sfx_section8c_config_review PRIVATE SFX_UI_REVIEW_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        target_link_libraries(sfx_section8c_config_review PRIVATE sfx_alpha_win32 sfx_desktop_theme_win32 sfx_account_administration_win32
            sfx_platform_win32_phase3a sfx_platform_win32_display sfx::archive comctl32 comdlg32 shell32 advapi32 ole32 uuid)
        sfx_configure_test_executable(sfx_section8c_config_review)
        target_link_libraries(sfx_alpha_r1_journal_host PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_alpha_r1_journal_host PRIVATE
            SFX_R1_JOURNAL_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output"
            _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_alpha_r1_journal_host)
        add_executable(sfx_alpha_r1_installed_seed test/integration/alpha_r1_installed_seed.cpp)
        target_link_libraries(sfx_alpha_r1_installed_seed PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_alpha_r1_installed_seed PRIVATE
            SFX_R1_JOURNAL_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output"
            _WIN32_WINNT=0x0601 WINVER=0x0601 WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
        sfx_configure_test_executable(sfx_alpha_r1_installed_seed)
        add_executable(sfx_alpha_ssh_backend_tests test/platform/alpha_ssh_backend_tests.cpp)
        add_executable(sfx_backup_permissions_ssh_key_tests EXCLUDE_FROM_ALL test/platform/backup_permissions_ssh_key_tests.cpp)
        target_link_libraries(sfx_backup_permissions_ssh_key_tests PRIVATE sfx_alpha_runtime_host Threads::Threads)
        sfx_configure_test_executable(sfx_backup_permissions_ssh_key_tests)
        target_link_libraries(sfx_alpha_ssh_backend_tests PRIVATE sfx_alpha_runtime_host Threads::Threads)
        target_compile_definitions(sfx_alpha_ssh_backend_tests PRIVATE
            SFX_SSH_BACKEND_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        sfx_configure_test_executable(sfx_alpha_ssh_backend_tests)
        add_test(NAME sfx.platform.alpha_ssh_backend COMMAND sfx_alpha_ssh_backend_tests)
        set_tests_properties(sfx.platform.alpha_ssh_backend PROPERTIES TIMEOUT 60)
        target_link_libraries(sfx_alpha_ssh_fixture_host PRIVATE sfx_alpha_runtime_host)
        target_compile_definitions(sfx_alpha_ssh_fixture_host PRIVATE
            SFX_SSH_TEST_SCRATCH="${CMAKE_CURRENT_BINARY_DIR}/test-output")
        sfx_configure_test_executable(sfx_alpha_ssh_fixture_host)
        add_executable(sfx_ssh_fixture_askpass test/tools/ssh_fixture_askpass.cpp)
        sfx_configure_test_executable(sfx_ssh_fixture_askpass)
        target_link_libraries(sfx_alpha_runtime_host_tests PRIVATE
            sfx_alpha_runtime_host ws2_32)
        sfx_configure_test_executable(sfx_alpha_runtime_host_tests)
        add_test(
            NAME sfx.integration.alpha_runtime_host
            COMMAND sfx_alpha_runtime_host_tests
                ${CMAKE_CURRENT_BINARY_DIR}/test-output/alpha-runtime-host
        )
        set_tests_properties(sfx.platform.alpha_runtime sfx.platform.alpha_ssh_backend
            sfx.integration.alpha_runtime_host PROPERTIES RESOURCE_LOCK sfx_qualification_installation RUN_SERIAL TRUE)
        foreach(fixture_target sfx_alpha_runtime_platform_tests sfx_alpha_ssh_backend_tests sfx_alpha_runtime_host_tests)
            target_compile_definitions(${fixture_target} PRIVATE
                SFX_QUALIFICATION_BUILD_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
        endforeach()
        if(SFX_ALPHA_TEST_AUTOMATION AND NOT SFX_ALPHA_TEST_INSTALL_ROOT STREQUAL "")
            add_executable(sfx_external_telnet_client_witness
                test/tools/external_telnet_client_witness.cpp
            )
            target_link_libraries(sfx_external_telnet_client_witness PRIVATE ws2_32)
            sfx_configure_test_executable(sfx_external_telnet_client_witness)
            add_executable(sfx_control_panel_blackbox_tests
                test/integration/control_panel_blackbox_tests.cpp
            )
            target_link_libraries(sfx_control_panel_blackbox_tests PRIVATE ws2_32 iphlpapi)
            sfx_configure_test_executable(sfx_control_panel_blackbox_tests)
            add_dependencies(sfx_control_panel_blackbox_tests SFXControlPanel SFXRuntime
                sfx_external_telnet_client_witness)
            add_test(
                NAME sfx.integration.control_panel_runtime_telnet
                COMMAND sfx_control_panel_blackbox_tests $<TARGET_FILE:SFXControlPanel>
            )
            set_tests_properties(sfx.integration.control_panel_runtime_telnet
                PROPERTIES TIMEOUT 45)
        endif()
    endif()

    add_test(
        NAME sfx.architecture.portable_boundary
        COMMAND ${CMAKE_COMMAND}
            -DSFX_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckPortableBoundary.cmake
    )
    add_test(
        NAME sfx.architecture.active_identity
        COMMAND ${CMAKE_COMMAND}
            -DSFX_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSfxActiveIdentity.cmake
    )
    add_test(
        NAME sfx.architecture.cr3_sfxconfig
        COMMAND ${CMAKE_COMMAND}
            -DSFX_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckCr3SfxConfigArchitecture.cmake
    )
    add_test(
        NAME sfx.architecture.storage_operation_surface
        COMMAND ${CMAKE_COMMAND}
            -DSFX_STORAGE_IMPLEMENTATION_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/storage/src/sqlite_storage.cpp
            -DSFX_ACCOUNT_REPOSITORY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/storage/src/sqlite_account_repository.cpp
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSfxStorageOperationSurface.cmake
    )
    add_test(
        NAME sfx.architecture.storage_operation_surface_failures
        COMMAND ${CMAKE_COMMAND}
            -DSFX_STORAGE_IMPLEMENTATION_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/storage/src/sqlite_storage.cpp
            -DSFX_ACCOUNT_REPOSITORY_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/src/adapters/storage/src/sqlite_account_repository.cpp
            -DSFX_STORAGE_OPERATION_AUDIT=${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSfxStorageOperationSurface.cmake
            -DSFX_STORAGE_OPERATION_AUDIT_SCRATCH=${CMAKE_CURRENT_BINARY_DIR}/test-output/storage-operation-surface-failures
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSfxStorageOperationSurfaceFailures.cmake
    )
    add_test(
        NAME sfx.integration.baseline_b1_failure_diagnostic
        COMMAND ${CMAKE_COMMAND}
            -DSFX_BASELINE_B1_EXECUTABLE=$<TARGET_FILE:sfx_baseline_b1_tests>
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSfxBaselineB1FailureDiagnostic.cmake
    )
endif()

# Every coordinator compiling Setup's engine needs its automatic-download
# implementation, including historical qualification targets built explicitly.
if(WIN32)
    get_property(_sfx_setup_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_sfx_setup_target IN LISTS _sfx_setup_targets)
        get_target_property(_sfx_setup_sources ${_sfx_setup_target} SOURCES)
        if("src/apps/setup/setup_engine.cpp" IN_LIST _sfx_setup_sources)
            if(NOT "src/apps/setup/clamav_download.cpp" IN_LIST _sfx_setup_sources)
                target_sources(${_sfx_setup_target} PRIVATE src/apps/setup/clamav_download.cpp)
            endif()
            target_link_libraries(${_sfx_setup_target} PRIVATE winhttp)
            sfx_clamav_https(${_sfx_setup_target})
        endif()
    endforeach()
    foreach(_sfx_config_review sfx_config_theme_review sfx_section9_config_model_tests sfx_door_config_tests sfx_section8c_config_review sfx_chat_ui_tests sfx_clamav_noav_config_tests sfx_door_round2_tests sfx_door_round3_tests sfx_door_actions_tests sfx_owner_scroll_tests)
        if(TARGET ${_sfx_config_review})
            target_link_libraries(${_sfx_config_review} PRIVATE sfx_clamav_component_win32 sfx_ftn_download_win32)
        endif()
    endforeach()
endif()

if(BUILD_TESTING)
    add_executable(sfx_rc12_external_installed_seed EXCLUDE_FROM_ALL test/platform/rc12_external_installed_seed.cpp)
    if(WIN32)
        target_link_libraries(sfx_rc12_external_installed_seed PRIVATE sfx_alpha_win32 sfx_account_administration_win32)
    else()
        target_link_libraries(sfx_rc12_external_installed_seed PRIVATE sfx_linux_host_files sfx::storage_sqlite)
    endif()
    sfx_configure_test_executable(sfx_rc12_external_installed_seed)
endif()
