#include "resource.h"
#include "routes.hpp"
#include "door_command_line.hpp"
#include "door_bulletin_window.hpp"
#include "sfx/platform/win32/configuration_link.hpp"
#include "sfx/platform/win32/botgate_rule_files.hpp"
#include "../product_version.h"

#include "sfx/application/external_telnet_client.hpp"
#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/win32/rlogin_secrets.hpp"
#include "sfx/platform/win32/bbs_health.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/portable_key_store.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/portable_preparation.hpp"
#include "sfx/platform/win32/trusted_display_asset_reader.hpp"
#include "sfx/application/scheduler.hpp"
#include "sfx/application/menu_session.hpp"
#include "sfx/application/control_panel.hpp"
#include "sfx/platform/win32/trusted_display_asset_reader.hpp"
#include "sfx/archive/backup_service.hpp"
#include "sfx/platform/win32/backup_collections.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/bbs_log_timestamp.hpp"
#include "sfx/platform/win32/bbs_time_zone_provider.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/service_mode_control.hpp"
#include "sfx/platform/win32/service_secrets.hpp"
#include "sfx/platform/win32/clamav_update.hpp"
#include "sfx/platform/win32/durable_journal_files.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/window_icon.hpp"
#include "sfx/platform/win32/desktop_theme.hpp"
#include "sfx/platform/win32/editor_placement.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"
#include "sfx/adapters/authentication/sodium_authentication.hpp"

#include <windows.h>
#include "chat_editor_state.hpp"
#include "ftn_editor_state.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <commdlg.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <set>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <map>
#include <ctime>

#include "sfx/application/external_events.hpp"
#include "sfx/application/external_event_files.hpp"
#include "sfx/platform/win32/bbs_time_zone_provider.hpp"

namespace {

enum : unsigned {
    IDC_EXTERNAL_EVENT_LIST = 2810, IDC_EXTERNAL_EVENT_NEW, IDC_EXTERNAL_EVENT_DELETE,
    IDC_EXTERNAL_EVENT_SAVE, IDC_EXTERNAL_EVENT_EXCLUSIVE, IDC_EXTERNAL_EVENT_TIMING,
    IDC_EXTERNAL_EVENT_PROGRAM, IDC_EXTERNAL_EVENT_DIRECTORY,
    IDC_EXTERNAL_EVENT_MONTHS, IDC_EXTERNAL_EVENT_DATES, IDC_EXTERNAL_EVENT_WEEKDAYS
};

namespace app = sfx::application;
namespace archive = sfx::archive;
namespace install = sfx::platform::win32::installation;
namespace win32 = sfx::platform::win32;

constexpr wchar_t kWindowClass[] = L"SFXConfigWindow";
constexpr wchar_t kControlPanelClass[] = L"SFXControlPanelWindow";
constexpr wchar_t kWindowTitle[] = L"Spitfire-X BBS Configuration";
// Timers share the top-level Config HWND. Each feature owns one distinct ID.
namespace config_timer {
constexpr UINT_PTR backup=1,botgate=2,message_maintenance=3,clam_job=4,
    clam_maintenance=5,hosting=6,bbs_health=7,ftn_import=8,ftn_setup=9;
}
constexpr wchar_t kTimeZonesRegistryPath[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones";

enum : UINT {
    IDC_NAVIGATION = 1001,
    IDC_STATUS = 1002,
    IDC_BBS_NAME = 1010,
    IDC_TIME_ZONE = 1011,
    IDC_SYSTEM_SAVE = 1012,
    IDC_CALLER_AFK = 1013,
    IDC_LOGIN_AFK = 1014,
    IDC_LOGICAL_NODES = 1020,
    IDC_NODES_SAVE = 1021,
    IDC_TELNET_ENABLED = 1030,
    IDC_TELNET_BIND = 1031,
    IDC_TELNET_PORT = 1032,
    IDC_SSH_ENABLED = 1033,
    IDC_SSH_BIND = 1034,
    IDC_SSH_PORT = 1035,
    IDC_ALLOW_USERS = 1036,
    IDC_HOSTNAME_LOOKUP = 1037,
    IDC_SERVERS_SAVE = 1038,
    IDC_CLIENT_TYPE = 1040,
    IDC_CLIENT_PATH = 1041,
    IDC_CLIENT_BROWSE = 1042,
    IDC_CLIENT_ARGUMENTS = 1043,
    IDC_CLIENT_SAVE = 1044,
    IDC_CLIENT_CLEAR = 1045,
    IDC_BACKUP_LIST = 1050,
    IDC_BACKUP_RUN = 1051,
    IDC_BACKUP_VERIFY = 1052,
    IDC_BACKUP_RESTORE = 1053,
    IDC_FTN_HISTORY_RECOVER = 1079,
    IDC_FTN_HISTORY_OPEN = 1080,
    IDC_BACKUP_REFRESH = 1054,
    IDC_BACKUP_DETAIL = 1055,
    IDC_BACKUP_CONTENTS = 1056,
    IDC_SCHEDULE_ENABLED = 1060,
    IDC_SCHEDULE_HOUR = 1061,
    IDC_SCHEDULE_MINUTE = 1062,
    IDC_SCHEDULE_SAVE = 1063,
    IDC_SCHEDULE_DETAIL = 1064,
    IDC_BACKUP_DESTINATION = 1065,
    IDC_BACKUP_RETENTION = 1066,
    IDC_BACKUP_BROWSE = 1067,
    IDC_ACCOUNT_FILTER = 1070, IDC_ACCOUNT_FIND, IDC_ACCOUNT_PREVIOUS, IDC_ACCOUNT_NEXT,
    IDC_ACCOUNT_LIST, IDC_ACCOUNT_ACTION, IDC_ACCOUNT_VALUE, IDC_ACCOUNT_CONFIRM, IDC_ACCOUNT_APPLY,
    IDC_SYSTEM_PASSWORD = 1090, IDC_SYSTEM_CONFIRM, IDC_SYSTEM_PASSWORD_APPLY, IDC_SECURITY_REFRESH,
    IDC_STRONG_PASSWORDS, IDC_PASSWORD_POLICY_SAVE,
    IDC_LEVEL_LIST = 1100, IDC_LEVEL_NUMBER, IDC_LEVEL_LABEL, IDC_LEVEL_DESCRIPTION,
    IDC_LEVEL_REPLACEMENT, IDC_LEVEL_NEW_USER, IDC_LEVEL_THRESHOLD,
    IDC_LEVEL_SAVE, IDC_LEVEL_RETIRE, IDC_LEVEL_DEFAULTS, IDC_LEVEL_REFRESH,
    IDC_LEVEL_CALL_MINUTES, IDC_LEVEL_DAY_MINUTES, IDC_LEVEL_CHAT_MINUTES, IDC_LEVEL_CHAT_DAY_MINUTES,
    IDC_SOURCE_LIMIT = 1120, IDC_ACCOUNT_LIMIT, IDC_SOURCE_EXCEPTIONS, IDC_CONNECTION_SAVE,
    IDC_THROTTLE_SCOPE = 1130, IDC_THROTTLE_SUBJECT, IDC_THROTTLE_CLEAR,
    IDC_GATE_GLOBAL=1150, IDC_GATE_TELNET, IDC_GATE_SSH, IDC_GATE_TIMEOUT,
    IDC_GATE_MANUAL, IDC_GATE_TRUST, IDC_GATE_EXCEPTIONS, IDC_GATE_DOMAINS,
    IDC_GATE_SAVE, IDC_GATE_UPDATE, IDC_LOGGING_SAVE,
    IDC_GATE_RULE_SELECT, IDC_GATE_RULE_TEXT, IDC_GATE_RULE_SAVE, IDC_GATE_RULE_RELOAD,
    IDC_REGIONAL_SAVE=1180, IDC_RUNTIME_SAVE, IDC_REGISTRATION_SAVE, IDC_OPEN_LEVELS,
    IDC_USER_NEW=1200, IDC_USER_RELOAD, IDC_USER_DELETE, IDC_USER_TOGGLE, IDC_USER_TABS,
    IDC_USER_NAME, IDC_USER_REAL, IDC_USER_LOCATION, IDC_USER_PHONE, IDC_USER_EMAIL,
    IDC_USER_BIRTH, IDC_USER_SEX, IDC_USER_LEVEL, IDC_USER_PURGE, IDC_USER_RESET,
    IDC_BBS_SECURITY_TABS=1220,
    IDC_MENU_TEXT=1240, IDC_MENU_INPUT, IDC_MENU_BARS, IDC_MENU_FOREGROUND, IDC_MENU_BACKGROUND,
    IDC_MENU_SAVE, IDC_MENU_SELECT, IDC_MENU_FORMAT, IDC_MENU_LEVEL, IDC_MENU_SYSOP,
    IDC_MENU_ARTWORK, IDC_MENU_REFRESH, IDC_MENU_FOLDER, IDC_MENU_FOOTER, IDC_MENU_PIPE_STYLE,
    IDC_OPENING_DATE=1280, IDC_OPENING_SAVE, IDC_OPENING_RELOAD,
    IDC_MESSAGE_GROUPS=1300,IDC_MESSAGE_ECHOES,IDC_MESSAGE_GROUP_NEXT,IDC_MESSAGE_ECHO_NEXT,IDC_MESSAGE_RELOAD,
    IDC_MESSAGE_NEW_GROUP,IDC_MESSAGE_NEW_ECHO,IDC_MESSAGE_NAME,IDC_MESSAGE_ORDER,IDC_MESSAGE_ACCESS,
    IDC_MESSAGE_READING,IDC_MESSAGE_POSTING,IDC_MESSAGE_PARENT,IDC_MESSAGE_ENABLED,IDC_MESSAGE_SAVE,IDC_MESSAGE_RECONCILE,
    IDC_MESSAGE_CLONE_SOURCE,IDC_MESSAGE_CLONE_GROUP,IDC_MESSAGE_CLONE_NEXT,IDC_MESSAGE_CLONE_TARGETS,IDC_MESSAGE_CLONE_ADD,
    IDC_MESSAGE_CLONE_CLEAR,IDC_MESSAGE_CLONE_ACCESS,IDC_MESSAGE_CLONE_READING,IDC_MESSAGE_CLONE_POSTING,IDC_MESSAGE_CLONE_PREVIEW,IDC_MESSAGE_CLONE_APPLY,
    IDC_MESSAGE_INSERT,IDC_MESSAGE_MOVE_UP,IDC_MESSAGE_MOVE_DOWN,IDC_MESSAGE_DELETE,
    IDC_SYSTEM_SYSOP_FILTER=1340,IDC_SYSTEM_SYSOP_FIND,IDC_SYSTEM_SYSOP_NEXT,IDC_SYSTEM_SYSOP_CHOICE,IDC_SYSTEM_SYSOP_ASSIGN,IDC_SYSTEM_SYSOP_RELOAD,
    IDC_LEVEL_MESSAGE_LINES=1350,IDC_LEVEL_MESSAGE_SAVE,
    IDC_MESSAGE_PIPE_COLORS=1360,IDC_PRIVATE_PIPE_COLORS,
    IDC_MESSAGE_MAX_COUNT=1370,IDC_MESSAGE_MAX_AGE,IDC_CLONE_MAX_COUNT,IDC_CLONE_MAX_AGE,
    IDC_SCHEDULE_JOB=1380,IDC_SCHEDULE_WEEKDAY,IDC_UNREAD_MAIL_AGE,IDC_UNREAD_MAIL_SAVE,IDC_MESSAGE_RUN_NOW,IDC_MESSAGE_COMPACT,
    IDC_QWK_ID=1400,IDC_QWK_MAXIMUM,IDC_QWK_SAVE,
    IDC_FILE_GROUPS=1500,IDC_FILE_AREAS,IDC_FILE_NAME,IDC_FILE_ORDER,IDC_FILE_ACCESS,IDC_FILE_UPLOAD,IDC_FILE_DOWNLOAD,IDC_FILE_PARENT,
    IDC_FILE_PATH,IDC_FILE_BROWSE,IDC_FILE_DESCRIPTION,IDC_FILE_ENABLED,IDC_FILE_UPLOADS,IDC_FILE_SYSOP_DOWNLOADS,IDC_FILE_OWN_EDIT,
    IDC_FILE_BACKUP_CONTENTS,IDC_FILE_RATIO_UPLOADS,IDC_FILE_RATIO_DOWNLOADS,IDC_FILE_AREA_CAP,IDC_FILE_REVIEW,IDC_FILE_SAVE,IDC_FILE_RELOAD,
    IDC_FILE_NEW_GROUP,IDC_FILE_NEW_AREA,IDC_FILE_RECONCILE,IDC_FILE_POLICY_CAP,IDC_FILE_LIGHTBAR,IDC_FILE_SCAN,
    IDC_FILE_DAILY_ON,IDC_FILE_DAILY,IDC_FILE_RATIO_ON,IDC_FILE_RATIO_DOWN,IDC_FILE_RATIO_UP,IDC_FILE_POLICY_SAVE,IDC_FILE_POLICY_RELOAD,
    IDC_FILE_RULE_LEVEL,IDC_FILE_RULE_INHERIT,IDC_FILE_RULE_DAILY_ON,IDC_FILE_RULE_DAILY,IDC_FILE_RULE_RATIO_ON,IDC_FILE_RULE_RATIO_DOWN,IDC_FILE_RULE_RATIO_UP,
    IDC_FILE_RULE_DAILY_EXEMPT,IDC_FILE_RULE_RATIO_EXEMPT,IDC_FILE_RULE_SAVE,IDC_FILE_RULE_RELOAD,
    IDC_FILE_EXEMPT_FILTER,IDC_FILE_EXEMPT_FIND,IDC_FILE_EXEMPT_NEXT,IDC_FILE_EXEMPT_SELECT,IDC_FILE_EXEMPT_DAILY,IDC_FILE_EXEMPT_RATIO,IDC_FILE_EXEMPT_SAVE,IDC_FILE_EXEMPT_RELOAD,
    IDC_FILE_NAME_LABEL,IDC_FILE_DELETE_GROUP,IDC_FILE_INSERT_BEFORE,IDC_FILE_INSERT_AFTER,IDC_FILE_MOVE_UP,IDC_FILE_MOVE_DOWN,
    IDC_FILE_MANAGER=1650,
    IDC_CLAM_DETAIL=1700,IDC_CLAM_SAVE,IDC_CLAM_REFRESH,IDC_CLAM_INSTALL,IDC_CLAM_UPDATE,IDC_CLAM_CANCEL,IDC_CLAM_LOG,
    IDC_RUN_MODE=1710,IDC_RUN_MODE_APPLY,IDC_RUN_MODE_RELOAD,IDC_RUN_MODE_DETAIL,
    IDC_RUN_AUTOMATIC,IDC_RUN_CURRENT,IDC_RUN_STATUS,IDC_RUN_POWER,IDC_RUN_HELP,IDC_RUNTIME_RETRY,
};

std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required) != required) return {};
    return result;
}

std::optional<std::string> wide_to_utf8(const std::wstring_view value) {
    if (value.empty() || value.size() > static_cast<std::size_t>(INT_MAX))
        return std::nullopt;
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return std::nullopt;
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required,
            nullptr, nullptr) != required) return std::nullopt;
    return result;
}

std::wstring text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring result(static_cast<std::size_t>(std::max(0, length)) + 1U, L'\0');
    const int copied = GetWindowTextW(control, result.data(), length + 1);
    result.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
    return result;
}

std::int64_t now_utc() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string transaction_id() {
    const auto value = install::unique_id();
    std::string result;
    result.reserve(value.size());
    for (const wchar_t character : value) {
        if ((character < L'0' || character > L'9') &&
            (character < L'A' || character > L'F')) return {};
        result.push_back(static_cast<char>(character));
    }
    return result;
}

struct WindowSearch {
    const install::Receipt* receipt{nullptr};
    const wchar_t* class_name{nullptr};
    std::size_t program_index{0};
    HWND result{nullptr};
    DWORD process_id{0};
    install::InstallationVerificationLease* verification{};
};

BOOL CALLBACK find_installed_window(HWND window, LPARAM parameter) {
    auto& search = *reinterpret_cast<WindowSearch*>(parameter);
    wchar_t class_name[128]{};
    if (!GetClassNameW(window, class_name, static_cast<int>(std::size(class_name))) ||
        wcscmp(class_name, search.class_name) != 0) return TRUE;
    DWORD process_id = 0;
    (void)GetWindowThreadProcessId(window, &process_id);
    try {
        if(search.verification)search.verification->verify_process(*search.receipt,process_id,search.program_index);
        else install::verify_installed_program_process(*search.receipt,process_id,search.program_index);
        search.result = window;
        search.process_id = process_id;
        return FALSE;
    } catch (...) {
        return TRUE;
    }
}

WindowSearch installed_window(const install::Receipt& receipt,
    const wchar_t* class_name, const std::size_t program_index,install::InstallationVerificationLease* verification=nullptr) {
    WindowSearch search{&receipt, class_name, program_index};
    search.verification=verification;
    (void)EnumWindows(find_installed_window, reinterpret_cast<LPARAM>(&search));
    return search;
}

bool relay_exchange(const std::filesystem::path& root,
    const install::Receipt& receipt, const app::LocalControlFrame& request,
    const app::LocalControlMessageType expected, app::LocalControlFrame& response) {
    try{
    const auto current=install::read_receipt(root,receipt.owner);
    if(install::encode_receipt(current)!=install::encode_receipt(receipt))return false;
    thread_local std::unique_ptr<install::InstallationVerificationLease> verification;
    thread_local std::vector<std::uint8_t> bound;
    const auto binding=install::encode_receipt(receipt);
    if(!verification||bound!=binding){verification=std::make_unique<install::InstallationVerificationLease>(receipt);bound=binding;}
    const auto control_panel = installed_window(receipt, kControlPanelClass,
        install::control_panel_program_index,verification.get());
    if (control_panel.result == nullptr) return false;
    const auto names = win32::installation_object_names(root);
    auto connection = win32::connect_local_control_server(
        names.configuration_pipe_name, control_panel.process_id, 1500);
    return connection.valid() && connection.write_frame(request) &&
        connection.read_frame(response) && response.type == expected &&
        response.request_id == request.request_id;
    }catch(...){return false;}
}

std::vector<std::wstring> installed_time_zones() {
    std::vector<std::wstring> result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kTimeZonesRegistryPath, 0,
            KEY_READ, &key) != ERROR_SUCCESS) return result;
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 256> name{};
        DWORD length = static_cast<DWORD>(name.size());
        const auto status = RegEnumKeyExW(key, index, name.data(), &length,
            nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || length == 0 || length >= name.size()) {
            result.clear();
            break;
        }
        result.emplace_back(name.data(), length);
    }
    RegCloseKey(key);
    std::sort(result.begin(), result.end());
    return result;
}

std::wstring operator_registry_key(const std::filesystem::path& root) {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    std::uint64_t hash = 14695981039346656037ULL;
    for (const auto character : root.native()) {
        hash ^= static_cast<std::uint16_t>(character);
        hash *= 1099511628211ULL;
    }
    return L"Software\\Spitfire-X BBS\\Tests\\NodeViewScale-" +
        std::to_wstring(hash);
#else
    (void)root;
    return L"Software\\Spitfire-X BBS\\Control Panel";
#endif
}

constexpr DWORD kClientProfileVersion = 1;

std::optional<DWORD> registry_dword(HKEY key, const wchar_t* name) {
    DWORD value = 0, type = 0, bytes = sizeof(value);
    return RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(value)
        ? std::optional<DWORD>(value) : std::nullopt;
}

std::optional<std::wstring> registry_string(HKEY key, const wchar_t* name,
    DWORD required_type, DWORD maximum_bytes) {
    DWORD type = 0, bytes = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS ||
        type != required_type || bytes < sizeof(wchar_t) || bytes > maximum_bytes ||
        bytes % sizeof(wchar_t) != 0) return std::nullopt;
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS ||
        value.empty() || value.back() != L'\0') return std::nullopt;
    value.pop_back();
    return value;
}

std::optional<app::ExternalTelnetClientProfile> read_client_profile(
    const std::filesystem::path& root) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, operator_registry_key(root).c_str(), 0,
            KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return std::nullopt;
    const auto version = registry_dword(key, L"TelnetClientProfileVersion");
    const auto type = registry_dword(key, L"TelnetClientType");
    const auto path = registry_string(key, L"TelnetClientPath", REG_SZ, 65536);
    std::vector<std::wstring> arguments;
    bool argument_ok = true;
    if (type && *type == static_cast<DWORD>(app::ExternalTelnetClientType::custom)) {
        const auto encoded = registry_string(key, L"TelnetClientArguments", REG_MULTI_SZ,
            static_cast<DWORD>((app::kExternalTelnetClientMaximumTokens *
                (app::kExternalTelnetClientMaximumTokenCharacters + 1U) + 1U) *
                sizeof(wchar_t)));
        argument_ok = encoded && !encoded->empty() && encoded->back() == L'\0';
        if (argument_ok) {
            std::size_t offset = 0;
            while (offset < encoded->size()) {
                const auto end = encoded->find(L'\0', offset);
                if (end == encoded->npos || end == offset ||
                    arguments.size() == app::kExternalTelnetClientMaximumTokens) {
                    argument_ok = false;
                    break;
                }
                arguments.emplace_back(encoded->substr(offset, end - offset));
                offset = end + 1U;
            }
        }
    }
    RegCloseKey(key);
    if (!version || *version != kClientProfileVersion || !type || !path ||
        !argument_ok || (*type != 1 && *type != 2)) return std::nullopt;
    app::ExternalTelnetClientProfile profile;
    profile.type = static_cast<app::ExternalTelnetClientType>(*type);
    profile.executable = *path;
    profile.custom_arguments = std::move(arguments);
    return app::validate_external_telnet_client_profile(profile).valid
        ? std::optional<app::ExternalTelnetClientProfile>(std::move(profile))
        : std::nullopt;
}

bool save_client_profile(const std::filesystem::path& root,
    const app::ExternalTelnetClientProfile& profile) {
    if (!app::external_telnet_client_executable_is_usable(profile)) return false;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, operator_registry_key(root).c_str(), 0,
            nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key,
            nullptr) != ERROR_SUCCESS) return false;
    (void)RegDeleteValueW(key, L"TelnetClientProfileVersion");
    const DWORD type = static_cast<DWORD>(profile.type);
    bool ok = RegSetValueExW(key, L"TelnetClientType", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&type), sizeof(type)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"TelnetClientPath", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(profile.executable.c_str()),
            static_cast<DWORD>((profile.executable.size() + 1U) * sizeof(wchar_t))) ==
            ERROR_SUCCESS;
    if (ok && profile.type == app::ExternalTelnetClientType::custom) {
        std::wstring encoded;
        for (const auto& argument : profile.custom_arguments) {
            encoded += argument;
            encoded.push_back(L'\0');
        }
        encoded.push_back(L'\0');
        ok = RegSetValueExW(key, L"TelnetClientArguments", 0, REG_MULTI_SZ,
            reinterpret_cast<const BYTE*>(encoded.data()),
            static_cast<DWORD>(encoded.size() * sizeof(wchar_t))) == ERROR_SUCCESS;
    } else if (ok) {
        const auto removed = RegDeleteValueW(key, L"TelnetClientArguments");
        ok = removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND;
    }
    if (ok) ok = RegSetValueExW(key, L"TelnetClientProfileVersion", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&kClientProfileVersion),
        sizeof(kClientProfileVersion)) == ERROR_SUCCESS;
    RegCloseKey(key);
    const auto verified = ok ? read_client_profile(root) : std::nullopt;
    return verified && verified->type == profile.type &&
        verified->executable == profile.executable &&
        verified->custom_arguments == profile.custom_arguments;
}

