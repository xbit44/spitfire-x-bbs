if(NOT DEFINED SFX_SOURCE_ROOT)
    message(FATAL_ERROR "SFX_SOURCE_ROOT is required")
endif()

function(read_source relative output)
    file(READ "${SFX_SOURCE_ROOT}/${relative}" value)
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(require_contains value needle label)
    string(FIND "${value}" "${needle}" found)
    if(found EQUAL -1)
        message(FATAL_ERROR "CR-3 architecture check failed: ${label}")
    endif()
endfunction()

function(require_absent value needle label)
    string(FIND "${value}" "${needle}" found)
    if(NOT found EQUAL -1)
        message(FATAL_ERROR "CR-3 architecture check failed: ${label}")
    endif()
endfunction()

read_source("src/apps/control_panel/main.cpp" control_panel)
require_absent("${control_panel}" "Run Backup Now" "Events retains a backup action")
require_absent("${control_panel}" "Backup Schedule..." "Events retains a schedule action")
require_absent("${control_panel}" "events_backup" "Events retains a hidden backup control")
require_absent("${control_panel}" "run_settings_dialog" "Control Panel retains the retired settings writer")
require_absent("${control_panel}" "run_scheduler_dialog" "Control Panel retains the retired scheduler writer")
require_absent("${control_panel}" "run_telnet_client_dialog" "Control Panel retains the retired client-profile writer")
require_contains("${control_panel}"
    "MoveWindow(state.events_view, page.left, page.top, page.right - page.left,\n        page.bottom - page.top, TRUE);"
    "Events does not fill the reclaimed page")
require_contains("${control_panel}"
    "AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L\"Exit\");"
    "File is not the exit-only menu")
require_contains("${control_panel}"
    "AppendMenuW(bbs, MF_STRING, ID_BBS_CONFIGURE, L\"SFX Config\");"
    "BBS Configure command is missing")
require_contains("${control_panel}"
    "AppendMenuW(bbs, MF_STRING, ID_BBS_RESTART, L\"Restart BBS Now\");"
    "Control Panel orderly Runtime restart command is missing")

read_source("src/apps/control_panel/control_panel.rc" control_panel_resources)
require_absent("${control_panel_resources}" "IDD_SETTINGS" "retired settings dialog resource remains")
require_absent("${control_panel_resources}" "IDD_SCHEDULER" "retired scheduler dialog resource remains")
require_absent("${control_panel_resources}" "IDD_TELNET_CLIENT" "retired client-profile dialog resource remains")

string(FIND "${control_panel}" "void launch_configuration(AppState& state,bool botgate=false,app::AccountId account_id=0,bool fileman=false,bool ftn=false)" launch_start)
string(FIND "${control_panel}" "void layout(AppState& state" layout_start)
if(launch_start EQUAL -1 OR layout_start EQUAL -1 OR launch_start LESS layout_start)
    message(FATAL_ERROR "CR-3 architecture check failed: fixed configuration launcher unavailable")
endif()
string(SUBSTRING "${control_panel}" ${launch_start} -1 launch_tail)
string(FIND "${launch_tail}" "bool close_runtime" launch_end)
if(launch_end EQUAL -1)
    message(FATAL_ERROR "CR-3 architecture check failed: configuration launcher boundary unavailable")
endif()
string(SUBSTRING "${launch_tail}" 0 ${launch_end} launcher)
require_contains("${launcher}" "verify_runtime_installation" "launcher does not reverify the installation")
require_contains("${launcher}" "root / L\"Bin\" / (fileman ? L\"SFXFileManager.exe\" : L\"SFXConfig.exe\")" "launcher does not use the two fixed installed program paths")
require_contains("${launcher}" "CreateProcessW(executable.c_str()" "launcher does not execute the verified fixed path")
require_absent("${launcher}" "SearchPath" "launcher searches PATH")
require_absent("${launcher}" "ShellExecute" "launcher delegates path resolution")