bool clear_client_profile(const std::filesystem::path& root) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, operator_registry_key(root).c_str(), 0,
            KEY_SET_VALUE, &key) != ERROR_SUCCESS) return !read_client_profile(root);
    bool ok = true;
    for (const auto* name : {L"TelnetClientProfileVersion", L"TelnetClientType",
            L"TelnetClientPath", L"TelnetClientArguments"}) {
        const auto removed = RegDeleteValueW(key, name);
        ok = ok && (removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok && !read_client_profile(root);
}

#include "door_editor_state.hpp"
#include "navigation_loader.inc"
struct State {
    std::unique_ptr<sfx::config_ui::DoorBulletinWindow> door_bulletins;
    DoorEditorState doors;
    ChatEditorState chat;
    FtnEditorState ftn;
    enum class ConnectionMode { online, offline, blocked };
    std::unique_ptr<ConfigNavigationWorker> navigation_worker;
    std::map<NavigationKey,NavigationCachedRead> navigation_reads;
    std::uint64_t navigation_epoch{};
    std::size_t navigation_bytes{};
    bool navigation_rendering{},navigation_resuming{},navigation_loading{},navigation_context_ready{},navigation_context_ok{},navigation_pending{},navigation_finishing{};
    int navigation_page{};
    std::wstring navigation_error;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    bool navigation_async_enabled{false}; // Native model fixtures have no installed worker authority.
#else
    bool navigation_async_enabled{true};
#endif

    HINSTANCE instance{nullptr};
    int dpi{96};
    HWND window{nullptr};
    win32::DesktopAppearance appearance;
    win32::EditorPlacementStore placement;
    HMENU theme_menu{nullptr};
    HWND navigation{nullptr};
    int navigation_width{204},navigation_width_start{204};
    bool resizing_navigation{};
    HWND heading{nullptr};
    HWND security_tabs{nullptr};
    HWND status{nullptr};
    std::array<HWND, 42> pages{};
    std::array<HTREEITEM, 42> route_items{};
    int current_page{-1};
    bool navigating{}, resolving_dirty{}, account_action_active{};
    struct DraftValue { HWND control; std::wstring value; int kind; };
    std::vector<DraftValue> baseline;
    struct ControlBounds { HWND control; RECT bounds; };
    std::array<std::vector<ControlBounds>,42> page_bounds;
    struct HealthJob {std::atomic<bool> cancel{false},done{false};win32::BbsHealthReport result;std::thread thread;~HealthJob(){cancel=true;if(thread.joinable())thread.join();}};
    std::unique_ptr<HealthJob> health_job;HWND health_run{},health_result{};
    struct ClamJob {
        std::atomic<bool> cancel{false},done{false};std::mutex mutex;std::wstring progress;bool events_complete{true};
        bool updating{},failed{},inspection_cancelled{};
        win32::ClamAvStatus status;std::optional<win32::ClamAvUpdateResult> result;std::thread thread;
        ~ClamJob(){cancel=true;if(thread.joinable())thread.join();}
    };
    std::shared_ptr<ClamJob> clam_job;
    std::shared_ptr<app::ClamAvAttentionTracker> clam_offline_health=std::make_shared<app::ClamAvAttentionTracker>();
    win32::ClamAvStatus clam_status;
    enum class ClamStatusRead { unread,checking,ready,cancelled,failed };
    ClamStatusRead clam_status_read{ClamStatusRead::unread};
    HWND clam_detail{},clam_save{},clam_refresh{},clam_install{},clam_update{},clam_cancel{},clam_log{};
    install::Handle clam_maintenance;bool close_after_clam{};
    HWND menu_text{},menu_input{},menu_bars{},menu_foreground{},menu_background{},menu_save{},menu_footer{};
    HWND menu_pipe_style{};
    HWND menu_select{},menu_format{},menu_level{},menu_sysop{},menu_artwork{},menu_preview{},menu_detail{};
    app::ControlPanelTerminalSnapshot menu_screen;
    HWND regional_detail{}, runtime_detail{}, registration_detail{}, policy_detail{};
    HWND run_mode{},run_mode_detail{};
    HWND run_automatic{},run_current{},run_status{},run_power{},run_help{},run_note{};
    std::optional<install::ServiceMode> hosting;
    std::optional<install::ServiceMode> hosting_current;
    std::optional<install::ServiceState> hosting_status;
    bool hosting_loaded{},hosting_busy{},hosting_review_required{};
    UINT_PTR hosting_epoch{1};DWORD hosting_valid_after{};int hosting_action{};
    struct HostingAction {
        std::atomic<bool> done{false};bool success{},start{};std::string detail;std::thread worker;
        ~HostingAction(){if(worker.joinable())worker.join();}
    };
    std::shared_ptr<HostingAction> hosting_job;
    std::optional<std::pair<std::wstring,std::wstring>> runtime_idle_draft;
    std::optional<std::pair<std::wstring,std::wstring>> runtime_idle_baseline;
    std::wstring hosting_notice;
    std::wstring prepare_notice;
    int initial_page{};
    app::AccountId initial_account{};
    HWND gate_global{},gate_telnet{},gate_ssh{},gate_timeout{},gate_save{},gate_update{},gate_detail{};
    HWND gate_rule_select{},gate_rule_text{},gate_rule_save{},gate_rule_help{};
    unsigned gate_rule_index{}; std::wstring gate_rule_original; std::string gate_rule_raw; bool gate_rule_loaded{};
    std::array<HWND,6> logging_limits{};
    HWND logging_save{};
    HFONT heading_font{nullptr};
    HFONT ui_font{nullptr};
    std::filesystem::path root;
    std::wstring owner;
    install::Receipt receipt;
    ConnectionMode mode{ConnectionMode::blocked};
    HANDLE instance_mutex{nullptr};
    std::uint32_t request_id{1};
    app::AlphaConfigurationSnapshot configuration;
    app::AlphaSchedulerStatus scheduler;
    struct {
        app::ExternalEventCatalog catalog;
        HWND list{}, name{}, program{}, directory{}, arguments{}, enabled{}, months{}, dates{}, weekdays{};
        HWND timing{}, time{}, exclusive{}, cutoff{}, wait{}, startup{}, missed{}, warning{}, save{}, remove{};
        std::uint64_t selected{};
        unsigned month_mask{}, date_mask{}, weekday_mask{};
        bool loaded{}, filling{};
    } external;
    std::optional<app::SchedulerConfiguration> backup_settings;
    std::vector<std::filesystem::path> backups;
    bool watching_backup{false};
    std::int64_t watched_last_run{0};

    HWND bbs_name{nullptr}, time_zone{nullptr}, system_save{nullptr}, caller_afk{nullptr}, login_afk{nullptr};
    HWND system_detail{nullptr}, logical_nodes{nullptr}, nodes_save{nullptr};
    HWND nodes_detail{nullptr};
    HWND telnet_enabled{nullptr}, telnet_bind{nullptr}, telnet_port{nullptr};
    HWND ssh_enabled{nullptr}, ssh_bind{nullptr}, ssh_port{nullptr};
    HWND allow_users{nullptr}, hostname_lookup{nullptr}, servers_save{nullptr};
    HWND servers_detail{nullptr};
    HWND client_type{nullptr}, client_path{nullptr}, client_arguments{nullptr};
    HWND client_browse{nullptr}, client_save{nullptr}, client_clear{nullptr};
    HWND backup_list{nullptr}, backup_run{nullptr}, backup_verify{nullptr};
    HWND backup_restore{nullptr}, backup_contents{nullptr}, backup_refresh{nullptr}, backup_detail{nullptr},ftn_history_recover{nullptr};
    HWND schedule_enabled{nullptr}, schedule_hour{nullptr}, schedule_minute{nullptr};
    HWND schedule_save{nullptr}, schedule_detail{nullptr};
    HWND backup_destination{nullptr}, backup_retention{nullptr}, backup_browse{nullptr};
    HWND schedule_job{},schedule_weekday{},unread_mail_age{},unread_mail_save{},message_run_now{},message_compact{};
    std::vector<HWND> backup_job_controls,message_job_controls;
    unsigned selected_job{};
    app::MessageMaintenanceStatus message_job_status;
    app::LocalMessageConfigurationSnapshot private_mail_policy;
    HWND account_filter{nullptr}, account_list{nullptr}, account_action{nullptr};
    HWND account_summary{nullptr}, account_value_label{nullptr}, account_value{nullptr};
    HWND account_confirm_label{nullptr}, account_confirm{nullptr}, account_apply{nullptr}, account_detail{nullptr};
    HWND account_previous{nullptr}, account_next{nullptr};
    HWND user_new{}, user_reload{}, user_delete{}, user_toggle{}, user_tabs{}, user_level{}, user_purge{}, user_reset{};
    std::array<HWND,7> user_fields{};
    std::array<std::vector<HWND>,2> user_tab_controls;
    bool creating_user{}, populating_user{};
    HWND system_password{nullptr}, system_confirm{nullptr}, system_password_apply{nullptr}, security_detail{nullptr};
    HWND strong_passwords{}, password_policy_save{};
    HWND source_limit{}, account_limit{}, source_exceptions{}, connection_save{}, connection_detail{};
    HWND throttle_scope{}, throttle_subject{}, throttle_clear{}, throttle_detail{};
    HWND level_list{}, level_number{}, level_label{}, level_description{}, level_replacement{};
    HWND level_call_minutes{}, level_day_minutes{}, level_chat_minutes{}, level_chat_day_minutes{};
    int selected_level_index{-1};
    HWND level_new_user{}, level_threshold{}, level_save{}, level_retire{}, level_defaults{}, level_detail{};
    app::LocalSecuritySnapshot security_catalog;
    app::LocalAccountSnapshot account_page, account_selection;
    app::LocalAccountQuery account_query;
    std::int64_t system_password_revision{};
    bool populating_accounts{false};
    bool system_recovery_only{false};
    bool diagnostic_only{false};
    HWND opening_date{},opening_detail{};
    app::LocalClassicInformationSnapshot classic_information;
    app::LocalMessageConfigurationSnapshot message_catalog,message_echo_catalog,message_limits;
    app::MessageCatalogCursor message_group_cursor,message_echo_cursor,clone_echo_cursor;
    std::optional<app::MessageGroup> editing_message_group;
    std::optional<app::LocalEcho> editing_message_echo,clone_source;
    std::optional<app::LocalEcho> message_insert_anchor;
    std::vector<app::LocalEcho> clone_targets,clone_page;
    bool editing_echo{},message_populating{};
    int message_group_index{-1},message_echo_index{-1};
    std::optional<app::MessageConfigurationChange> pending_message_apply;
    app::LocalQwkPolicyResult qwk_policy;
    std::optional<app::QwkPolicyChange> pending_qwk_policy;
    HWND qwk_id{},qwk_maximum{},qwk_save{};
    HWND message_groups{},message_echoes{},message_name{},message_order{},message_access{},message_reading{},message_posting{},message_parent{},message_enabled{},message_detail{};
    HWND message_pipe_colors{},private_pipe_colors{};
    HWND private_pipe_reconcile{};
    bool private_pipe_available{};
    HWND message_max_count{},message_max_age{},clone_max_count{},clone_max_age{};
    HWND clone_source_text{},clone_group{},clone_list{},clone_access{},clone_reading{},clone_posting{},clone_preview{};
    HWND sysop_filter{},sysop_choice{},sysop_detail{},level_message_lines{},level_message_save{};
    std::vector<app::AccountRecord> sysop_candidates;
    std::uint32_t sysop_offset{};bool sysop_more{};
    struct FileEditor {
        bool populating{},ready{};int group_index{-1},area_index{-1},rule_index{-1},account_index{-1};
        std::vector<app::FileGroup> groups;
        std::vector<app::FileAreaConfiguration> areas;
        std::vector<app::FileSecurityRule> rules;
        app::FilePolicy policy;
        std::optional<app::FileGroup> group;
        std::optional<app::FileAreaConfiguration> area;
        app::FileSecurityRule rule;
        std::optional<app::FileCallerExemption> exemption;
        std::optional<app::FileConfigurationChange> pending;
        app::LocalAccountSnapshot accounts;app::LocalAccountQuery account_query;
        app::FileAreaId insert_anchor;std::int64_t insert_revision{};bool insert_after{};
        HWND group_list{},area_list{},name_label{},name{},order{},access{},upload{},download{},parent{},path{},browse{},description{},enabled{},uploads{},sysop_downloads{},own_edit{},backup_contents{},ratio_uploads{},ratio_downloads{},area_cap{},review{},detail{};
        HWND policy_cap{},lightbar{},scan{},daily_on{},daily{},ratio_on{},ratio_down{},ratio_up{},policy_detail{};
        HWND rule_level{},rule_inherit{},rule_daily_on{},rule_daily{},rule_ratio_on{},rule_ratio_down{},rule_ratio_up{},rule_daily_exempt{},rule_ratio_exempt{},rule_detail{};
        HWND exempt_filter{},exempt_select{},exempt_daily{},exempt_ratio{},exempt_detail{};
    } file_editor;
};

app::LocalMessageConfigurationSnapshot query_message_configuration(State&,const app::LocalMessageConfigurationQuery&);
void discard_ftn(State&);
void populate_message_jobs(State&,bool=false);
bool save_message_schedule(State&);
bool save_unread_mail_age(State&);
void delete_public_area_ui(State&,const app::MessageDeletionTarget&);
void refresh_message_configuration(State&);
void refresh_system_sysop(State&,bool=false);
void refresh_message_limit(State&);
void select_message_limit(State&);
void save_message_limit(State&);

void refresh_menu_preview(State& state);

void set_status(State& state, const std::wstring& message) {
    SetWindowTextW(state.status, message.c_str());
}

// Draft comparison never copies password text into a snapshot.
std::vector<State::DraftValue> page_values(State& state) {
    std::vector<State::DraftValue> values;
    if(state.current_page<0) return values;
    for(HWND child=GetWindow(state.pages[state.current_page],GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        const int id=GetDlgCtrlID(child);
        if(state.current_page==26){if(!state.doors.draft_control(child))continue;}
        else if((state.current_page!=31 && !(GetWindowLongPtrW(child,GWL_STYLE)&WS_VISIBLE)) || id==IDC_SCHEDULE_JOB)continue;
        if(id>=IDC_MENU_SELECT && id<=IDC_MENU_FOLDER) continue; // preview controls are not saved settings
        if(state.current_page!=26&&(id==IDC_DOOR_VIEW||id==IDC_DOOR_SELECT||id==IDC_DOOR_MENU_DUP_ID||id==IDC_DOOR_DUP_ID||id==IDC_DOOR_COPY_MENU||id==IDC_DOOR_COPY_NUMBER))continue;
        if(id==IDC_ACCOUNT_FILTER || id==IDC_GATE_RULE_SELECT || id==IDC_ACCOUNT_ACTION || id==IDC_THROTTLE_SCOPE ||
            id==IDC_MESSAGE_CLONE_GROUP || id==IDC_SYSTEM_SYSOP_FILTER || id==IDC_SYSTEM_SYSOP_CHOICE ||
            id==IDC_FILE_RULE_LEVEL || id==IDC_FILE_EXEMPT_FILTER || id==IDC_FILE_EXEMPT_SELECT) continue;
        wchar_t cls[64]{}; GetClassNameW(child,cls,64);
        const auto style=GetWindowLongPtrW(child,GWL_STYLE);
        if(id>=IDC_EXTERNAL_EVENT_MONTHS && id<=IDC_EXTERNAL_EVENT_WEEKDAYS) {
            const auto& event=state.external;
            const auto mask=id==IDC_EXTERNAL_EVENT_MONTHS?event.month_mask:
                id==IDC_EXTERNAL_EVENT_DATES?event.date_mask:event.weekday_mask;
            values.push_back({child,std::to_wstring(mask),0});
        } else if(_wcsicmp(cls,L"Edit")==0 && !(style&ES_READONLY)) {
            if((state.current_page==31 && id>=IDC_FTN_SESSION_SECRET && id<=IDC_FTN_TIC_SECRET)||(state.current_page==37 && id>=IDC_NS_SECRET0 && id<=IDC_NS_SECRET3)) values.push_back({child,L"",4});
            else if(SendMessageW(child,EM_GETPASSWORDCHAR,0,0)) values.push_back({child,L"",3});
            else values.push_back({child,text(child),0});
        } else if(_wcsicmp(cls,L"ComboBox")==0) values.push_back({child,std::to_wstring(SendMessageW(child,CB_GETCURSEL,0,0)),1});
        else if(_wcsicmp(cls,L"Button")==0 && (style&BS_TYPEMASK)==BS_AUTOCHECKBOX)
            values.push_back({child,std::to_wstring(Button_GetCheck(child)),2});
    }
    return values;
}
void remember_page(State& state) { state.baseline=page_values(state); }
bool hosting_dirty(State&);
bool refresh_hosting(State&,bool);
bool page_dirty(State& state) {
    if(state.current_page==14&&hosting_dirty(state))return true;
    if(state.current_page==35&&state.ftn.import_changed)return true;
    if(state.current_page==38&&state.ftn.permission_edits)return true;
    if(state.current_page>=29&&state.current_page<39&&state.ftn.draft_changed)return true;
    const auto current=page_values(state);
    if(current.size()!=state.baseline.size()) return false; // initial construction
    for(std::size_t i=0;i<current.size();++i)
        if(current[i].control!=state.baseline[i].control || current[i].kind!=state.baseline[i].kind ||
            (current[i].kind==4 ? SendMessageW(current[i].control,EM_GETMODIFY,0,0)!=0 : current[i].kind==3 ? GetWindowTextLengthW(current[i].control)!=0 : current[i].value!=state.baseline[i].value)) return true;
    return false;
}
void discard_page(State& state) {
    if(state.current_page>=29&&state.current_page<39){discard_ftn(state);return;}
    for(const auto& value:state.baseline) {
        if(value.kind==0) SetWindowTextW(value.control,value.value.c_str());
        else if(value.kind==1) SendMessageW(value.control,CB_SETCURSEL,std::stoi(value.value),0);
        else if(value.kind==2) Button_SetCheck(value.control,std::stoi(value.value));
        else SetWindowTextW(value.control,L"");
    }
    if(state.current_page==14){state.hosting_review_required=false;refresh_hosting(state,true);}
}
void persist_route(State& state) {
    if(state.system_recovery_only || state.current_page<0) return;
    HKEY key{};
    if(RegCreateKeyExW(HKEY_CURRENT_USER,operator_registry_key(state.root).c_str(),0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr)!=ERROR_SUCCESS) return;
    const auto id=sfx::config_ui::route(state.current_page).id;
    RegSetValueExW(key,L"ConfigurationRoute",0,REG_SZ,reinterpret_cast<const BYTE*>(id.data()),static_cast<DWORD>((id.size()+1)*sizeof(wchar_t)));
    RegCloseKey(key);
}

app::LocalControlFrame request(State& state, app::LocalControlMessageType type,
    std::vector<std::uint8_t> payload = {}) {
    app::LocalControlFrame value;
    value.type = type;
    value.request_id = state.request_id++;
    value.payload = std::move(payload);
    return value;
}

bool online_exchange(State& state, const app::LocalControlFrame& outgoing,
    app::LocalControlMessageType expected, app::LocalControlFrame& response) {
    if(state.navigation_rendering){
        const NavigationKey key{outgoing.type,outgoing.payload};
        auto found=state.navigation_reads.find(key);
        if(found==state.navigation_reads.end()){
            if(state.navigation_reads.size()>=1024)return false;
            found=state.navigation_reads.emplace(key,NavigationCachedRead{}).first;
            state.navigation_worker->submit(state.navigation_epoch,outgoing,expected);
        }
        if(!found->second.ready){state.navigation_pending=true;return false;}
        if(!found->second.ok||found->second.response.type!=expected)return false;
        response=found->second.response;response.request_id=outgoing.request_id;return true;
    }
    return relay_exchange(state.root, state.receipt, outgoing, expected, response);
}

std::optional<std::uint16_t> port_value(HWND control) {
    const auto value = text(control);
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
    if (value.empty() || end == nullptr || *end != L'\0' || parsed == 0 ||
        parsed > 65535UL) return std::nullopt;
    return static_cast<std::uint16_t>(parsed);
}

std::optional<std::uint16_t> node_count_value(HWND control) {
    const auto value = text(control);
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
    if (value.empty() || end == nullptr || *end != L'\0' || parsed < 1 || parsed > 128)
        return std::nullopt;
    return static_cast<std::uint16_t>(parsed);
}

std::wstring activation_text(const app::AlphaConfigurationSnapshot& snapshot) {
    return utf8_to_wide(snapshot.detail) + L"  Active revision " +
        std::to_wstring(snapshot.active.configuration_revision) +
        L"; saved revision " + std::to_wstring(snapshot.saved.configuration_revision) + L".";
}

bool load_runtime_status(State& state) {
    const auto decoded = app::read_status_snapshot(0, [&state](const app::AlphaStatusRequest& query)
        -> std::optional<std::vector<std::uint8_t>> {
        app::LocalControlFrame response;
        const auto outgoing = request(state, app::LocalControlMessageType::status_request,
            app::encode_status_request(query));
        if (!online_exchange(state, outgoing, app::LocalControlMessageType::status_response, response))
            return std::nullopt;
        return std::move(response.payload);
    });
    if (!decoded) return false;
    state.scheduler = decoded->scheduler;
    return true;
}

bool load_editor_state(State& state) {
    if(state.diagnostic_only)return false;
    if(state.navigation_rendering)return state.navigation_context_ready&&state.navigation_context_ok;
    // Read-only when online: Runtime publishes destination changes over its
    // authenticated control channel. Only retention editing remains offline.
    state.backup_settings = win32::read_scheduler_configuration_file(state.root, state.owner).value;
    const auto panel = installed_window(state.receipt, kControlPanelClass,
        install::control_panel_program_index);
    const bool stopped_service=[&]{try{const auto selected=install::read_service_selection(state.root);return selected&&selected->enabled&&!selected->pending&&install::query_owned_service(*selected).state==SERVICE_STOPPED;}catch(...){return false;}}();
    if (panel.result != nullptr && !stopped_service) {
        app::LocalControlFrame response;
        const auto outgoing = request(state,
            app::LocalControlMessageType::configuration_snapshot_request);
        if (!online_exchange(state, outgoing,
                app::LocalControlMessageType::configuration_snapshot_response, response)) {
            state.mode = State::ConnectionMode::blocked;
            set_status(state,
                L"Refused — the running Control Panel could not prove its Runtime connection. "
                L"Close SFX normally, then reopen it or run matching Setup Repair.");
            return false;
        }
        const auto snapshot = app::decode_configuration_snapshot(response.payload);
        if (!snapshot || !load_runtime_status(state)) {
            state.mode = State::ConnectionMode::blocked;
            set_status(state, L"Refused — the running installation returned invalid configuration state.");
            return false;
        }
        state.configuration = *snapshot;
        state.mode = State::ConnectionMode::online;
        set_status(state, L"Runtime running — " + activation_text(state.configuration));
        return true;
    }

    win32::OfflineInstallationGuard guard;
    const auto acquired = guard.acquire(state.root, state.owner);
    if (!acquired) {
        state.mode = State::ConnectionMode::blocked;
        set_status(state,
            L"Refused — Runtime or another offline transaction owns this installation. "
            L"Close SFX normally and reload.");
        return false;
    }
    const auto loaded = win32::read_operational_configuration_file(state.root);
    if (loaded.state != win32::ConfigurationFileState::loaded || !loaded.configuration) {
        state.mode = State::ConnectionMode::blocked;
        set_status(state,
            L"Refused — the completed operational configuration could not be loaded. Run matching Setup Repair.");
        return false;
    }
    state.configuration.active = *loaded.configuration;
    state.configuration.saved = *loaded.configuration;
    state.configuration.activation = app::AlphaConfigurationActivation::saved_next_start;
    state.configuration.detail = "Saved - applies on next Runtime start";
    const auto scheduler_configuration =
        win32::read_scheduler_configuration_file(state.root, state.owner);
    const auto scheduler_state = win32::read_scheduler_state_file(state.root, state.owner);
    state.scheduler = {};
    if (scheduler_configuration.value && scheduler_state.value) {
        state.scheduler.configuration_loaded = true;
        state.scheduler.enabled = scheduler_configuration.value->enabled;
        state.scheduler.configuration_revision = scheduler_configuration.value->revision;
        state.scheduler.local_hour = scheduler_configuration.value->local_hour;
        state.scheduler.local_minute = scheduler_configuration.value->local_minute;
        state.scheduler.next_run_utc = scheduler_state.value->next_run_utc;
        state.scheduler.active = scheduler_state.value->active;
        state.scheduler.active_scheduled = scheduler_state.value->active_scheduled;
        state.scheduler.active_run_id = scheduler_state.value->active_run_id;
        state.scheduler.active_started_utc = scheduler_state.value->active_started_utc;
        state.scheduler.last_run_utc = scheduler_state.value->last_run_utc;
        state.scheduler.last_result = scheduler_state.value->last_result;
        state.scheduler.last_detail = scheduler_state.value->last_detail;
        state.scheduler.history = scheduler_state.value->history;
        for (auto& item : state.scheduler.history)
            item.detail = app::bounded_scheduler_status_history_detail(item.detail);
        state.scheduler.diagnostic_code = "scheduler_offline";
    } else {
        state.scheduler.diagnostic_code = scheduler_configuration.diagnostic_code + ";" +
            scheduler_state.diagnostic_code;
    }
    state.mode = State::ConnectionMode::offline;
    set_status(state, L"Runtime stopped — saved changes apply on next Runtime start.");
    return true;
}

void select_combo_text(HWND combo, const std::wstring& value) {
    const auto count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
        std::array<wchar_t, 256> candidate{};
        if (SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(index),
                reinterpret_cast<LPARAM>(candidate.data())) != CB_ERR &&
            _wcsicmp(candidate.data(), value.c_str()) == 0) {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(index), 0);
            return;
        }
    }
}

std::wstring active_saved_line(const State& state, const wchar_t* name, const std::wstring& active,
    const std::wstring& saved) {
    if(state.mode==State::ConnectionMode::offline)
        return std::wstring(name)+L" — Saved: "+saved+L"; Runtime stopped.";
    if(state.mode==State::ConnectionMode::blocked)
        return std::wstring(name)+L" — active state unavailable; saved snapshot: "+saved;
    return std::wstring(name) + L" — Active: " + active + L"; Saved: " + saved;
}

std::wstring yes_no(const bool value) {
    return value ? L"Yes" : L"No";
}

std::wstring bbs_local_time(const State& state, const std::int64_t value) {
    if (value <= 0) return L"Not yet";
    win32::WindowsBbsLogTimestamp formatter;
    if (!formatter.configure(state.configuration.saved.bbs_time_zone))
        return L"Unavailable";
    const auto formatted = formatter.format(value);
    return formatted ? utf8_to_wide(*formatted) + L" BBS local" : L"Unavailable";
}

const wchar_t* scheduler_result_text(const app::ScheduledJobResult result) {
    switch (result) {
        case app::ScheduledJobResult::never: return L"Never run";
        case app::ScheduledJobResult::running: return L"RUNNING";
        case app::ScheduledJobResult::success: return L"COMPLETED";
        case app::ScheduledJobResult::failed: return L"FAILED";
        case app::ScheduledJobResult::interrupted: return L"WARNING — interrupted by Runtime stop; no retry";
        case app::ScheduledJobResult::partial: return L"PARTIAL — earlier batches committed";
        case app::ScheduledJobResult::skipped: return L"SKIPPED — no automatic retry";
    }
    return L"Unknown";
}

#include "hosting_page.inc"
void populate_controls(State& state) {
    if(!state.hosting_loaded)refresh_hosting(state,true);
    const auto& value = state.configuration.saved;
    const auto& active = state.configuration.active;
    SendMessageW(state.menu_text,CB_SETCURSEL,value.menus.text_color-1,0);
    SendMessageW(state.menu_input,CB_SETCURSEL,value.menus.input_color-1,0);
    Button_SetCheck(state.menu_bars,value.menus.lightbars_enabled?BST_CHECKED:BST_UNCHECKED);
    SendMessageW(state.menu_foreground,CB_SETCURSEL,value.menus.selected_foreground,0);
    SendMessageW(state.menu_background,CB_SETCURSEL,value.menus.selected_background,0);
    SendMessageW(state.menu_footer,CB_SETCURSEL,value.menus.compact_footer?1:0,0);
    SendMessageW(state.menu_pipe_style, CB_SETCURSEL, value.menus.legacy_message_pipe_colors ? 1 : 0, 0);
    Button_SetCheck(state.gate_global,value.botgate.enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(state.gate_telnet,value.telnet.bot_gate_enabled ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(state.gate_ssh,value.botgate.ssh_enabled ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(state.gate_timeout,std::to_wstring(value.botgate.timeout_seconds).c_str());
    const std::array<app::LogRetention,3> logs{{value.logging.runtime,value.logging.callers,value.logging.attention}};
    for (std::size_t i=0;i<logs.size();++i) {
        SetWindowTextW(state.logging_limits[i*2],std::to_wstring(logs[i].size_mib).c_str());
        SetWindowTextW(state.logging_limits[i*2+1],std::to_wstring(logs[i].archives).c_str());
    }
    SetWindowTextW(state.bbs_name, utf8_to_wide(value.bbs_name).c_str());
    SetWindowTextW(state.caller_afk, std::to_wstring(value.caller_afk_minutes).c_str());
    SetWindowTextW(state.login_afk, std::to_wstring(value.login_afk_seconds).c_str());
    select_combo_text(state.time_zone, utf8_to_wide(value.bbs_time_zone));
    SetWindowTextW(state.logical_nodes, std::to_wstring(value.logical_nodes).c_str());
    Button_SetCheck(state.telnet_enabled, value.telnet.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(state.telnet_bind, utf8_to_wide(value.telnet.bind_address).c_str());
    SetWindowTextW(state.telnet_port, std::to_wstring(value.telnet.port).c_str());
    Button_SetCheck(state.ssh_enabled, value.ssh.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(state.ssh_bind, utf8_to_wide(value.ssh.bind_address).c_str());
    SetWindowTextW(state.ssh_port, std::to_wstring(value.ssh.port).c_str());
    Button_SetCheck(state.allow_users, value.allow_new_users ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(state.hostname_lookup, value.hostname_lookup ? BST_CHECKED : BST_UNCHECKED);
    Button_SetCheck(state.schedule_enabled,
        state.scheduler.configuration_loaded && state.scheduler.enabled ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(state.schedule_hour, state.scheduler.configuration_loaded
        ? std::to_wstring(state.scheduler.local_hour).c_str() : L"");
    SetWindowTextW(state.schedule_minute, state.scheduler.configuration_loaded
        ? std::to_wstring(state.scheduler.local_minute).c_str() : L"");
    const auto displayed_destination = state.backup_settings
        ? state.backup_settings->backup_destination.empty()
            ? (state.root / L"Backup" / L"Scheduled").native()
            : utf8_to_wide(state.backup_settings->backup_destination) : std::wstring{};
    SetWindowTextW(state.backup_destination, displayed_destination.c_str());
    SetWindowTextW(state.backup_retention, state.backup_settings
        ? std::to_wstring(state.backup_settings->retained_backups).c_str() : L"");
    const auto context=state.mode==State::ConnectionMode::offline
        ? std::wstring(L"Runtime stopped; no active values. Saved values apply at next start.") : activation_text(state.configuration);
    SetWindowTextW(state.system_detail,(active_saved_line(state,L"BBS name",utf8_to_wide(active.bbs_name),utf8_to_wide(value.bbs_name))+L"\r\n"+context).c_str());
    SetWindowTextW(state.regional_detail,(active_saved_line(state,L"BBS time zone",utf8_to_wide(active.bbs_time_zone),utf8_to_wide(value.bbs_time_zone))+L"\r\n"+context).c_str());
    const auto runtime_detail =
        active_saved_line(state,L"AFK minutes (new authenticated sessions)",
            std::to_wstring(active.caller_afk_minutes),
            std::to_wstring(value.caller_afk_minutes)) + L"\r\n" +
        active_saved_line(state,L"Login idle seconds",std::to_wstring(active.login_afk_seconds),
            std::to_wstring(value.login_afk_seconds)) + L"\r\n" +
        context;
    SetWindowTextW(state.runtime_detail, runtime_detail.c_str());
    const auto mode=[](app::PasswordMode v){return v==app::PasswordMode::strong ? L"Strong" : L"Standard";};
    SetWindowTextW(state.policy_detail,(active_saved_line(state,L"Password policy",mode(active.password_mode),mode(value.password_mode))+L"\r\n"+context).c_str());
    const auto nodes_detail = active_saved_line(state,L"Logical nodes",
        std::to_wstring(active.logical_nodes), std::to_wstring(value.logical_nodes)) +
        L"\r\n" + activation_text(state.configuration);
    SetWindowTextW(state.nodes_detail, nodes_detail.c_str());
    const auto servers_detail =
        active_saved_line(state,L"Telnet", yes_no(active.telnet.enabled) + L" — " +
            utf8_to_wide(active.telnet.bind_address) + L":" +
            std::to_wstring(active.telnet.port), yes_no(value.telnet.enabled) + L" — " +
            utf8_to_wide(value.telnet.bind_address) + L":" +
            std::to_wstring(value.telnet.port)) + L"\r\n" +
        active_saved_line(state,L"SSH", yes_no(active.ssh.enabled) + L" — " +
            utf8_to_wide(active.ssh.bind_address) + L":" +
            std::to_wstring(active.ssh.port), yes_no(value.ssh.enabled) + L" — " +
            utf8_to_wide(value.ssh.bind_address) + L":" +
            std::to_wstring(value.ssh.port)) + L"\r\n" +
        active_saved_line(state,L"Hostname Lookup", yes_no(active.hostname_lookup),
            yes_no(value.hostname_lookup)) + L"\r\n" +
        activation_text(state.configuration);
    SetWindowTextW(state.servers_detail, servers_detail.c_str());
    auto scheduler_detail = state.scheduler.configuration_loaded
        ? L"Built-in job: Backup\r\nDaily local time: " +
          std::to_wstring(state.scheduler.local_hour) + L":" +
          (state.scheduler.local_minute < 10 ? L"0" : L"") +
          std::to_wstring(state.scheduler.local_minute) +
        L"\r\nNext eligible run: " + bbs_local_time(state, state.scheduler.next_run_utc) +
        L"\r\nCurrent state: " + (state.scheduler.queued ? L"QUEUED" :
            state.scheduler.active ? L"RUNNING" : L"Idle") +
        (state.scheduler.active
            ? (state.scheduler.active_scheduled ? L" — scheduled backup"
                                                : L" — manual backup")
            : L"") +
        L"\r\nLast completion: " + bbs_local_time(state, state.scheduler.last_run_utc) +
        L"\r\nLast result: " + scheduler_result_text(state.scheduler.last_result) +
        L"\r\nDetail/archive: " + utf8_to_wide(state.scheduler.last_detail) +
        L"\r\nRetained history entries: " + std::to_wstring(state.scheduler.history.size()) + L" / 32" +
        L"\r\nMissed run policy: Skip after Runtime restart" +
        L"\r\nAutomatic retry: None"
        : L"Built-in job: Backup\r\nState: Unavailable\r\nDiagnostic: " +
          utf8_to_wide(state.scheduler.diagnostic_code);
    if (state.scheduler.configuration_loaded && !state.scheduler.history.empty()) {
        scheduler_detail += L"\r\n\r\nHistory (oldest to newest):";
        for (const auto& item : state.scheduler.history) {
            scheduler_detail += L"\r\n" + bbs_local_time(state, item.finished_utc) +
                L" | " + scheduler_result_text(item.result) + L" | " +
                std::to_wstring(item.finished_utc - item.started_utc) + L"s | " +
                utf8_to_wide(item.detail);
        }
    }
    SetWindowTextW(state.schedule_detail, scheduler_detail.c_str());
    if (state.backup_settings) {
        const auto destination = state.backup_settings->backup_destination.empty()
            ? state.root / L"Backup" / L"Scheduled" : std::filesystem::u8path(state.backup_settings->backup_destination);
        scheduler_detail += L"\r\nMutable-data backup destination: " + destination.native() +
            L"\r\nVerified managed archives retained: " + std::to_wstring(state.backup_settings->retained_backups);
        if (state.backup_settings->format_version == 1)
            scheduler_detail += L"\r\nLegacy saved schedule preserved (including its old enabled default). Saving upgrades only the backup configuration to version 2.";
        SetWindowTextW(state.schedule_detail, scheduler_detail.c_str());
    }

    const auto profile = read_client_profile(state.root);
    if (profile) {
        SendMessageW(state.client_type, CB_SETCURSEL,
            profile->type == app::ExternalTelnetClientType::syncterm ? 0 : 1, 0);
        SetWindowTextW(state.client_path, profile->executable.c_str());
        std::wstring arguments;
        for (const auto& argument : profile->custom_arguments) {
            if (!arguments.empty()) arguments += L"\r\n";
            arguments += argument;
        }
        SetWindowTextW(state.client_arguments, arguments.c_str());
    } else {
        SendMessageW(state.client_type, CB_SETCURSEL, 0, 0);
        SetWindowTextW(state.client_path, L"");
        SetWindowTextW(state.client_arguments, L"");
    }
    const bool writable = state.mode != State::ConnectionMode::blocked;
    EnableWindow(state.menu_save,writable && !state.system_recovery_only);
    refresh_menu_preview(state);
    EnableWindow(GetDlgItem(state.pages[13],IDC_REGIONAL_SAVE),writable && !state.system_recovery_only);
    EnableWindow(GetDlgItem(state.pages[41],IDC_RUNTIME_SAVE),writable && !state.system_recovery_only);
    EnableWindow(GetDlgItem(state.pages[15],IDC_REGISTRATION_SAVE),writable && !state.system_recovery_only);
    EnableWindow(state.gate_save,writable && !state.system_recovery_only);
    EnableWindow(state.logging_save,writable && !state.system_recovery_only);
    EnableWindow(state.gate_update,state.mode==State::ConnectionMode::online && !state.system_recovery_only);
    Button_SetCheck(state.strong_passwords,value.password_mode == app::PasswordMode::strong ? BST_CHECKED : BST_UNCHECKED);
    SetWindowTextW(state.source_limit,std::to_wstring(value.maximum_connections_per_source).c_str());
    SetWindowTextW(state.account_limit,std::to_wstring(value.maximum_sessions_per_account).c_str());
    std::wstring exception_lines;
    for (const auto& entry : value.source_connection_exceptions) {
        if (!exception_lines.empty()) exception_lines += L"\r\n";
        exception_lines += utf8_to_wide(entry.address) + L"|" + std::to_wstring(entry.maximum_connections);
    }
    SetWindowTextW(state.source_exceptions,exception_lines.c_str());
    const auto connection_detail = active_saved_line(state,L"Connections per source",
        std::to_wstring(active.maximum_connections_per_source),std::to_wstring(value.maximum_connections_per_source)) + L"\r\n" +
        L"Ordinary callers: one session. Security 999: up to configured nodes.\r\n" +
        active_saved_line(state,L"Exact-address exceptions",std::to_wstring(active.source_connection_exceptions.size()),
            std::to_wstring(value.source_connection_exceptions.size())) + L"\r\n" + activation_text(state.configuration);
    SetWindowTextW(state.connection_detail,connection_detail.c_str());
    for (const auto control : {state.source_limit,state.account_limit,state.source_exceptions,state.connection_save})
        EnableWindow(control,writable && !state.system_recovery_only);
    EnableWindow(state.account_limit,FALSE); // retained legacy value, not an effective concurrency policy
    for (const auto control : {state.throttle_scope,state.throttle_clear})
        EnableWindow(control,writable && !state.system_recovery_only);
    const auto throttle_selection = SendMessageW(state.throttle_scope,CB_GETCURSEL,0,0);
    EnableWindow(state.throttle_subject,writable && !state.system_recovery_only && throttle_selection >= 0 && throttle_selection < 2);
    EnableWindow(state.strong_passwords,writable && !state.system_recovery_only);
    EnableWindow(state.password_policy_save,writable && !state.system_recovery_only);
    for (const auto control : {state.system_save, state.nodes_save, state.servers_save,
            state.client_save, state.client_clear, state.backup_run})
        EnableWindow(control, writable);
    for (const auto control : {state.schedule_enabled, state.schedule_hour,
            state.schedule_minute, state.schedule_save})
        EnableWindow(control, writable && state.scheduler.configuration_loaded);
    EnableWindow(state.backup_restore,
        state.mode == State::ConnectionMode::offline);
    EnableWindow(state.backup_contents,state.mode==State::ConnectionMode::offline&&!state.system_recovery_only);
    EnableWindow(state.ftn_history_recover,state.mode==State::ConnectionMode::offline&&!state.system_recovery_only);
    for (const auto control : {state.backup_destination, state.backup_browse})
        EnableWindow(control, writable && !state.system_recovery_only &&
            state.scheduler.configuration_loaded && state.backup_settings.has_value());
    EnableWindow(state.backup_retention,
        state.mode == State::ConnectionMode::offline && state.backup_settings.has_value());
    SetWindowTextW(state.schedule_enabled,L"Enable built-in Backup job");
    populate_message_jobs(state);
}

std::optional<app::OperationalConfiguration> candidate_for_page(
    State& state, const int page) {
    auto candidate = state.configuration.saved;
    if (page == 17) {
        const auto color=[](HWND box){return static_cast<int>(SendMessageW(box,CB_GETCURSEL,0,0));};
        const auto t=color(state.menu_text),i=color(state.menu_input),f=color(state.menu_foreground),b=color(state.menu_background);
        if(t<0 || t>14 || i<0 || i>14 || f<0 || f>15 || b<0 || b>7) return std::nullopt;
        candidate.menus={static_cast<std::uint8_t>(t+1),static_cast<std::uint8_t>(i+1),
            Button_GetCheck(state.menu_bars)==BST_CHECKED,static_cast<std::uint8_t>(f),static_cast<std::uint8_t>(b)};
        const auto footer=color(state.menu_footer), pipe_style=color(state.menu_pipe_style);
        if(footer<0 || footer>1 || pipe_style<0 || pipe_style>1) return std::nullopt;
        candidate.menus.compact_footer=footer==1;
        candidate.menus.legacy_message_pipe_colors=pipe_style==1;
    } else if (page == 0) {
        const auto bbs_name = wide_to_utf8(text(state.bbs_name));
        if (!bbs_name) return std::nullopt;
        candidate.bbs_name = *bbs_name;
    } else if (page == 13) {
        const int selection = static_cast<int>(SendMessageW(state.time_zone, CB_GETCURSEL, 0, 0));
        if (selection == CB_ERR) return std::nullopt;
        std::array<wchar_t, 256> zone{};
        if (SendMessageW(state.time_zone, CB_GETLBTEXT, static_cast<WPARAM>(selection),
                reinterpret_cast<LPARAM>(zone.data())) == CB_ERR) return std::nullopt;
        const auto encoded_zone = wide_to_utf8(zone.data());
        if (!encoded_zone) return std::nullopt;
        candidate.bbs_time_zone = *encoded_zone;
    } else if (page == 41) {
        const auto afk = port_value(state.caller_afk);
        if (!afk || *afk < 2 || *afk > 1440) return std::nullopt;
        candidate.caller_afk_minutes = *afk;
        const auto login_afk=port_value(state.login_afk);
        if (!login_afk || *login_afk<30 || *login_afk>600) return std::nullopt;
        candidate.login_afk_seconds=*login_afk;
    } else if (page == 1) {
        const auto nodes = node_count_value(state.logical_nodes);
        if (!nodes) return std::nullopt;
        candidate.logical_nodes = *nodes;
    } else if (page == 2) {
        const auto telnet_bind = wide_to_utf8(text(state.telnet_bind));
        const auto ssh_bind = wide_to_utf8(text(state.ssh_bind));
        const auto telnet_port = port_value(state.telnet_port);
        const auto ssh_port = port_value(state.ssh_port);
        if (!telnet_bind || !ssh_bind || !telnet_port || !ssh_port) return std::nullopt;
        candidate.telnet.enabled = Button_GetCheck(state.telnet_enabled) == BST_CHECKED;
        candidate.telnet.bind_address = *telnet_bind;
        candidate.telnet.port = *telnet_port;
        candidate.ssh.enabled = Button_GetCheck(state.ssh_enabled) == BST_CHECKED;
        candidate.ssh.bind_address = *ssh_bind;
        candidate.ssh.port = *ssh_port;
        candidate.hostname_lookup = Button_GetCheck(state.hostname_lookup) == BST_CHECKED;
    } else if (page == 15) {
        candidate.allow_new_users = Button_GetCheck(state.allow_users) == BST_CHECKED;
    } else if (page == 16) {
        candidate.password_mode = Button_GetCheck(state.strong_passwords) == BST_CHECKED
            ? app::PasswordMode::strong : app::PasswordMode::standard;
    } else if (page == 11) {
        candidate.botgate.enabled=Button_GetCheck(state.gate_global)==BST_CHECKED;
        candidate.telnet.bot_gate_enabled=Button_GetCheck(state.gate_telnet)==BST_CHECKED;
        candidate.botgate.ssh_enabled=Button_GetCheck(state.gate_ssh)==BST_CHECKED;
        const auto timeout=port_value(state.gate_timeout);
        if (!timeout) return {};
        candidate.botgate.timeout_seconds=*timeout;
        // File-based rules are saved independently, never overwritten by these
        // general switches. Legacy fields are migration input only.
    } else if (page == 12) {
        const std::array<app::LogRetention*,3> limits{{&candidate.logging.runtime,&candidate.logging.callers,&candidate.logging.attention}};
        for (std::size_t i=0;i<limits.size();++i) {
            const auto size=port_value(state.logging_limits[i*2]),count=port_value(state.logging_limits[i*2+1]);
            if (!size || !count) return {};
            *limits[i]={*size,*count};
        }
    } else if (page == 9) {
        const auto source = node_count_value(state.source_limit);
        if (!source) return std::nullopt;
        candidate.maximum_connections_per_source = *source;
        std::vector<std::string> rows;
        std::wistringstream lines(text(state.source_exceptions));
        std::wstring line;
        while (std::getline(lines,line)) {
            if (!line.empty() && line.back() == L'\r') line.pop_back();
            if (line.empty()) continue;
            const auto encoded = wide_to_utf8(line);
            if (!encoded || encoded->find(',') != std::string::npos || *encoded == "-" ||
                rows.size() == app::kMaximumSourceConnectionExceptions) return std::nullopt;
            rows.push_back(*encoded);
        }
        std::sort(rows.begin(),rows.end(),[](const auto& left,const auto& right) {
            return left.substr(0,left.find('|')) < right.substr(0,right.find('|'));
        });
        std::string encoded;
        for (const auto& row : rows) { if (!encoded.empty()) encoded += ','; encoded += row; }
        const auto exceptions = app::parse_source_connection_limits(encoded.empty() ? "-" : encoded);
        if (!exceptions) return std::nullopt;
        candidate.source_connection_exceptions = *exceptions;
    }
    if (!app::validate_operational_configuration(candidate).valid()) return std::nullopt;
    return candidate;
}

bool widens_connection_limits(const app::OperationalConfiguration& previous,
    const app::OperationalConfiguration& next) {
    const auto limit_for=[](const app::OperationalConfiguration& settings,std::string_view address){
        const auto found=std::find_if(settings.source_connection_exceptions.begin(),settings.source_connection_exceptions.end(),
            [&](const auto& entry){return entry.address==address;});
        return found==settings.source_connection_exceptions.end()?settings.maximum_connections_per_source:found->maximum_connections;
    };
    if(next.maximum_connections_per_source>previous.maximum_connections_per_source)return true;
    for(const auto& entry:next.source_connection_exceptions)
        if(entry.maximum_connections>limit_for(previous,entry.address))return true;
    for(const auto& entry:previous.source_connection_exceptions)
        if(limit_for(next,entry.address)>entry.maximum_connections)return true;
    return false;
}

bool save_configuration_page(State& state, const int page) {
    if (state.system_recovery_only) return false;
    if(page==14)return !hosting_dirty(state)||save_hosting(state);
    const auto candidate_value = candidate_for_page(state, page);
    if (!candidate_value) {
        set_status(state, L"Refused — one or more values are invalid. Check names, addresses, ports, and bounds.");
        MessageBeep(MB_ICONWARNING);
        return false;
    }
    auto candidate = *candidate_value;
    auto activation=app::AlphaConfigurationActivation::saved_next_start;
    std::wstring activation_detail;
    if (page == 9 && widens_connection_limits(state.configuration.saved,candidate) &&
        MessageBoxW(state.window,L"Allow more simultaneous connections from one or more source addresses?\n\n"
        L"After the BBS restarts, affected addresses can use more caller nodes. Login and throttling still apply.",
        L"Review Higher Connection Limits",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING) != IDYES) return false;
    if (page == 16) {
        if (state.system_recovery_only) return false;
        if (candidate.password_mode == app::PasswordMode::strong &&
            !sfx::adapters::authentication::PinnedSecListsBlocklist().ready()) {
            set_status(state,L"Refused — the packaged Strong-password blocklist is unavailable."); return false;
        }
        if (MessageBoxW(state.window,L"Save the selected password policy?\n\n"
            L"Applies on next Runtime start to new or changed account and SY: passwords. "
            L"Existing credentials are not replaced or invalidated. Standard allows 4–128 printable ASCII characters; "
            L"Strong requires 15–128 with the pinned local blocklist and context checks.",
            L"Review Password Policy",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING) != IDYES) return false;
    }
    if (state.configuration.saved.configuration_revision ==
            std::numeric_limits<std::uint64_t>::max()) {
        set_status(state, L"Refused — the configuration revision cannot advance.");
        return false;
    }
    candidate.configuration_revision = state.configuration.saved.configuration_revision + 1U;
    if (state.mode == State::ConnectionMode::online) {
        app::AlphaConfigurationUpdateRequest update;
        update.expected_revision = state.configuration.saved.configuration_revision;
        update.configuration = candidate;
        const auto outgoing = request(state,
            app::LocalControlMessageType::configuration_update_request,
            app::encode_configuration_update_request(update));
        app::LocalControlFrame response;
        if (outgoing.payload.empty() || !online_exchange(state, outgoing,
                app::LocalControlMessageType::configuration_update_response, response)) {
            set_status(state, L"Refused — the authenticated Runtime configuration channel failed.");
            return false;
        }
        const auto result = app::decode_configuration_update_result(response.payload);
        if (!result || result->code != app::LocalControlResponseCode::ok) {
            set_status(state, result ? L"Refused — " + utf8_to_wide(result->detail) :
                L"Refused — Runtime returned an invalid configuration response.");
            return false;
        }
        activation=result->activation;
        activation_detail=utf8_to_wide(result->detail);
        set_status(state,activation_detail);
    } else if (state.mode == State::ConnectionMode::offline) {
        win32::OfflineInstallationGuard guard;
        const auto acquired = guard.acquire(state.root, state.owner);
        if (!acquired) {
            set_status(state, L"Refused — Runtime started or another offline transaction owns this installation.");
            return false;
        }
        const auto current = win32::read_operational_configuration_file(state.root);
        if (current.state != win32::ConfigurationFileState::loaded || !current.configuration ||
            current.configuration->configuration_revision !=
                state.configuration.saved.configuration_revision) {
            set_status(state, L"Refused — configuration changed; reload before saving.");
            return false;
        }
        const auto published = win32::publish_operational_configuration_file(
            state.root, state.owner, candidate);
        if (!published) {
            set_status(state, L"Refused — the prior configuration remains authoritative (" +
                utf8_to_wide(published.operation) + L").");
            return false;
        }
        set_status(state, L"Saved — applies on next Runtime start.");
    } else {
        set_status(state, L"Refused — reload after the installation owner is available.");
        return false;
    }
    const auto action_status = activation==app::AlphaConfigurationActivation::saved_next_start || activation==app::AlphaConfigurationActivation::restart_required
        ? std::wstring(L"Saved — restart required. ")+activation_detail
        : activation==app::AlphaConfigurationActivation::pending_safe_boundary
        ? std::wstring(L"Saved — applies to new sessions after current callers leave. ")+activation_detail
        : (page==11 || page==41) ? std::wstring(L"Saved — applies to new sessions. ")+activation_detail
        : std::wstring(L"Applied — settings saved. ")+activation_detail;
    if (load_editor_state(state)) {
        populate_controls(state);
        set_status(state, action_status);
    } else {
        state.mode=State::ConnectionMode::blocked;
        set_status(state,L"Saved — state refresh unavailable. Reopen Configuration before another save; do not repeat the committed change.");
    }
    return true;
}

std::vector<std::wstring> argument_lines(const std::wstring& value) {
    std::vector<std::wstring> result;
    std::size_t position = 0;
    while (position < value.size()) {
        auto end = value.find_first_of(L"\r\n", position);
        if (end == std::wstring::npos) end = value.size();
        if (end != position) result.emplace_back(value.substr(position, end - position));
        position = end;
        while (position < value.size() && (value[position] == L'\r' || value[position] == L'\n'))
            ++position;
    }
    return result;
}

void save_external_client(State& state) {
    const int selection = static_cast<int>(SendMessageW(state.client_type, CB_GETCURSEL, 0, 0));
    app::ExternalTelnetClientProfile profile;
    profile.type = selection == 0 ? app::ExternalTelnetClientType::syncterm
                                  : app::ExternalTelnetClientType::custom;
    profile.executable = text(state.client_path);
    if (profile.type == app::ExternalTelnetClientType::custom)
        profile.custom_arguments = argument_lines(text(state.client_arguments));
    const auto validation = app::validate_external_telnet_client_profile(profile);
    if (!validation.valid || !save_client_profile(state.root, profile)) {
        set_status(state,
            L"Refused — select an existing SyncTERM 1.10-series executable or a valid Custom profile using only controlled {HOST}/{PORT} tokens.");
        MessageBeep(MB_ICONWARNING);
        return;
    }
    set_status(state, L"Applied — this Windows operator's client profile will be used by the next exact-node launch.");
}

void browse_external_client(State& state) {
    std::array<wchar_t, 32768> path{};
    const auto current = text(state.client_path);
    if (current.size() < path.size()) std::copy(current.begin(), current.end(), path.begin());
    OPENFILENAMEW dialog{sizeof(dialog)};
    dialog.hwndOwner = state.window;
    dialog.lpstrFilter = L"Applications (*.exe)\0*.exe\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = path.data();
    dialog.nMaxFile = static_cast<DWORD>(path.size());
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR |
                   OFN_DONTADDTORECENT | OFN_HIDEREADONLY;
    dialog.lpstrTitle = L"Select Default Telnet Client";
    if (GetOpenFileNameW(&dialog)) {
        SetWindowTextW(state.client_path, path.data());
        const auto detected = app::external_telnet_client_type_for_path(path.data());
        SendMessageW(state.client_type, CB_SETCURSEL,
            detected == app::ExternalTelnetClientType::syncterm ? 0 : 1, 0);
    }
}

int CALLBACK backup_folder_initialized(HWND window, UINT message, LPARAM, LPARAM initial) {
    if (message == BFFM_INITIALIZED && initial)
        SendMessageW(window, BFFM_SETSELECTIONW, TRUE, initial);
    return 0;
}

void browse_backup_destination(State& state) {
    if (!IsWindowEnabled(state.backup_destination) || state.system_recovery_only) return;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) {
        set_status(state, L"Unavailable — the Windows folder picker could not be opened.");
        return;
    }
    struct ComScope { ~ComScope() { CoUninitialize(); } } release;
    const auto initial = text(state.backup_destination);
    BROWSEINFOW browse{};
    browse.hwndOwner = state.window;
    browse.lpszTitle = L"Select the folder for future manual and scheduled backups. Existing backups will not be moved.";
    browse.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    browse.lpfn = backup_folder_initialized;
    browse.lParam = reinterpret_cast<LPARAM>(initial.c_str());
    auto pidl = SHBrowseForFolderW(&browse);
    if (!pidl) return; // Cancel leaves both the field and saved settings untouched.
    std::array<wchar_t, MAX_PATH> path{};
    const auto selected = SHGetPathFromIDListW(pidl, path.data());
    CoTaskMemFree(pidl);
    if (!selected) {
        set_status(state, L"Refused — choose a local filesystem folder for backups.");
        return;
    }
    SetWindowTextW(state.backup_destination, path.data());
    set_status(state, L"Selected — Save Changes validates and applies this folder to future backups. Existing backups stay where they are.");
}

void refresh_backup_inventory(State& state) {
    state.backups.clear();
    ListView_DeleteAllItems(state.backup_list);
    std::error_code error;
    std::vector<std::filesystem::path> directories{state.root / L"Backup"};
    std::vector<win32::BackupCollection> collections;
    auto legacy_anchor = install::open_object(state.root / L"Backup", true);
    for (const auto kind : {archive::BackupKind::setup_upgrade, archive::BackupKind::manual}) {
        try {
            auto collection = win32::open_backup_collection(state.root, state.owner, kind,
                state.backup_settings ? state.backup_settings->backup_destination : std::string{}, false);
            if (std::find(directories.begin(), directories.end(), collection.directory) == directories.end())
                directories.push_back(collection.directory);
            collections.push_back(std::move(collection));
        } catch (const std::exception&) {
            // Inventory does not create, repair or replace unavailable pools.
            // Backup Now separately reports the exact destination refusal.
        }
    }
    std::vector<std::filesystem::path> found;
    for (const auto& directory : directories) {
    error.clear();
    for (std::filesystem::directory_iterator iterator(directory,
             std::filesystem::directory_options::skip_permission_denied, error), end;
         !error && iterator != end && found.size() < 256U; iterator.increment(error)) {
        const auto& entry = *iterator;
        const auto attributes = GetFileAttributesW(entry.path().c_str());
        const bool regular_file = attributes != INVALID_FILE_ATTRIBUTES &&
            !(attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
        if (error) {
            error.clear();
            continue;
        }
        if (!regular_file) continue;
        const auto filename = entry.path().filename().native();
        if (filename.size() >= 8U && filename.rfind(L"SFX-", 0) == 0 &&
            _wcsicmp(entry.path().extension().c_str(), L".zip") == 0)
            found.push_back(entry.path());
    }
    }
    std::sort(found.begin(), found.end(), std::greater<std::filesystem::path>());
    state.backups = std::move(found);
    for (std::size_t index = 0; index < state.backups.size(); ++index) {
        const auto filename = state.backups[index].filename().native();
        std::wstring type = L"Unknown";
        std::size_t timestamp_offset = std::wstring::npos;
        if (filename.rfind(L"SFX-setup-backup-", 0) == 0) {
            type = L"Setup/update";
            timestamp_offset = 17;
        } else if (filename.rfind(L"SFX-scheduled-backup-", 0) == 0) {
            type = L"Scheduled";
            timestamp_offset = 21;
        } else if (filename.rfind(L"SFX-pre-restore-backup-", 0) == 0) {
            type = L"Pre-restore";
            timestamp_offset = 23;
        } else if (filename.rfind(L"SFX-backup-", 0) == 0) {
            type = L"Manual";
            timestamp_offset = 11;
        }
        std::wstring created = L"Unknown";
        if (timestamp_offset != std::wstring::npos &&
            filename.size() == timestamp_offset + 19U &&
            filename[timestamp_offset + 8U] == L'-' &&
            _wcsicmp(filename.c_str() + filename.size() - 4U, L".zip") == 0) {
            const auto component = filename.substr(timestamp_offset, 15U);
            const bool digits = std::all_of(component.begin(), component.end(),
                [](const wchar_t value) { return value == L'-' || (value >= L'0' && value <= L'9'); });
            if (digits) {
                created = component.substr(0, 4) + L"-" + component.substr(4, 2) +
                    L"-" + component.substr(6, 2) + L" " + component.substr(9, 2) +
                    L":" + component.substr(11, 2) + L":" + component.substr(13, 2);
            }
        }
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(index);
        item.pszText = const_cast<wchar_t*>(filename.c_str());
        ListView_InsertItem(state.backup_list, &item);
        ListView_SetItemText(state.backup_list, static_cast<int>(index), 1,
            const_cast<wchar_t*>(type.c_str()));
        ListView_SetItemText(state.backup_list, static_cast<int>(index), 2,
            const_cast<wchar_t*>(created.c_str()));
        ListView_SetItemText(state.backup_list, static_cast<int>(index), 3,
            const_cast<wchar_t*>(L"Unverified"));
        const auto bytes = std::filesystem::file_size(state.backups[index], error);
        const auto size = error ? L"Unavailable" : std::to_wstring(bytes) + L" bytes";
        error.clear();
        ListView_SetItemText(state.backup_list, static_cast<int>(index), 4,
            const_cast<wchar_t*>(size.c_str()));
    }
    const auto detail = L"Locations: " + (state.root / L"Backup").native() +
        L"\\Upgrades and the saved ordinary destination (default Scheduled).\r\n"
        L"Retention: newest 3 verified upgrade points; manual/scheduled use the saved count, plus required content dependencies.\r\n"
        L"Cleanup follows a verified publication/commit only; unknown and recovery materials stay.\r\n" +
        std::to_wstring(state.backups.size()) +
        L" recognized archive(s) shown (bounded to 256).\r\n"
        L"To move this board, stop BBS and copy the complete folder. Launch the copied Control Panel and choose Prepare SFX on this computer.";
    SetWindowTextW(state.backup_detail, detail.c_str());
}

std::optional<std::size_t> selected_backup(State& state) {
    const int selected = ListView_GetNextItem(state.backup_list, -1, LVNI_SELECTED);
    if (selected < 0 || static_cast<std::size_t>(selected) >= state.backups.size())
        return std::nullopt;
    return static_cast<std::size_t>(selected);
}

void verify_selected_backup(State& state) {
    const auto selected = selected_backup(state);
    if (!selected) {
        set_status(state, L"Select a recognized SFX backup archive to verify.");
        return;
    }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const auto inspection = archive::inspect_backup(state.backups[*selected]);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if (!inspection.result.ok()) {
        ListView_SetItemText(state.backup_list, static_cast<int>(*selected), 3,
            const_cast<wchar_t*>(L"Invalid"));
        set_status(state, L"Refused — archive verification failed: " +
            utf8_to_wide(std::string(archive::error_name(inspection.result.error)) +
                ": " + inspection.result.detail));
        return;
    }
    try {
        const auto index=archive::inspect_content_index(state.backups[*selected],inspection);
        if(!index.areas.empty()) {
            const auto path=state.backups[*selected];
            const auto kind=path.parent_path()==state.root/L"Backup"/L"Upgrades"?archive::BackupKind::setup_upgrade:archive::BackupKind::manual;
            auto collection=win32::open_backup_collection(state.root,state.owner,kind,path.parent_path().u8string(),false);
            const auto members=win32::verified_backup_members(state.root,state.owner,collection,kind);
            const auto found=std::find_if(members.begin(),members.end(),[&](const auto& member){return member.path==path;});
            if(found==members.end())throw std::runtime_error("file_content_restore_point_not_managed_by_this_installation");
            win32::verify_content_dependencies(members,{static_cast<std::size_t>(found-members.begin())});
        }
    } catch(const std::exception& e) {
        ListView_SetItemText(state.backup_list,static_cast<int>(*selected),3,const_cast<wchar_t*>(L"Incomplete"));
        set_status(state,L"Core archive verified, but selected file-content chain verification failed: "+utf8_to_wide(e.what()));
        return;
    }
    ListView_SetItemText(state.backup_list, static_cast<int>(*selected), 3,
        const_cast<wchar_t*>(L"Verified"));
    SetWindowTextW(state.backup_detail,
        (L"Verified owner-data backup\r\nType: " + utf8_to_wide(inspection.metadata.backup_type) +
         L"\r\nCreated: " + utf8_to_wide(inspection.metadata.created.local) +
         L"\r\nFiles: " + std::to_wstring(inspection.files.size()) +
         L"\r\nUncompressed bytes: " + std::to_wstring(inspection.total_bytes) +
         L"\r\nArchive: " + state.backups[*selected].native()).c_str());
    set_status(state, L"Completed — manifest paths, sizes and SHA-256 hashes match; any selected file-content dependencies are verified.");
}

void run_backup_now(State& state) {
    if (state.mode == State::ConnectionMode::online) {
        const auto outgoing = request(state,
            app::LocalControlMessageType::scheduler_run_request,
            app::encode_scheduler_run_request({}));
        app::LocalControlFrame response;
        if (outgoing.payload.empty() || !online_exchange(state, outgoing,
                app::LocalControlMessageType::scheduler_run_response, response)) {
            set_status(state, L"Refused — the authenticated Runtime backup channel failed.");
            return;
        }
        const auto result = app::decode_control_response(response.payload);
        if (!result || result->code != app::LocalControlResponseCode::ok) {
            set_status(state, L"Refused — " + utf8_to_wide(result
                ? result->diagnostic_code : "invalid Runtime response"));
            return;
        }
        set_status(state,
            L"Running — Runtime owns this backup. Closing Configuration will not cancel it; Events will report its outcome.");
        state.watching_backup = true;
        state.watched_last_run = state.scheduler.last_run_utc;
        SetTimer(state.window,config_timer::backup, 750, nullptr);
        return;
    }
    if (state.mode != State::ConnectionMode::offline) {
        set_status(state, L"Refused — the installation owner is not available.");
        return;
    }
    SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    const auto saved = win32::offline_create_backup(state.root,state.owner);
    SetCursor(LoadCursorW(nullptr, IDC_ARROW));
    if(!saved.status){set_status(state,L"Failed — "+utf8_to_wide(saved.status.operation)+(saved.archive.empty()?L"":L"; verified archive retained: "+saved.archive.native()));return;}
    set_status(state, L"Completed — verified full core and selected file contents: " + saved.archive.native() +
        L"; managed archives pruned: " + std::to_wstring(saved.pruned));
    refresh_backup_inventory(state);
}

#include "file_contents_restore_review.inc"

void restore_selected_contents(State& state) {
    if(state.mode!=State::ConnectionMode::offline){set_status(state,L"Stop Runtime and Control Panel before restoring file contents.");return;}
    const auto selected=selected_backup(state);if(!selected){set_status(state,L"Select a restore point first.");return;}
    try{
        const auto path=state.backups[*selected];const auto inspection=archive::inspect_backup(path);
        const auto index=archive::inspect_content_index(path,inspection);
        if(index.areas.empty()){set_status(state,L"This point has no selected-area file-content snapshot. Legacy/core-only backups do not imply a file-content restore.");return;}
        auto restore=review_file_contents_restore(state,path,index.areas);
        if(!restore){set_status(state,L"Content restore cancelled. No files changed.");return;}
        SetCursor(LoadCursorW(nullptr,IDC_WAIT));const auto result=restore->execute();SetCursor(LoadCursorW(nullptr,IDC_ARROW));
        set_status(state,(result.completed?L"Completed — ":L"Incomplete — ")+std::to_wstring(result.restored)+L" restored; "+std::to_wstring(result.identical)+L" identical; "+std::to_wstring(result.conflicts)+L" conflicts skipped. "+utf8_to_wide(result.diagnostic));
    }catch(const std::exception& e){SetCursor(LoadCursorW(nullptr,IDC_ARROW));set_status(state,L"Content restore refused: "+utf8_to_wide(e.what()));}
}

void recover_ftn_history(State& state){
    if(state.mode!=State::ConnectionMode::offline||state.system_recovery_only){set_status(state,L"Stop the BBS and Control Panel before recovering FTN history.");return;}
    try{
        auto recovery=win32::preview_ftn_history_recovery(state.root,state.owner);
        const auto& preview=recovery->preview();
        if(preview.state.recovery_required){
            const auto detail=std::wstring(L"Current UTC seconds: ")+std::to_wstring(preview.utc)+
                L"\nVerified pending outbound files: "+std::to_wstring(preview.pending_outbound)+
                L"; incoming files: "+std::to_wstring(preview.pending_ingress)+
                L".\nSeparate held items remain held: "+std::to_wstring(preview.held_outbound+preview.held_ingress)+
                L".\n\nStop any independently launched BinkD before continuing. This action preserves existing message IDs, queued bytes and known completed states. The BBS remains stopped.\n\nThis restored BBS will replace the previous copy. Any other copy using these FTN addresses is stopped.";
            if(MessageBoxW(state.window,detail.c_str(),L"Recover FTN History",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING)!=IDYES){set_status(state,L"FTN recovery cancelled. No history state was changed.");return;}
        }else if(!preview.state.recovery_generation){set_status(state,L"FTN message ID history is healthy. Recovery is not required. The BBS remains stopped.");return;}
        const auto result=recovery->execute(recovery->confirmation_token());
        if(!result.status||!result.receipt){set_status(state,utf8_to_wide(app::ftn_history_recovery_problem(result.status.operation)));return;}
        set_status(state,result.report_written?L"FTN recovery complete. You can start the BBS.":L"FTN history recovery committed. The Logs report is pending; use Recover FTN History again to retry the report. The BBS remains stopped.");
    }catch(const std::exception& error){set_status(state,utf8_to_wide(app::ftn_history_recovery_problem(error.what())));}
}
void restore_selected_backup(State& state) {
    if (state.mode != State::ConnectionMode::offline) {
        set_status(state, L"Refused — Restore requires Runtime and Control Panel to be stopped.");
        return;
    }
    const auto selected = selected_backup(state);
    if (!selected) {
        set_status(state, L"Select and verify an eligible SFX backup before Restore.");
        return;
    }
    const auto inspection = archive::inspect_backup(state.backups[*selected]);
    if (!inspection.result.ok()) {
        set_status(state, L"Refused — selected archive did not pass a fresh full verification.");
        return;
    }
    const auto prompt = L"Restore this verified owner-data backup?\n\n" +
        state.backups[*selected].native() +
        L"\n\nSFX will first create a verified pre-restore backup. Protected programs and file contents are NOT restored. Use Restore Contents separately for explicitly selected areas.";
    if (MessageBoxW(state.window, prompt.c_str(), L"Confirm Restore",
            MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) {
        set_status(state, L"Restore cancelled — no live data was changed.");
        return;
    }
    archive::RestoreResult restored;bool ftn_recovery_required{},file_recovery_attention{};
    {
        win32::OfflineInstallationGuard guard;
        const auto acquired = guard.acquire(state.root, state.owner);
        if (!acquired) {
            set_status(state, L"Refused — Runtime started or another offline transaction owns this installation.");
            return;
        }
        const auto ftn_serial=win32::OfflineFtnSerialRestore::capture(state.root,state.owner,guard);
        archive::RestoreRequest restore;
        restore.installation_root = state.root;
        restore.archive_path = state.backups[*selected];
        restore.transaction_id = transaction_id();
        restore.sfx_version = SFX_PRODUCT_VERSION_SHORT_STRING;
        restore.source_generation = std::to_string(state.receipt.generation);
        if(state.receipt.ftn_layout)install::verify_ftn_directory(state.receipt);
        restore.ftn_layout=state.receipt.ftn_layout;
        restore.pre_restore_created = archive::current_timestamp();
        const auto prior_restore_root=state.root/L"Work"/
            std::filesystem::u8path("restore-"+restore.transaction_id)/L"previous";
        restore.secure_live_directory = [&](const std::filesystem::path& path) {
            win32::WindowsStorageDaclPolicy policy(state.owner);
            // Role roots remain the original live directories. Nested folders
            // are replacements; their authenticated prior objects are retained
            // by the existing restore transaction before publication.
            const auto secured = path.parent_path()==state.root
                ? policy.apply(path,win32::StorageDaclObjectKind::directory)
                : policy.apply_for_publication(path,win32::StorageDaclObjectKind::directory,
                    prior_restore_root/path.lexically_relative(state.root));
            return secured ? archive::Result{} : archive::Result{
                archive::Error::publication_failed, secured.operation};
        };
        restore.secure_live_file = [&](const std::filesystem::path& path) {
            win32::WindowsStorageDaclPolicy policy(state.owner);
            const auto secured = policy.apply_for_publication(path,win32::StorageDaclObjectKind::file,
                prior_restore_root/path.lexically_relative(state.root));
            return secured ? archive::Result{} : archive::Result{
                archive::Error::publication_failed, secured.operation};
        };
        restore.verify_live_publication=[&]{
            if(!guard.acquired())return archive::Result{archive::Error::publication_failed,"restore exclusive ownership lost"};
            const auto serial=ftn_serial->publish();if(!serial)return archive::Result{archive::Error::publication_failed,serial.operation};ftn_recovery_required=ftn_serial->recovery_required();
            const auto guarded=win32::invalidate_restored_message_verification(state.root,state.owner);
            if(!guarded)return archive::Result{archive::Error::publication_failed,guarded.operation};
            // Restored files have new native IDs. Rebind only bytes proved by
            // the restored authenticated map; mismatches stay held for review.
            if(win32::load_portable_board_key(state.root)){
                const auto identities=win32::prepare_copied_file_bindings(state.root,state.owner);
                file_recovery_attention=!identities.published||identities.deferred;
            }else file_recovery_attention=true;
            return archive::Result{};
        };
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        restored = archive::restore_backup(restore);
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        if (!restored.result.ok()) {
            set_status(state, L"Failed — restore did not complete: " +
                utf8_to_wide(restored.result.detail) +
                (restored.rollback_attempted
                    ? (restored.rollback_succeeded ? L"; prior live state restored."
                                                   : L"; recovery review is required.")
                    : L"; live state was not changed."));
            return;
        }
    }
    set_status(state, L"Completed — verified restore published; prior state retained at " +
        restored.retained_previous_state.native()+(ftn_recovery_required?L". FTN sending paused: message ID history needs recovery.":L"")+
        (file_recovery_attention?L". Some copy-recovery file records need review. Run BBS Health Check before using affected file or FTN work.":L""));
    refresh_backup_inventory(state);
    const auto action_status = text(state.status);
    if (load_editor_state(state)) {
        populate_controls(state);
        set_status(state, action_status);
    }
}

std::optional<std::uint8_t> byte_value(HWND control, const unsigned maximum) {
    const auto value = text(control);
    wchar_t* end = nullptr;
    const unsigned long parsed = wcstoul(value.c_str(), &end, 10);
    if (value.empty() || end == nullptr || *end != L'\0' || parsed > maximum)
        return std::nullopt;
    return static_cast<std::uint8_t>(parsed);
}

void save_scheduler(State& state) {
    if(state.selected_job){(void)save_message_schedule(state);return;}
    const auto hour = byte_value(state.schedule_hour, 23);
    const auto minute = byte_value(state.schedule_minute, 59);
    if (!hour || !minute || !state.scheduler.configuration_loaded) {
        set_status(state, L"Refused — enter a valid 24-hour local time.");
        return;
    }
    app::AlphaSchedulerSettingsRequest update;
    update.expected_revision = state.scheduler.configuration_revision;
    update.enabled = Button_GetCheck(state.schedule_enabled) == BST_CHECKED;
    update.local_hour = *hour;
    update.local_minute = *minute;
    const auto destination_text = text(state.backup_destination);
    const auto default_destination = (state.root / L"Backup" / L"Scheduled").native();
    const auto encoded_destination = destination_text.empty() ||
        _wcsicmp(destination_text.c_str(), default_destination.c_str()) == 0
        ? std::optional<std::string>(std::string{}) : wide_to_utf8(destination_text);
    if (!encoded_destination || !app::valid_backup_destination_text(*encoded_destination)) {
        set_status(state, L"Refused — choose an absolute local backup folder, or leave it blank for the default.");
        return;
    }
    update.backup_destination = *encoded_destination;
    if (state.mode == State::ConnectionMode::online) {
        const auto outgoing = request(state,
            app::LocalControlMessageType::scheduler_settings_request,
            app::encode_scheduler_settings_request(update));
        app::LocalControlFrame response;
        if (outgoing.payload.empty() || !online_exchange(state, outgoing,
                app::LocalControlMessageType::scheduler_settings_response, response)) {
            set_status(state, L"Refused — the authenticated Runtime scheduler channel failed.");
            return;
        }
        const auto result = app::decode_control_response(response.payload);
        if (!result || result->code != app::LocalControlResponseCode::ok) {
            set_status(state, L"Refused — " + utf8_to_wide(result
                ? result->diagnostic_code : "invalid Runtime response"));
            return;
        }
        set_status(state, L"Applied — future backups use the saved folder and schedule. Any backup already started keeps its original folder; existing backups are not moved.");
    } else if (state.mode == State::ConnectionMode::offline) {
        win32::OfflineInstallationGuard guard;
        const auto acquired = guard.acquire(state.root, state.owner);
        if (!acquired) {
            set_status(state, L"Refused — Runtime started or another offline transaction owns this installation.");
            return;
        }
        const auto current = win32::read_scheduler_configuration_file(state.root, state.owner);
        const auto current_state = win32::read_scheduler_state_file(state.root, state.owner);
        if (!current.value || !current_state.value ||
            current.value->revision != update.expected_revision ||
            current.value->revision == std::numeric_limits<std::uint64_t>::max()) {
            set_status(state, L"Refused — scheduler configuration changed; reload before saving.");
            return;
        }
        auto replacement = *current.value;
        replacement.format_version = 2;
        replacement.backup_destination = *encoded_destination;
        const auto retention_text = text(state.backup_retention);
        wchar_t* retention_end{};
        const auto retention = wcstoul(retention_text.c_str(), &retention_end, 10);
        if (retention_text.empty() || !retention_end || *retention_end || retention < 1 || retention > 365 ||
            !app::valid_backup_destination_text(replacement.backup_destination)) {
            set_status(state, L"Refused — use an absolute local folder (or blank for default) and retain 1–365 archives."); return;
        }
        replacement.retained_backups = static_cast<std::uint32_t>(retention);
        // Validation holds the selected destination against redirection; never
        // repairs an existing folder's privacy or silently selects a fallback.
        std::optional<win32::BackupCollection> destination;
        try {
            destination = win32::open_backup_collection(state.root, state.owner,
                archive::BackupKind::manual, replacement.backup_destination);
        } catch (const std::exception& error) {
            set_status(state, L"Refused — backup folder was not changed: " + utf8_to_wide(error.what()));
            return;
        }
        ++replacement.revision;
        replacement.enabled = update.enabled;
        replacement.local_hour = update.local_hour;
        replacement.local_minute = update.local_minute;
        const auto zone = state.configuration.saved.bbs_time_zone;
        app::RuntimeScheduler scheduler(*current.value, *current_state.value,
            [zone](const std::int64_t after, const std::uint8_t h, const std::uint8_t m) {
                return win32::next_bbs_daily_run(zone, after, h, m);
            });
        if (!scheduler.reconfigure(replacement, now_utc())) {
            set_status(state, L"Refused — the next eligible run could not be calculated.");
            return;
        }
        const auto config_saved = win32::publish_scheduler_configuration_file(
            state.root, state.owner, replacement);
        const auto state_saved = config_saved ? win32::publish_scheduler_state_file(
            state.root, state.owner, scheduler.state()) : config_saved;
        if (!config_saved || !state_saved) {
            if (config_saved) {
                const auto rolled_back = win32::publish_scheduler_configuration_file(
                    state.root, state.owner, *current.value);
                set_status(state, rolled_back
                    ? L"Refused — the scheduler state could not be published; the prior schedule was restored."
                    : L"Refused — scheduler publication and rollback both failed. Close this window and inspect the installation before restarting Runtime.");
            } else {
                set_status(state, L"Refused — the scheduler configuration could not be published.");
            }
            return;
        }
        set_status(state, L"Saved — applies on next Runtime start.");
    } else {
        set_status(state, L"Refused — the installation owner is not available.");
        return;
    }
    const auto action_status = text(state.status);
    if (load_editor_state(state)) {
        populate_controls(state);
        set_status(state, action_status);
    }
}

#ifdef SFX_ALPHA_TEST_AUTOMATION
// Native-control fixture seams only; absent from the hook-free owner binaries.
app::LocalAccountSnapshot (*account_query_fixture)(const app::LocalAccountQuery&)=nullptr;
app::LocalAccountResult (*account_admin_fixture)(app::LocalAccountRequest&)=nullptr;
#endif
app::LocalAccountSnapshot query_accounts(State& state, const app::LocalAccountQuery& query) {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(account_query_fixture) return account_query_fixture(query);
#endif
    if (state.system_recovery_only) {
        if (query.account_id || !query.name_prefix.empty() || query.offset)
            return {app::LocalAccountResultCode::unauthorized};
        return win32::elevated_system_password_snapshot(state.root);
    }
    if (!load_editor_state(state)) return {};
    if (state.mode == State::ConnectionMode::offline && !state.navigation_rendering)
        return win32::offline_account_snapshot(state.root, state.owner, query);
    if (state.mode != State::ConnectionMode::online && !state.navigation_rendering) return {};
    app::LocalControlFrame response;
    const auto outgoing = request(state, app::LocalControlMessageType::account_query_request,
        app::encode_account_query(query));
    if (!online_exchange(state, outgoing, app::LocalControlMessageType::account_query_response, response)) return {};
    return app::decode_account_snapshot(response.payload).value_or(app::LocalAccountSnapshot{});
}

app::LocalAccountResult administer_account(State& state, app::LocalAccountRequest& command) {
    struct Erase { app::LocalAccountRequest& value;
        ~Erase() { app::erase_local_account_secrets(value); } } erase{command};
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if(account_admin_fixture) return account_admin_fixture(command);
#endif
    if (state.system_recovery_only)
        return win32::elevated_replace_system_password(state.root,command);
    if (!load_editor_state(state)) return {};
    if (state.mode == State::ConnectionMode::offline)
        return win32::offline_administer_account(state.root, state.owner, command);
    if (state.mode != State::ConnectionMode::online) return {};
    auto outgoing = request(state, app::LocalControlMessageType::account_command_request,
        app::encode_account_request(command));
    struct ErasePayload { std::vector<std::uint8_t>& value;
        ~ErasePayload() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
    } erase_payload{outgoing.payload};
    app::LocalControlFrame response;
    if (outgoing.payload.empty() || !online_exchange(state, outgoing,
        app::LocalControlMessageType::account_command_response, response)) return {};
    return app::decode_account_result(response.payload).value_or(app::LocalAccountResult{});
}

const wchar_t* account_result_text(const app::LocalAccountResult& result) {
    if (result.code == app::LocalAccountResultCode::ok) return L"Completed — saved and audited.";
    if (result.committed) return L"Partially completed — saved state changed, but the action did not finish. Reload and review before retrying. A failed deletion may have left the user Inactive; history is retained.";
    switch (result.code) {
        case app::LocalAccountResultCode::unauthorized: return L"Refused — this Windows identity is not authorized.";
        case app::LocalAccountResultCode::invalid_request: return L"Refused — invalid fields, confirmation, or revision. No change made.";
        case app::LocalAccountResultCode::last_sysop_protected: return L"SFX must have at least one active Sysop account with a usable password. This is the last usable Sysop account, so this change was not saved. Give another active user Sysop access before changing, deactivating or deleting this account. Your edits have been kept.";
        case app::LocalAccountResultCode::system_sysop_protected: return L"This caller is the designated System Sysop. Assign another eligible active Sysop under Board Identity before locking, deleting, demoting this caller or changing security policy to exclude them. Nothing was saved; your edits have been kept.";
        case app::LocalAccountResultCode::conflict: return L"Refused — the account/policy changed, the name is unavailable, or this would remove the last usable Sysop. Your draft remains; reload and review.";
        case app::LocalAccountResultCode::policy_rejected: return L"Refused — the entered password does not meet the active policy, or its required blocklist is unavailable.";
        case app::LocalAccountResultCode::revocation_failed: return L"Refused — required session cleanup failed. Review Runtime before retrying.";
        default: return L"Administration unavailable or response lost. Reload and verify the result before retrying; an uncertain change is never repeated automatically.";
    }
}

// Never return a temporary wide/UTF-8 password without wiping that owned buffer.
bool consume_password(HWND edit, std::string& output) {
    auto wide = text(edit);
    // Keep the masked draft on validation/revision failure. A committed action
    // refreshes/clears controls; temporary owned encodings are always erased.
    auto encoded = wide_to_utf8(wide);
    if (!wide.empty()) SecureZeroMemory(wide.data(), wide.size() * sizeof(wchar_t));
    if (!encoded) return false;
    output = *encoded;
    if (!encoded->empty()) SecureZeroMemory(encoded->data(), encoded->size());
    return !output.empty() && output.size() <= app::kPasswordMaximum;
}

bool password_fields_valid(State& state, const app::LocalAccountRequest& command, std::string_view account_name) {
    if(command.password.empty()||command.password!=command.confirmation)return false;
    const auto& settings=state.configuration.active;
    const app::PasswordPolicyContext context{settings.password_mode,std::string(account_name),settings.bbs_name};
    if(context.mode==app::PasswordMode::standard)
        return app::evaluate_password_policy(command.password,context,nullptr)==app::PasswordPolicyCode::accepted;
    const sfx::adapters::authentication::PinnedSecListsBlocklist blocklist;
    return app::evaluate_password_policy(command.password,context,&blocklist)==app::PasswordPolicyCode::accepted;
}


void populate_user_editor(State& state);
void select_account(State& state) {
    if (state.populating_accounts) return;
    state.account_selection = {};
    const int index = ListView_GetNextItem(state.account_list, -1, LVNI_SELECTED);
    if (index >= 0 && static_cast<std::size_t>(index) < state.account_page.accounts.size())
        state.account_selection = query_accounts(state, {state.account_page.accounts[index].account_id,"",0});
    if (state.account_selection.code != app::LocalAccountResultCode::ok || state.account_selection.accounts.size() != 1) {
        SetWindowTextW(state.account_summary, L"Select an account. Refresh if details are unavailable.");
    } else {
        const auto& account = state.account_selection.accounts.front();
        const auto summary = L"Account " + std::to_wstring(account.account_id) + L" — " +
            utf8_to_wide(account.display_name.value_or("(deleted)")) +
            L"\r\nState: " + utf8_to_wide(std::string(app::storage_code(account.state))) +
            L"; level " + std::to_wstring(account.security_level.value_or(0)) +
            L"; revision " + std::to_wstring(account.revision) +
            L"\r\nPurge Allowed: " + (account.purge_allowed.value_or(false) ? L"Yes" : L"No");
        SetWindowTextW(state.account_summary, summary.c_str());
    }
    state.creating_user=false;
    populate_user_editor(state);
}

void refresh_accounts(State& state, bool reset_offset) {
    if (reset_offset) {
        state.account_query = {};
        state.account_query.name_prefix = wide_to_utf8(text(state.account_filter)).value_or("");
        const auto& filter=state.account_query.name_prefix;
        if(!filter.empty() && std::all_of(filter.begin(),filter.end(),[](char c){return c>='0' && c<='9';})) {
            try {state.account_query.account_id=std::stoll(filter);state.account_query.name_prefix.clear();}
            catch(...) {set_status(state,L"Refused — user number is out of range.");return;}
        }
    }
    state.account_selection = {};
    state.account_page = query_accounts(state, state.account_query);
    // Numeric searches use the history-capable detail endpoint. Hide deleted
    // accounts here too, without deleting their retained records.
    auto& visible_accounts=state.account_page.accounts;
    visible_accounts.erase(std::remove_if(visible_accounts.begin(),visible_accounts.end(),
        [](const auto& a){return a.state==app::AccountState::tombstoned;}),visible_accounts.end());
    state.populating_accounts = true;
    ListView_DeleteAllItems(state.account_list);
    if (state.account_page.code == app::LocalAccountResultCode::ok) {
        for (std::size_t i=0;i<state.account_page.accounts.size();++i) {
            auto name = utf8_to_wide(state.account_page.accounts[i].display_name.value_or("(deleted)"));
            LVITEMW item{}; item.mask = LVIF_TEXT; item.iItem = static_cast<int>(i); item.pszText = name.data();
            ListView_InsertItem(state.account_list, &item);
        }
    } else set_status(state, L"Unavailable — account list could not load. No account changes have been requested.");
    state.populating_accounts = false;
    EnableWindow(state.account_previous, state.account_query.offset > 0);
    EnableWindow(state.account_next, state.account_page.code == app::LocalAccountResultCode::ok && state.account_page.has_more);
    if(!state.account_page.accounts.empty()) {
        state.populating_accounts=true;
        ListView_SetItemState(state.account_list,0,LVIS_SELECTED|LVIS_FOCUSED,LVIS_SELECTED|LVIS_FOCUSED);
        state.populating_accounts=false;
    }
    select_account(state);
}


void refresh_security(State& state) {
    SetWindowTextW(state.system_password, L""); SetWindowTextW(state.system_confirm, L"");
    if (!state.system_recovery_only && load_editor_state(state)) populate_controls(state);
    if (state.system_recovery_only) {
        EnableWindow(state.strong_passwords,FALSE);
        EnableWindow(state.password_policy_save,FALSE);
    }
    const auto snapshot = query_accounts(state, {});
    state.system_password_revision = snapshot.code == app::LocalAccountResultCode::ok ? snapshot.system_revision : 0;
    EnableWindow(state.system_password_apply, state.system_password_revision > 0);
    const auto detail = state.system_password_revision > 0
        ? L"Permanent System Password revision: " + std::to_wstring(state.system_password_revision) +
          L"\r\nNew-user security level: " + std::to_wstring(snapshot.policy.new_user_level) +
          L"; Sysop threshold: " + std::to_wstring(snapshot.policy.sysop_threshold) +
          L"\r\nPolicy revision: " + std::to_wstring(snapshot.policy.revision)
        : L"Security administration unavailable. Close Runtime normally to use the exclusive offline path.";
    const auto mode = [](app::PasswordMode value) { return value == app::PasswordMode::strong ? L"Strong" : L"Standard"; };
    const auto policy_detail = state.system_recovery_only ? std::wstring{} : L"\r\n" +
        active_saved_line(state,L"Password policy",mode(state.configuration.active.password_mode),mode(state.configuration.saved.password_mode)) +
        L"\r\n" + activation_text(state.configuration);
    SetWindowTextW(state.security_detail, detail.c_str());
    SetWindowTextW(state.policy_detail,policy_detail.c_str());
}

void replace_system_password(State& state) {
    if (state.system_password_revision < 1) return;
    app::LocalAccountRequest command;
    struct Erase { app::LocalAccountRequest& value; ~Erase(){ app::erase_local_account_secrets(value); } } erase{command};
    command.action = app::LocalAccountAction::replace_system_password;
    command.expected_revision = state.system_password_revision;
    const bool first = consume_password(state.system_password, command.password);
    const bool second = consume_password(state.system_confirm, command.confirmation);
    if (!first || !second || !password_fields_valid(state,command,{})) {
        set_status(state,L"Refused — enter and confirm the same policy-compliant permanent System Password."); return;
    }
    if (MessageBoxW(state.window, L"Replace the permanent SY: System Password?\n\n"
        L"Active Sysop-level sessions will disconnect. Ordinary caller sessions remain connected.",
        L"Replace permanent System Password", MB_YESNO | MB_DEFBUTTON2 | MB_ICONWARNING) != IDYES) return;
    const auto result = administer_account(state, command);
    if(result.committed || result.code==app::LocalAccountResultCode::ok) refresh_security(state);
    set_status(state, account_result_text(result));
}

void select_level(State& state);
void refresh_levels(State& state) {
    const auto selected_number=port_value(state.level_number).value_or(0);
    state.security_catalog = {};
    state.selected_level_index=-1;
    SendMessageW(state.level_list,LB_RESETCONTENT,0,0);
    if(!state.system_recovery_only && load_editor_state(state)) {
        if(state.mode==State::ConnectionMode::offline&&!state.navigation_rendering) state.security_catalog=win32::offline_security_snapshot(state.root,state.owner);
        else if(state.mode==State::ConnectionMode::online||state.navigation_rendering) {
            for(std::uint32_t offset=0;offset<999;offset+=32) {
                app::LocalControlFrame response;
                if(!online_exchange(state,request(state,app::LocalControlMessageType::security_query_request,
                    app::encode_account_query({0,"",offset})),app::LocalControlMessageType::security_query_response,response)) {state.security_catalog={};break;}
                const auto part=app::decode_security_snapshot(response.payload);
                if(!part || part->code!=app::LocalAccountResultCode::ok ||
                    (offset && (part->policy.revision!=state.security_catalog.policy.revision || part->policy.policy_hash!=state.security_catalog.policy.policy_hash))) {state.security_catalog={};break;}
                if(!offset) state.security_catalog=*part;
                else state.security_catalog.levels.insert(state.security_catalog.levels.end(),part->levels.begin(),part->levels.end());
                if(!part->has_more) {state.security_catalog.has_more=false;break;}
            }
        }
    }
    const bool available = state.security_catalog.code == app::LocalAccountResultCode::ok;
    for (const auto field : {state.level_number,state.level_label,state.level_description,state.level_replacement,state.level_call_minutes,state.level_day_minutes,state.level_chat_minutes,state.level_chat_day_minutes,
            state.level_new_user,state.level_threshold,state.level_save,state.level_retire,state.level_defaults})
        EnableWindow(field,available);
    if (!available) {
        SetWindowTextW(state.level_detail,L"Security Levels unavailable — the authorized Runtime/offline connection or a consistent policy snapshot could not be read. Reload to retry.");
        return;
    }
    for (const auto& level : state.security_catalog.levels) {
        const auto caption = std::to_wstring(level.level) + L"  " + utf8_to_wide(level.label) +
            (level.state == app::SecurityLevelState::retired ? L" (removed)" : L"");
        SendMessageW(state.level_list,LB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption.c_str()));
    }
    SetWindowTextW(state.level_new_user,std::to_wstring(state.security_catalog.policy.new_user_level).c_str());
    SetWindowTextW(state.level_threshold,std::to_wstring(state.security_catalog.policy.sysop_threshold).c_str());
    SetWindowTextW(state.level_detail,(L"Saved policy revision " + std::to_wstring(state.security_catalog.policy.revision) +
        (state.mode==State::ConnectionMode::online ? L". Saved access changes apply to online callers." : L". Runtime is stopped; saved policy is used at startup.")+
        std::wstring(L" Shared definitions/defaults are separate from a user's assigned level.")).c_str());
    int selected=0;
    for(std::size_t i=0;i<state.security_catalog.levels.size();++i) if(state.security_catalog.levels[i].level==selected_number) selected=static_cast<int>(i);
    SendMessageW(state.level_list,LB_SETCURSEL,selected,0);select_level(state);
    refresh_message_limit(state);
}

void select_level(State& state) {
    const auto selected = SendMessageW(state.level_list,LB_GETCURSEL,0,0);
    if (selected < 0 || static_cast<std::size_t>(selected) >= state.security_catalog.levels.size()) return;
    const auto& level = state.security_catalog.levels[static_cast<std::size_t>(selected)];
    state.selected_level_index=static_cast<int>(selected);
    SetWindowTextW(state.level_number,std::to_wstring(level.level).c_str());
    SetWindowTextW(state.level_label,utf8_to_wide(level.label).c_str());
    SetWindowTextW(state.level_description,utf8_to_wide(level.description.value_or("")).c_str());
    SetWindowTextW(state.level_call_minutes,std::to_wstring(level.minutes_per_call.value_or(0)).c_str());
    SetWindowTextW(state.level_day_minutes,std::to_wstring(level.minutes_per_day.value_or(0)).c_str());
    SetWindowTextW(state.level_chat_minutes,std::to_wstring(level.private_chat_minutes.value_or(0)).c_str());
    SetWindowTextW(state.level_chat_day_minutes,std::to_wstring(level.private_chat_minutes_per_day.value_or(0)).c_str());
    for(HWND field:{state.level_chat_minutes,state.level_chat_day_minutes})EnableWindow(field,level.state==app::SecurityLevelState::active);
    for(HWND field:{state.level_call_minutes,state.level_day_minutes}) EnableWindow(field,level.state==app::SecurityLevelState::active && level.level!=999);
    for(HWND field:{state.level_number,state.level_label,state.level_description,state.level_retire})
        EnableWindow(field,level.state==app::SecurityLevelState::active);
    select_message_limit(state);
}

#include "level_removal.inl"
void apply_level_change(State& state, const app::LocalSecurityAction action) {
    if (state.security_catalog.code != app::LocalAccountResultCode::ok || state.system_recovery_only) return;
    app::LocalSecurityChange change;
    change.expected_revision = state.security_catalog.policy.revision;
    change.action = action;
    change.new_user_level = port_value(state.level_new_user).value_or(0);
    change.sysop_threshold = port_value(state.level_threshold).value_or(0);
    if (action != app::LocalSecurityAction::set_defaults) change.level = port_value(state.level_number).value_or(0);
    if (action == app::LocalSecurityAction::upsert_level) {
        change.label = wide_to_utf8(text(state.level_label)).value_or("");
        change.description = wide_to_utf8(text(state.level_description));
        if (change.description && change.description->empty()) change.description.reset();
        const auto read_minutes=[](HWND field,std::optional<std::uint32_t>& result) {
            const auto input=text(field);std::uint64_t amount{};
            if(input.empty() || input.size()>10) return false;
            for(const wchar_t ch:input) {if(ch<L'0' || ch>L'9') return false;amount=amount*10+static_cast<unsigned>(ch-L'0');}
            if(amount>UINT32_MAX) return false;
            if(amount) result=static_cast<std::uint32_t>(amount);else result.reset();return true;
        };
        if(!read_minutes(state.level_call_minutes,change.minutes_per_call) || !read_minutes(state.level_day_minutes,change.minutes_per_day)||!read_minutes(state.level_chat_minutes,change.private_chat_minutes)||!read_minutes(state.level_chat_day_minutes,change.private_chat_minutes_per_day)) {
            set_status(state,L"Refused — enter whole minutes, or 0 for unlimited.");return;
        }
    }
    if (action == app::LocalSecurityAction::retire_level && !review_level_removal(state,change)) return;
    if (!app::prepare_local_security_change(state.security_catalog,change)) {
        set_status(state,L"Refused — use active levels 1–999 for defaults/reassignment and a valid label. Removed levels cannot return."); return;
    }
    if (action != app::LocalSecurityAction::retire_level) {
        const auto summary = std::wstring(L"Save the reviewed security catalog/settings?\n\nNew User level: ") +
            std::to_wstring(change.new_user_level) + L"\nSysop threshold: " + std::to_wstring(change.sysop_threshold) +
            L"\nEvery caller at or above that threshold has Sysop authority. Only level 999 has unlimited calling time, no AFK timeout and simultaneous sessions. Reduced access applies to online callers; new Sysop access requires a fresh login and SY: authentication."+
            (action==app::LocalSecurityAction::upsert_level?L"\nMinutes Per Call: "+std::to_wstring(change.minutes_per_call.value_or(0))+L"\nMinutes Per Day: "+std::to_wstring(change.minutes_per_day.value_or(0))+L"\nPrivate Chat Minutes: "+std::to_wstring(change.private_chat_minutes.value_or(0))+L"\nPrivate Chat Minutes Per Day: "+std::to_wstring(change.private_chat_minutes_per_day.value_or(0))+L" (0 = unlimited)":std::wstring{});
        if (MessageBoxW(state.window,summary.c_str(),L"Review Security Policy",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING) != IDYES) return;
    }
    app::LocalAccountResult result;
    if(state.mode==State::ConnectionMode::online) {
        app::LocalControlFrame response;
        if(online_exchange(state,request(state,app::LocalControlMessageType::security_change_request,
            app::encode_security_change(change)),app::LocalControlMessageType::security_change_response,response))
            result=app::decode_account_result(response.payload).value_or(app::LocalAccountResult{});
    } else if(state.mode==State::ConnectionMode::offline) result=win32::offline_change_security_policy(state.root,state.owner,change);
    if (result.committed && result.code != app::LocalAccountResultCode::ok) {
        state.security_catalog.code = app::LocalAccountResultCode::unavailable;
        for (const auto button : {state.level_save,state.level_retire,state.level_defaults}) EnableWindow(button,FALSE);
        set_status(state,L"Failed — database change committed and audited, but snapshot publication needs recovery. Do not repeat the change. Reopen Config; Runtime refuses an unexplained mismatch."); return;
    }
    if(result.code==app::LocalAccountResultCode::ok) refresh_levels(state);
    set_status(state,result.code == app::LocalAccountResultCode::ok
        ? L"Saved — policy audited; running callers use the new access rules."
        : L"Refused — a newer edit, invalid replacement, last-Sysop protection or unavailable storage prevented saving. Reload and review.");
}

LRESULT CALLBACK focus_scroll_proc(HWND window,UINT message,WPARAM wparam,LPARAM lparam,UINT_PTR id,DWORD_PTR) {
    if(message==WM_SETFOCUS) PostMessageW(GetParent(window),WM_APP+17,reinterpret_cast<WPARAM>(window),0);
    if(message==WM_NCDESTROY) RemoveWindowSubclass(window,focus_scroll_proc,id);
    return DefSubclassProc(window,message,wparam,lparam);
}

HWND control(State& state, HWND parent, const wchar_t* class_name,
    const wchar_t* caption, const DWORD style, const int x, const int y,
    const int width, const int height, const int id = 0,
    const DWORD extended_style = 0) {
    const auto scaled = [&state](const int value) { return MulDiv(value, state.dpi, 96); };
    const HWND result = CreateWindowExW(extended_style, class_name, caption,
        WS_CHILD | WS_VISIBLE | style |
            (_wcsicmp(class_name, L"COMBOBOX") == 0 ? CBS_OWNERDRAWFIXED | CBS_HASSTRINGS : 0) |
            (_wcsicmp(class_name, L"LISTBOX") == 0 ? LBS_OWNERDRAWFIXED | LBS_HASSTRINGS : 0),
        scaled(x), scaled(y), scaled(width), scaled(height), parent,
        id == 0 ? nullptr : reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        state.instance, nullptr);
    if (result != nullptr && state.ui_font != nullptr)
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(state.ui_font), TRUE);
    if (result && _wcsicmp(class_name, L"COMBOBOX") == 0) {
        SendMessageW(result, CB_SETITEMHEIGHT, 0, scaled(19));
        SendMessageW(result, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), scaled(19));
    }
    if (result && _wcsicmp(class_name, L"LISTBOX") == 0)
        SendMessageW(result, LB_SETITEMHEIGHT, 0, scaled(19));
    if(result && (style&WS_TABSTOP) && GetParent(parent)==state.window)
        SetWindowSubclass(result,focus_scroll_proc,3,0);
    return result;
}

HWND label(State& state, HWND parent, const wchar_t* caption,
    const int x, const int y, const int width, const int height) {
    return control(state, parent, L"STATIC", caption, SS_LEFT | SS_NOPREFIX,
        x, y, width, height);
}

void move_page_children(HWND window,int delta) {
    // Move clipped children explicitly, without copying pixels from their old
    // positions. Repainting children one at a time can preserve pieces of a
    // sibling that has not moved yet (especially when scrolling back upward).
    if(!delta) return;
    for(HWND child=GetWindow(window,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
        RECT bounds{};GetWindowRect(child,&bounds);
        MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&bounds),2);
        SetWindowPos(child,nullptr,bounds.left,bounds.top+delta,0,0,
            SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW);
    }
    // The page clips its children: invalidating the background alone cannot
    // repair stale client/nonclient pixels inside the moved controls.
    RedrawWindow(window,nullptr,nullptr,
        RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
}
LRESULT CALLBACK page_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR id, DWORD_PTR data) {
    if(message==WM_ERASEBKGND) {
        auto* state=reinterpret_cast<State*>(data);RECT area{};GetClientRect(window,&area);
        FillRect(reinterpret_cast<HDC>(wparam),&area,state->appearance.background_brush());return 1;
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(window, page_proc, id);
    if(message==WM_APP+17 && GetParent(reinterpret_cast<HWND>(wparam))==window) {
        RECT r{},viewport{};GetWindowRect(reinterpret_cast<HWND>(wparam),&r);
        MapWindowPoints(nullptr,window,reinterpret_cast<POINT*>(&r),2); GetClientRect(window,&viewport);
        const int delta=r.top<0 ? r.top : r.bottom>viewport.bottom ? r.bottom-viewport.bottom : 0;
        const int before=GetScrollPos(window,SB_VERT);
        SCROLLINFO info{sizeof(info),SIF_POS};info.nPos=before+delta;SetScrollInfo(window,SB_VERT,&info,TRUE);
        move_page_children(window,before-GetScrollPos(window,SB_VERT));
        return 0;
    }
    // Native controls notify their immediate page parent, not the main window.
    if(message==WM_VSCROLL || message==WM_MOUSEWHEEL) {
        SCROLLINFO info{sizeof(info),SIF_ALL}; GetScrollInfo(window,SB_VERT,&info);
        int next=info.nPos;
        if(message==WM_MOUSEWHEEL) next-=GET_WHEEL_DELTA_WPARAM(wparam)/WHEEL_DELTA*60;
        else switch(LOWORD(wparam)) {
            case SB_LINEUP: next-=24; break; case SB_LINEDOWN: next+=24; break;
            case SB_PAGEUP: next-=info.nPage; break; case SB_PAGEDOWN: next+=info.nPage; break;
            case SB_THUMBTRACK: next=info.nTrackPos; break;
            case SB_THUMBPOSITION: next=info.nTrackPos; break;
            case SB_TOP: next=0; break; case SB_BOTTOM: next=info.nMax; break;
        }
        info.fMask=SIF_POS; info.nPos=next; const int old=GetScrollPos(window,SB_VERT);
        SetScrollInfo(window,SB_VERT,&info,TRUE);
        move_page_children(window,old-GetScrollPos(window,SB_VERT));
        return 0;
    }
    if (message == WM_COMMAND || message == WM_NOTIFY)
        return SendMessageW(GetParent(window), message, wparam, lparam);
    return DefSubclassProc(window, message, wparam, lparam);
}

HWND page(State& state) {
    // STATIC's hit testing bypasses native nonclient scrollbar handling even
    // with SS_NOTIFY. Use an ordinary window container so arrows/track/thumb
    // receive the real mouse messages handled by DefWindowProc.
    constexpr auto class_name=L"SFXConfigScrollablePage";
    WNDCLASSW definition{};
    if(!GetClassInfoW(state.instance,class_name,&definition)) {
        definition.hInstance=state.instance;definition.lpfnWndProc=DefWindowProcW;
        definition.lpszClassName=class_name;definition.hCursor=LoadCursorW(nullptr,IDC_ARROW);
        if(!RegisterClassW(&definition)) return nullptr;
    }
    const auto result = control(state, state.window, class_name, L"",
        WS_CLIPCHILDREN | WS_VSCROLL, 0, 0, 100, 100, 0, WS_EX_CONTROLPARENT);
    if (result) SetWindowSubclass(result, page_proc, 2, reinterpret_cast<DWORD_PTR>(&state));
    return result;
}

void create_system_page(State& state) {
    auto parent = state.pages[0] = page(state);
    label(state,parent,L"Set the public identity of this BBS.",12,8,500,30);
    label(state,parent,L"BBS name:",12,52,150,22);
    state.bbs_name=control(state,parent,L"EDIT",L"",WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,
        170,49,342,25,IDC_BBS_NAME,WS_EX_CLIENTEDGE);
    SendMessageW(state.bbs_name,EM_SETLIMITTEXT,app::kBbsNameMaximum,0);
    state.system_save=control(state,parent,L"BUTTON",L"Save Changes",WS_TABSTOP|BS_PUSHBUTTON,
        12,100,190,30,IDC_SYSTEM_SAVE);
    state.system_detail=control(state,parent,L"EDIT",L"",WS_BORDER|ES_MULTILINE|ES_READONLY|WS_VSCROLL,
        12,150,500,100,0,WS_EX_CLIENTEDGE);

    parent=state.pages[13]=page(state);
    label(state,parent,L"Choose the time zone used for BBS timestamps and scheduled jobs.",12,8,500,44);
    label(state,parent,L"BBS time zone:",12,62,150,22);
    state.time_zone=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
        170,59,342,300,IDC_TIME_ZONE);
    for(const auto& zone:installed_time_zones())
        SendMessageW(state.time_zone,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(zone.c_str()));
    label(state,parent,L"Historical UTC values and recorded offsets are not rewritten.",12,103,500,44);
    control(state,parent,L"BUTTON",L"Save Changes",WS_TABSTOP|BS_PUSHBUTTON,12,157,190,30,IDC_REGIONAL_SAVE);
    state.regional_detail=control(state,parent,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,
        12,205,500,100,0,WS_EX_CLIENTEDGE);

    parent=state.pages[14]=page(state);
    label(state,parent,L"BBS Hosting",12,8,500,26);
    label(state,parent,L"Current mode:",12,44,125,22);
    state.run_current=control(state,parent,L"STATIC",L"Loading...",SS_LEFT,142,44,370,22,IDC_RUN_CURRENT);
    label(state,parent,L"Service status:",12,77,125,22);
    state.run_status=control(state,parent,L"STATIC",L"Loading...",SS_LEFT,142,77,180,22,IDC_RUN_STATUS);
    state.run_power=control(state,parent,L"BUTTON",L"Start BBS",WS_TABSTOP|BS_PUSHBUTTON|WS_DISABLED,344,71,168,30,IDC_RUN_POWER);
    state.run_note=control(state,parent,L"STATIC",L"",SS_LEFT,12,110,500,36);
    label(state,parent,L"Run BBS as:",12,156,125,22);
    state.run_mode=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,142,153,370,100,IDC_RUN_MODE);
    for(const auto* choice:{L"Desktop Mode",L"Windows Service (LocalSystem)"})SendMessageW(state.run_mode,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(choice));
    state.run_automatic=control(state,parent,L"BUTTON",L"Start BBS when Windows starts",WS_TABSTOP|BS_AUTOCHECKBOX,142,192,370,26,IDC_RUN_AUTOMATIC);
    control(state,parent,L"BUTTON",L"Apply Hosting Settings",WS_TABSTOP|BS_PUSHBUTTON,12,233,222,30,IDC_RUN_MODE_APPLY);
    control(state,parent,L"BUTTON",L"Reload Hosting Settings",WS_TABSTOP|BS_PUSHBUTTON,250,233,222,30,IDC_RUN_MODE_RELOAD);
    state.run_help=control(state,parent,L"STATIC",L"",SS_LEFT,12,278,500,50,IDC_RUN_HELP);
    state.run_mode_detail=control(state,parent,L"STATIC",L"",SS_LEFT,12,330,500,22,IDC_RUN_MODE_DETAIL);
    parent=state.pages[41]=page(state);
    label(state,parent,L"Caller Idle Limits",12,8,500,26);
    label(state,parent,L"Caller AFK (minutes, 2-1440):",12,45,265,22);
    state.caller_afk=control(state,parent,L"EDIT",L"5",WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,290,42,80,25,IDC_CALLER_AFK,WS_EX_CLIENTEDGE);
    SendMessageW(state.caller_afk,EM_SETLIMITTEXT,4,0);
    label(state,parent,L"Login idle (seconds, 30-600):",12,80,265,22);
    state.login_afk=control(state,parent,L"EDIT",L"60",WS_TABSTOP|WS_BORDER|ES_NUMBER|ES_AUTOHSCROLL,290,77,80,25,IDC_LOGIN_AFK,WS_EX_CLIENTEDGE);
    SendMessageW(state.login_afk,EM_SETLIMITTEXT,3,0);
    label(state,parent,L"New sessions use saved limits. Authenticated Sysops are exempt from caller AFK.",12,115,500,40);
    control(state,parent,L"BUTTON",L"Save Idle Settings",WS_TABSTOP|BS_PUSHBUTTON,12,167,210,30,IDC_RUNTIME_SAVE);
    control(state,parent,L"BUTTON",L"Retry Idle Settings",WS_TABSTOP|BS_PUSHBUTTON,240,167,210,30,IDC_RUNTIME_RETRY);
    state.runtime_detail=control(state,parent,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,12,214,500,90,0,WS_EX_CLIENTEDGE);

    parent=state.pages[15]=page(state);
    label(state,parent,L"Control whether callers may register a new account. Existing accounts remain eligible to log in.",12,8,500,44);
    state.allow_users=control(state,parent,L"BUTTON",L"Allow New Users",WS_TABSTOP|BS_AUTOCHECKBOX,
        12,67,250,25,IDC_ALLOW_USERS);
    control(state,parent,L"BUTTON",L"Open Security Levels...",WS_TABSTOP|BS_PUSHBUTTON,
        12,114,230,30,IDC_OPEN_LEVELS);
    label(state,parent,L"The New User level is maintained in Security Levels. When Save reports Applied, "
        L"new connections use this choice. Existing login prompts keep their captured choice.",12,158,500,55);
    control(state,parent,L"BUTTON",L"Save Changes",WS_TABSTOP|BS_PUSHBUTTON,12,230,190,30,IDC_REGISTRATION_SAVE);
    state.registration_detail=control(state,parent,L"EDIT",L"",ES_MULTILINE|ES_READONLY|WS_VSCROLL,
        12,280,500,120,0,WS_EX_CLIENTEDGE);
}

void create_nodes_page(State& state) {
    auto parent = state.pages[1] = page(state);
    label(state, parent, L"Nodes", 12, 8, 500, 28);
    label(state, parent, L"Configured logical nodes (1–128):", 12, 52, 240, 22);
    state.logical_nodes = control(state, parent, L"EDIT", L"",
        WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL,
        260, 49, 80, 25, IDC_LOGICAL_NODES, WS_EX_CLIENTEDGE);
    SendMessageW(state.logical_nodes, EM_SETLIMITTEXT, 3, 0);
    label(state, parent,
        L"Changing the worker/node-pool size is saved but requires an orderly BBS restart. "
        L"It never disconnects current callers automatically. Enable/Disable remains a live Control Panel operation.",
        12, 94, 650, 62);
    state.nodes_save = control(state, parent, L"BUTTON", L"Save Node Definition",
        WS_TABSTOP | BS_PUSHBUTTON, 260, 180, 190, 30, IDC_NODES_SAVE);
    state.nodes_detail = control(state, parent, L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        12, 242, 658, 100, 0, WS_EX_CLIENTEDGE);
}

void create_servers_page(State& state) {
    auto parent = state.pages[2] = page(state);
    label(state, parent, L"Listener endpoints", 12, 8, 500, 28);
    state.telnet_enabled = control(state, parent, L"BUTTON", L"Enable Telnet",
        WS_TABSTOP | BS_AUTOCHECKBOX, 12, 48, 150, 24, IDC_TELNET_ENABLED);
    label(state, parent, L"IPv4 bind:", 175, 51, 75, 22);
    state.telnet_bind = control(state, parent, L"EDIT", L"", WS_TABSTOP | WS_BORDER |
        ES_AUTOHSCROLL, 250, 48, 135, 25, IDC_TELNET_BIND, WS_EX_CLIENTEDGE);
    label(state, parent, L"Port:", 400, 51, 35, 22);
    state.telnet_port = control(state, parent, L"EDIT", L"", WS_TABSTOP | WS_BORDER |
        ES_NUMBER | ES_AUTOHSCROLL, 435, 48, 70, 25, IDC_TELNET_PORT, WS_EX_CLIENTEDGE);
    state.ssh_enabled = control(state, parent, L"BUTTON", L"Enable SSH",
        WS_TABSTOP | BS_AUTOCHECKBOX, 12, 88, 150, 24, IDC_SSH_ENABLED);
    label(state, parent, L"IPv4 bind:", 175, 91, 75, 22);
    state.ssh_bind = control(state, parent, L"EDIT", L"", WS_TABSTOP | WS_BORDER |
        ES_AUTOHSCROLL, 250, 88, 135, 25, IDC_SSH_BIND, WS_EX_CLIENTEDGE);
    label(state, parent, L"Port:", 400, 91, 35, 22);
    state.ssh_port = control(state, parent, L"EDIT", L"", WS_TABSTOP | WS_BORDER |
        ES_NUMBER | ES_AUTOHSCROLL, 435, 88, 70, 25, IDC_SSH_PORT, WS_EX_CLIENTEDGE);
    state.hostname_lookup = control(state, parent, L"BUTTON",
        L"Hostname Lookup for Telnet and SSH", WS_TABSTOP | BS_AUTOCHECKBOX,
        12, 174, 310, 24, IDC_HOSTNAME_LOOKUP);
    label(state, parent,
        L"Listener changes are validated by Runtime. A failed bind is refused and the prior working endpoint remains authoritative.",
        12, 218, 650, 42);
    state.servers_save = control(state, parent, L"BUTTON", L"Save Server Settings",
        WS_TABSTOP | BS_PUSHBUTTON, 250, 280, 190, 30, IDC_SERVERS_SAVE);
    state.servers_detail = control(state, parent, L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        12, 340, 658, 132, 0, WS_EX_CLIENTEDGE);
}

void create_client_page(State& state) {
    auto parent = state.pages[3] = page(state);
    label(state, parent, L"External Client", 12, 8, 500, 28);
    label(state, parent, L"Client type:", 12, 52, 120, 22);
    state.client_type = control(state, parent, WC_COMBOBOXW, L"",
        WS_TABSTOP | CBS_DROPDOWNLIST, 140, 49, 230, 120, IDC_CLIENT_TYPE);
    SendMessageW(state.client_type, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"SyncTERM 1.10 series"));
    SendMessageW(state.client_type, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Custom command-line client"));
    label(state, parent, L"Executable:", 12, 92, 120, 22);
    state.client_path = control(state, parent, L"EDIT", L"", WS_TABSTOP | WS_BORDER |
        ES_AUTOHSCROLL, 140, 89, 270, 25, IDC_CLIENT_PATH, WS_EX_CLIENTEDGE);
    state.client_browse = control(state, parent, L"BUTTON", L"Browse...",
        WS_TABSTOP | BS_PUSHBUTTON, 420, 88, 95, 28, IDC_CLIENT_BROWSE);
    label(state, parent,
        L"Custom arguments — one token per line; use controlled {HOST} and {PORT} tokens:",
        12, 135, 650, 22);
    state.client_arguments = control(state, parent, L"EDIT", L"",
        WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        12, 160, 658, 115, IDC_CLIENT_ARGUMENTS, WS_EX_CLIENTEDGE);
    state.client_save = control(state, parent, L"BUTTON", L"Save Client",
        WS_TABSTOP | BS_PUSHBUTTON, 140, 300, 140, 30, IDC_CLIENT_SAVE);
    state.client_clear = control(state, parent, L"BUTTON", L"Clear / Reset",
        WS_TABSTOP | BS_PUSHBUTTON, 290, 300, 140, 30, IDC_CLIENT_CLEAR);
    label(state, parent,
        L"This profile is stored for the current Windows operator and applies to the next exact-node local launch.",
        12, 352, 650, 42);
}

void create_backup_page(State& state) {
    auto parent = state.pages[4] = page(state);
    label(state, parent, L"Backup & Restore", 12, 8, 500, 28);
    state.backup_list = control(state, parent, WC_LISTVIEWW, L"",
        WS_TABSTOP | WS_BORDER | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        12, 45, 660, 245, IDC_BACKUP_LIST, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(state.backup_list,
        LVS_EX_FULLROWSELECT | LVS_EX_LABELTIP | LVS_EX_DOUBLEBUFFER);
    LVCOLUMNW column{LVCF_TEXT | LVCF_WIDTH};
    column.cx = MulDiv(230, state.dpi, 96); column.pszText = const_cast<wchar_t*>(L"Archive");
    ListView_InsertColumn(state.backup_list, 0, &column);
    column.cx = MulDiv(105, state.dpi, 96); column.pszText = const_cast<wchar_t*>(L"Type");
    ListView_InsertColumn(state.backup_list, 1, &column);
    column.cx = MulDiv(135, state.dpi, 96); column.pszText = const_cast<wchar_t*>(L"Created");
    ListView_InsertColumn(state.backup_list, 2, &column);
    column.cx = MulDiv(85, state.dpi, 96); column.pszText = const_cast<wchar_t*>(L"Status");
    ListView_InsertColumn(state.backup_list, 3, &column);
    column.cx = MulDiv(105, state.dpi, 96); column.pszText = const_cast<wchar_t*>(L"Size");
    ListView_InsertColumn(state.backup_list, 4, &column);
    state.backup_run = control(state, parent, L"BUTTON", L"Run Backup Now",
        WS_TABSTOP | BS_PUSHBUTTON, 12, 305, 145, 30, IDC_BACKUP_RUN);
    state.backup_verify = control(state, parent, L"BUTTON", L"Verify",
        WS_TABSTOP | BS_PUSHBUTTON, 167, 305, 95, 30, IDC_BACKUP_VERIFY);
    state.backup_restore = control(state, parent, L"BUTTON", L"Restore Core...",
        WS_TABSTOP | BS_PUSHBUTTON, 272, 305, 105, 30, IDC_BACKUP_RESTORE);
    state.backup_refresh = control(state, parent, L"BUTTON", L"Refresh",
        WS_TABSTOP | BS_PUSHBUTTON, 387, 305, 95, 30, IDC_BACKUP_REFRESH);
    state.backup_contents=control(state,parent,L"BUTTON",L"Restore Contents...",WS_TABSTOP|BS_PUSHBUTTON,492,305,180,30,IDC_BACKUP_CONTENTS);
    state.ftn_history_recover=control(state,parent,L"BUTTON",L"Recover FTN History...",WS_TABSTOP|BS_PUSHBUTTON,12,342,210,30,IDC_FTN_HISTORY_RECOVER);
    state.backup_detail = control(state, parent, L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        12, 386, 660, 96, IDC_BACKUP_DETAIL, WS_EX_CLIENTEDGE);
}

void create_scheduler_page(State& state) {
    auto parent = state.pages[4];
    std::vector<HWND> backup_controls;
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT))
        backup_controls.push_back(child);
    label(state, parent, L"Scheduler", 12, 8, 210, 28);
    state.schedule_job=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,250,8,422,180,IDC_SCHEDULE_JOB);
    for(const auto* caption:{L"Backup",L"Message Retention",L"Message Integrity"})SendMessageW(state.schedule_job,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption));
    SendMessageW(state.schedule_job,CB_SETCURSEL,0,0);
    state.schedule_enabled = control(state, parent, L"BUTTON", L"Enable built-in Backup job",
        WS_TABSTOP | BS_AUTOCHECKBOX, 12, 50, 260, 24, IDC_SCHEDULE_ENABLED);
    state.schedule_weekday=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,350,48,190,200,IDC_SCHEDULE_WEEKDAY);
    for(const auto* caption:{L"Sunday",L"Monday",L"Tuesday",L"Wednesday",L"Thursday",L"Friday",L"Saturday"})SendMessageW(state.schedule_weekday,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(caption));
    label(state, parent, L"BBS local time (24-hour):", 12, 91, 235, 22);
    state.schedule_hour = control(state, parent, L"EDIT", L"",
        WS_TABSTOP | WS_BORDER | ES_NUMBER, 250, 88, 52, 25,
        IDC_SCHEDULE_HOUR, WS_EX_CLIENTEDGE);
    label(state, parent, L":", 307, 91, 8, 22);
    state.schedule_minute = control(state, parent, L"EDIT", L"",
        WS_TABSTOP | WS_BORDER | ES_NUMBER, 322, 88, 52, 25,
        IDC_SCHEDULE_MINUTE, WS_EX_CLIENTEDGE);
    state.schedule_save = control(state, parent, L"BUTTON", L"Save Schedule",
        WS_TABSTOP | BS_PUSHBUTTON, 390, 86, 150, 30, IDC_SCHEDULE_SAVE);
    state.backup_job_controls.push_back(label(state, parent, L"Manual/scheduled destination (blank = Backup\\Scheduled):", 12, 134, 650, 22));
    state.backup_destination = control(state, parent, L"EDIT", L"",
        WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, 12, 162, 545, 25,
        IDC_BACKUP_DESTINATION, WS_EX_CLIENTEDGE);
    state.backup_browse = control(state, parent, L"BUTTON", L"Browse...",
        WS_TABSTOP | BS_PUSHBUTTON, 570, 159, 102, 30, IDC_BACKUP_BROWSE);
    state.backup_job_controls.push_back(label(state, parent, L"Verified managed archives retained (1–365):", 12, 205, 355, 22));
    state.backup_retention = control(state, parent, L"EDIT", L"",
        WS_TABSTOP | WS_BORDER | ES_NUMBER, 380, 202, 70, 25,
        IDC_BACKUP_RETENTION, WS_EX_CLIENTEDGE);
    state.backup_job_controls.push_back(label(state, parent, L"Folder changes apply to future backups; existing backups are not moved. Stop the BBS only to edit retention.",
        12, 239, 650, 42));
    for(const auto h:{state.backup_destination,state.backup_browse,state.backup_retention})state.backup_job_controls.push_back(h);
    state.message_job_controls.push_back(label(state,parent,L"Unread private recipient inbox age, full days (1–65,535 or Unlimited):",12,134,660,22));
    state.unread_mail_age=control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,12,162,190,25,IDC_UNREAD_MAIL_AGE,WS_EX_CLIENTEDGE);
    SendMessageW(state.unread_mail_age,EM_SETLIMITTEXT,10,0);
    state.unread_mail_save=control(state,parent,L"BUTTON",L"Save Unread-Mail Age",WS_TABSTOP|BS_PUSHBUTTON,220,159,220,30,IDC_UNREAD_MAIL_SAVE);
    state.message_job_controls.push_back(label(state,parent,L"Shorter ages can expire older unread inbox copies on a later run. Save keeps current mail. Read mail and sender copies remain.",12,199,660,38));
    state.message_run_now=control(state,parent,L"BUTTON",L"Run Retention Now",WS_TABSTOP|BS_PUSHBUTTON,12,246,210,30,IDC_MESSAGE_RUN_NOW);
    state.message_compact=control(state,parent,L"BUTTON",L"Compact Stopped Database...",WS_TABSTOP|BS_PUSHBUTTON,240,246,290,30,IDC_MESSAGE_COMPACT);
    for(const auto h:{state.unread_mail_age,state.unread_mail_save,state.message_run_now,state.message_compact})state.message_job_controls.push_back(h);
    state.schedule_detail = control(state, parent, L"EDIT", L"",
        WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
        12, 291, 660, 260, IDC_SCHEDULE_DETAIL, WS_EX_CLIENTEDGE);
    label(state, parent,
        L"Missed runs are skipped after Runtime restart. Jobs never overlap, and failures do not retry automatically.",
        12, 568, 650, 42);
    for (HWND child = GetWindow(parent, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        if (std::find(backup_controls.begin(), backup_controls.end(), child) != backup_controls.end()) continue;
        RECT bounds{};
        GetWindowRect(child, &bounds);
        MapWindowPoints(HWND_DESKTOP, parent, reinterpret_cast<POINT*>(&bounds), 2);
        SetWindowPos(child, nullptr, bounds.left, bounds.top + MulDiv(505, state.dpi, 96), 0, 0,
            SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
}


#include "user_editor.inl"

void create_security_page(State& state) {
    const auto parent = state.pages[7] = page(state);
    label(state,parent,L"Security — passwords and local policy",12,8,510,26);
    label(state,parent,L"Enter a permanent replacement; the old value is not required. "
        L"Only Sysop-level sessions are disconnected. No credentials are displayed or recoverable.",12,43,510,44);
    label(state,parent,L"New permanent System Password:",12,96,280,22);
    state.system_password = control(state,parent,L"EDIT",L"",WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
        12,121,395,25,IDC_SYSTEM_PASSWORD,WS_EX_CLIENTEDGE);
    label(state,parent,L"Confirm permanent System Password:",12,155,300,22);
    state.system_confirm = control(state,parent,L"EDIT",L"",WS_TABSTOP | ES_AUTOHSCROLL | ES_PASSWORD,
        12,180,395,25,IDC_SYSTEM_CONFIRM,WS_EX_CLIENTEDGE);
    for (const auto edit : {state.system_password,state.system_confirm}) SendMessageW(edit,EM_SETLIMITTEXT,app::kPasswordMaximum,0);
    state.system_password_apply = control(state,parent,L"BUTTON",L"Replace Permanent SY:...",WS_TABSTOP | BS_PUSHBUTTON,
        12,218,230,30,IDC_SYSTEM_PASSWORD_APPLY);
    control(state,parent,L"BUTTON",L"Refresh",WS_TABSTOP | BS_PUSHBUTTON,257,218,100,30,IDC_SECURITY_REFRESH);
    state.security_detail = control(state,parent,L"EDIT",L"",ES_READONLY | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL,
        12,280,500,100,0,WS_EX_CLIENTEDGE);
    EnableWindow(state.system_password_apply,FALSE);
    const auto policy_parent=state.pages[16]=page(state);
    state.strong_passwords = control(state,policy_parent,L"BUTTON",L"Require Strong Passwords",WS_TABSTOP|BS_AUTOCHECKBOX,
        12,48,300,24,IDC_STRONG_PASSWORDS);
    label(state,policy_parent,L"Choose the policy for newly set passwords; existing credentials are preserved.",12,8,500,34);
    label(state,policy_parent,L"Off: 4–128 printable ASCII characters (compatibility, not NIST-aligned). "
        L"On: 15–128 plus the pinned local blocklist and context checks. No runtime downloads.",12,92,500,60);
    state.password_policy_save = control(state,policy_parent,L"BUTTON",L"Save Changes",WS_TABSTOP|BS_PUSHBUTTON,
        12,180,220,30,IDC_PASSWORD_POLICY_SAVE);
    state.policy_detail=control(state,policy_parent,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,
        12,230,500,100,0,WS_EX_CLIENTEDGE);
}

void create_levels_page(State& state) {
    const auto parent = state.pages[8] = page(state);
    label(state,parent,L"Security Levels",12,8,500,26);
    state.level_list = control(state,parent,L"LISTBOX",L"",WS_TABSTOP|LBS_NOTIFY|WS_VSCROLL,
        12,48,185,278,IDC_LEVEL_LIST,WS_EX_CLIENTEDGE);
    const auto edit = [&](const wchar_t* name, int y, int id, bool numeric, std::size_t maximum) {
        label(state,parent,name,212,y,294,20);
        auto value = control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL|(numeric ? ES_NUMBER : 0),
            212,y+20,294,22,id,WS_EX_CLIENTEDGE);
        SendMessageW(value,EM_SETLIMITTEXT,maximum,0); return value;
    };
    state.level_number = edit(L"Level number (1–999):",44,IDC_LEVEL_NUMBER,true,3);
    state.level_label = edit(L"Label:",88,IDC_LEVEL_LABEL,false,app::kSecurityLabelMaximum);
    state.level_description = edit(L"Description (optional):",132,IDC_LEVEL_DESCRIPTION,false,app::kSecurityDescriptionMaximum);
    state.level_call_minutes=edit(L"Minutes Per Call (0 = unlimited):",176,IDC_LEVEL_CALL_MINUTES,true,10);
    state.level_day_minutes=edit(L"Minutes Per Day (0 = unlimited):",220,IDC_LEVEL_DAY_MINUTES,true,10);
    state.level_chat_minutes=edit(L"Minutes Per Private Chat (0 = unlimited):",264,IDC_LEVEL_CHAT_MINUTES,true,10);
    state.level_chat_day_minutes=edit(L"Private Chat Minutes Per Day (0 = unlimited):",308,IDC_LEVEL_CHAT_DAY_MINUTES,true,10);
    label(state,parent,L"Shared defaults (not the selected user's record)",212,360,294,32);
    state.level_new_user = edit(L"New User level:",396,IDC_LEVEL_NEW_USER,true,3);
    state.level_threshold = edit(L"Sysop threshold:",440,IDC_LEVEL_THRESHOLD,true,3);
    state.level_defaults = control(state,parent,L"BUTTON",L"New Level",WS_TABSTOP|BS_PUSHBUTTON,12,512,110,30,IDC_LEVEL_DEFAULTS);
    state.level_save = control(state,parent,L"BUTTON",L"Save",WS_TABSTOP|BS_PUSHBUTTON,130,512,90,30,IDC_LEVEL_SAVE);
    state.level_retire = control(state,parent,L"BUTTON",L"Remove Level...",WS_TABSTOP|BS_PUSHBUTTON,228,512,150,30,IDC_LEVEL_RETIRE);
    control(state,parent,L"BUTTON",L"Reload",WS_TABSTOP|BS_PUSHBUTTON,386,512,120,30,IDC_LEVEL_REFRESH);
    state.level_detail = control(state,parent,L"EDIT",L"",ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
        12,554,494,50,0,WS_EX_CLIENTEDGE);
}

std::optional<app::LocalThrottleClearRequest> throttle_request(State& state) {
    static constexpr std::array<app::RateLimitScope,4> scopes{
        app::RateLimitScope::source,app::RateLimitScope::account,app::RateLimitScope::system_password,app::RateLimitScope::global};
    const auto selection = SendMessageW(state.throttle_scope,CB_GETCURSEL,0,0);
    if (selection < 0 || selection >= static_cast<LRESULT>(scopes.size())) return {};
    app::LocalThrottleClearRequest request{scopes[selection],{}};
    if (selection < 2) {
        const auto subject = wide_to_utf8(text(state.throttle_subject));
        if (!subject) return {};
        request.subject = *subject;
    }
    return app::valid_local_throttle_clear(request) ? std::optional<app::LocalThrottleClearRequest>(request) : std::nullopt;
}

void clear_local_throttle(State& state) {
    if (state.system_recovery_only || state.mode == State::ConnectionMode::blocked) return;
    const auto command = throttle_request(state);
    if (!command) { set_status(state,L"Choose an IP address or user number. For a shared login delay, leave the entry blank."); return; }
    const auto target = command->scope == app::RateLimitScope::source ? L"Source IP: " + utf8_to_wide(command->subject) :
        command->scope == app::RateLimitScope::account ? L"Account ID: " + utf8_to_wide(command->subject) :
        command->scope == app::RateLimitScope::system_password ? std::wstring(L"Shared System Password delay") : std::wstring(L"Shared BBS login delay");
    const auto review = L"Clear this temporary login delay?\n\n" + target + L"\n\n"
        L"The action is logged. Password checks, connection limits and other login delays stay in place. "
        L"A short wait already in progress may finish normally. More failed logins can start another delay. BotGate IP bans are not cleared.";
    if (MessageBoxW(state.window,review.c_str(),L"Clear Login Delay",MB_YESNO|MB_DEFBUTTON2|MB_ICONWARNING) != IDYES) return;
    bool succeeded = false;
    if (state.mode == State::ConnectionMode::online) {
        app::LocalControlFrame response;
        const auto outgoing = request(state,app::LocalControlMessageType::throttle_clear_request,
            app::encode_throttle_clear_request(*command));
        if (online_exchange(state,outgoing,app::LocalControlMessageType::throttle_clear_response,response)) {
            const auto result = app::decode_control_response(response.payload);
            succeeded = result && result->code == app::LocalControlResponseCode::ok;
        }
    } else {
        succeeded = win32::offline_clear_throttle(state.root,state.owner,*command).succeeded();
    }
    const auto result = succeeded ? L"Completed — selected login delay cleared and logged. Other protection is unchanged." :
        L"Not confirmed — the login delay could not be cleared or the response was lost. Check before trying again.";
    SetWindowTextW(state.throttle_detail,result); set_status(state,result);
}

void create_throttle_page(State& state) {
    const auto parent = state.pages[10] = page(state);
    label(state,parent,L"Failed Login Attempts",12,8,500,26);
    label(state,parent,L"Repeated wrong passwords cause temporary login delays. They expire automatically, even with BotGate off. "
        L"Use this page to help a caller try again sooner.",12,48,500,48);
    label(state,parent,L"Clear login delay for:",12,109,200,22);
    state.throttle_scope = control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST,
        12,136,500,150,IDC_THROTTLE_SCOPE);
    for (const auto* name : {L"One IP address",L"One user",L"Shared System Password",L"Shared BBS login delay"})
        SendMessageW(state.throttle_scope,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
    SendMessageW(state.throttle_scope,CB_SETCURSEL,0,0);
    label(state,parent,L"IP address or user number (shown in Accounts):",12,183,500,22);
    state.throttle_subject = control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_AUTOHSCROLL,
        12,210,500,25,IDC_THROTTLE_SUBJECT,WS_EX_CLIENTEDGE);
    SendMessageW(state.throttle_subject,EM_SETLIMITTEXT,45,0);
    label(state,parent,L"Leave the entry blank for a shared delay. Password checks and the three-attempt disconnect still apply. "
        L"This does not unblock an IP address banned by BotGate.",12,251,500,55);
    state.throttle_clear = control(state,parent,L"BUTTON",L"Clear Login Delay...",WS_TABSTOP|BS_PUSHBUTTON,
        12,320,300,30,IDC_THROTTLE_CLEAR);
    state.throttle_detail = control(state,parent,L"EDIT",L"No login delay cleared.",ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
        12,369,500,59,0,WS_EX_CLIENTEDGE);
}