read_source("src/apps/config_ui/main.cpp" config_ui)
require_contains("${config_ui}" "L\"Spitfire-X BBS Configuration\"" "configuration identity is missing")
require_contains("${config_ui}" "std::array<HWND, 42> pages" "current forty-two configuration page slots are missing")
read_source("src/apps/config_ui/routes.hpp" config_routes)
require_contains("${config_routes}" "std::array<Route, 31> routes" "current thirty-one primary routes are missing")
set(required_routes general.board_identity general.regional_time general.displays_menus general.system_information
    messages.local_echoes messages.clone_permissions files.groups_areas files.transfer_policy files.count_rules
    files.caller_exemptions doors.manager chat.rooms ftn.networks system.runtime_behavior system.nodes system.system_password
    users.accounts users.security_levels users.new_user_registration users.caller_password_policy
    connections.telnet_ssh connections.caller_idle_limits connections.external_client security.connection_limits security.throttle_recovery
    security.botgate security.clamav tools.backup_restore tools.scheduled_jobs tools.logging tools.bbs_health
    ftn.network ftn.connections ftn.echoes ftn.files ftn.routing ftn.import ftn.held ftn.review ftn.permissions
    general.displays_menus) # Commands subpage shares the stable Displays & Menus route.
string(REGEX MATCHALL "[{][ \t]*L\"[a-z_.]+\"" route_definitions "${config_routes}")
set(observed_routes)
foreach(definition IN LISTS route_definitions)
    string(REGEX REPLACE "^[{][ \t]*L\"([^\"]+)\"$" "\\1" route_id "${definition}")
    list(APPEND observed_routes "${route_id}")
endforeach()
list(SORT required_routes)
list(SORT observed_routes)
if(NOT "${observed_routes}" STREQUAL "${required_routes}")
    message(FATAL_ERROR "CR-3 architecture check failed: current stable route definition set mismatch")
endif()
foreach(label "Build Status" "About")
    require_absent("${config_routes}" "L\"${label}\"" "unauthorized navigation entry: ${label}")
endforeach()
require_contains("${config_ui}" "Save and Continue" "dirty save option missing")
require_contains("${config_ui}" "Discard Changes" "dirty discard option missing")
require_contains("${config_ui}" "Cancel Navigation" "dirty cancel option missing")
require_contains("${config_ui}" "root / L\"Backup\"" "canonical Backup location is missing")
require_contains("${config_ui}" "state.mode != State::ConnectionMode::offline" "restore is not stopped-only")
require_contains("${config_ui}" "verify_installed_program_process(state.receipt" "direct launch does not verify itself")
require_contains("${config_ui}" "names.configuration_mutex_name" "per-installation editor singleton is missing")
require_contains("${config_ui}" "OfflineInstallationGuard" "offline configuration lock is missing")
require_contains("${config_ui}" "SS_LEFT | SS_NOPREFIX" "literal ampersands are not preserved in configuration labels")
require_contains("${config_ui}" "const auto action_status = text(state.status)" "successful action status is not preserved across editor refresh")

read_source("src/apps/config_ui/config_ui.manifest" config_manifest)
require_contains("${config_manifest}" "level=\"asInvoker\"" "configuration requests elevation")
require_contains("${config_manifest}" "{35138b9a-5d96-4fbd-8e2d-a2440225f93a}" "Windows 7 compatibility declaration is missing")

read_source("cmake/GenerateSetupPayload.cmake" payload)
require_contains("${payload}" "SFX-PAYLOAD-8" "current DOS-helper payload version is missing")
require_contains("${payload}" "file(SHA256 \"\${CONFIG}\" CONFIG_HASH)" "SFXConfig payload hash is missing")
require_contains("${payload}" "206 RCDATA" "SFXConfig payload resource is missing")

read_source("src/apps/setup/setup_engine.cpp" setup)
require_contains("${setup}" "configuration_shortcut_name" "configuration shortcut ownership is missing")
require_contains("${setup}" "configuration_program_index" "configuration program role is missing")
require_contains("${setup}" "remove_shortcut_if_matching" "identity-safe shortcut cleanup is missing")

message(STATUS "CR-3 SFXConfig architecture source contract PASS")

# Retention must be configured before startup recovery can touch old journals.
read_source("src/apps/runtime/alpha_runtime_host.cpp" runtime_host)
string(FIND "${runtime_host}" "candidate->configure_logging(configuration_->logging)" configure_journals)
string(FIND "${runtime_host}" "candidate->start()" start_journals)
if(configure_journals EQUAL -1 OR start_journals EQUAL -1 OR configure_journals GREATER start_journals)
    message(FATAL_ERROR "Saved logging retention must precede durable journal recovery")
endif()