void create_connection_page(State& state) {
    const auto parent = state.pages[9] = page(state);
    label(state,parent,L"Connections",12,8,500,26);
    label(state,parent,L"Connections per source address (1–128):",12,48,390,22);
    state.source_limit = control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,
        422,46,90,25,IDC_SOURCE_LIMIT,WS_EX_CLIENTEDGE);
    label(state,parent,L"Legacy per-account limit (retained, not used):",12,87,390,22);
    state.account_limit = control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_NUMBER|ES_AUTOHSCROLL,
        422,85,90,25,IDC_ACCOUNT_LIMIT,WS_EX_CLIENTEDGE);
    for (const auto edit : {state.source_limit,state.account_limit}) SendMessageW(edit,EM_SETLIMITTEXT,3,0);
    label(state,parent,L"Ordinary callers have one session. Only security 999 may have multiple "
        L"sessions, up to the configured node count; Sysop threshold does not change this.",12,121,500,40);
    label(state,parent,L"Separate source exceptions — one canonical IP|limit per line, maximum 32. "
        L"For example: 192.0.2.1|8. Blank means none. No hostnames or subnets.",12,171,500,42);
    state.source_exceptions = control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
        12,219,500,95,IDC_SOURCE_EXCEPTIONS,WS_EX_CLIENTEDGE);
    SendMessageW(state.source_exceptions,EM_SETLIMITTEXT,2112,0);
    state.connection_save = control(state,parent,L"BUTTON",L"Save Connection Security",WS_TABSTOP|BS_PUSHBUTTON,
        12,326,250,30,IDC_CONNECTION_SAVE);
    label(state,parent,L"Saved limits take effect after the BBS restarts.",276,326,236,40);
    state.connection_detail = control(state,parent,L"EDIT",L"",ES_READONLY|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL,
        12,369,500,59,0,WS_EX_CLIENTEDGE);
}

void refresh_feed(State& state) {
    app::LocalControlFrame response;
    if (state.mode!=State::ConnectionMode::online || !online_exchange(state,
            request(state,app::LocalControlMessageType::botgate_feed_status_request),
            app::LocalControlMessageType::botgate_feed_status_response,response)) {
        SetWindowTextW(state.gate_detail,L"Feed status unavailable. Updates are owned by running Runtime; local rules and challenge remain configured.");
        EnableWindow(state.gate_update,FALSE); return;
    }
    const auto status=app::decode_botgate_feed_status(response.payload);
    if (!status) { SetWindowTextW(state.gate_detail,L"Feed status response invalid."); return; }
    const auto message=(status->available ? std::wstring(L"Active feed: ") + std::to_wstring(status->entries)+L" addresses" : L"DEGRADED: no valid cached feed")+
        L"\r\nState: "+utf8_to_wide(status->result)+L"; received "+std::to_wstring(status->bytes_received)+L" bytes"+
        L"\r\nLast attempt: "+bbs_local_time(state,status->last_attempt)+L"\r\nLast success: "+bbs_local_time(state,status->last_success)+
        (status->last_success ? L"; age "+std::to_wstring(std::max<std::int64_t>(0,now_utc()-status->last_success)/60)+L" minutes" : L"")+
        L"\r\nRules: "+utf8_to_wide(status->rule_status)+(status->rules_ready ? L"; last good snapshot active" : L"; unavailable")+
        L"\r\nGeo: "+(status->geo_summary.empty() ? std::wstring(L"none") : utf8_to_wide(status->geo_summary));
    SetWindowTextW(state.gate_detail,message.c_str()); EnableWindow(state.gate_update,!status->updating);
}

void load_botgate_editor(State& state,unsigned index) {
    try {
        win32::initialize_botgate_files(state.root,state.owner,state.configuration.saved.botgate);
        const auto raw=win32::read_botgate_rule(state.root,state.owner,index);
        if(raw.size()>65535) throw std::runtime_error("rule file exceeds editor limit; use external text editor");
        std::string display;
        for(std::size_t i=0;i<raw.size();++i) {if(raw[i]=='\n' && (!i || raw[i-1]!='\r')) display+='\r';display+=raw[i];}
        state.gate_rule_raw=raw;state.gate_rule_original=utf8_to_wide(display);state.gate_rule_index=index;state.gate_rule_loaded=true;
        SetWindowTextW(state.gate_rule_text,state.gate_rule_original.c_str());
        SendMessageW(state.gate_rule_text,EM_SETREADONLY,index==4,0);
        EnableWindow(state.gate_rule_save,index!=4);
        const std::array<const wchar_t*,6> help{{
            L"Exempt from filtering, Geo, feed and per-IP throttling. Human challenge and login still required.",
            L"Block addresses/ranges. One rule per line; * and ? wildcards; ! adds a same-file exception.",
            L"Block addresses without per-attempt log lines. Dashboard totals still count refusals.",
            L"Block forward-verified hostnames; wildcard patterns allowed. DNS failures skip only this check.",
            L"Runtime-managed temporary bans with expiry. Read-only here; expired bans no longer block.",
            L"Ignore downloaded-feed matches only. Local/Geo/domain blocks and human challenge still apply."}};
        SetWindowTextW(state.gate_rule_help,help[index]);
    } catch(const std::exception& e) {EnableWindow(state.gate_rule_save,FALSE);set_status(state,L"Unavailable — "+utf8_to_wide(e.what()));}
}
void create_botgate_page(State& state) {
    const auto parent=state.pages[11]=page(state);
    state.gate_global=control(state,parent,L"BUTTON",L"BotGate enabled",WS_TABSTOP|BS_AUTOCHECKBOX,12,8,180,24,IDC_GATE_GLOBAL);
    state.gate_telnet=control(state,parent,L"BUTTON",L"Telnet",WS_TABSTOP|BS_AUTOCHECKBOX,205,8,100,24,IDC_GATE_TELNET);
    state.gate_ssh=control(state,parent,L"BUTTON",L"Challenge new SSH registrations",WS_TABSTOP|BS_AUTOCHECKBOX,12,41,410,24,IDC_GATE_SSH);
    label(state,parent,L"Challenge timeout (5–120 seconds):",12,78,300,24);
    state.gate_timeout=control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_NUMBER,360,76,70,25,IDC_GATE_TIMEOUT,WS_EX_CLIENTEDGE);
    SendMessageW(state.gate_timeout,EM_SETLIMITTEXT,3,0);
    state.gate_rule_select=control(state,parent,WC_COMBOBOXW,L"",WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,12,115,510,220,IDC_GATE_RULE_SELECT);
    for(const auto* item:{L"Exempt addresses - ipfilter_exempt.cfg",L"Blocked addresses - ip.can",L"Silent blocks - ip-silent.can",
        L"Blocked hostnames - host.can",L"Temporary bans - temp_ip.can",L"Feed-only exceptions - feed-exempt.can"})
        SendMessageW(state.gate_rule_select,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(item));
    SendMessageW(state.gate_rule_select,CB_SETCURSEL,0,0);
    state.gate_rule_help=control(state,parent,L"STATIC",L"Select a rule file. Empty means no rules.",0,12,146,510,38,0);
    state.gate_rule_text=control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_MULTILINE|ES_AUTOVSCROLL|WS_VSCROLL|ES_WANTRETURN,12,188,510,105,IDC_GATE_RULE_TEXT,WS_EX_CLIENTEDGE);
    SendMessageW(state.gate_rule_text,EM_SETLIMITTEXT,65535,0);
    state.gate_rule_save=control(state,parent,L"BUTTON",L"Save Rule File",WS_TABSTOP|BS_PUSHBUTTON,12,300,150,26,IDC_GATE_RULE_SAVE);
    control(state,parent,L"BUTTON",L"Reload Rules",WS_TABSTOP|BS_PUSHBUTTON,172,300,120,26,IDC_GATE_RULE_RELOAD);
    label(state,parent,L"BotGate\\geo: add/remove country .txt files; changes reload within 2 seconds.",12,332,510,22);
    state.gate_save=control(state,parent,L"BUTTON",L"Save BotGate Settings",WS_TABSTOP|BS_PUSHBUTTON,12,358,210,28,IDC_GATE_SAVE);
    state.gate_update=control(state,parent,L"BUTTON",L"Update Threat Feed Now",WS_TABSTOP|BS_PUSHBUTTON,240,358,232,28,IDC_GATE_UPDATE);
    state.gate_detail=control(state,parent,L"EDIT",L"",ES_READONLY|ES_MULTILINE|WS_VSCROLL,12,397,510,68,0,WS_EX_CLIENTEDGE);
}

void create_logging_page(State& state) {
    const auto parent=state.pages[12]=page(state);
    label(state,parent,L"Each log: 1–100 MiB active file; 1–100 completed archives. Defaults: 10 MiB and 10 archives.",12,8,510,40);
    label(state,parent,L"Log",12,65,230,24); label(state,parent,L"MiB",270,65,85,24); label(state,parent,L"Archives",385,65,90,24);
    const std::array<const wchar_t*,3> names{{L"SFXRuntime.log",L"CALLERS.LOG",L"HEYSYSOP.LOG"}};
    for (std::size_t i=0;i<names.size();++i) {
        const int y=100+static_cast<int>(i)*48; label(state,parent,names[i],12,y,230,24);
        for (std::size_t j=0;j<2;++j) {
            auto& field=state.logging_limits[i*2+j];
            field=control(state,parent,L"EDIT",L"",WS_TABSTOP|ES_NUMBER,270+static_cast<int>(j)*115,y,85,25,0,WS_EX_CLIENTEDGE);
            SendMessageW(field,EM_SETLIMITTEXT,3,0);
        }
    }
    label(state,parent,L"Completed archives over the limit are pruned oldest first. Active records are never split. Clearing Live Logs does not delete files.",12,259,510,60);
    state.logging_save=control(state,parent,L"BUTTON",L"Save Logging Settings",WS_TABSTOP|BS_PUSHBUTTON,12,335,220,30,IDC_LOGGING_SAVE);
}

#include "bbs_health_page.inc"

bool save_rule(State& state) {
    try {
        const auto entered=text(state.gate_rule_text);
        const auto value=entered.empty() ? std::optional<std::string>(std::string{}) : wide_to_utf8(entered);
        if(!value) throw std::runtime_error("Rule text encoding invalid.");
        if(win32::read_botgate_rule(state.root,state.owner,state.gate_rule_index)!=state.gate_rule_raw)
            throw std::runtime_error("Rule file changed externally. Reload before saving; unsaved text retained.");
        win32::save_botgate_rule(state.root,state.owner,state.gate_rule_index,*value);
        state.gate_rule_raw=*value; state.gate_rule_original=entered;
        for(auto& field:state.baseline) if(field.control==state.gate_rule_text) field.value=entered;
        set_status(state,L"Applied — rule file saved; reload within 2 seconds. Other lists unchanged.");
        return true;
    } catch(const std::exception& error) { set_status(state,L"Failed — "+utf8_to_wide(error.what())); return false; }
}

bool action_succeeded(State& state) {
    const auto status=text(state.status);
    return status.find(L"Applied")==0 || status.find(L"Saved")==0 || status.find(L"Completed")==0;
}
#include "display_menu_page.inc"

bool changed_control(State& state,HWND control) {
    const auto current=page_values(state);
    for(const auto& before:state.baseline) if(before.control==control)
        for(const auto& after:current) if(after.control==control) return before.value!=after.value;
    return false;
}
#include "classic_information_page.inc"
#include "message_configuration_page.inc"
#include "qwk_configuration_page.inc"
#include "file_configuration_page.inc"
#include "door_configuration_page.inc"
#include "door_bulletin_page.inc"
#include "chat_configuration_page.inc"
#include "ftn_configuration_page.inc"
#include "clamav_page.inc"
#include "maintenance_progress.inc"
#include "message_maintenance_page.inc"
#include "external_events_page.inc"
bool save_current_page(State& state) {
    if(state.current_page==40){set_status(state,L"Health Check is report-only; there are no settings to save here.");return false;}
    if(state.current_page==35)return ftn_import_apply(state);
    if(state.current_page>=29&&state.current_page<39)return save_ftn(state);
    if(state.current_page==27)return save_chat_room(state);
    if(state.current_page==28)return save_chat_command(state);
    if(state.current_page==26){
        if(state.doors.pending){set_status(state,L"Door save not confirmed. Choose Check Save Result before saving again.");return false;}
        if(state.doors.conflict){set_status(state,L"Door settings changed elsewhere. Reload Latest Settings and review before saving.");return false;}
        return save_door_configuration_page(state);
    }
    if(state.file_editor.pending){set_status(state,L"File configuration outcome unresolved — use Reconcile before leaving.");return false;}
    if(state.pending_message_apply){set_status(state,L"Outcome unresolved — reconcile the pending message operation first.");return false;}
    set_status(state,L"Refused — no change was committed.");
    switch(state.current_page) {
        case 0:
            if(state.pending_qwk_policy||qwk_policy_dirty(state)){
                if(changed_control(state,state.bbs_name)){set_status(state,L"Save QWK policy separately before saving BBS-name edits. Both drafts retained.");return false;}
                return save_qwk_policy(state);
            }
            if(!save_configuration_page(state,0))return false;break;
        case 3: save_external_client(state); break;
        case 5: return save_external_event(state, false);
        case 4:
            if(state.selected_job && changed_control(state,state.unread_mail_age)){
                if(changed_control(state,state.schedule_enabled)||changed_control(state,state.schedule_hour)||changed_control(state,state.schedule_minute)||changed_control(state,state.schedule_weekday)){
                    set_status(state,L"Refused — save unread-mail age separately before schedule edits. Both drafts retained.");return false;}
                if(!save_unread_mail_age(state))return false;
            }else save_scheduler(state);
            break;
        case 6: save_user_editor(state); break;
        case 7: replace_system_password(state); break;
        case 8: {
            const bool definition=changed_control(state,state.level_number)||changed_control(state,state.level_label)||changed_control(state,state.level_description)||
                changed_control(state,state.level_call_minutes)||changed_control(state,state.level_day_minutes)||changed_control(state,state.level_chat_minutes)||changed_control(state,state.level_chat_day_minutes);
            if(changed_control(state,state.level_message_lines)){
                if(definition || changed_control(state,state.level_new_user) || changed_control(state,state.level_threshold)){set_status(state,L"Refused — save the message limit separately before saving security/time edits. Drafts retained.");return false;}
                save_message_limit(state);break;
            }
            apply_level_change(state,definition ? app::LocalSecurityAction::upsert_level : app::LocalSecurityAction::set_defaults);
            break;
        }
        case 10: clear_local_throttle(state); break;
        case 18:
            if(changed_control(state,state.chat.stats)){
                if(changed_control(state,state.opening_date)){set_status(state,L"Save the opening date and statistics setting separately. Your edits remain.");return false;}
                if(!state.chat.config)return false;auto next=*state.chat.config;next.exclude_sysop_statistics=door_checked(state.chat.stats);return chat_commit(state,next);
            }
            save_opening_date(state);break;
        case 19:
            if(!save_message_catalog(state))return false;
            remember_page(state);return true;
        case 20:apply_clone(state);break;
        case 21:case 22:case 23:case 24:case 25:return save_file_configuration_page(state);
        case 11: {
            // Preserve the independent rule draft across a configuration refresh.
            if(text(state.gate_rule_text)!=state.gate_rule_original && !save_rule(state)) return false;
            if(!save_configuration_page(state,11)) return false;
            break;
        }
        case 14:case 41:if(!save_configuration_page(state,state.current_page))return false;remember_page(state);return true;
        default: if(!save_configuration_page(state,state.current_page)) return false; break;
    }
    if(!action_succeeded(state)) return false;
    remember_page(state); return true;
}

bool resolve_dirty(State& state) {
    if(state.diagnostic_only)return true;
    if(state.navigation_loading)return true; // read-only page; edits are disabled
    if(state.ftn.pending){set_status(state,L"The FTN save is not confirmed. Choose Check Save Result before leaving or saving again.");return false;}
    if(state.ftn.import_job&&!state.ftn.import_job->done){set_status(state,L"Echo list download is running. Cancel the download or wait before navigating.");return false;}
    if(clam_busy(state)){set_status(state,L"ClamAV operation active — wait or Cancel before navigating.");return false;}
    if(state.file_editor.pending){set_status(state,L"File outcome unresolved — use Reconcile before navigation or another save.");return false;}
    if(state.pending_qwk_policy){set_status(state,L"QWK outcome unresolved - use Reconcile QWK before leaving this page.");return false;}
    if(state.pending_message_apply){set_status(state,L"Outcome unresolved — use Reconcile before navigation or another apply.");return false;}
    if(state.doors.pending){set_status(state,L"The Door save is not confirmed. Choose Check Save Result before leaving.");return false;}
    if(state.resolving_dirty || state.account_action_active) return false;
    if(!page_dirty(state)) return true;
    struct Guard { bool& value; explicit Guard(bool& v):value(v){value=true;} ~Guard(){value=false;} } guard(state.resolving_dirty);
    const TASKDIALOG_BUTTON choices[]={{101,L"Save and Continue"},{102,L"Discard Changes"},{103,L"Cancel Navigation"}};
    TASKDIALOGCONFIG dialog{}; dialog.cbSize=sizeof(dialog); dialog.hwndParent=state.window;
    dialog.dwFlags=TDF_ALLOW_DIALOG_CANCELLATION|TDF_POSITION_RELATIVE_TO_WINDOW;
    dialog.pszWindowTitle=kWindowTitle; dialog.pszMainInstruction=L"This page has unsaved changes.";
    dialog.pszContent=L"Save this page, discard its edits, or remain here. Sensitive changes still require their normal review.";
    dialog.cButtons=3; dialog.pButtons=choices; dialog.nDefaultButton=103;
    int selected=103;
    if(FAILED(TaskDialogIndirect(&dialog,&selected,nullptr,nullptr))) {
        set_status(state,L"Unavailable — the unsaved-changes dialog could not open. Edits retained."); return false;
    }
    if(selected==101) return save_current_page(state);
    if(selected==102) { discard_page(state); remember_page(state); return true; }
    return false;
}

void show_page(State& state, int index) {
    if(index==25&&!win32::clamav_available())index=11;
    if (index < 0 || index >= static_cast<int>(state.pages.size())) index = 0;
    for (std::size_t page_index = 0; page_index < state.pages.size(); ++page_index)
        ShowWindow(state.pages[page_index], static_cast<int>(page_index) == index
            ? SW_SHOW : SW_HIDE);
    SetWindowTextW(state.account_value,L""); SetWindowTextW(state.account_confirm,L"");
    SetWindowTextW(state.system_password,L""); SetWindowTextW(state.system_confirm,L"");
    state.current_page=index;
    if(index!=6) SetWindowTextW(state.window,kWindowTitle);
    const auto& selected=sfx::config_ui::route(index);
    const bool security=sfx::config_ui::security_page(index);
    ShowWindow(state.security_tabs,security ? SW_SHOW : SW_HIDE);
    if(security) for(int tab=0;tab<4;++tab) if(sfx::config_ui::security_tab_pages[tab]==index) TabCtrl_SetCurSel(state.security_tabs,tab);
    const auto breadcrumb=security ? std::wstring(L"Security > BBS Security") : std::wstring(selected.category)+L" > "+std::wstring(selected.title);
    SetWindowTextW(state.heading,breadcrumb.c_str());
    if (index==11 && !state.system_recovery_only) { if(!state.gate_rule_loaded) load_botgate_editor(state,0); refresh_feed(state); SetTimer(state.window,config_timer::botgate,1500,nullptr); }
    else KillTimer(state.window,config_timer::botgate);
    remember_page(state);
}

void activate_page(State& state,int index) {
    if(state.diagnostic_only&&index!=40)return;
    if(index==40){
        ++state.navigation_epoch;state.navigation_page=40;state.navigation_reads.clear();state.navigation_bytes=0;state.navigation_rendering=state.navigation_resuming=state.navigation_pending=false;state.navigation_context_ready=state.navigation_context_ok=false;state.navigation_error.clear();
        state.navigation_loading=state.navigation_finishing=state.navigation_worker!=nullptr;
        if(state.navigation_worker)state.navigation_worker->suspend(state.navigation_epoch);
        show_page(state,index);EnableWindow(state.pages[index],TRUE);EnableWindow(state.health_run,!state.navigation_loading);
        if(state.navigation_loading)set_status(state,L"Finishing the previous settings read before Health Check...");return;
    }
    if(!state.system_recovery_only&&state.navigation_async_enabled){
        if(!state.navigation_resuming){
            if(!state.navigation_worker)state.navigation_worker=std::make_unique<ConfigNavigationWorker>(state.window,state.root,state.owner,state.receipt);
            ++state.navigation_epoch;state.navigation_page=index;state.navigation_reads.clear();state.navigation_bytes=0;state.navigation_context_ready=state.navigation_context_ok=false;state.navigation_finishing=false;state.navigation_error.clear();
            state.navigation_worker->begin(state.navigation_epoch,index,state.hosting_job!=nullptr);
        }
        state.navigation_rendering=true;state.navigation_loading=true;state.navigation_pending=false;
    }
    if(index>=30&&index<39&&index!=36){
        if(!state.ftn.config)refresh_ftn(state);
        const auto* c=state.ftn.draft?&*state.ftn.draft:state.ftn.config?&*state.ftn.config:nullptr;
        if(!c||state.ftn.network<0||static_cast<std::size_t>(state.ftn.network)>=c->networks.size())index=29;
    }
    show_page(state,index);
    if(index==29)refresh_ftn(state);
    if(index==30)ftn_network_fill(state);
    if(index==31)ftn_link_fill(state);
    const bool ftn_catalog_ready=(index!=32&&index!=33&&index!=38&&!(index==37&&state.ftn.setup.active))||ftn_catalogs(state);
    if(index==32&&ftn_catalog_ready)ftn_echo_fill(state);
    if(index==33&&ftn_catalog_ready)ftn_file_fill(state);
    if(index==34)ftn_route_fill(state);
    if(index==36)ftn_held_open(state);
    if(index==38&&ftn_catalog_ready)ftn_permissions_fill(state);
    if(index==37)ns_fill(state);
    if(index==6) refresh_accounts(state,true);
    if(index==7 || index==16) refresh_security(state);
    if(index==8) refresh_levels(state);
    if(index==18){refresh_classic_information(state);refresh_chat(state);}
    if(index==27||index==28)refresh_chat(state);
    if(index==0){refresh_system_sysop(state);refresh_qwk_policy(state);}
    if(index==17)refresh_private_pipe_colors(state);
    if(index==19)refresh_message_configuration(state);
    if(index==20){refresh_message_configuration(state);load_clone_targets(state);clone_preview(state);}
    if(index>=21&&index<=24)refresh_file_configuration_page(state);
    if(index==26)refresh_door_configuration_page(state);
    if(index==25)refresh_clam_page(state);
    if(index==4){if(load_editor_state(state))populate_controls(state);}
    if(index==5 && (!state.navigation_rendering || state.navigation_context_ready)) read_external_events(state);
    if(index==15) {
        const auto snapshot=query_accounts(state,{});
        const auto summary=snapshot.code==app::LocalAccountResultCode::ok
            ? L"New User security level: "+std::to_wstring(snapshot.policy.new_user_level)
            : std::wstring(L"New User security level: unavailable");
        SetWindowTextW(state.registration_detail,(summary+L"\r\n"+
            active_saved_line(state,L"Allow New Users",yes_no(state.configuration.active.allow_new_users),yes_no(state.configuration.saved.allow_new_users))+
            L"\r\n"+activation_text(state.configuration)).c_str());
    }
    remember_page(state); persist_route(state);
    if(state.navigation_rendering){
        state.navigation_rendering=false;
        EnableWindow(state.pages[state.current_page],state.current_page==14);
        if(state.current_page==14)refresh_hosting(state,false);
        if(state.current_page==41)refresh_runtime_settings(state,false);
        if(!state.navigation_context_ready||state.navigation_pending)set_status(state,L"Loading settings...");
        else if(!state.navigation_finishing){state.navigation_finishing=true;state.navigation_worker->finish(state.navigation_epoch);}
    }
}

void complete_navigation(State& state){
    if(!state.navigation_worker)return;
    bool render=false;
    for(auto& result:state.navigation_worker->take()){
        if(result.epoch!=state.navigation_epoch)continue;
        if(result.finished){
            if(state.current_page==40){state.navigation_loading=state.navigation_finishing=false;EnableWindow(state.pages[40],TRUE);EnableWindow(state.health_run,TRUE);set_status(state,L"Health Check is ready. Stop the BBS before running it.");continue;}
            state.navigation_loading=false;state.navigation_finishing=false;
            EnableWindow(state.pages[state.current_page],state.current_page==14||state.current_page==41||state.navigation_context_ok);
            if(state.current_page==14)refresh_hosting(state,false);
            if(state.current_page==41){
                refresh_runtime_settings(state,state.navigation_context_ok);
                remember_page(state);
                if(state.runtime_idle_draft){
                    if(!state.navigation_context_ok&&state.runtime_idle_baseline)for(auto& v:state.baseline){
                        if(v.control==state.caller_afk)v.value=state.runtime_idle_baseline->first;
                        if(v.control==state.login_afk)v.value=state.runtime_idle_baseline->second;
                    }
                    SetWindowTextW(state.caller_afk,state.runtime_idle_draft->first.c_str());SetWindowTextW(state.login_afk,state.runtime_idle_draft->second.c_str());
                    state.runtime_idle_draft.reset();state.runtime_idle_baseline.reset();
                }
            }else remember_page(state);
            if(!state.navigation_context_ok)set_status(state,state.navigation_error==L"setup_required"?
                L"First-run setup is required. Open Bin\\SFXControlPanel.exe in this installation, finish First-Run Setup, then reopen SFXConfig.":
                state.current_page==14?
                L"Hosting controls remain available here. Open the Control Panel for ordinary settings while Service runs.":
                state.current_page==41?L"Open the Control Panel while Service runs, then Retry Idle Settings. Your edits remain.":
                L"Settings unavailable. Select another section and return to retry; no changes were made. "+state.navigation_error);
            else if(text(state.status)==L"Loading settings...")set_status(state,L"Settings loaded.");
            if(!state.hosting_notice.empty()){set_status(state,state.hosting_notice);state.hosting_notice.clear();}
            if(!state.prepare_notice.empty()){
                if(state.navigation_context_ok)set_status(state,state.prepare_notice);
                state.prepare_notice.clear();
            }
            continue;
        }
        if(result.context){
            state.navigation_context_ready=true;state.navigation_context_ok=result.ok;
            if(!result.ok)state.navigation_error=utf8_to_wide(result.state.diagnostic);
            if(result.ok){state.mode=result.state.online?State::ConnectionMode::online:State::ConnectionMode::offline;state.configuration=std::move(result.state.configuration);if(state.navigation_page==4){state.scheduler=std::move(result.state.scheduler);state.backup_settings=std::move(result.state.backup);}}
            else state.mode=State::ConnectionMode::blocked;
        }else{
            const auto found=state.navigation_reads.find(result.key);if(found==state.navigation_reads.end())continue;
            auto& cached=found->second;cached.ready=true;
            if(!result.ok){state.navigation_context_ok=false;state.navigation_error=utf8_to_wide(result.diagnostic.empty()?"navigation_read_unavailable":result.diagnostic);}
            if(result.response.payload.size()<=64U*1024U*1024U-state.navigation_bytes){state.navigation_bytes+=result.response.payload.size();cached.ok=result.ok;cached.response=std::move(result.response);}
        }
        render=true;
    }
    if(render&&!state.navigation_finishing){state.navigation_resuming=true;activate_page(state,state.navigation_page);state.navigation_resuming=false;}
}
bool navigate_route(State& state,std::wstring_view id) {
    const auto index=sfx::config_ui::route(id).page;
    if(index==state.current_page){if(!state.navigation_loading&&!page_dirty(state))activate_page(state,index);return true;}
    if(!resolve_dirty(state)) return false;
    state.navigating=true;
    TreeView_SelectItem(state.navigation,state.route_items[index]);
    state.navigating=false;
    activate_page(state,index); return true;
}

bool open_user_record(State& state,app::AccountId id) {
    if(id<=0 || state.system_recovery_only || !resolve_dirty(state)) return false;
    if(!navigate_route(state,L"users.accounts")) return false;
    SetWindowTextW(state.account_filter,std::to_wstring(id).c_str());
    if(state.navigation_async_enabled){activate_page(state,6);return true;}
    refresh_accounts(state,true);
    remember_page(state);
    if(state.account_selection.accounts.size()!=1 || state.account_selection.accounts[0].account_id!=id) {
        set_status(state,L"This user record is unavailable. No other user was opened and no changes were made.");
        return false;
    }
    SetFocus(state.user_fields[0]); return true;
}

#include "navigation_splitter.inc"
void layout(State& state, const int width, const int height) {
    const auto scaled = [&state](const int value) { return MulDiv(value, state.dpi, 96); };
    const int margin = scaled(12);
    const int navigation_width = std::clamp(scaled(state.navigation_width),scaled(140),std::max(scaled(140),width-scaled(554)));
    // Relayout can shift every control when validation/Advanced changes height.
    // Use the same no-copy/all-child repaint contract as ordinary scrolling.
    const auto place=[](HWND h,int x,int y,int w,int z){if(h)SetWindowPos(h,nullptr,x,y,w,z,SWP_NOZORDER|SWP_NOACTIVATE|SWP_NOCOPYBITS|SWP_NOREDRAW);};
    const int heading_height = scaled(34);
    const int status_height = scaled(48);
    const int content_x = margin + navigation_width + margin;
    const int content_width = std::max(200, width - content_x - margin);
    const int page_y = margin + heading_height;
    const int page_height = std::max(120, height - page_y - status_height - margin * 2);
    place(state.navigation, margin, margin, navigation_width,
        std::max(120, height - status_height - margin * 3));
    place(state.heading, content_x, margin, content_width, heading_height);
    place(state.security_tabs,content_x,page_y,content_width,scaled(28));
    for(std::size_t index=0;index<state.pages.size();++index) {
        const auto parent=state.pages[index];
        if(!parent)continue;
        const int tab_height=sfx::config_ui::security_page(static_cast<int>(index)) ? scaled(34) : 0;
        place(parent,content_x,page_y+tab_height,content_width,page_height-tab_height);
        const bool stack_event_editor = index == 5 && content_width < scaled(910);
        const auto bounds = [&](const auto& item) {
            auto rect = item.bounds;
            if (index == 5 && rect.top < scaled(20) && rect.left >= scaled(650)) {
                const auto shift = std::min(0, content_width - scaled(908));
                OffsetRect(&rect, shift, 0);
            } else if (stack_event_editor) {
                if (item.control == state.external.save) OffsetRect(&rect, 0, scaled(450));
                else if (rect.top >= scaled(180) && rect.left >= scaled(449))
                    OffsetRect(&rect, -scaled(437), scaled(415));
            }
            return rect;
        };
        int bottom=0;
        for(const auto& item:state.page_bounds[index])if((index!=26&&index!=31) || (GetWindowLongPtrW(item.control,GWL_STYLE)&WS_VISIBLE))bottom=std::max(bottom,static_cast<int>(bounds(item).bottom)+scaled(12+(index==26?state.doors.notice_offset(item.control):0)));
        SCROLLINFO info{sizeof(info),SIF_RANGE|SIF_PAGE|SIF_POS,0,bottom-1,static_cast<UINT>(page_height-tab_height),GetScrollPos(parent,SB_VERT),0};
        SetScrollInfo(parent,SB_VERT,&info,TRUE);
        RECT viewport{};GetClientRect(parent,&viewport);
        const int offset=GetScrollPos(parent,SB_VERT);
        for(const auto& item:state.page_bounds[index]) {
            const auto r=bounds(item);
            const int designed_width=static_cast<int>(r.right-r.left);
            if (index == 4 && (item.control == state.backup_destination || item.control == state.backup_browse)) {
                const int browse_left = std::min(scaled(570), static_cast<int>(viewport.right) - scaled(114));
                const int left = item.control == state.backup_browse ? browse_left : r.left;
                const int field_width = item.control == state.backup_browse ? scaled(102) : browse_left - r.left - scaled(13);
                place(item.control, left, r.top-offset, field_width, r.bottom-r.top);
                continue;
            }
            // Small labels (notably the time separator) must not expand into
            // neighboring edit borders when the general minimum width applies.
            place(item.control,r.left,r.top-offset+(index==26?scaled(state.doors.notice_offset(item.control)):0),std::max(std::min(scaled(30),designed_width),std::min(designed_width,static_cast<int>(viewport.right-r.left-scaled(12)))),r.bottom-r.top);
        }
    }
    place(state.status, margin, height - status_height - margin,
        std::max(100, width - margin * 2), status_height);
    if(state.current_page==37)ns_layout(state);
    RedrawWindow(state.window,nullptr,nullptr,RDW_INVALIDATE|RDW_ERASE|RDW_FRAME|RDW_ALLCHILDREN|RDW_UPDATENOW);
}

bool create_ui(State& state) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    if (!SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        return false;
    state.ui_font = CreateFontIndirectW(&metrics.lfMessageFont);
    LOGFONTW heading = metrics.lfMessageFont;
    heading.lfWeight = FW_SEMIBOLD;
    heading.lfHeight = static_cast<LONG>(heading.lfHeight * 3 / 2);
    state.heading_font = CreateFontIndirectW(&heading);
    state.navigation=control(state,state.window,WC_TREEVIEWW,L"",
        WS_TABSTOP|WS_BORDER|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT|TVS_SHOWSELALWAYS,
        0,0,100,100,IDC_NAVIGATION,WS_EX_CLIENTEDGE);
    SetWindowSubclass(state.navigation,door_surface_keys,87,reinterpret_cast<DWORD_PTR>(&state));
    TreeView_SetExtendedStyle(state.navigation,TVS_EX_DOUBLEBUFFER,TVS_EX_DOUBLEBUFFER);
    HTREEITEM category=nullptr;
    std::wstring_view previous;
    for(const auto& item:sfx::config_ui::routes) {
        if(state.diagnostic_only&&item.page!=40)continue;
        if(item.page==9 || item.page==10 || item.page==25) {state.route_items[item.page]=state.route_items[11];continue;}
        TVINSERTSTRUCTW node{}; node.hInsertAfter=TVI_LAST;
        node.item.mask=TVIF_TEXT|TVIF_PARAM;
        if(item.category!=previous) {
            node.hParent=TVI_ROOT; node.item.pszText=const_cast<wchar_t*>(item.category.data());
            node.item.lParam=item.page; category=TreeView_InsertItem(state.navigation,&node);
            previous=item.category;
        }
        node.hParent=category; node.item.pszText=const_cast<wchar_t*>(item.title.data());
        if(item.page==26){node.item.pszText=const_cast<wchar_t*>(L"Door Menu");node.item.mask|=TVIF_CHILDREN;node.item.cChildren=1;}
        else{node.item.mask&=~TVIF_CHILDREN;node.item.cChildren=0;}
        node.item.lParam=item.page;
        state.route_items[item.page]=TreeView_InsertItem(state.navigation,&node);
        TreeView_Expand(state.navigation,category,TVE_EXPAND);
    }
    for(int i=30;i<39;++i)state.route_items[i]=state.route_items[29];
    state.heading = control(state, state.window, L"STATIC", L"System",
        SS_LEFT | SS_NOPREFIX, 0, 0, 100, 30);
    if (state.heading && state.heading_font)
        SendMessageW(state.heading, WM_SETFONT,
            reinterpret_cast<WPARAM>(state.heading_font), TRUE);
    state.security_tabs=control(state,state.window,WC_TABCONTROLW,L"",WS_TABSTOP,0,0,100,28,IDC_BBS_SECURITY_TABS);
    for(auto title:{L"BotGate",L"Connections",L"Failed Login Attempts",L"ClamAV"}) {
        if(!win32::clamav_available()&&std::wstring_view(title)==L"ClamAV")continue;
        TCITEMW tab{};tab.mask=TCIF_TEXT;tab.pszText=const_cast<wchar_t*>(title);
        TabCtrl_InsertItem(state.security_tabs,TabCtrl_GetItemCount(state.security_tabs),&tab);
    }
    state.status = control(state, state.window, L"STATIC", L"Loading...",
        SS_LEFT | SS_NOTIFY, 0, 0, 100, 44, IDC_STATUS, WS_EX_CLIENTEDGE);
    if(!state.diagnostic_only){
    create_system_page(state);
    create_nodes_page(state);
    create_servers_page(state);
    create_client_page(state);
    create_backup_page(state);
    create_scheduler_page(state);
    create_external_events_page(state);
    create_accounts_page(state);
    create_security_page(state);
    create_levels_page(state);
    create_connection_page(state);
    create_throttle_page(state);
    create_botgate_page(state);
    create_logging_page(state);
    create_display_menu_page(state);
    create_classic_information_pages(state);
    create_message_configuration_pages(state);
    create_qwk_policy_controls(state);
    create_file_configuration_pages(state);
    create_door_configuration_page(state);
    create_chat_pages(state);
    create_ftn_pages(state);
    }
    create_bbs_health_page(state);
    if(!state.diagnostic_only&&win32::clamav_available())create_clam_page(state);
    for(std::size_t index=0;index<state.pages.size();++index) {
        if(!state.pages[index])continue;
        for(HWND child=GetWindow(state.pages[index],GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)) {
            RECT r{};GetWindowRect(child,&r);MapWindowPoints(nullptr,state.pages[index],reinterpret_cast<POINT*>(&r),2);
            state.page_bounds[index].push_back({child,r});
        }
    }
    for(const auto button:{state.system_save,state.nodes_save,state.servers_save,state.client_save,state.schedule_save,state.connection_save,state.gate_save,state.logging_save})
        SetWindowTextW(button,L"Save Changes");
    SetWindowTextW(state.gate_update,L"Update Threat Feed Now");
    SetWindowTextW(state.system_password_apply,L"Replace System Password...");
    for(std::size_t i=0;i<state.pages.size();++i)
        if(!state.diagnostic_only&&i!=39&&!state.pages[i]&&(i!=25||win32::clamav_available()))return false;
    return state.navigation && state.heading && state.status;
}

enum : UINT { ID_FILE_EXIT = 2001, ID_HELP_ABOUT = 2002 };

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<State*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        state = reinterpret_cast<State*>(create->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    if(message==kConfigNavigationMessage){complete_navigation(*state);return 0;}
    if(message==WM_APP+43){
        if(wparam==state->hosting_epoch&&static_cast<int>(lparam)==state->hosting_action&&state->hosting_action)
            hosting_power(*state,state->hosting_action==1);
        return 0;
    }
    if(const auto handled=navigation_splitter_message(*state,message,wparam,lparam))return *handled;
    if(message==RegisterWindowMessageW(win32::edit_user_message)&&!state->diagnostic_only) {
        open_user_record(*state,win32::edit_user_message_id(static_cast<std::uint32_t>(wparam),static_cast<std::uint32_t>(lparam)));
        return 0;
    }
    if (message==RegisterWindowMessageW(L"SFXConfig.ShowBotGate.v1") && !state->system_recovery_only&&!state->diagnostic_only) {
        navigate_route(*state,L"security.botgate"); return 0;
    }
    if(message==RegisterWindowMessageW(L"SFXConfig.ShowFTN.v1")&&!state->system_recovery_only&&!state->diagnostic_only){navigate_route(*state,L"ftn.networks");return 0;}
    if ((message == win32::DesktopAppearance::changed_message() && message != 0) ||
            (message == WM_ACTIVATEAPP && wparam)) {
        if (state->appearance.reload()) {
            state->appearance.check_menu(state->theme_menu);
            state->appearance.apply_controls(window);
        }
    }
    LRESULT theme_result = 0;
    if (state->appearance.control_message(window, message, wparam, lparam, theme_result))
        return theme_result;
    switch (message) {
        case WM_CREATE: {
            if (!create_ui(*state)) return -1;
            state->appearance.apply_controls(window);
            RECT client{};
            GetClientRect(window, &client);
            layout(*state, client.right, client.bottom);
            if(state->diagnostic_only){
                show_page(*state,40);state->navigating=true;TreeView_SelectItem(state->navigation,state->route_items[40]);state->navigating=false;
                EnableWindow(state->navigation,FALSE);
                set_status(*state,L"Diagnosis only — this board needs preparation or matching files. Health Check does not repair, migrate or start the BBS.");
            }else if (state->system_recovery_only) {
                show_page(*state,7);
                state->navigating=true; TreeView_SelectItem(state->navigation,state->route_items[7]); state->navigating=false;
                EnableWindow(state->navigation,FALSE);
                refresh_security(*state);
                set_status(*state,L"Elevated offline SY: recovery only — Runtime must be stopped. Other administration remains owner-only.");
            } else {
                if(state->initial_page!=40){(void)load_editor_state(*state);populate_controls(*state);}
                navigate_route(*state,sfx::config_ui::route(state->initial_page).id);
                if(state->initial_account) open_user_record(*state,state->initial_account);
                if(state->initial_page!=40)refresh_backup_inventory(*state);
                if(!state->navigation_loading&&!state->prepare_notice.empty()){
                    if(state->mode!=State::ConnectionMode::blocked)set_status(*state,state->prepare_notice);
                    state->prepare_notice.clear();
                }
                SetTimer(window,config_timer::hosting,1000,nullptr);
            }
            return 0;
        }
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lparam);
            info->ptMinTrackSize.x = MulDiv(780, state->dpi, 96);
            info->ptMinTrackSize.y = MulDiv(610, state->dpi, 96);
            MONITORINFO monitor{sizeof(monitor)};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor)) {
                info->ptMinTrackSize.x = std::min(info->ptMinTrackSize.x,
                    monitor.rcWork.right - monitor.rcWork.left);
                info->ptMinTrackSize.y = std::min(info->ptMinTrackSize.y,
                    monitor.rcWork.bottom - monitor.rcWork.top);
            }
            return 0;
        }
        case WM_SIZE:
            layout(*state, LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_TIMER:
            if(wparam==config_timer::ftn_import){ftn_import_poll(*state);return 0;}
            if(wparam==config_timer::ftn_setup){ns_poll(*state);return 0;}
            if(wparam==config_timer::bbs_health){poll_bbs_health(*state);return 0;}
            if(wparam==config_timer::hosting){if(!state->hosting_busy&&(state->current_page==14||state->hosting_job))poll_hosting(*state);return 0;}
            if(wparam==config_timer::clam_job){poll_clam_job(*state);return 0;}
            if(wparam==config_timer::clam_maintenance){if(!clam_maintenance_active(*state)){KillTimer(window,config_timer::clam_maintenance);if(state->current_page==25)refresh_clam_page(*state);}return 0;}
            if (wparam==config_timer::botgate) { if(!state->navigation_loading)refresh_feed(*state); return 0; }
            if(wparam==config_timer::message_maintenance){
                if(!state->selected_job || state->mode!=State::ConnectionMode::online){KillTimer(window,config_timer::message_maintenance);return 0;}
                state->message_job_status=query_message_job(*state);render_message_job(*state);
                if(!state->message_job_status.available){KillTimer(window,config_timer::message_maintenance);set_status(*state,L"Outcome unconfirmed — message-maintenance status channel unavailable. No success claimed.");}
                else if(!state->message_job_status.state.active){KillTimer(window,config_timer::message_maintenance);EnableWindow(state->message_run_now,TRUE);set_status(*state,L"Finished — see the actual committed result/counts below; failure/partial/skipped is not success.");}
                return 0;
            }
            if (wparam==config_timer::backup && state->watching_backup) {
                if (!load_runtime_status(*state)) {
                    KillTimer(window,config_timer::backup);
                    state->watching_backup = false;
                    set_status(*state,
                        L"Backup continues in Runtime, but Configuration lost its status channel. Review Control Panel Events.");
                } else if (!state->scheduler.active &&
                           state->scheduler.last_run_utc > state->watched_last_run) {
                    KillTimer(window,config_timer::backup);
                    state->watching_backup = false;
                    refresh_backup_inventory(*state);
                    set_status(*state, state->scheduler.last_result ==
                            app::ScheduledJobResult::success
                        ? L"Completed — " + utf8_to_wide(state->scheduler.last_detail)
                        : L"Failed — " + utf8_to_wide(state->scheduler.last_detail));
                    if(!page_dirty(*state)) { populate_controls(*state); remember_page(*state); }
                }
            }
            return 0;
        case WM_COMMAND: {
            if(state->hosting_busy)return 0;
            const auto id = LOWORD(wparam);
            if(id==IDC_BBS_HEALTH_RUN){start_bbs_health(*state);return 0;}
            if(state->diagnostic_only&&id!=ID_FILE_EXIT&&id!=IDCANCEL&&id!=ID_HELP_ABOUT&&id!=win32::DesktopAppearance::classic_command&&id!=win32::DesktopAppearance::dark_command)return 0;
            if(state->navigation_loading&&id!=ID_FILE_EXIT&&id!=ID_HELP_ABOUT)return 0;
            if (state->system_recovery_only && id != IDC_SYSTEM_PASSWORD_APPLY &&
                id != IDC_SECURITY_REFRESH && id != ID_FILE_EXIT && id != ID_HELP_ABOUT &&
                id != win32::DesktopAppearance::classic_command && id != win32::DesktopAppearance::dark_command)
                return 0;
            if(clam_command(*state,id,HIWORD(wparam)))return 0;
            if(file_configuration_command(*state,id,HIWORD(wparam)))return 0;
            if(door_configuration_command(*state,id,HIWORD(wparam)))return 0;
            if(chat_configuration_command(*state,id,HIWORD(wparam)))return 0;
            if(ftn_configuration_command(*state,id,HIWORD(wparam)))return 0;
            if (id == win32::DesktopAppearance::classic_command ||
                    id == win32::DesktopAppearance::dark_command) {
                const auto theme = id == win32::DesktopAppearance::classic_command
                    ? win32::DesktopTheme::classic : win32::DesktopTheme::dark_retro;
                if (state->appearance.save(theme)) {
                    state->appearance.check_menu(state->theme_menu);
                    state->appearance.apply_controls(window);
                } else set_status(*state, L"The theme could not be saved. The previous theme remains selected.");
                return 0;
            }
            if(state->current_page==6 && !state->populating_user &&
                (HIWORD(wparam)==EN_CHANGE || id==IDC_USER_PURGE)) user_save_enabled(*state);
            if(id==IDC_SCHEDULE_JOB && HIWORD(wparam)==CBN_SELCHANGE){
                const auto chosen=SendMessageW(state->schedule_job,CB_GETCURSEL,0,0);SendMessageW(state->schedule_job,CB_SETCURSEL,state->selected_job,0);
                if(chosen<0 || chosen>2 || !resolve_dirty(*state))return 0;
                state->selected_job=static_cast<unsigned>(chosen);KillTimer(window,config_timer::message_maintenance);
                if(load_editor_state(*state))populate_controls(*state);remember_page(*state);return 0;
            }
            if(id==IDC_GATE_RULE_SELECT && HIWORD(wparam)==CBN_SELCHANGE) {
                const auto index=static_cast<unsigned>(SendMessageW(state->gate_rule_select,CB_GETCURSEL,0,0));
                if(!resolve_dirty(*state)) {
                    SendMessageW(state->gate_rule_select,CB_SETCURSEL,state->gate_rule_index,0);return 0;
                }
                if(index<6) { load_botgate_editor(*state,index); remember_page(*state); } return 0;
            }
            switch (id) {
                case IDC_UNREAD_MAIL_SAVE:(void)save_unread_mail_age(*state);return 0;
                case IDC_MESSAGE_RUN_NOW:run_message_job_now(*state);return 0;
                case IDC_MESSAGE_COMPACT:compact_messages_ui(*state);return 0;
                case IDC_MESSAGE_RECONCILE:reconcile_message_apply(*state);return 0;
                case IDC_SYSTEM_SYSOP_FIND:refresh_system_sysop(*state);return 0;
                case IDC_SYSTEM_SYSOP_NEXT:refresh_system_sysop(*state,true);return 0;
                case IDC_SYSTEM_SYSOP_RELOAD:if(resolve_dirty(*state)){refresh_system_sysop(*state);refresh_qwk_policy(*state);remember_page(*state);}return 0;
                case IDC_QWK_SAVE:save_qwk_policy(*state);return 0;
                case IDC_SYSTEM_SYSOP_ASSIGN:assign_system_sysop(*state);return 0;
                case IDC_LEVEL_MESSAGE_SAVE:save_message_limit(*state);if(action_succeeded(*state))for(auto& value:state->baseline)if(value.control==state->level_message_lines)value.value=text(state->level_message_lines);return 0;
                case IDC_MESSAGE_RELOAD:if(resolve_dirty(*state)){state->message_group_cursor={};refresh_message_configuration(*state);remember_page(*state);}return 0;
                case IDC_MESSAGE_NEW_GROUP:case IDC_MESSAGE_NEW_ECHO:
                    if(resolve_dirty(*state)){state->message_insert_anchor.reset();state->editing_echo=id==IDC_MESSAGE_NEW_ECHO;state->editing_message_group.reset();state->editing_message_echo.reset();populate_message_form(*state);remember_page(*state);SetFocus(state->message_name);}return 0;
                case IDC_MESSAGE_INSERT:insert_message_echo(*state);return 0;
                case IDC_MESSAGE_MOVE_UP:case IDC_MESSAGE_MOVE_DOWN:case IDC_MESSAGE_DELETE:
                    if(id==IDC_MESSAGE_DELETE && !state->editing_echo){delete_message_group(*state);return 0;}
                    operate_message_echo(*state,id==IDC_MESSAGE_DELETE?app::MessageConfigurationAction::delete_echo:id==IDC_MESSAGE_MOVE_UP?app::MessageConfigurationAction::move_echo_up:app::MessageConfigurationAction::move_echo_down);return 0;
                case IDC_MESSAGE_GROUPS:
                    if(HIWORD(wparam)==LBN_SELCHANGE && !state->message_populating){const auto chosen=SendMessageW(state->message_groups,LB_GETCURSEL,0,0);SendMessageW(state->message_groups,LB_SETCURSEL,state->message_group_index,0);
                        if(chosen<0 || static_cast<std::size_t>(chosen)>=state->message_catalog.catalog.groups.size())return 0;
                        const auto selected=state->message_catalog.catalog.groups[static_cast<std::size_t>(chosen)];
                        if(resolve_dirty(*state)){const auto& groups=state->message_catalog.catalog.groups;const auto found=std::find_if(groups.begin(),groups.end(),[&](const app::MessageGroup& g){return g.id==selected.id && g.revision==selected.revision;});
                            if(found==groups.end()){set_status(*state,L"Selection changed during Save/Discard. Reload and choose explicitly; no row was substituted.");return 0;}
                            state->message_insert_anchor.reset();state->message_group_index=static_cast<int>(found-groups.begin());SendMessageW(state->message_groups,LB_SETCURSEL,state->message_group_index,0);state->editing_echo=false;state->editing_message_group=*found;state->editing_message_echo.reset();state->message_echo_cursor={};load_message_echoes(*state);populate_message_form(*state);remember_page(*state);}}
                    return 0;
                case IDC_MESSAGE_ECHOES:
                    if(HIWORD(wparam)==LBN_SELCHANGE && !state->message_populating){const auto chosen=SendMessageW(state->message_echoes,LB_GETCURSEL,0,0);SendMessageW(state->message_echoes,LB_SETCURSEL,state->message_echo_index,0);
                        if(chosen<0 || static_cast<std::size_t>(chosen)>=state->message_echo_catalog.catalog.echoes.size())return 0;
                        const auto selected=state->message_echo_catalog.catalog.echoes[static_cast<std::size_t>(chosen)];
                        if(resolve_dirty(*state)){const auto& echoes=state->message_echo_catalog.catalog.echoes;const auto found=std::find_if(echoes.begin(),echoes.end(),[&](const app::LocalEcho& e){return e.id==selected.id && e.revision==selected.revision;});
                            if(found==echoes.end()){set_status(*state,L"Selection changed during Save/Discard. Reload and choose explicitly; no row was substituted.");return 0;}
                            state->message_insert_anchor.reset();state->message_echo_index=static_cast<int>(found-echoes.begin());SendMessageW(state->message_echoes,LB_SETCURSEL,state->message_echo_index,0);state->editing_echo=true;state->editing_message_echo=*found;populate_message_form(*state);remember_page(*state);}}
                    return 0;
                case IDC_MESSAGE_GROUP_NEXT:
                    if(state->message_catalog.catalog.next){const auto cursor=*state->message_catalog.catalog.next;const auto editing=state->editing_message_echo;
                        if(resolve_dirty(*state)){if(cursor.configuration_revision!=state->message_catalog.catalog.revision){set_status(*state,L"Catalog changed during Save/Discard. Review the refreshed page before advancing.");return 0;}
                            state->message_group_cursor=cursor;refresh_message_configuration(*state);
                            if(editing && state->message_catalog.code==app::LocalAccountResultCode::ok){state->editing_echo=true;state->editing_message_echo=editing;populate_message_form(*state);}remember_page(*state);}}return 0;
                case IDC_MESSAGE_ECHO_NEXT:
                    if(state->message_echo_catalog.catalog.next){const auto cursor=*state->message_echo_catalog.catalog.next;
                        if(resolve_dirty(*state)){if(cursor.configuration_revision!=state->message_catalog.catalog.revision){set_status(*state,L"Catalog changed during Save/Discard. Review the refreshed page before advancing.");return 0;}
                            state->message_echo_cursor=cursor;load_message_echoes(*state);remember_page(*state);}}return 0;
                case IDC_MESSAGE_SAVE:save_current_page(*state);return 0;
                case IDC_PRIVATE_PIPE_COLORS:
                    if (HIWORD(wparam) == BN_CLICKED && state->current_page == 17 && IsWindowEnabled(state->private_pipe_colors))
                        toggle_private_pipe_colors(*state);
                    return 0;
                case IDC_MESSAGE_CLONE_SOURCE:
                    if(state->editing_echo && state->editing_message_echo && resolve_dirty(*state) && state->editing_message_echo){state->clone_source=state->editing_message_echo;state->clone_targets.clear();state->clone_echo_cursor={state->message_catalog.catalog.revision};
                        SetWindowTextW(state->clone_source_text,(L"Source: "+utf8_to_wide(state->clone_source->name)+L" (#"+std::to_wstring(state->clone_source->conference_number)+L")").c_str());
                        for(HWND box:{state->clone_access,state->clone_reading,state->clone_posting,state->clone_max_count,state->clone_max_age})Button_SetCheck(box,BST_UNCHECKED);
                        navigate_route(*state,L"messages.clone_permissions");}return 0;
                case IDC_MESSAGE_CLONE_GROUP:if(HIWORD(wparam)==CBN_SELCHANGE)load_clone_targets(*state);return 0;
                case IDC_MESSAGE_CLONE_NEXT:load_clone_targets(*state,true);return 0;
                case IDC_MESSAGE_CLONE_ADD:add_clone_targets(*state);clone_preview(*state);return 0;
                case IDC_MESSAGE_CLONE_CLEAR:state->clone_targets.clear();clone_preview(*state);return 0;
                case IDC_MESSAGE_CLONE_PREVIEW:case IDC_MESSAGE_CLONE_ACCESS:case IDC_MESSAGE_CLONE_READING:case IDC_MESSAGE_CLONE_POSTING:case IDC_CLONE_MAX_COUNT:case IDC_CLONE_MAX_AGE:clone_preview(*state);return 0;
                case IDC_MESSAGE_CLONE_APPLY:apply_clone(*state);if(action_succeeded(*state))remember_page(*state);return 0;
                case IDC_OPENING_SAVE:save_current_page(*state);return 0;
                case IDC_OPENING_RELOAD:
                    if(resolve_dirty(*state)) {refresh_classic_information(*state);remember_page(*state);}return 0;
                case IDC_MENU_SAVE: save_current_page(*state); return 0;
                case IDC_MENU_TEXT: case IDC_MENU_INPUT: case IDC_MENU_FOREGROUND: case IDC_MENU_BACKGROUND:
                case IDC_MENU_SELECT: case IDC_MENU_FORMAT: case IDC_MENU_FOOTER:
                    if(HIWORD(wparam)==CBN_SELCHANGE) refresh_menu_preview(*state); return 0;
                case IDC_MENU_LEVEL:
                    if(HIWORD(wparam)==EN_CHANGE) refresh_menu_preview(*state); return 0;
                case IDC_MENU_BARS: case IDC_MENU_SYSOP: case IDC_MENU_ARTWORK: case IDC_MENU_REFRESH:
                    refresh_menu_preview(*state); return 0;
                case IDC_MENU_FOLDER: {
                    const auto folder=state->root/L"Display";
                    const auto verified=win32::Win32TrustedDisplayAssetReader::open(folder.wstring());
                    if(!verified || reinterpret_cast<INT_PTR>(ShellExecuteW(window,L"open",folder.c_str(),nullptr,nullptr,SW_SHOWNORMAL))<=32)
                        set_status(*state,L"Display folder could not be opened safely.");
                    return 0;
                }
                case IDC_LEVEL_LIST:
                    if (HIWORD(wparam) == LBN_SELCHANGE) {
                        const auto chosen=SendMessageW(state->level_list,LB_GETCURSEL,0,0);
                        SendMessageW(state->level_list,LB_SETCURSEL,state->selected_level_index,0);
                        if(resolve_dirty(*state)) {SendMessageW(state->level_list,LB_SETCURSEL,chosen,0);select_level(*state);remember_page(*state);}
                    } return 0;
                case IDC_LEVEL_REFRESH: if(resolve_dirty(*state)) { refresh_levels(*state); remember_page(*state); } return 0;
                case IDC_LEVEL_SAVE: save_current_page(*state); return 0;
                case IDC_LEVEL_RETIRE:
                    if(!resolve_dirty(*state)) return 0;
                    set_status(*state,L"Refused — no level removed.");
                    apply_level_change(*state,app::LocalSecurityAction::retire_level);
                    if(action_succeeded(*state)) remember_page(*state); return 0;
                case IDC_LEVEL_DEFAULTS:
                    if(resolve_dirty(*state)) {
                        state->selected_level_index=-1;SendMessageW(state->level_list,LB_SETCURSEL,static_cast<WPARAM>(-1),0);
                        for(HWND field:{state->level_number,state->level_label,state->level_description}) SetWindowTextW(field,L"");
                        for(HWND field:{state->level_number,state->level_label,state->level_description}) EnableWindow(field,TRUE);
                        for(HWND field:{state->level_call_minutes,state->level_day_minutes,state->level_chat_minutes,state->level_chat_day_minutes}) {SetWindowTextW(field,L"0");EnableWindow(field,TRUE);}
                        SetWindowTextW(state->level_message_lines,L"");EnableWindow(state->level_message_lines,FALSE);EnableWindow(state->level_message_save,FALSE);
                        remember_page(*state);SetFocus(state->level_number);
                    } return 0;
                case IDC_ACCOUNT_FIND: if(resolve_dirty(*state)) { refresh_accounts(*state, true); remember_page(*state); } return 0;
                case IDC_ACCOUNT_PREVIOUS: navigate_user(*state,false); return 0;
                case IDC_ACCOUNT_NEXT: navigate_user(*state,true); return 0;
                case IDC_USER_NEW:
                    if(resolve_dirty(*state)) {
                        const auto available=query_accounts(*state,{});
                        if(available.code!=app::LocalAccountResultCode::ok) return 0;
                        state->account_page.policy=available.policy;
                        state->populating_accounts=true;
                        ListView_SetItemState(state->account_list,-1,0,LVIS_SELECTED|LVIS_FOCUSED);
                        state->populating_accounts=false;
                        state->account_selection={};state->creating_user=true;populate_user_editor(*state);
                    } return 0;
                case IDC_USER_RELOAD:
                    if(resolve_dirty(*state)) {
                        if(state->account_selection.accounts.size()==1) reload_selected_user(*state,state->account_selection.accounts[0].account_id);
                        else {state->creating_user=false;refresh_accounts(*state,true);}
                    } return 0;
                case IDC_USER_DELETE: user_consequential_action(*state,app::LocalAccountAction::tombstone);return 0;
                case IDC_USER_TOGGLE:
                    if(state->account_selection.accounts.size()==1) user_consequential_action(*state,
                        state->account_selection.accounts[0].state==app::AccountState::active ? app::LocalAccountAction::lock : app::LocalAccountAction::unlock);
                    return 0;
                case IDC_USER_RESET: reset_user_password(*state);return 0;
                case IDC_ACCOUNT_APPLY: case IDC_SYSTEM_PASSWORD_APPLY: save_current_page(*state); return 0;
                case IDC_PASSWORD_POLICY_SAVE: if(save_configuration_page(*state,16)) remember_page(*state); return 0;
                case IDC_CONNECTION_SAVE: if(save_configuration_page(*state,9)) remember_page(*state); return 0;
                case IDC_GATE_SAVE: save_current_page(*state); return 0;
                case IDC_GATE_RULE_RELOAD:
                    if(resolve_dirty(*state)) { load_botgate_editor(*state,state->gate_rule_index); remember_page(*state); }
                    return 0;
                case IDC_GATE_RULE_SAVE: {
                    save_rule(*state);
                    return 0;
                }
                case IDC_LOGGING_SAVE: if(save_configuration_page(*state,12)) remember_page(*state); return 0;
                case IDC_GATE_UPDATE: {
                    app::LocalControlFrame response;
                    if (!online_exchange(*state,request(*state,app::LocalControlMessageType::botgate_feed_update_request),
                            app::LocalControlMessageType::botgate_feed_update_response,response))
                        set_status(*state,L"Unavailable — running Runtime connection required for feed update.");
                    else {
                        const auto result=app::decode_control_response(response.payload);
                        set_status(*state,result && result->code==app::LocalControlResponseCode::ok ? L"Running — threat feed update requested." : L"Refused — feed update not started; another update may be running.");
                    }
                    refresh_feed(*state); return 0;
                }
                case IDC_THROTTLE_CLEAR: clear_local_throttle(*state); return 0;
                case IDC_THROTTLE_SCOPE:
                    if (HIWORD(wparam) == CBN_SELCHANGE) {
                        SetWindowTextW(state->throttle_subject,L"");
                        const auto selection = SendMessageW(state->throttle_scope,CB_GETCURSEL,0,0);
                        EnableWindow(state->throttle_subject,!state->system_recovery_only &&
                            state->mode != State::ConnectionMode::blocked && selection >= 0 && selection < 2);
                    }
                    return 0;
                case IDC_SECURITY_REFRESH: if(resolve_dirty(*state)) { refresh_security(*state); remember_page(*state); } return 0;
                case IDC_RUN_MODE:
                case IDC_RUN_AUTOMATIC:refresh_hosting(*state,false);return 0;
                case IDC_RUN_POWER:
                    if(HIWORD(wparam)==BN_CLICKED&&reinterpret_cast<HWND>(lparam)==state->run_power&&IsWindowEnabled(state->run_power)&&
                        (InSendMessage()||static_cast<LONG>(static_cast<DWORD>(GetMessageTime())-state->hosting_valid_after)>=0))
                        PostMessageW(window,WM_APP+43,state->hosting_epoch,state->hosting_action);
                    return 0;
                case IDC_RUNTIME_RETRY:reload_runtime_context(*state);return 0;
                case IDC_RUN_MODE_APPLY:save_hosting(*state);return 0;
                case IDC_RUN_MODE_RELOAD:
                    if(hosting_dirty(*state)&&MessageBoxW(state->window,L"Reload confirmed hosting settings and discard only the pending hosting choices? Idle-setting edits are retained.",L"Reload Run Mode",MB_YESNO|MB_DEFBUTTON2|MB_ICONQUESTION)!=IDYES)return 0;
                    state->hosting_review_required=false;
                    if(refresh_hosting(*state,true)){hosting_baseline(*state);set_status(*state,L"Hosting settings reloaded from protected metadata and Windows. BBS was not started.");}return 0;
                case IDC_RUNTIME_SAVE:if(save_configuration_page(*state,41))remember_page(*state);return 0;
                case IDC_SYSTEM_SAVE: case IDC_REGIONAL_SAVE: case IDC_REGISTRATION_SAVE:
                    if(state->current_page==0&&(state->pending_qwk_policy||qwk_policy_dirty(*state))){set_status(*state,L"Use Save QWK / Reconcile QWK first. BBS-name and QWK drafts retained separately.");return 0;}
                    if(save_configuration_page(*state,state->current_page)) remember_page(*state); return 0;
                case IDC_OPEN_LEVELS: navigate_route(*state,L"users.security_levels"); return 0;
                case IDC_NODES_SAVE: if(save_configuration_page(*state,1)) remember_page(*state); return 0;
                case IDC_SERVERS_SAVE: if(save_configuration_page(*state,2)) remember_page(*state); return 0;
                case IDC_CLIENT_BROWSE: browse_external_client(*state); return 0;
                case IDC_BACKUP_BROWSE: browse_backup_destination(*state); return 0;
                case IDC_CLIENT_SAVE: save_current_page(*state); return 0;
                case IDC_CLIENT_CLEAR:
                    if (clear_client_profile(state->root)) {
                        SetWindowTextW(state->client_path, L"");
                        SetWindowTextW(state->client_arguments, L"");
                        SendMessageW(state->client_type, CB_SETCURSEL, 0, 0);
                        set_status(*state,
                            L"Applied — this Windows operator's saved client profile was cleared.");
                    } else set_status(*state, L"Refused — the saved client profile could not be cleared.");
                    return 0;
                case IDC_BACKUP_RUN: run_backup_now(*state); return 0;
                case IDC_BACKUP_VERIFY: verify_selected_backup(*state); return 0;
                case IDC_BACKUP_RESTORE: restore_selected_backup(*state); return 0;
                case IDC_FTN_HISTORY_RECOVER: recover_ftn_history(*state); return 0;
                case IDC_FTN_HISTORY_OPEN: activate_page(*state,4); set_status(*state,L"Stop the BBS, Control Panel and any independent BinkD, then choose Recover FTN History."); return 0;
                case IDC_BACKUP_CONTENTS: restore_selected_contents(*state); return 0;
                case IDC_BACKUP_REFRESH: refresh_backup_inventory(*state); return 0;
                case IDC_SCHEDULE_SAVE: save_current_page(*state); return 0;
                case IDC_EXTERNAL_EVENT_NEW: if(resolve_dirty(*state)) fill_external_event(*state, {}); return 0;
                case IDC_EXTERNAL_EVENT_SAVE: save_external_event(*state, false); return 0;
                case IDC_EXTERNAL_EVENT_DELETE: save_external_event(*state, true); return 0;
                case IDC_EXTERNAL_EVENT_EXCLUSIVE: update_external_event_controls(*state); return 0;
                case IDC_EXTERNAL_EVENT_TIMING: update_external_event_controls(*state); return 0;
                case IDC_EXTERNAL_EVENT_PROGRAM: browse_external_event(*state, false); return 0;
                case IDC_EXTERNAL_EVENT_DIRECTORY: browse_external_event(*state, true); return 0;
                case IDC_EXTERNAL_EVENT_MONTHS: pick_external_calendar(state->external.months, state->external.month_mask, 12); return 0;
                case IDC_EXTERNAL_EVENT_DATES: pick_external_calendar(state->external.dates, state->external.date_mask, 31); return 0;
                case IDC_EXTERNAL_EVENT_WEEKDAYS: pick_external_calendar(state->external.weekdays, state->external.weekday_mask, 7); return 0;
                case ID_FILE_EXIT: SendMessageW(window, WM_CLOSE, 0, 0); return 0;
                case IDCANCEL: SendMessageW(window, WM_CLOSE, 0, 0); return 0;
                case ID_HELP_ABOUT:
                    MessageBoxW(window,
                        L"Spitfire-X BBS Configuration\nVersion "
                        SFX_PRODUCT_VERSION_SHORT_WSTRING L"\n\nPersistent BBS settings and administration.",
                        kWindowTitle, MB_OK | MB_ICONINFORMATION);
                    return 0;
                default: break;
            }
            break;
        }
        case WM_NOTIFY: {
            if(state->hosting_busy)return TRUE;
            if(const auto handled=ftn_configuration_notify(*state,reinterpret_cast<NMHDR*>(lparam)))return *handled;
            if(const auto result=door_navigation_notify(*state,reinterpret_cast<NMHDR*>(lparam)))return *result;
            if(const auto result=door_configuration_notify(*state,reinterpret_cast<NMHDR*>(lparam)))return *result;
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header && header->idFrom == IDC_EXTERNAL_EVENT_LIST && !state->external.filling) {
                const auto* change = reinterpret_cast<const NMLISTVIEW*>(lparam);
                if ((header->code == LVN_ITEMCHANGING || header->code == LVN_ITEMCHANGED) &&
                    (change->uChanged & LVIF_STATE) && (change->uNewState & LVIS_SELECTED) &&
                    !(change->uOldState & LVIS_SELECTED)) {
                    if (header->code == LVN_ITEMCHANGING) return resolve_dirty(*state) ? FALSE : TRUE;
                    if (change->iItem >= 0 && static_cast<std::size_t>(change->iItem) < state->external.catalog.events.size())
                        fill_external_event(*state, state->external.catalog.events[change->iItem]);
                    return 0;
                }
            }
            if(header && header->idFrom==IDC_BBS_SECURITY_TABS) {
                if(header->code==TCN_SELCHANGING) return resolve_dirty(*state) ? FALSE : TRUE;
                if(header->code==TCN_SELCHANGE) {
                    const int tab=TabCtrl_GetCurSel(state->security_tabs);
                    if(tab>=0 && tab<(win32::clamav_available()?4:3)) activate_page(*state,sfx::config_ui::security_tab_pages[tab]);
                    return 0;
                }
            }
            if(header && header->idFrom==IDC_USER_TABS && header->code==TCN_SELCHANGE) {user_tab(*state);return 0;}
            if(header && header->idFrom==IDC_NAVIGATION && !state->navigating) {
                if(header->code==TVN_SELCHANGINGW) return resolve_dirty(*state) ? FALSE : TRUE;
                if(header->code==TVN_SELCHANGEDW) {
                    const auto* selection=reinterpret_cast<NMTREEVIEWW*>(lparam);
                    if(ftn_navigation_open(*state,selection->itemNew.hItem))return 0;
                    if(door_navigation_open(*state,selection->itemNew.hItem))return 0;
                    if(selection->itemNew.lParam==26){state->doors.menu=0;state->doors.view=DoorPage::categories;}
                    activate_page(*state,static_cast<int>(selection->itemNew.lParam)); return 0;
                }
            }
            if(header && header->idFrom==IDC_ACCOUNT_LIST && !state->populating_accounts) {
                const auto* change=reinterpret_cast<NMLISTVIEW*>(lparam);
                if(header->code==LVN_ITEMCHANGING && (change->uChanged&LVIF_STATE) &&
                    (change->uOldState&LVIS_SELECTED) && !(change->uNewState&LVIS_SELECTED))
                    return resolve_dirty(*state) ? FALSE : TRUE;
                if(header->code==LVN_ITEMCHANGED && (change->uChanged&LVIF_STATE) &&
                    !(change->uOldState&LVIS_SELECTED) && (change->uNewState&LVIS_SELECTED)) {select_account(*state);return 0;}
            }
            if (header != nullptr && header->idFrom == IDC_BACKUP_LIST &&
                header->code == NM_DBLCLK) {
                verify_selected_backup(*state);
                return 0;
            }
            break;
        }
        case WM_ERASEBKGND: {
            RECT area{}; GetClientRect(window, &area);
            FillRect(reinterpret_cast<HDC>(wparam), &area, state->appearance.background_brush());
            return 1;
        }
        case WM_CLOSE:
            if(state->hosting_busy||state->hosting_job){set_status(*state,L"Wait for the current hosting action to finish before closing.");return 0;}
            if(clam_busy(*state)){state->close_after_clam=true;state->clam_job->cancel=true;set_status(*state,L"Closing after ClamAV reaches its safe cancellation/child-exit boundary...");return 0;}
            if(!resolve_dirty(*state)) return 0;
            if(state->door_bulletins&&!state->door_bulletins->confirm_close())return 0;
            persist_route(*state);
            (void)state->placement.save(window);
            DestroyWindow(window);
            return 0;
        case WM_QUERYENDSESSION:
            if(state->hosting_busy||state->hosting_job)return FALSE;
            if(!resolve_dirty(*state)) return FALSE;
            if(state->door_bulletins&&!state->door_bulletins->confirm_close())return FALSE;
            persist_route(*state);
            (void)state->placement.save(window);
            return TRUE;
        case WM_EXITSIZEMOVE:
            (void)state->placement.save(window);
            return 0;
        case WM_DESTROY:
            state->navigation_worker.reset();
            state->hosting_job.reset(); // Its bounded worker joins before State can die.
            KillTimer(window,config_timer::backup);
            KillTimer(window,config_timer::hosting);
            PostQuitMessage(0);
            return 0;
        default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

HMENU create_menu(State& state) {
    const HMENU menu = CreateMenu();
    const HMENU file = CreatePopupMenu();
    const HMENU help = CreatePopupMenu();
    if (!menu || !file || !help) return nullptr;
    AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L"E&xit");
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"&About...");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"&File");
    const HMENU view = CreatePopupMenu();
    state.theme_menu = state.appearance.append_menu(view);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"&View");
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"&Help");
    return menu;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
    if(const auto worker=win32::portable_preparation_worker(command_line?command_line:L""))return *worker;
    if(const auto worker=win32::service_mode_worker(command_line?command_line:L""))return *worker;
    State state;
    state.instance = instance;
    try {
        state.root = win32::alpha_installation_root();
        install::require(GetFileAttributesW((state.root/L"Bin"/L"preparation.pending").c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"preparation_incomplete_retry");
        const auto registration=install::read_service_registration(state.root,true);
        win32::configure_service_secret_scope(registration&&registration->enabled);
        state.system_recovery_only = install::elevated();
        if (state.system_recovery_only) {
            state.receipt = install::verify_configuration_recovery_entry(state.root);
            state.owner = state.receipt.owner;
        } else {
            state.owner = win32::current_process_user_sid_string();
            state.receipt = install::read_receipt(state.root, state.owner);
            install::verify_structure(state.receipt);
            install::verify_installed_program_process(state.receipt,
                GetCurrentProcessId(), install::configuration_program_index);
        }
    } catch (const std::exception& error) {
        try{
            install::require(!install::elevated(),"diagnosis_requires_ordinary_current_account");
            const auto prepared=win32::offer_portable_preparation(install::configuration_program_index);
            if(prepared.confirmed){
                state.owner=win32::current_process_user_sid_string();state.receipt=install::read_receipt(state.root,state.owner);install::verify_structure(state.receipt);
                const auto registration=install::read_service_registration(state.root,true);win32::configure_service_secret_scope(registration&&registration->enabled);state.system_recovery_only=false;
                state.prepare_notice=prepared.detail.empty()?L"Prepare SFX completed. The BBS remains stopped.":
                    utf8_to_wide(prepared.detail);
            }else{
                if(!prepared.cancelled&&!prepared.detail.empty())MessageBoxW(nullptr,(L"SFX remains stopped. BBS Health Check is available below.\n\n"+utf8_to_wide(prepared.detail)).c_str(),kWindowTitle,MB_OK|MB_ICONINFORMATION);
            // Validate this actual Config image independently of missing peer
            // files or historical host bindings. No normal editor/storage starts.
            state.receipt=install::verify_portable_entry(state.root,install::configuration_program_index);
            state.owner=win32::current_process_user_sid_string();state.system_recovery_only=false;
            state.diagnostic_only=true;state.initial_page=40;
            }
        }catch(...){
            const auto message = L"The configuration utility is missing or does not match this installation. "
                L"Close SFX and run the matching Setup Repair.\n\n" + utf8_to_wide(error.what());
            MessageBoxW(nullptr, message.c_str(), kWindowTitle, MB_OK | MB_ICONERROR);return 8;
        }
    }
    if(!state.system_recovery_only&&!state.diagnostic_only) {
        HKEY key{};
        if(RegOpenKeyExW(HKEY_CURRENT_USER,operator_registry_key(state.root).c_str(),0,KEY_QUERY_VALUE,&key)==ERROR_SUCCESS) {
            const auto saved=registry_string(key,L"ConfigurationRoute",REG_SZ,512);
            if(saved) state.initial_page=sfx::config_ui::route(*saved).page;
            RegCloseKey(key);
        }
    }
    load_navigation_width(state);
    const bool requested_botgate=command_line && wcscmp(command_line,L"--botgate")==0 && !state.system_recovery_only&&!state.diagnostic_only;
    const bool requested_ftn=command_line && wcscmp(command_line,L"--ftn")==0 && !state.system_recovery_only&&!state.diagnostic_only;
    state.initial_account=state.system_recovery_only||state.diagnostic_only ? 0 : win32::edit_user_argument(command_line ? command_line : L"");
    if (requested_ftn) state.initial_page=29;
    else if (requested_botgate) state.initial_page=sfx::config_ui::route(L"security.botgate").page;
    else if(state.initial_account) state.initial_page=6;
    else if(state.diagnostic_only)state.initial_page=40;
    else {
#ifndef SFX_ALPHA_TEST_AUTOMATION
    if (command_line != nullptr && *command_line != L'\0') {
        MessageBoxW(nullptr, L"Unknown configuration option or invalid user number.",
            kWindowTitle, MB_OK | MB_ICONERROR);
        return 2;
    }
#else
    (void)command_line;
#endif
    }
    const auto names = win32::installation_object_names(state.root);
    state.instance_mutex = CreateMutexW(nullptr, FALSE, names.configuration_mutex_name.c_str());
    const DWORD instance_mutex_error = GetLastError();
    if (state.instance_mutex == nullptr) return 3;
    if (instance_mutex_error == ERROR_ALREADY_EXISTS) {
        CloseHandle(state.instance_mutex);
        state.instance_mutex = nullptr;
        const auto existing = installed_window(state.receipt, kWindowClass,
            install::configuration_program_index);
        if (existing.result != nullptr) {
            if (requested_botgate) PostMessageW(existing.result,RegisterWindowMessageW(L"SFXConfig.ShowBotGate.v1"),0,0);
            if(requested_ftn)PostMessageW(existing.result,RegisterWindowMessageW(L"SFXConfig.ShowFTN.v1"),0,0);
            if(state.initial_account) PostMessageW(existing.result,RegisterWindowMessageW(win32::edit_user_message),
                static_cast<WPARAM>(static_cast<std::uint64_t>(state.initial_account)&0xffffffffu),
                static_cast<LPARAM>(static_cast<std::uint64_t>(state.initial_account)>>32));
            if (IsIconic(existing.result)) ShowWindow(existing.result, SW_RESTORE);
            ShowWindow(existing.result, SW_SHOW);
            SetForegroundWindow(existing.result);
        } else {
            MessageBoxW(nullptr,
                L"Configuration is already open for this installation, but its window could not be safely identified.",
                kWindowTitle, MB_OK | MB_ICONINFORMATION);
        }
        return 0;
    }
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    if (!InitCommonControlsEx(&controls)) {
        CloseHandle(state.instance_mutex);
        return 4;
    }
    if (const HDC screen = GetDC(nullptr)) {
        state.dpi = std::max(96, GetDeviceCaps(screen, LOGPIXELSX));
        ReleaseDC(nullptr, screen);
    }
    win32::WindowIcon large_icon(instance, IDI_SFX_CONFIG,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    win32::WindowIcon small_icon(instance, IDI_SFX_CONFIG,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON));
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.style = CS_HREDRAW | CS_VREDRAW;
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = instance;
    window_class.hIcon = large_icon.get();
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kWindowClass;
    window_class.hIconSm = small_icon.get();
    if (!RegisterClassExW(&window_class)) {
        CloseHandle(state.instance_mutex);
        return 5;
    }
    const auto saved_placement = state.placement.load(
        MulDiv(780, state.dpi, 96), MulDiv(610, state.dpi, 96));
    const RECT placement = saved_placement ? saved_placement->rectangle :
        RECT{CW_USEDEFAULT, CW_USEDEFAULT, 0, 0};
    const HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, placement.left, placement.top,
        saved_placement ? placement.right - placement.left : MulDiv(940, state.dpi, 96),
        saved_placement ? placement.bottom - placement.top : MulDiv(700, state.dpi, 96),
        nullptr, create_menu(state), instance, &state);
    if (window == nullptr) {
        CloseHandle(state.instance_mutex);
        return 6;
    }
    ShowWindow(window, saved_placement && saved_placement->maximized ? SW_SHOWMAXIMIZED : show);
    UpdateWindow(window);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if(state.door_bulletins&&state.door_bulletins->translate(message))continue;
        // This is a window with a menu, not a dialog resource. Keep Alt menu
        // mnemonics and F10 on DefWindowProc's menu path; IsDialogMessage is
        // used only for normal form navigation (Tab, arrows and Enter).
        const bool menu_key = message.message == WM_SYSKEYDOWN ||
            message.message == WM_SYSKEYUP || message.message == WM_SYSCHAR ||
            (message.message == WM_KEYDOWN && message.wParam == VK_F10);
        if (menu_key || !IsDialogMessageW(window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    if (state.ui_font) DeleteObject(state.ui_font);
    if (state.heading_font) DeleteObject(state.heading_font);
    if (state.instance_mutex) CloseHandle(state.instance_mutex);
    return static_cast<int>(message.wParam);
}
