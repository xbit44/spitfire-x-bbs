#include "sfx/application/chat_notice.hpp"
#include "resource.h"
#include "../product_version.h"

#include "sfx/application/local_control_protocol.hpp"
#include "sfx/application/external_telnet_client.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/portable_preparation.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/service_secrets.hpp"
#include "sfx/platform/win32/control_panel_metrics.hpp"
#include "sfx/platform/win32/bbs_log_timestamp.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/window_icon.hpp"
#include "sfx/platform/win32/desktop_theme.hpp"
#include "sfx/platform/win32/configuration_link.hpp"
#include "sfx/platform/win32/event_sound_player.hpp"
#include "sfx/platform/win32/secure_identifier_generator.hpp"

#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <richedit.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <deque>
#ifdef SFX_ALPHA_TEST_AUTOMATION
#include <fstream>
#include <iomanip>
#include <sstream>
#endif
#include <limits>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

namespace app = sfx::application;
namespace win32 = sfx::platform::win32;

constexpr wchar_t kWindowClass[] = L"SFXControlPanelWindow";
constexpr wchar_t kWindowTitle[] =
    SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE L" - Control Panel";
constexpr wchar_t kTerminalClass[] = L"SFXControlPanelTerminal";
constexpr wchar_t kMonitorClass[] = L"SFXControlPanelSystemMonitor";
constexpr wchar_t kStatisticsClass[] = L"SFXControlPanelStatisticsGrid";
win32::DesktopAppearance g_appearance(L"Software\\Spitfire-X BBS\\Appearance", true);
const COLORREF& kBackgroundColor = g_appearance.palette().background;
const COLORREF& kPanelColor = g_appearance.palette().panel;
const COLORREF& kSurfaceColor = g_appearance.palette().surface;
const COLORREF& kAlternateSurfaceColor = g_appearance.palette().alternate;
const COLORREF& kTextColor = g_appearance.palette().text;
const COLORREF& kMutedTextColor = g_appearance.palette().muted;
const COLORREF& kBorderColor = g_appearance.palette().border;
const COLORREF& kGridColor = g_appearance.palette().grid;
const COLORREF& kGoldColor = g_appearance.palette().gold;
const COLORREF& kGreenColor = g_appearance.palette().green;
const COLORREF& kCyanColor = g_appearance.palette().cyan;
const COLORREF& kAmberColor = g_appearance.palette().amber;
const COLORREF& kErrorColor = g_appearance.palette().error;
const COLORREF& kSelectionColor = g_appearance.palette().selection;
constexpr UINT kRefreshTimer = 1;
constexpr UINT kPulseTimer = 4;
constexpr UINT kStartupActivityTimer = 5;
constexpr UINT kStartupActivityIntervalMs = 350;
constexpr UINT kLogPageTimer = 20;
#ifdef SFX_ALPHA_TEST_AUTOMATION
constexpr UINT kAutomationTimer = 2;
constexpr UINT kWizardAutomationTimer = 3;
#endif
constexpr UINT kOpenSetupMessage = WM_APP + 1;
constexpr UINT kCloseNodeTabMessage = WM_APP + 2;
constexpr UINT kConfigurationAdminMessage = WM_APP + 3;
constexpr UINT kServiceButtonMessage = WM_APP + 4;
constexpr UINT kStartupMessage = WM_APP + 5;
constexpr UINT kStartupNodesMessage = WM_APP + 6;

#ifdef SFX_ALPHA_TEST_AUTOMATION
UINT g_automation_dpi{0};
bool g_suppress_external_client_warning{false};
#endif

UINT window_dpi(const HWND window) noexcept {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if (g_automation_dpi != 0) return g_automation_dpi;
#endif
    using GetDpiForWindowFunction = UINT (WINAPI*)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpiForWindowFunction>(
        GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
    if (get_dpi != nullptr) {
        const UINT dpi = get_dpi(window);
        if (dpi != 0) return dpi;
    }
    const HDC dc = GetDC(window);
    const int dpi = dc != nullptr ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc != nullptr) ReleaseDC(window, dc);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96U;
}

int scaled(const HWND window, const int logical) noexcept {
    return MulDiv(logical, static_cast<int>(window_dpi(window)), 96);
}

std::optional<app::ControlPanelTerminalSnapshot> load_owner_ansi(
    const HINSTANCE instance, const UINT identifier) noexcept {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (resource == nullptr) return std::nullopt;
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const auto* bytes = loaded != nullptr
        ? static_cast<const char*>(LockResource(loaded)) : nullptr;
    if (bytes == nullptr || size == 0) return std::nullopt;
    return app::parse_control_panel_ansi_asset(std::string_view(bytes, size));
}

using EmbeddedCp437Font = std::array<std::uint8_t, 256U * 16U>;

std::optional<EmbeddedCp437Font> load_embedded_cp437_font(
    const HINSTANCE instance, const UINT identifier) noexcept {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (resource == nullptr) return std::nullopt;
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const auto* bytes = loaded != nullptr
        ? static_cast<const std::uint8_t*>(LockResource(loaded)) : nullptr;
    if (bytes == nullptr || size != 256U * 16U) return std::nullopt;
    EmbeddedCp437Font font{};
    std::copy_n(bytes, font.size(), font.begin());
    return font;
}

struct EmbeddedBitmap {
    HBITMAP handle{nullptr};
    UINT width{0};
    UINT height{0};
};

std::optional<EmbeddedBitmap> load_embedded_png(
    const HINSTANCE instance, const UINT identifier) noexcept {
    const HRSRC resource = FindResourceW(instance, MAKEINTRESOURCEW(identifier), RT_RCDATA);
    if (resource == nullptr) return std::nullopt;
    const DWORD size = SizeofResource(instance, resource);
    const HGLOBAL loaded = LoadResource(instance, resource);
    const auto* bytes = loaded != nullptr
        ? static_cast<const BYTE*>(LockResource(loaded)) : nullptr;
    if (bytes == nullptr || size == 0U) return std::nullopt;
    IStream* stream = SHCreateMemStream(bytes, size);
    if (stream == nullptr) return std::nullopt;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    HBITMAP bitmap = nullptr;
    UINT width = 0, height = 0;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(result)) result = factory->CreateDecoderFromStream(stream, nullptr,
        WICDecodeMetadataCacheOnLoad, &decoder);
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(result)) result = frame->GetSize(&width, &height);
    // This is the owner-locked square master embedded by control_panel.rc.
    // Reject unexpected resources before allocating a bitmap, but do not retain
    // the dimensions of the superseded provisional About image.
    if (SUCCEEDED(result) && (width != 1254U || height != 1254U)) result = E_FAIL;
    if (SUCCEEDED(result)) result = factory->CreateFormatConverter(&converter);
    if (SUCCEEDED(result)) result = converter->Initialize(frame,
        GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
        WICBitmapPaletteTypeCustom);
    void* pixels = nullptr;
    if (SUCCEEDED(result)) {
        BITMAPINFO information{};
        information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        information.bmiHeader.biWidth = static_cast<LONG>(width);
        information.bmiHeader.biHeight = -static_cast<LONG>(height);
        information.bmiHeader.biPlanes = 1;
        information.bmiHeader.biBitCount = 32;
        information.bmiHeader.biCompression = BI_RGB;
        const HDC dc = GetDC(nullptr);
        bitmap = CreateDIBSection(dc, &information, DIB_RGB_COLORS, &pixels, nullptr, 0);
        if (dc != nullptr) ReleaseDC(nullptr, dc);
        if (bitmap == nullptr || pixels == nullptr) result = E_OUTOFMEMORY;
    }
    if (SUCCEEDED(result)) {
        const UINT stride = width * 4U;
        result = converter->CopyPixels(nullptr, stride, stride * height,
            static_cast<BYTE*>(pixels));
    }
    if (converter != nullptr) converter->Release();
    if (frame != nullptr) frame->Release();
    if (decoder != nullptr) decoder->Release();
    if (factory != nullptr) factory->Release();
    stream->Release();
    if (FAILED(result)) {
        if (bitmap != nullptr) DeleteObject(bitmap);
        return std::nullopt;
    }
    return EmbeddedBitmap{bitmap, width, height};
}

constexpr UINT ID_FILE_EXIT = 40002;
constexpr UINT ID_BBS_CONFIGURE = 40013;
constexpr UINT ID_BBS_FILEMAN = 40024;
constexpr UINT ID_BBS_RESTART = 40014;
constexpr UINT ID_BBS_SERVICE_START = 40046;
constexpr UINT ID_BBS_SERVICE_STOP = 40047;
constexpr UINT ID_NODE_DISCONNECT = 40015;
constexpr UINT ID_NODE_TERMINATE_DOOR = 40030;
constexpr UINT ID_BBS_DRAIN_1 = 40016;
constexpr UINT ID_BBS_DRAIN_5 = 40017;
constexpr UINT ID_BBS_DRAIN_15 = 40018;
constexpr UINT ID_BBS_CANCEL_DRAIN = 40019;
constexpr UINT ID_VIEW_MONITOR = 40003;
constexpr UINT ID_VIEW_MONITOR_COLOR = 40008;
constexpr UINT ID_VIEW_MONITOR_GRAYSCALE = 40009;
constexpr UINT ID_VIEW_FULL_SCREEN = 40010;
constexpr UINT ID_VIEW_MODEM_LIGHTS = 40011;
constexpr UINT ID_VIEW_MONITOR_COMPACT = 40020;
constexpr UINT ID_VIEW_MONITOR_MEDIUM = 40021;
constexpr UINT ID_VIEW_MONITOR_WIDE = 40022;
constexpr UINT ID_VIEW_SCALE_AUTO = 40100;
constexpr UINT ID_VIEW_SCALE_100 = 40101;
constexpr UINT ID_VIEW_SCALE_125 = 40102;
constexpr UINT ID_VIEW_SCALE_150 = 40103;
constexpr UINT ID_VIEW_SCALE_200 = 40104;
constexpr std::array<app::NodeViewScale, 5> kNodeViewScales{{app::NodeViewScale::auto_fit,
    app::NodeViewScale::percent100, app::NodeViewScale::percent125,
    app::NodeViewScale::percent150, app::NodeViewScale::percent200}};

std::wstring node_view_operator_key() {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    // Private fixture key, stable across its CP restarts; never touch the real
    // Windows operator's setting from an engineering build.
    std::uint64_t hash = 14695981039346656037ULL;
    const auto root = win32::alpha_installation_root();
    for (const auto c : root.native()) {
        hash ^= static_cast<std::uint16_t>(c); hash *= 1099511628211ULL;
    }
    return L"Software\\Spitfire-X BBS\\Tests\\NodeViewScale-" + std::to_wstring(hash);
#else
    return L"Software\\Spitfire-X BBS\\Control Panel";
#endif
}

app::NodeViewScale read_node_view_scale() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, node_view_operator_key().c_str(), 0,
            KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return app::NodeViewScale::auto_fit;
    DWORD value = 0, type = 0, bytes = sizeof(value);
    const auto result = RegQueryValueExW(key, L"NodeViewScale", nullptr, &type,
        reinterpret_cast<BYTE*>(&value), &bytes);
    RegCloseKey(key);
    return result == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(value)
        ? app::node_view_scale(value).value_or(app::NodeViewScale::auto_fit)
        : app::NodeViewScale::auto_fit;
}

bool save_node_view_scale(const app::NodeViewScale scale) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, node_view_operator_key().c_str(), 0, nullptr,
            0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = static_cast<DWORD>(scale);
    bool ok = RegSetValueExW(key, L"NodeViewScale", 0, REG_DWORD,
        reinterpret_cast<const BYTE*>(&value), sizeof(value)) == ERROR_SUCCESS;
    DWORD saved = 0, type = 0, bytes = sizeof(saved);
    ok = ok && RegQueryValueExW(key, L"NodeViewScale", nullptr, &type,
        reinterpret_cast<BYTE*>(&saved), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(saved) && saved == value;
    RegCloseKey(key);
    return ok;
}

constexpr DWORD kTelnetClientProfileVersion = 1;

std::optional<DWORD> read_registry_dword(const HKEY key, const wchar_t* name) {
    DWORD value = 0, type = 0, bytes = sizeof(value);
    return RegQueryValueExW(key, name, nullptr, &type,
            reinterpret_cast<BYTE*>(&value), &bytes) == ERROR_SUCCESS &&
        type == REG_DWORD && bytes == sizeof(value) ? std::optional<DWORD>(value) : std::nullopt;
}

std::optional<std::wstring> read_registry_string(const HKEY key, const wchar_t* name,
    const DWORD required_type, const DWORD maximum_bytes) {
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

std::optional<std::vector<std::wstring>> read_registry_multistring(
    const HKEY key, const wchar_t* name) {
    auto encoded = read_registry_string(key, name, REG_MULTI_SZ,
        static_cast<DWORD>((app::kExternalTelnetClientMaximumTokens *
            (app::kExternalTelnetClientMaximumTokenCharacters + 1U) + 1U) * sizeof(wchar_t)));
    if (!encoded || encoded->empty() || encoded->back() != L'\0') return std::nullopt;
    std::vector<std::wstring> tokens;
    std::size_t offset = 0;
    while (offset < encoded->size()) {
        const auto end = encoded->find(L'\0', offset);
        if (end == encoded->npos || end == offset ||
            tokens.size() == app::kExternalTelnetClientMaximumTokens) return std::nullopt;
        tokens.emplace_back(encoded->substr(offset, end - offset));
        offset = end + 1U;
    }
    if (offset != encoded->size() || tokens.empty()) return std::nullopt;
    return tokens;
}

std::optional<app::ExternalTelnetClientProfile> read_telnet_client_profile() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, node_view_operator_key().c_str(), 0,
            KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return std::nullopt;
    const auto version = read_registry_dword(key, L"TelnetClientProfileVersion");
    const auto type = read_registry_dword(key, L"TelnetClientType");
    const auto path = read_registry_string(key, L"TelnetClientPath", REG_SZ, 65536);
    std::optional<std::vector<std::wstring>> arguments;
    if (type && *type == static_cast<DWORD>(app::ExternalTelnetClientType::custom))
        arguments = read_registry_multistring(key, L"TelnetClientArguments");
    RegCloseKey(key);
    if (!version || *version != kTelnetClientProfileVersion || !type || !path ||
        (*type != static_cast<DWORD>(app::ExternalTelnetClientType::syncterm) &&
            *type != static_cast<DWORD>(app::ExternalTelnetClientType::custom)) ||
        (*type == static_cast<DWORD>(app::ExternalTelnetClientType::custom) && !arguments))
        return std::nullopt;
    app::ExternalTelnetClientProfile profile;
    profile.type = static_cast<app::ExternalTelnetClientType>(*type);
    profile.executable = *path;
    if (arguments) profile.custom_arguments = std::move(*arguments);
    return app::validate_external_telnet_client_profile(profile).valid
        ? std::optional<app::ExternalTelnetClientProfile>(std::move(profile)) : std::nullopt;
}

#ifdef SFX_ALPHA_TEST_AUTOMATION
bool save_telnet_client_profile(const app::ExternalTelnetClientProfile& profile) {
    if (!app::external_telnet_client_executable_is_usable(profile)) return false;
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, node_view_operator_key().c_str(), 0, nullptr,
            0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    (void)RegDeleteValueW(key, L"TelnetClientProfileVersion"); // incomplete writes never load
    const DWORD type = static_cast<DWORD>(profile.type);
    bool ok = RegSetValueExW(key, L"TelnetClientType", 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&type), sizeof(type)) == ERROR_SUCCESS &&
        RegSetValueExW(key, L"TelnetClientPath", 0, REG_SZ,
            reinterpret_cast<const BYTE*>(profile.executable.c_str()),
            static_cast<DWORD>((profile.executable.size() + 1U) * sizeof(wchar_t))) == ERROR_SUCCESS;
    if (ok && profile.type == app::ExternalTelnetClientType::custom) {
        std::wstring encoded;
        for (const auto& token : profile.custom_arguments) {
            encoded += token;
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
        reinterpret_cast<const BYTE*>(&kTelnetClientProfileVersion),
        sizeof(kTelnetClientProfileVersion)) == ERROR_SUCCESS;
    RegCloseKey(key);
    if (!ok) return false;
    const auto verified = read_telnet_client_profile();
    return verified && verified->type == profile.type &&
        verified->executable == profile.executable &&
        verified->custom_arguments == profile.custom_arguments;
}

bool clear_telnet_client_profile() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, node_view_operator_key().c_str(), 0,
            KEY_SET_VALUE, &key) != ERROR_SUCCESS) return !read_telnet_client_profile();
    bool ok = true;
    for (const auto* name : {L"TelnetClientProfileVersion", L"TelnetClientType",
            L"TelnetClientPath", L"TelnetClientArguments"}) {
        const auto removed = RegDeleteValueW(key, name);
        ok = ok && (removed == ERROR_SUCCESS || removed == ERROR_FILE_NOT_FOUND);
    }
    RegCloseKey(key);
    return ok && !read_telnet_client_profile();
}
#endif
constexpr UINT ID_NODE_OPEN = 40004;
constexpr UINT ID_NODE_ENABLE = 40005;
constexpr UINT ID_NODE_DISABLE = 40006;
constexpr UINT ID_NODE_LOGON = 40012;
constexpr UINT ID_NODE_EDIT_USER = 40023;
constexpr UINT ID_HELP_ABOUT = 40007;
constexpr UINT ID_HELP_WIKI = 40048;
constexpr UINT IDC_MAIN_TAB = 5100;
constexpr UINT IDC_NODE_LIST = 5101;
constexpr UINT IDC_OPEN_NODE = 5102;
constexpr UINT IDC_SERVICE_BUTTON = 5120;
constexpr UINT IDC_CLOSE_NODE = 5103;
constexpr UINT IDC_EVENTS_VIEW = 5104;
constexpr UINT IDC_LOG_VIEW = 5105;
constexpr UINT IDC_STATS_VIEW = 5106;
constexpr UINT IDC_TERMINAL_VIEW = 5107;
constexpr UINT IDC_MONITOR_VIEW = 5108;
constexpr UINT IDC_LOG_PAUSE = 5109;
constexpr UINT IDC_LOG_CLEAR = 5110;
constexpr UINT IDC_LOG_FILTER = 5116;
constexpr UINT IDC_NODE_HEADER = 5111;

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) return {};
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required) != required) return {};
    return result;
}

std::optional<std::string> wide_to_utf8(const std::wstring& value) {
    if (value.empty()) return std::string{};
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (required <= 0) return std::nullopt;
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), required, nullptr, nullptr) != required)
        return std::nullopt;
    return result;
}

std::wstring log_health_notice(std::uint8_t health, bool hostname_exhausted = false) {
    std::wstring result;
    for (std::uint8_t bit = 1; bit <= 16; bit <<= 1)
        if (health & bit) result += utf8_to_wide(std::string(app::operational_log_health_text(bit))) + L"\r\n";
    if (hostname_exhausted) result += L"Hostname Lookup: unavailable; numeric IP display continues.\r\n";
    return result;
}

LONG log_selection_position(std::wstring_view value) {
    // Rich Edit stores a CRLF paragraph as one selection position.
    return static_cast<LONG>(value.size() - std::count(value.begin(), value.end(), L'\n'));
}

void erase(std::wstring& value) noexcept {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
    value.clear();
    value.shrink_to_fit();
}

void erase_utf8(std::string& value) noexcept {
    if (!value.empty()) SecureZeroMemory(value.data(), value.size());
    value.clear();
    value.shrink_to_fit();
}

std::wstring window_text(const HWND window) {
    const int length = GetWindowTextLengthW(window);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length) + 1U, L'\0');
    const int copied = GetWindowTextW(window, result.data(), length + 1);
    result.resize(copied > 0 ? static_cast<std::size_t>(copied) : 0U);
    return result;
}

bool write_exact(const HANDLE handle, const void* source, DWORD bytes) {
    const auto* cursor = static_cast<const std::uint8_t*>(source);
    while (bytes != 0) {
        DWORD written = 0;
        if (!WriteFile(handle, cursor, bytes, &written, nullptr) || written == 0) return false;
        cursor += written;
        bytes -= written;
    }
    return true;
}

class RuntimeClient final {
public:
    ~RuntimeClient() {
        connection_.disconnect();
        if (process_ != nullptr) CloseHandle(process_);
    }

    void swap(RuntimeClient& other) noexcept {
        using std::swap;
        swap(process_,other.process_);swap(desktop_detached_,other.desktop_detached_);
        swap(service_mode_,other.service_mode_);swap(process_id_,other.process_id_);
        swap(next_request_id_,other.next_request_id_);swap(connection_,other.connection_);swap(status_tracker_,other.status_tracker_);
    }
    bool cancel_owned_start() noexcept {
        if(service_mode_) {connection_.disconnect();return true;}
        try{if(connection_.valid()){std::string ignored;(void)shutdown(ignored);}}catch(...){}
        connection_.disconnect();
        if(process_&&WaitForSingleObject(process_,20000)==WAIT_TIMEOUT){
            // Only the exact child handle returned by our CreateProcess is
            // eligible. A failed pre-control launch normally exits through its
            // existing 15-second owner-handshake timeout and orderly shutdown.
            (void)TerminateProcess(process_,1);(void)WaitForSingleObject(process_,5000);
        }
        return !child_alive();
    }
    bool launch(std::string& diagnostic,const std::function<bool()>& cancelled={}) {
        if (process_ != nullptr) return false;
        if(cancelled&&cancelled()){diagnostic="startup_cancelled";return false;}
        try {
            service_mode_=win32::installation::read_service_mode(win32::alpha_installation_root());
            win32::configure_service_secret_scope(service_mode_.has_value());
            if(service_mode_)return attach_service(diagnostic,false);
        } catch(const std::exception& e) {diagnostic=e.what();return false;}
        std::array<std::uint8_t, app::kLocalControlNonceBytes> nonce{};
        if (!win32::fill_secure_random(nonce.data(), nonce.size())) {
            diagnostic = "secure_random_unavailable";
            return false;
        }

        SECURITY_ATTRIBUTES pipe_security{sizeof(pipe_security), nullptr, TRUE};
        HANDLE owner_read = INVALID_HANDLE_VALUE;
        HANDLE owner_write = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&owner_read, &owner_write, &pipe_security, 0) ||
            !SetHandleInformation(owner_write, HANDLE_FLAG_INHERIT, 0)) {
            diagnostic = "owner_channel_creation_failed";
            if (owner_read != INVALID_HANDLE_VALUE) CloseHandle(owner_read);
            if (owner_write != INVALID_HANDLE_VALUE) CloseHandle(owner_write);
            SecureZeroMemory(nonce.data(), nonce.size());
            return false;
        }

        std::vector<wchar_t> module(32768U);
        const DWORD module_length = GetModuleFileNameW(nullptr, module.data(),
            static_cast<DWORD>(module.size()));
        if (module_length == 0 || module_length >= module.size()) {
            diagnostic = "control_panel_path_unavailable";
            CloseHandle(owner_read); CloseHandle(owner_write);
            SecureZeroMemory(nonce.data(), nonce.size());
            return false;
        }
        std::filesystem::path runtime_path(std::wstring(module.data(), module_length));
        runtime_path.replace_filename(L"SFXRuntime.exe");
        if (GetFileAttributesW(runtime_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            diagnostic = "sibling_runtime_missing";
            CloseHandle(owner_read); CloseHandle(owner_write);
            SecureZeroMemory(nonce.data(), nonce.size());
            return false;
        }

        SIZE_T attribute_bytes = 0;
        (void)InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
        std::vector<std::uint8_t> attribute_storage(attribute_bytes);
        auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
        if (attribute_bytes == 0 || !InitializeProcThreadAttributeList(attributes, 1, 0,
                &attribute_bytes) ||
            !UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                &owner_read, sizeof(owner_read), nullptr, nullptr)) {
            diagnostic = "owner_handle_allowlist_failed";
            CloseHandle(owner_read); CloseHandle(owner_write);
            SecureZeroMemory(nonce.data(), nonce.size());
            return false;
        }

        const unsigned long long inherited_value =
            static_cast<unsigned long long>(reinterpret_cast<ULONG_PTR>(owner_read));
        std::wstring command = L"\"" + runtime_path.native() + L"\" --owner-handle=" +
            std::to_wstring(inherited_value);
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.lpAttributeList = attributes;
        PROCESS_INFORMATION process{};
        const BOOL launched = CreateProcessW(runtime_path.c_str(), mutable_command.data(), nullptr,
            nullptr, TRUE, EXTENDED_STARTUPINFO_PRESENT|CREATE_SUSPENDED, nullptr,
            runtime_path.parent_path().c_str(), &startup.StartupInfo, &process);
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(owner_read);
        if (!launched) {
            diagnostic = "runtime_launch_failed";
            CloseHandle(owner_write);
            SecureZeroMemory(nonce.data(), nonce.size());
            return false;
        }
        process_ = process.hProcess;
        process_id_ = process.dwProcessId;
        // Keep Runtime outside a parent job: Win7 transfer/door/scanner children
        // must still be able to enter their existing individual jobs.
        if((cancelled&&cancelled())||
           ResumeThread(process.hThread)==static_cast<DWORD>(-1)){
            (void)TerminateProcess(process_,1);(void)WaitForSingleObject(process_,5000);
            CloseHandle(process.hThread);CloseHandle(owner_write);SecureZeroMemory(nonce.data(),nonce.size());
            diagnostic="runtime_startup_cancelled_or_resume_failed";return false;
        }
        CloseHandle(process.hThread);
        const DWORD owner_process_id = GetCurrentProcessId();
        const bool bootstrapped = write_exact(owner_write, &owner_process_id,
                sizeof(owner_process_id)) &&
            write_exact(owner_write, nonce.data(), static_cast<DWORD>(nonce.size()));
        CloseHandle(owner_write);
        if (!bootstrapped) {
            diagnostic = "runtime_owner_bootstrap_failed";
            SecureZeroMemory(nonce.data(), nonce.size());
            return false;
        }

        const auto names = win32::installation_object_names(win32::alpha_installation_root());
        connection_ = win32::connect_local_control_server(names.pipe_name, process_id_, 15000,cancelled);
        if (!connection_.valid()) {
            diagnostic = "runtime_control_channel_unavailable";
            SecureZeroMemory(nonce.data(), nonce.size());
            return false;
        }
        app::LocalControlFrame hello;
        hello.type = app::LocalControlMessageType::hello;
        hello.request_id = next_request_id_++;
        hello.payload = app::encode_hello_payload(nonce);
        SecureZeroMemory(nonce.data(), nonce.size());
        app::LocalControlFrame response;
        const bool hello_ok = exchange(hello, app::LocalControlMessageType::hello_response, response);
        if (!hello.payload.empty()) SecureZeroMemory(hello.payload.data(), hello.payload.size());
        const auto accepted = hello_ok ? app::decode_control_response(response.payload) : std::nullopt;
        if (!accepted || (accepted->code != app::LocalControlResponseCode::ok &&
                accepted->code != app::LocalControlResponseCode::setup_required &&
                accepted->code != app::LocalControlResponseCode::recovery_required)) {
            diagnostic = accepted ? accepted->diagnostic_code : "runtime_handshake_failed";
            if (WaitForSingleObject(process_, 1000) == WAIT_OBJECT_0) {
                DWORD exit_code = 0;
                if (GetExitCodeProcess(process_, &exit_code)) {
                    diagnostic += "_exit_" + std::to_string(exit_code);
                }
            }
            return false;
        }
        if (!status_tracker_.bind_authenticated_epoch(accepted->runtime_epoch)) {
            diagnostic = "runtime_epoch_invalid";
            connection_.disconnect();
            return false;
        }
        diagnostic = accepted->diagnostic_code;
        return true;
    }

    std::optional<app::AlphaRuntimeStatus> status(const std::uint16_t selected = 0) {
        if(service_mode_){
            try{
                const auto current=win32::installation::read_service_selection(service_mode_->root);
                if(!current||current->pending)return {};
                if(current->revision!=service_mode_->revision){
                    connection_.disconnect();process_id_=0;
                    if(current->enabled)service_mode_=win32::installation::read_service_mode(current->root);
                    else {service_mode_.reset();desktop_detached_=true;win32::configure_service_secret_scope(false);}
                }
            }catch(...){connection_.disconnect();return {};}
        }
        if(desktop_detached_){
            app::AlphaRuntimeStatus result;result.lifecycle.owner_mode=app::RuntimeOwnershipMode::desktop;
            result.lifecycle.state=app::RuntimeState::stopped;result.state=app::AlphaRuntimePresentationState::stopped;
            result.diagnostic_code="desktop_selected_close_and_reopen_control_panel";return result;
        }
        if(service_mode_) {
            try {
                const auto actual=win32::installation::query_owned_service(*service_mode_);
                if(actual.state!=SERVICE_RUNNING) {
                    connection_.disconnect();process_id_=0;
                    app::AlphaRuntimeStatus result;result.lifecycle.owner_mode=app::RuntimeOwnershipMode::service;
                    result.lifecycle.state=actual.state==SERVICE_STOPPED?app::RuntimeState::stopped:
                        actual.state==SERVICE_START_PENDING?app::RuntimeState::starting:app::RuntimeState::stopping;
                    result.state=actual.state==SERVICE_STOPPED?app::AlphaRuntimePresentationState::stopped:
                        actual.state==SERVICE_START_PENDING?app::AlphaRuntimePresentationState::starting:app::AlphaRuntimePresentationState::stopping;
                    result.diagnostic_code=actual.state==SERVICE_STOPPED?"service_stopped":"service_transition_in_progress";
                    return result;
                }
                if(!connection_.valid()||process_id_!=actual.process_id) {
                    std::string diagnostic;if(!attach_service(diagnostic,false))return {};
                }
            }catch(...){connection_.disconnect();return {};}
        }
        auto decoded = app::read_status_snapshot(selected, [this](const app::AlphaStatusRequest& query)
            -> std::optional<std::vector<std::uint8_t>> {
            app::LocalControlFrame request;
            request.type = app::LocalControlMessageType::status_request;
            request.request_id = next_request_id_++;
            request.payload = app::encode_status_request(query);
            app::LocalControlFrame response;
            if (request.payload.empty() || !exchange(request, app::LocalControlMessageType::status_response, response))
                return std::nullopt;
            return std::move(response.payload);
        });
        if (!decoded || !status_tracker_.accept(*decoded)) {
            connection_.disconnect();
            return std::nullopt;
        }
        return decoded;
    }

    std::optional<app::SoundPollSnapshot> sound_poll(const app::SoundPollRequest& cursor) {
        app::LocalControlFrame request;request.type=app::LocalControlMessageType::sound_poll_request;
        request.request_id=next_request_id_++;request.payload=app::encode_sound_poll_request(cursor);
        app::LocalControlFrame response;
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::sound_poll_response,response))return {};
        return app::decode_sound_poll_snapshot(response.payload);
    }

    std::optional<app::OperationalLogPage> log_page(app::OperationalLogCursor cursor) {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::log_page_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_log_cursor(cursor);
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request, app::LocalControlMessageType::log_page_response, response))
            return std::nullopt;
        auto page = app::decode_log_page(response.payload);
        if (!page || page->cursor.epoch != cursor.epoch || page->cursor.after != cursor.after) {
            connection_.disconnect(); return std::nullopt;
        }
        return page;
    }

    std::optional<app::AlphaRuntimeMetricSnapshot> metrics() {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::metrics_request;
        request.request_id = next_request_id_++;
        app::LocalControlFrame response;
        if (!exchange(request, app::LocalControlMessageType::metrics_response, response))
            return std::nullopt;
        return app::decode_runtime_metrics(response.payload);
    }

    app::AlphaControlResponse bootstrap(app::AlphaBootstrapRequest& setup) {
        const auto current = status();
        if (!current) return {app::LocalControlResponseCode::internal_failure, "runtime_status_unavailable"};
        if (const auto failure = app::bootstrap_prerequisite_failure(*current)) return *failure;
        setup.configuration.security_policy_revision = current->security_policy_revision;
        setup.configuration.security_policy_hash = current->security_policy_hash;
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::bootstrap_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_bootstrap_request(setup);
        if (request.payload.empty()) {
            return {app::LocalControlResponseCode::invalid_request, "bootstrap_request_invalid"};
        }
        app::LocalControlFrame response;
        const bool exchanged = exchange(request,
            app::LocalControlMessageType::bootstrap_response, response);
        SecureZeroMemory(request.payload.data(), request.payload.size());
        request.payload.clear();
        if (!exchanged) {
            return {app::LocalControlResponseCode::internal_failure, "bootstrap_channel_failed"};
        }
        const auto decoded = app::decode_control_response(response.payload);
        return decoded ? *decoded : app::AlphaControlResponse{
            app::LocalControlResponseCode::internal_failure, "bootstrap_response_invalid"};
    }

    app::FtnRuntimeResponse ftn_runtime(const app::FtnRuntimeRequest& value){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::ftn_runtime_request;request.request_id=next_request_id_++;request.payload=app::encode_ftn_runtime_request(value);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::ftn_runtime_response,response))return {};
        return app::decode_ftn_runtime_response(response.payload).value_or(app::FtnRuntimeResponse{});
    }
    app::FtnHeldControlResponse ftn_held(const app::FtnHeldControlRequest& value){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::ftn_held_request;request.request_id=next_request_id_++;request.payload=app::encode_ftn_held_request(value);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::ftn_held_response,response))return {};
        return app::decode_ftn_held_response(response.payload).value_or(app::FtnHeldControlResponse{});
    }
    app::FtnConfigurationResponse ftn_configuration(const app::FtnConfigurationRequest& value){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::ftn_configuration_request;request.request_id=next_request_id_++;request.payload=app::encode_ftn_configuration_request(value);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::ftn_configuration_response,response))return {};
        return app::decode_ftn_configuration_response(response.payload).value_or(app::FtnConfigurationResponse{});
    }
    app::FtnConfigurationResponse ftn_hatch_catalog(const app::FtnConfigurationRequest& value){app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::ftn_hatch_catalog_request;request.request_id=next_request_id_++;request.payload=app::encode_ftn_configuration_request(value);if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::ftn_hatch_catalog_response,response))return {};return app::decode_ftn_configuration_response(response.payload).value_or(app::FtnConfigurationResponse{});}
    app::ChatConfigurationResult chat_configuration(){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::chat_configuration_query_request;request.request_id=next_request_id_++;
        if(!exchange(request,app::LocalControlMessageType::chat_configuration_query_response,response))return {};
        return app::decode_chat_result(response.payload).value_or(app::ChatConfigurationResult{});
    }
    app::ChatConfigurationResult change_chat_configuration(const app::ChatConfiguration& value){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::chat_configuration_change_request;request.request_id=next_request_id_++;request.payload=app::encode_chat_configuration(value);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::chat_configuration_change_response,response))return {};
        return app::decode_chat_result(response.payload).value_or(app::ChatConfigurationResult{});
    }
    app::ChatResult send_notice(const app::ChatNoticeRequest& notice){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::chat_notice_request;request.request_id=next_request_id_++;request.payload=app::encode_chat_notice(notice);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::chat_notice_response,response)||response.payload.size()!=1||response.payload[0]>static_cast<unsigned>(app::ChatResult::invalid))return app::ChatResult::unavailable;
        return static_cast<app::ChatResult>(response.payload[0]);
    }
    app::ChatOperatorSnapshot chat_operator(const app::ChatOperatorRequest& command){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::chat_operator_request;request.request_id=next_request_id_++;
        request.payload=app::encode_chat_operator_request(command);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::chat_operator_response,response))return {};
        return app::decode_chat_operator_snapshot(response.payload).value_or(app::ChatOperatorSnapshot{});
    }

    app::AlphaControlResponse command_node(const app::AlphaNodeCommandRequest& command) {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::node_command_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_node_command_request(command);
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request,
                app::LocalControlMessageType::node_command_response, response)) {
            return {app::LocalControlResponseCode::internal_failure, "node_command_channel_failed"};
        }
        const auto decoded = app::decode_control_response(response.payload);
        return decoded ? *decoded : app::AlphaControlResponse{
            app::LocalControlResponseCode::internal_failure, "node_command_response_invalid"};
    }

    app::AlphaControlResponse command_lifecycle(const app::AlphaLifecycleCommandRequest& command) {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::lifecycle_command_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_lifecycle_command_request(command);
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request,
                app::LocalControlMessageType::lifecycle_command_response, response))
            return {app::LocalControlResponseCode::internal_failure, "lifecycle_command_channel_failed"};
        const auto decoded = app::decode_control_response(response.payload);
        return decoded ? *decoded : app::AlphaControlResponse{
            app::LocalControlResponseCode::internal_failure, "lifecycle_command_response_invalid"};
    }

    app::AlphaLocalSessionStartResult start_local_session(const std::uint16_t node) {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::local_session_start_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_local_session_start_request({node});
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request,
                app::LocalControlMessageType::local_session_start_response, response)) {
            return {{app::LocalControlResponseCode::internal_failure,
                "local_session_channel_failed"}, {}, 0, {}, 0};
        }
        const auto decoded = app::decode_local_session_start_result(response.payload);
        return decoded ? *decoded : app::AlphaLocalSessionStartResult{{
            app::LocalControlResponseCode::internal_failure,
            "local_session_response_invalid"}, {}, 0, {}, 0};
    }

    app::AlphaControlResponse cancel_local_session(
        const app::AlphaLocalSessionCancelRequest& cancellation) {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::local_session_cancel_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_local_session_cancel_request(cancellation);
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request,
                app::LocalControlMessageType::local_session_cancel_response, response)) {
            return {app::LocalControlResponseCode::internal_failure,
                "local_session_cancel_channel_failed"};
        }
        const auto decoded = app::decode_control_response(response.payload);
        return decoded ? *decoded : app::AlphaControlResponse{
            app::LocalControlResponseCode::internal_failure,
            "local_session_cancel_response_invalid"};
    }

    app::MessageMaintenanceStatus message_maintenance_status(app::MessageMaintenanceKind kind){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::message_maintenance_query_request;request.request_id=next_request_id_++;request.payload=app::encode_message_maintenance_kind(kind);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::message_maintenance_query_response,response))return {};
        return app::decode_message_maintenance_status(response.payload).value_or(app::MessageMaintenanceStatus{});
    }
    app::AlphaControlResponse configure_message_schedule(const app::MessageMaintenanceScheduleRequest& settings){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::message_maintenance_schedule_request;request.request_id=next_request_id_++;request.payload=app::encode_message_maintenance_schedule(settings);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::message_maintenance_schedule_response,response))return {app::LocalControlResponseCode::internal_failure,"message_schedule_channel_failed"};
        return app::decode_control_response(response.payload).value_or(app::AlphaControlResponse{app::LocalControlResponseCode::internal_failure,"message_schedule_response_invalid"});
    }
    app::AlphaControlResponse run_message_maintenance_now(app::MessageMaintenanceKind kind){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::message_maintenance_run_request;request.request_id=next_request_id_++;request.payload=app::encode_message_maintenance_kind(kind);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::message_maintenance_run_response,response))return {app::LocalControlResponseCode::internal_failure,"message_job_channel_failed"};
        return app::decode_control_response(response.payload).value_or(app::AlphaControlResponse{app::LocalControlResponseCode::internal_failure,"message_job_response_invalid"});
    }
    app::AlphaControlResponse manual_clamav_notice(const app::ManualClamAvNotice& notice){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::clamav_manual_notice_request;request.request_id=next_request_id_++;
        request.payload=app::encode_manual_clamav_notice(notice);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::clamav_manual_notice_response,response))return {app::LocalControlResponseCode::internal_failure,"clamav_events_channel_failed"};
        return app::decode_control_response(response.payload).value_or(app::AlphaControlResponse{app::LocalControlResponseCode::internal_failure,"clamav_events_response_invalid"});
    }
    app::LocalQwkPolicyResult qwk_policy_snapshot() {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::qwk_policy_query_request;
        request.request_id=next_request_id_++;
        if(!exchange(request,app::LocalControlMessageType::qwk_policy_query_response,response))return {};
        return app::decode_qwk_policy_result(response.payload).value_or(app::LocalQwkPolicyResult{});
    }
    app::LocalQwkPolicyResult change_qwk_policy(const app::QwkPolicyChange& change) {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::qwk_policy_change_request;
        request.request_id=next_request_id_++;
        request.payload=app::encode_qwk_policy_change(change);
        if(request.payload.empty())return {app::LocalAccountResultCode::invalid_request};
        if(!exchange(request,app::LocalControlMessageType::qwk_policy_change_response,response))return {};
        return app::decode_qwk_policy_result(response.payload).value_or(app::LocalQwkPolicyResult{});
    }
    app::FileManagementJobStatus file_management(const app::LocalControlMessageType kind,const std::vector<std::uint8_t>& payload){
        if(!app::file_management_control_request(kind)||payload.empty())return {app::LocalAccountResultCode::invalid_request};
        app::LocalControlFrame request,response;request.type=kind;request.request_id=next_request_id_++;request.payload=payload;
        const auto expected=kind==app::LocalControlMessageType::file_management_start_request?app::LocalControlMessageType::file_management_start_response:kind==app::LocalControlMessageType::file_management_status_request?app::LocalControlMessageType::file_management_status_response:app::LocalControlMessageType::file_management_cancel_response;
        if(!exchange(request,expected,response))return {};return app::decode_file_management_status(response.payload).value_or(app::FileManagementJobStatus{});
    }
    app::FileManagementReadSnapshot query_file_management(const app::FileManagementReadRequest& query){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::file_management_query_request;request.request_id=next_request_id_++;request.payload=app::encode_file_management_read_request(query);
        if(request.payload.empty())return {app::LocalAccountResultCode::invalid_request};if(!exchange(request,app::LocalControlMessageType::file_management_query_response,response))return {};
        return app::decode_file_management_read_snapshot(response.payload).value_or(app::FileManagementReadSnapshot{});
    }
    app::LocalDoorSnapshot door_configuration_snapshot(const app::LocalDoorQuery& query){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::door_configuration_query_request;request.request_id=next_request_id_++;
        request.payload=app::encode_door_configuration_query(query);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::door_configuration_query_response,response))return {};
        return app::decode_door_configuration_snapshot(response.payload).value_or(app::LocalDoorSnapshot{});
    }
    app::LocalDoorResult change_door_configuration(const app::DoorConfigurationChange& change){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::door_configuration_change_request;request.request_id=next_request_id_++;
        request.payload=app::encode_door_configuration_change(change);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::door_configuration_change_response,response))return {};
        return app::decode_door_configuration_result(response.payload).value_or(app::LocalDoorResult{});
    }
    // Reuse the authenticated runtime exchange and bounded Door envelopes.
    app::LocalDoorSnapshot door_bulletin_snapshot(const app::LocalDoorBulletinQuery& query){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::door_bulletin_query_request;request.request_id=next_request_id_++;
        request.payload=app::encode_door_bulletin_query(query);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::door_bulletin_query_response,response))return {};
        return app::decode_door_configuration_snapshot(response.payload).value_or(app::LocalDoorSnapshot{});
    }
    app::LocalDoorResult change_door_bulletin(const app::DoorBulletinChange& change){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::door_bulletin_change_request;request.request_id=next_request_id_++;
        request.payload=app::encode_door_bulletin_change(change);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::door_bulletin_change_response,response))return {};
        return app::decode_door_configuration_result(response.payload).value_or(app::LocalDoorResult{});
    }
    app::LocalFileConfigurationSnapshot file_configuration_snapshot(const app::LocalFileConfigurationQuery& query){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::file_configuration_query_request;request.request_id=next_request_id_++;
        request.payload=app::encode_file_configuration_query(query);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::file_configuration_query_response,response))return {};
        return app::decode_file_configuration_snapshot(response.payload).value_or(app::LocalFileConfigurationSnapshot{});
    }
    app::LocalFileConfigurationResult change_file_configuration(const app::FileConfigurationChange& change){
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::file_configuration_change_request;request.request_id=next_request_id_++;
        request.payload=app::encode_file_configuration_change(change);
        if(request.payload.empty()||!exchange(request,app::LocalControlMessageType::file_configuration_change_response,response))return {};
        return app::decode_file_configuration_result(response.payload).value_or(app::LocalFileConfigurationResult{});
    }
    app::LocalMessageConfigurationSnapshot message_configuration_snapshot(const app::LocalMessageConfigurationQuery& query) {
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::message_configuration_query_request;request.request_id=next_request_id_++;
        request.payload=app::encode_message_configuration_query(query);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::message_configuration_query_response,response))return {};
        return app::decode_message_configuration_snapshot(response.payload).value_or(app::LocalMessageConfigurationSnapshot{});
    }
    app::LocalMessageConfigurationResult change_message_configuration(const app::MessageConfigurationChange& change) {
        app::LocalControlFrame request,response;request.type=app::LocalControlMessageType::message_configuration_change_request;request.request_id=next_request_id_++;
        request.payload=app::encode_message_configuration_change(change);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::message_configuration_change_response,response))return {};
        return app::decode_message_configuration_result(response.payload).value_or(app::LocalMessageConfigurationResult{});
    }
    app::LocalClassicInformationSnapshot classic_information_snapshot(std::int64_t after) {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::classic_information_query_request;request.request_id=next_request_id_++;
        request.payload=app::encode_classic_information_query(after);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::classic_information_query_response,response))return {};
        return app::decode_classic_information_snapshot(response.payload).value_or(app::LocalClassicInformationSnapshot{});
    }
    app::LocalAccountResult change_classic_information(const app::ClassicInformationChange& change) {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::classic_information_change_request;request.request_id=next_request_id_++;
        request.payload=app::encode_classic_information_change(change);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::classic_information_change_response,response))return {};
        return app::decode_account_result(response.payload).value_or(app::LocalAccountResult{});
    }
    app::LocalSecuritySnapshot security_snapshot(const app::LocalAccountQuery& query) {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::security_query_request; request.request_id=next_request_id_++;
        request.payload=app::encode_account_query(query);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::security_query_response,response)) return {};
        return app::decode_security_snapshot(response.payload).value_or(app::LocalSecuritySnapshot{});
    }
    app::LocalAccountResult change_security(const app::LocalSecurityChange& change) {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::security_change_request; request.request_id=next_request_id_++;
        request.payload=app::encode_security_change(change);
        if(request.payload.empty() || !exchange(request,app::LocalControlMessageType::security_change_response,response)) return {};
        return app::decode_account_result(response.payload).value_or(app::LocalAccountResult{});
    }
    app::LocalAccountSnapshot account_snapshot(const app::LocalAccountQuery& query) {
        app::LocalControlFrame request, response;
        request.type = app::LocalControlMessageType::account_query_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_account_query(query);
        if (request.payload.empty() || !exchange(request,
            app::LocalControlMessageType::account_query_response, response)) return {};
        return app::decode_account_snapshot(response.payload).value_or(app::LocalAccountSnapshot{});
    }

    app::AlphaControlResponse clear_throttle(const app::LocalThrottleClearRequest& command) {
        app::LocalControlFrame request, response;
        request.type = app::LocalControlMessageType::throttle_clear_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_throttle_clear_request(command);
        if (request.payload.empty() || !exchange(request,app::LocalControlMessageType::throttle_clear_response,response))
            return {app::LocalControlResponseCode::internal_failure,"throttle_clear_channel_failed"};
        return app::decode_control_response(response.payload).value_or(
            app::AlphaControlResponse{app::LocalControlResponseCode::internal_failure,"throttle_clear_response_invalid"});
    }

    app::LocalAccountResult administer_account(app::LocalAccountRequest& command) {
        struct Erase { app::LocalAccountRequest& value;
            ~Erase() { app::erase_local_account_secrets(value); } } erase{command};
        app::LocalControlFrame request, response;
        request.type = app::LocalControlMessageType::account_command_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_account_request(command);
        struct ErasePayload { std::vector<std::uint8_t>& value;
            ~ErasePayload() { if (!value.empty()) SecureZeroMemory(value.data(), value.size()); }
        } erase_payload{request.payload};
        if (request.payload.empty() || !exchange(request,
            app::LocalControlMessageType::account_command_response, response)) return {};
        return app::decode_account_result(response.payload).value_or(app::LocalAccountResult{});
    }

    std::optional<app::AlphaConfigurationSnapshot> configuration_snapshot() {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::configuration_snapshot_request;
        request.request_id = next_request_id_++;
        app::LocalControlFrame response;
        if (!exchange(request,
                app::LocalControlMessageType::configuration_snapshot_response, response))
            return std::nullopt;
        return app::decode_configuration_snapshot(response.payload);
    }
    std::optional<app::BotGateFeedStatus> botgate_feed_status() {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::botgate_feed_status_request;
        request.request_id=next_request_id_++;
        if (!exchange(request,app::LocalControlMessageType::botgate_feed_status_response,response)) return {};
        return app::decode_botgate_feed_status(response.payload);
    }
    app::AlphaControlResponse update_botgate_feed() {
        app::LocalControlFrame request,response;
        request.type=app::LocalControlMessageType::botgate_feed_update_request;
        request.request_id=next_request_id_++;
        if (!exchange(request,app::LocalControlMessageType::botgate_feed_update_response,response))
            return {app::LocalControlResponseCode::internal_failure,"feed_channel_unavailable"};
        return app::decode_control_response(response.payload).value_or(
            app::AlphaControlResponse{app::LocalControlResponseCode::internal_failure,"feed_response_invalid"});
    }

    app::AlphaConfigurationUpdateResult update_configuration(
        const app::AlphaConfigurationUpdateRequest& update) {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::configuration_update_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_configuration_update_request(update);
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request,
                app::LocalControlMessageType::configuration_update_response, response)) {
            return {app::LocalControlResponseCode::internal_failure,
                "configuration_update_channel_failed",
                app::AlphaConfigurationActivation::refused, 1, 1,
                "Refused - Runtime control channel failed"};
        }
        const auto decoded = app::decode_configuration_update_result(response.payload);
        return decoded ? *decoded : app::AlphaConfigurationUpdateResult{
            app::LocalControlResponseCode::internal_failure,
            "configuration_update_response_invalid",
            app::AlphaConfigurationActivation::refused, 1, 1,
            "Refused - Runtime returned an invalid response"};
    }

    app::AlphaControlResponse configure_scheduler(
        const app::AlphaSchedulerSettingsRequest& settings) {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::scheduler_settings_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_scheduler_settings_request(settings);
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request,
                app::LocalControlMessageType::scheduler_settings_response, response)) {
            return {app::LocalControlResponseCode::internal_failure,
                    "scheduler_settings_channel_failed"};
        }
        const auto decoded = app::decode_control_response(response.payload);
        return decoded ? *decoded : app::AlphaControlResponse{
            app::LocalControlResponseCode::internal_failure,
            "scheduler_settings_response_invalid"};
    }

    app::AlphaControlResponse run_scheduler_now() {
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::scheduler_run_request;
        request.request_id = next_request_id_++;
        request.payload = app::encode_scheduler_run_request({});
        app::LocalControlFrame response;
        if (request.payload.empty() || !exchange(request,
                app::LocalControlMessageType::scheduler_run_response, response)) {
            return {app::LocalControlResponseCode::internal_failure,
                    "scheduler_run_channel_failed"};
        }
        const auto decoded = app::decode_control_response(response.payload);
        return decoded ? *decoded : app::AlphaControlResponse{
            app::LocalControlResponseCode::internal_failure,
            "scheduler_run_response_invalid"};
    }

    bool restart(std::string& diagnostic) {
        if(service_mode_) {
            if(child_alive()&&!connection_.valid()&&!attach_service(diagnostic,false))return false;
            if(!shutdown(diagnostic))return false;
            return attach_service(diagnostic,true);
        }
        if (!shutdown(diagnostic)) return false;
        connection_.disconnect();
        if (process_ != nullptr) { CloseHandle(process_); process_ = nullptr; }
        process_id_ = 0;
        next_request_id_ = 1;
        return launch(diagnostic);
    }

    bool shutdown(std::string& diagnostic) {
        if (!connection_.valid()) return !child_alive();
        app::LocalControlFrame request;
        request.type = app::LocalControlMessageType::shutdown_request;
        request.request_id = next_request_id_++;
        app::LocalControlFrame response;
        if (!exchange(request, app::LocalControlMessageType::shutdown_response, response)) {
            diagnostic = "shutdown_channel_failed";
            return false;
        }
        const auto decoded = app::decode_control_response(response.payload);
        if (!decoded || decoded->code != app::LocalControlResponseCode::ok) {
            diagnostic = decoded ? decoded->diagnostic_code : "shutdown_response_invalid";
            return false;
        }
        connection_.disconnect();
        if(service_mode_) {
            const auto deadline=GetTickCount64()+15000;
            try {
                do {
                    if(win32::installation::query_owned_service(*service_mode_).state==SERVICE_STOPPED) {
                        process_id_=0;diagnostic=decoded->diagnostic_code;return true;
                    }
                    Sleep(50);
                }while(GetTickCount64()<deadline);
            }catch(const std::exception& e){diagnostic=e.what();return false;}
            diagnostic="service_stop_pending";return false;
        }
        if (WaitForSingleObject(process_, 15000) != WAIT_OBJECT_0) {
            diagnostic = "runtime_shutdown_timeout";
            return false;
        }
        diagnostic = decoded->diagnostic_code;
        return true;
    }

    bool child_alive() const noexcept {
        if(service_mode_) {try{return win32::installation::query_owned_service(*service_mode_).state!=SERVICE_STOPPED;}catch(...){return true;}}
        return process_ != nullptr && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
    }
    bool is_service_mode() const noexcept {return service_mode_.has_value();}
    win32::installation::ServiceState service_control_state(bool action=false) const {
        if(!service_mode_)throw std::runtime_error("service_mode_not_selected");
        if(action){
            const auto selected=win32::installation::read_service_mode(service_mode_->root);
            if(!selected||selected->revision!=service_mode_->revision||selected->id!=service_mode_->id||selected->owner!=service_mode_->owner)
                throw std::runtime_error("service_selection_changed_reopen_control_panel");
        }
        return win32::installation::query_selected_service(*service_mode_);
    }
    bool start_service(std::string& diagnostic){return service_mode_&&attach_service(diagnostic,true);}
#ifdef SFX_ALPHA_TEST_AUTOMATION
    void test_disconnect_control() { connection_.disconnect(); }
    DWORD test_process_id() const noexcept { return process_id_; }
#endif

private:
    bool attach_service(std::string& diagnostic,bool start) {
        try {
            connection_.disconnect();process_id_=0;next_request_id_=1;
            if(!service_mode_){diagnostic="service_mode_not_selected";return false;}
            if(start&&!win32::installation::start_owned_service(*service_mode_,diagnostic))return false;
            const auto actual=win32::installation::query_owned_service(*service_mode_);
            if(actual.state!=SERVICE_RUNNING){diagnostic=actual.state==SERVICE_STOPPED?"service_stopped":"service_transition_in_progress";return true;}
            process_id_=actual.process_id;
            win32::installation::verify_owned_service_peer(*service_mode_,process_id_);
            const auto names=win32::installation_object_names(service_mode_->root);
            connection_=win32::connect_local_control_server(names.pipe_name,process_id_,3000);
            if(!connection_.valid()){diagnostic="service_control_channel_unavailable";return false;}
            if(win32::installation::query_owned_service(*service_mode_).process_id!=process_id_){connection_.disconnect();diagnostic="service_epoch_changed";return false;}
            app::LocalControlFrame challenge;
            std::array<std::uint8_t,app::kLocalControlNonceBytes> nonce{};
            if(!connection_.read_frame(challenge)||challenge.type!=app::LocalControlMessageType::hello||
                !app::decode_hello_payload(challenge.payload,nonce)){
                connection_.disconnect();diagnostic="service_challenge_invalid";return false;
            }
            app::LocalControlFrame hello;hello.type=app::LocalControlMessageType::hello;
            hello.request_id=next_request_id_++;hello.payload=app::encode_hello_payload(nonce);
            SecureZeroMemory(nonce.data(),nonce.size());
            app::LocalControlFrame response;
            const auto ok=exchange(hello,app::LocalControlMessageType::hello_response,response);
            if(!hello.payload.empty())SecureZeroMemory(hello.payload.data(),hello.payload.size());
            const auto accepted=ok?app::decode_control_response(response.payload):std::nullopt;
            if(!accepted||(accepted->code!=app::LocalControlResponseCode::ok&&accepted->code!=app::LocalControlResponseCode::setup_required&&
                accepted->code!=app::LocalControlResponseCode::recovery_required)){
                connection_.disconnect();diagnostic="service_handshake_failed";return false;
            }
            if(!status_tracker_.bind_authenticated_epoch(accepted->runtime_epoch)) {
                connection_.disconnect();diagnostic="service_epoch_invalid";return false;
            }
            diagnostic="service_attached";return true;
        }catch(const std::exception& e){connection_.disconnect();diagnostic=e.what();return false;}
    }
    bool exchange(const app::LocalControlFrame& request,
        const app::LocalControlMessageType expected,
        app::LocalControlFrame& response) {
        return connection_.write_frame(request) && connection_.read_frame(response) &&
            response.type == expected && response.request_id == request.request_id;
    }

    HANDLE process_{nullptr};
    bool desktop_detached_{};
    std::optional<win32::installation::ServiceMode> service_mode_;
    DWORD process_id_{0};
    std::uint32_t next_request_id_{1};
    win32::NamedPipeConnection connection_;
    app::AlphaStatusTracker status_tracker_;
};

struct ConfigurationAdminExchange final {
    enum class Phase { queued, running, completed, cancelled };
    app::LocalControlFrame request;
    app::LocalControlFrame response;
    bool handled{false};
    std::atomic<Phase> phase{Phase::queued};
    HANDLE completed{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    ~ConfigurationAdminExchange() {
        if (!request.payload.empty()) SecureZeroMemory(request.payload.data(), request.payload.size());
        if (!response.payload.empty()) SecureZeroMemory(response.payload.data(), response.payload.size());
        if (completed) CloseHandle(completed);
    }
    bool begin() noexcept {
        auto expected = Phase::queued;
        return phase.compare_exchange_strong(expected, Phase::running);
    }
    void expire() noexcept {
        auto expected = Phase::queued;
        (void)phase.compare_exchange_strong(expected, Phase::cancelled);
    }
    void finish() noexcept {
        phase.store(Phase::completed, std::memory_order_release);
        if (completed) SetEvent(completed);
    }
};

class ConfigurationAdminRelay final {
public:
    ~ConfigurationAdminRelay() { stop(); }
    ConfigurationAdminRelay(const ConfigurationAdminRelay&) = delete;
    ConfigurationAdminRelay& operator=(const ConfigurationAdminRelay&) = delete;
    ConfigurationAdminRelay() = default;

    bool start(HWND window, std::filesystem::path root, std::wstring owner) {
        if (worker_.joinable() || window == nullptr) return false;
        verification_.reset();
        window_ = window;
        root_ = std::move(root);
        owner_ = std::move(owner);
        stopping_.store(false);
        try {
            worker_ = std::thread([this] { run(); });
            return true;
        } catch (...) {
            return false;
        }
    }

    void stop() noexcept {
        stopping_.store(true);
        if (worker_.joinable()) {
            (void)CancelSynchronousIo(worker_.native_handle());
            worker_.join();
        }
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (pending_) pending_->expire();
        pending_.reset();
    }

    std::shared_ptr<ConfigurationAdminExchange> begin(const WPARAM id) {
        std::lock_guard<std::mutex> lock(pending_mutex_);
        if (!pending_ || id != pending_id_ || !pending_->begin()) return {};
        return pending_;
    }

private:
    void run() noexcept {
        const auto names = win32::installation_object_names(root_);
        while (!stopping_.load()) {
            try {
                std::string diagnostic;
                auto connection = win32::accept_local_control_client(
                    names.configuration_pipe_name, owner_, 0, 200, &diagnostic);
                if (!connection.valid()) continue;
                ULONG client = 0;
                if (!GetNamedPipeClientProcessId(connection.native_handle(), &client) ||
                    client == 0) continue;
                const auto receipt = sfx::platform::win32::installation::read_receipt(
                    root_, owner_);
                if(!verification_)verification_=std::make_unique<sfx::platform::win32::installation::InstallationVerificationLease>(receipt);
                bool manager_client=false;
                try{verification_->verify_process(receipt,client,sfx::platform::win32::installation::configuration_program_index);}
                catch(...){if(receipt.program_count<=sfx::platform::win32::installation::file_manager_program_index)continue;verification_->verify_process(receipt,client,sfx::platform::win32::installation::file_manager_program_index);manager_client=true;}
                auto exchange = std::make_shared<ConfigurationAdminExchange>();
                if (!exchange->completed || !connection.read_frame(exchange->request)) continue;
                if(manager_client?!app::file_manager_control_request(exchange->request.type):(app::file_management_control_request(exchange->request.type)||exchange->request.type==app::LocalControlMessageType::file_management_query_request))continue;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    if (pending_id_ == std::numeric_limits<WPARAM>::max()) break;
                    ++pending_id_;
                    pending_ = exchange;
                }
                // The message carries only a lookup ID. No cross-thread stack
                // pointer survives a timeout, shutdown or destroyed window.
                const bool posted = PostMessageW(window_, kConfigurationAdminMessage, pending_id_, 0) != FALSE;
                const auto deadline = GetTickCount64() + 30000;
                while (posted && !stopping_.load() && GetTickCount64() < deadline &&
                    WaitForSingleObject(exchange->completed, 100) == WAIT_TIMEOUT) {}
                exchange->expire(); // queued-but-expired work must never execute
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    if (pending_ == exchange) pending_.reset();
                }
                // A running GUI handler retains its own shared owner even when
                // the client goes away. No automatic retry of an uncertain write.
                if (exchange->phase.load(std::memory_order_acquire) ==
                        ConfigurationAdminExchange::Phase::completed &&
                    exchange->handled && !exchange->response.payload.empty() && !stopping_.load())
                    (void)connection.write_frame(exchange->response);
            } catch (...) {
                // A mismatched, stale or disappearing client is refused without
                // affecting Runtime ownership or the Control Panel.
            }
        }
    }

    HWND window_{nullptr};
    std::filesystem::path root_;
    std::wstring owner_;
    std::atomic_bool stopping_{false};
    std::thread worker_;
    std::mutex pending_mutex_;
    std::shared_ptr<ConfigurationAdminExchange> pending_;
    WPARAM pending_id_{0};
    std::unique_ptr<sfx::platform::win32::installation::InstallationVerificationLease> verification_;
};

struct AutomationOptions {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    bool fixture_bootstrap{false};
    bool fixture_ssh_disabled{false};
    std::filesystem::path multinode_directory;
    std::uint64_t multinode_request{0};
    bool confirm_node_command{false};
    std::array<std::array<bool, 6>, app::kAlphaNodePoolCapacity> painted_bold{};
    std::wstring confirmation_heading, confirmation_detail;
    bool auto_close{false};
    std::filesystem::path capture_path;
    std::filesystem::path wizard_capture_path;
    std::filesystem::path capture_suite_directory;
    std::filesystem::path correction_capture_directory;
    std::filesystem::path owner_review_capture_directory;
    std::filesystem::path about_capture_path;
    unsigned int about_close_method{0};
    bool expect_persisted_columns{false};
    bool capture_monitor_appearance{false};
    int expected_monitor_appearance{-1};
    bool final_monitor_color{false};
    std::uint64_t monitor_paints{0};
    std::string monitor_content;
    bool authored_ansi_fixture{false};
    app::ControlPanelTerminalSnapshot last_painted_terminal;
    app::ControlPanelNodeViewFields last_live_fields;
    std::array<std::optional<std::uint64_t>, app::kAlphaNodePoolCapacity + 1> saver_steps;
    RECT painted_canvas{}, painted_art{};
    bool painted_saver{false};
    unsigned native_terminal_paints{0};
    bool native_saver_painted{false};
    unsigned painted_saver_color{0};
    UINT dpi_override{0};
    UINT completion_delay_millis{1000};
    unsigned int suite_step{0};
    int exit_code{0};
    std::optional<app::NodeAssignmentIdentity> last_local_reservation;
    app::LocalControlResponseCode last_local_code{app::LocalControlResponseCode::invalid_request};
    std::string last_local_diagnostic;
    std::string last_local_host;
    std::uint16_t last_local_port{0};
    std::uint32_t last_local_timeout{0};
#endif
};

#include "binkp_console.inc"
struct StartupWork final {
    enum class Phase { verification, runtime, metrics, cleanup } phase{Phase::verification};
    std::atomic<bool> cancelled{false},done{false};
    std::thread thread;
    RuntimeClient runtime;
    std::optional<app::AlphaRuntimeStatus> status;
    std::optional<win32::installation::PreparedFirstStart> first_start;
    std::unique_ptr<win32::WindowsControlPanelMetricSampler> metrics;
    bool success{},service{},cleanup_complete{true};
    std::string diagnostic;
    ~StartupWork(){cancelled.store(true);if(thread.joinable())thread.join();}
};
struct AppState {
    std::unique_ptr<StartupWork> startup;
    bool startup_pending{},startup_close{},preparation_attempted{};
    // Display-only configured identities; never Runtime status or readiness.
    std::vector<app::AlphaNodeStatus> startup_nodes;
    BinkPConsole binkp;
    bool close_after_drain{false};
    app::AlphaStatusSnapshotCache configuration_status_cache;
    HINSTANCE instance{nullptr};
    HWND window{nullptr};
    HWND heading{nullptr};
    HWND subheading{nullptr};
    unsigned startup_dot_count{};
    HWND tab{nullptr};
    HWND node_list{nullptr};
    HWND open_node_button{nullptr};
    HWND notice_button{nullptr};
    HWND disable_all_nodes_button{nullptr};
    HWND enable_all_nodes_button{nullptr};
    HWND confirm_disable_all_button{nullptr}, cancel_disable_all_button{nullptr}, bulk_node_status{nullptr};
    bool bulk_node_change_pending{};
    struct BulkDisableReview {
        app::Identifier128 epoch;
        std::vector<std::pair<std::uint16_t, std::uint64_t>> nodes;
        std::size_t callers{};
    };
    std::optional<BulkDisableReview> bulk_disable_review;
    std::wstring bulk_node_feedback;
    HWND modem_strip{nullptr}, modem_tooltip{nullptr};
    std::wstring modem_hover_text;
    std::wstring modem_tooltip_text;
    HWND events_view{nullptr};
    HWND log_view{nullptr};
    HWND log_pause_button{nullptr};
    HWND log_clear_button{nullptr};
    HWND log_filter{nullptr};
    int selected_log_filter{};
    HWND stats_view{nullptr};
    HWND botgate_view{nullptr};
    HWND terminal_view{nullptr};
    HWND node_header{nullptr};
    HWND monitor_view{nullptr};
    HWND status_bar{nullptr};
    HWND setup_button{nullptr};
    HWND service_button{nullptr};
    std::optional<win32::installation::ServiceState> service_button_state;
    app::Identifier128 service_button_runtime{};
    UINT_PTR service_button_epoch{1};
    DWORD service_button_valid_after{};
    UINT service_button_command{};
    bool service_action_busy{},service_action_start{};
    HFONT heading_font{nullptr};
    HFONT ui_font{nullptr};
    HFONT bold_font{nullptr};
    std::optional<EmbeddedCp437Font> terminal_font;
    HBRUSH background_brush{nullptr};
    HBRUSH panel_brush{nullptr};
    WNDPROC original_tab_proc{nullptr};
    WNDPROC original_header_proc{nullptr};
    WNDPROC original_node_list_proc{nullptr};
    HWND list_header{nullptr};
    std::wstring status_text{L"Starting…"};
    std::wstring compact_status_text;
    RuntimeClient runtime;
    app::SoundPollRequest sound_cursor{win32::WindowsSecureIdentifierGenerator{}.generate(),{},0,true};
    app::EventSoundConfiguration sound_mappings;
    win32::EventSoundPlayer event_sound_player;
    std::deque<app::SoundEventItem> pending_sounds;
    bool sound_preview_open{};
    std::wstring sound_warning;
    ConfigurationAdminRelay configuration_admin;
    app::AlphaRuntimeStatus status;
    app::OperationalLogCache log_cache;
    bool log_page_failed{false};
    app::ControlPanelMetricSnapshot metrics;
    app::ControlPanelSqliteMetricSnapshot sqlite_metrics;
    app::AlphaRuntimeMetricSnapshot runtime_metrics;
    std::unique_ptr<win32::WindowsControlPanelMetricSampler> metric_sampler;
    app::BoundedMetricHistory cpu_history{60};
    app::BoundedMetricHistory memory_history{60};
    app::BoundedMetricHistory disk_history{60};
    app::BoundedMetricHistory sqlite_history{60};
    app::BoundedMetricHistory network_history{60};
    std::optional<app::ControlPanelTerminalSnapshot> owner_node_view;
    std::optional<app::ControlPanelTerminalSnapshot> owner_screen_saver;
    std::optional<EmbeddedBitmap> about_image;
    app::ControlPanelUiPreferences preferences;
    app::NodeViewScale node_view_scale{app::NodeViewScale::auto_fit};
    // Node numbers are one-based and may span the entire Runtime node pool.
    std::array<app::ScreenSaverPlacement, app::kAlphaNodePoolCapacity + 1> saver_placement;
    HMENU scale_menu{nullptr};
    HMENU theme_menu{nullptr};
    HMENU monitor_appearance_menu{nullptr};
    HMENU monitor_width_menu{nullptr};
    std::vector<std::pair<CHARRANGE, COLORREF>> rendered_log_colors;
    std::vector<std::uint16_t> open_node_views;
    app::AlphaNodeIndicators indicators;
    app::AlphaModemIndicators modem_indicators;
    std::array<bool, 129> observed_screen_active{};
    std::array<bool, 129> observed_ready{};
    std::array<ULONGLONG, 129> node_view_interaction{};
    std::uint16_t displayed_node{0};
    win32::WindowsBbsLogTimestamp log_time;
    std::string rendered_log_zone;
    std::uint8_t rendered_log_health{0};
    bool rendered_hostname_exhausted{false};
    bool logs_paused{false};
    std::wstring rendered_status_text,rendered_compact_status_text;
    std::uint64_t logs_cleared_through{0};
    std::uint64_t rendered_log_first_sequence{0};
    std::uint64_t rendered_log_last_sequence{0};
    std::uint64_t rendered_log_clear_through{0};
    std::size_t rendered_log_count{0};
    bool log_view_initialized{false};
    bool applying_node_columns{false};
    HMODULE rich_edit_module{nullptr};
    HMENU node_menu{nullptr};
    app::AccountId menu_edit_account{};
    app::NodeAssignmentIdentity menu_edit_assignment{};
    bool have_status{false};
    app::ChatOperatorSnapshot chat_status;
    std::vector<HWND> operator_windows;
    std::uint64_t last_page_alert{};
    bool closing{false};
    bool full_screen{false}, restoring_placement{false};
    WINDOWPLACEMENT windowed_placement{sizeof(WINDOWPLACEMENT)};
    LONG_PTR windowed_style{0}, windowed_ex_style{0};
    HMENU windowed_menu{nullptr};
    AutomationOptions automation;

    ~AppState() {
        startup.reset();
        if (heading_font != nullptr) DeleteObject(heading_font);
        if (ui_font != nullptr) DeleteObject(ui_font);
        if (bold_font != nullptr) DeleteObject(bold_font);
        if (about_image && about_image->handle != nullptr) DeleteObject(about_image->handle);
        if (background_brush != nullptr) DeleteObject(background_brush);
        if (panel_brush != nullptr) DeleteObject(panel_brush);
    }
};
#include "chat_notice_dialog.inc"
#include "operator_chat_dialog.inc"
#include "event_sounds_dialog.inc"


const app::AlphaNodeStatus* node_status(const AppState& state, std::uint16_t number);
void begin_startup(AppState& state,StartupWork::Phase phase);
void show_selected_page(AppState& state, bool interacted = true, bool refresh = true);
void layout(AppState& state, int width, int height);
void fill_rectangle(HDC dc, const RECT& rectangle, COLORREF color);
void frame_rectangle(HDC dc, RECT rectangle, COLORREF color);

std::wstring runtime_state_text(const app::AlphaRuntimePresentationState state) {
    switch (state) {
        case app::AlphaRuntimePresentationState::starting: return L"STARTING";
        case app::AlphaRuntimePresentationState::setup_required: return L"SETUP REQUIRED";
        case app::AlphaRuntimePresentationState::ready: return L"READY";
        case app::AlphaRuntimePresentationState::recovery_required: return L"RECOVERY REQUIRED";
        case app::AlphaRuntimePresentationState::stopping: return L"STOPPING";
        case app::AlphaRuntimePresentationState::faulted: return L"FAULTED";
        case app::AlphaRuntimePresentationState::draining: return L"GRACEFUL SHUT DOWN";
        case app::AlphaRuntimePresentationState::stopped: return L"STOPPED";
    }
    return L"FAULTED";
}

std::wstring node_state_text(const sfx::core::NodeState state) {
    switch (state) {
        case sfx::core::NodeState::offline: return L"OFFLINE";
        case sfx::core::NodeState::ready: return L"READY";
        case sfx::core::NodeState::connecting: return L"CONNECTING";
        case sfx::core::NodeState::online: return L"ONLINE";
        case sfx::core::NodeState::door: return L"DOOR";
        case sfx::core::NodeState::transfer: return L"TRANSFER";
        case sfx::core::NodeState::chat: return L"CHAT";
        case sfx::core::NodeState::draining: return L"DISABLING";
    }
    return L"FAULTED";
}

void update_node_header(AppState& state) {
    if (state.displayed_node == 0) {
        SetWindowTextW(state.node_header, L"");
        return;
    }
    const auto* displayed = node_status(state, state.displayed_node);
    if (displayed == nullptr) { SetWindowTextW(state.node_header, L""); return; }
    const std::wstring header = L"Node " + std::to_wstring(displayed->number) + L"  •  " +
        (!displayed->available ? L"NOT AVAILABLE" : !displayed->enabled ? L"DISABLED" :
            node_state_text(displayed->state)) + L"  •  Caller: " +
        (displayed->caller_handle.empty() ? L"—" : utf8_to_wide(displayed->caller_handle)) +
        L"  •  Connection: " +
        (displayed->connection.empty() ? L"—" : utf8_to_wide(displayed->connection)) +
        L"  •  Activity: " + utf8_to_wide(displayed->activity) +
        L"  •  Duration: " + std::to_wstring(displayed->connected_seconds) + L"s  •  " +
        L"Terminal: " + std::to_wstring(displayed->terminal_columns) + L"×" +
        std::to_wstring(displayed->terminal_rows) + L" (mirror 80×25)" +
        (displayed->details == "Ready for connection" ? std::wstring{} :
            L"  •  " + utf8_to_wide(displayed->details)) +
        (displayed->source_ip.empty() ? std::wstring{} :
            L"  •  Source: " + (displayed->hostname.empty() ? utf8_to_wide(displayed->source_ip) :
                utf8_to_wide(displayed->hostname + " [" + displayed->source_ip + "]")));
    SetWindowTextW(state.node_header, header.c_str());
}

// Disposable presentation only. RuntimeClient retains authenticated epoch and
// ordering high-water marks. No command, reconnect, or caller cancellation.
void clear_node_presentation(AppState& state) {
    state.have_status = false;
    state.status = {};
    state.indicators.clear();
    state.modem_indicators.clear();
    state.observed_screen_active = {};
    state.observed_ready = {};
    state.node_view_interaction = {};
    state.displayed_node = 0;
    state.open_node_views.clear();
    while (TabCtrl_GetItemCount(state.tab) > 6) TabCtrl_DeleteItem(state.tab, 6);
    select_panel_page(state.tab, 0);
    ListView_DeleteAllItems(state.node_list);
    SetWindowTextW(state.node_header, L"");
    KillTimer(state.window, kPulseTimer);
    state.logs_cleared_through = 0;
    state.log_cache.clear_presentation();
    state.runtime_metrics = {};
    state.log_page_failed = false;
    KillTimer(state.window, kLogPageTimer);
    state.rendered_log_first_sequence = state.rendered_log_last_sequence = 0;
    state.rendered_log_clear_through = 0;
    state.rendered_log_count = 0;
    state.log_view_initialized = false;
    state.rendered_log_zone.clear(); state.rendered_log_health = 0;
    state.rendered_hostname_exhausted = false;
    SetWindowTextW(state.log_view, L"");
#ifdef SFX_ALPHA_TEST_AUTOMATION
    state.automation.last_painted_terminal = {};
    state.automation.last_live_fields = {};
    state.automation.painted_bold = {};
#endif
    show_selected_page(state, false, false);
    InvalidateRect(state.terminal_view, nullptr, TRUE);
}

#include "alpha_live_logs.inl"

std::wstring recorded_local_time(const AppState& state, const std::int64_t value) {
    win32::WindowsBbsLogTimestamp formatter;
    if (!formatter.configure(state.status.bbs_time_zone)) return L"Unavailable";
    const auto formatted = formatter.format(value);
    return formatted ? utf8_to_wide(*formatted) + L" BBS local" : L"Unavailable";
}
std::wstring scheduler_local_time(const AppState& state, const std::int64_t value) {
    return value <= 0 ? L"Not yet" : recorded_local_time(state,value);
}

const wchar_t* scheduler_result_text(const app::ScheduledJobResult result) {
    switch (result) {
        case app::ScheduledJobResult::never: return L"Never run";
        case app::ScheduledJobResult::running: return L"RUNNING";
        case app::ScheduledJobResult::success: return L"COMPLETED";
        case app::ScheduledJobResult::failed: return L"FAILED";
        case app::ScheduledJobResult::interrupted: return L"WARNING — interrupted by Runtime stop; no retry";
    }
    return L"Unknown";
}

void update_scheduler_page(AppState& state) {
    const bool ready = state.have_status &&
        (state.status.state == app::AlphaRuntimePresentationState::ready ||
            state.status.state == app::AlphaRuntimePresentationState::draining);
    const auto& scheduler = state.status.scheduler;
    std::wstring text;
    if (!ready || !scheduler.configuration_loaded) {
        text = L"Built-in job: Backup\r\n\r\nScheduler: Not Available\r\nDiagnostic: " +
            utf8_to_wide(scheduler.diagnostic_code);
    } else {
        wchar_t scheduled_time[16]{};
        swprintf_s(scheduled_time, L"%02u:%02u",
            scheduler.local_hour, scheduler.local_minute);
        text = L"Built-in job: Backup\r\n"
            L"Backup location: " + win32::alpha_installation_root().native() +
            L"\\Backup\r\n\r\nSchedule: " +
            std::wstring(scheduler.enabled ? L"Enabled - daily at " : L"Disabled - saved time ") +
            scheduled_time + L" local time\r\n"
            L"Missed run policy: Skip after Runtime restart\r\n"
            L"Automatic retry: None\r\n"
            L"Next eligible run: " + scheduler_local_time(state, scheduler.next_run_utc) +
            L"\r\n\r\nCurrent state: " +
            std::wstring(scheduler.queued ? L"QUEUED" :
                scheduler.active ? L"RUNNING" : L"Idle") +
            (scheduler.active ?
                (scheduler.active_scheduled ? L" — scheduled backup" : L" — manual backup") : L"") +
            (scheduler.active ? L"\r\nRun identity: " + utf8_to_wide(scheduler.active_run_id) +
                L"\r\nStarted: " + scheduler_local_time(state, scheduler.active_started_utc) : L"") +
            L"\r\nLast result: " + scheduler_result_text(scheduler.last_result) +
            L"\r\nLast completion: " + scheduler_local_time(state, scheduler.last_run_utc) +
            L"\r\nDetail/archive: " + utf8_to_wide(scheduler.last_detail) +
            L"\r\nRetained history entries: " +
            std::to_wstring(scheduler.history.size()) + L" / " +
            std::to_wstring(app::kSchedulerHistoryLimit) +
            L"\r\nConfiguration revision: " +
            std::to_wstring(scheduler.configuration_revision) +
            L"\r\nDiagnostic: " + utf8_to_wide(scheduler.diagnostic_code);
        if (!scheduler.history.empty()) {
            text += L"\r\n\r\nHistory (oldest to newest):";
            for (const auto& item : scheduler.history) {
                text += L"\r\n" + scheduler_local_time(state, item.finished_utc) +
                    L" | " + scheduler_result_text(item.result) + L" | " +
                    std::to_wstring(item.finished_utc - item.started_utc) + L"s | " +
                    utf8_to_wide(item.detail);
            }
        }
    }
    std::wstring report = L"EVENTS — read-only; administration is in BBS > SFX Config\r\n\r\n";
    if (state.have_status && !state.status.external_event_activity.empty())
        report += L"Active external events\r\n" + utf8_to_wide(state.status.external_event_activity) + L"\r\n";
    report += L"Recent operating events — this Runtime only (newest first, up to 32):\r\n";
    if (!state.have_status || !state.status.reporting.available) report += L"Not Available\r\n";
    else if (state.status.reporting.recent.empty()) report += L"No recorded operating events yet.\r\n";
    else for (auto it = state.status.reporting.recent.rbegin(); it != state.status.reporting.recent.rend(); ++it)
        report += recorded_local_time(state,it->utc_seconds) + L" | " + utf8_to_wide(it->level) +
            L" | " + utf8_to_wide(it->message) + L"\r\n";
    if (state.status.log_health) report += L"Recording warning: " +
        utf8_to_wide(std::string(app::operational_log_health_text(state.status.log_health))) + L"\r\n";
    report += L"\r\nCommitted account/security audit — persisted, newest 32 records:\r\n";
    if (!state.have_status || !state.status.administration_history.available) report += L"Not Available\r\n";
    else if (state.status.administration_history.recent.empty()) report += L"No committed audit records.\r\n";
    else for (const auto& entry : state.status.administration_history.recent) {
        report += recorded_local_time(state,entry.occurred_utc_seconds) + L" | " +
            utf8_to_wide(std::string(app::administration_summary_label(entry)));
        if (entry.subject_account_id) report += L" | account #" + std::to_wstring(entry.subject_account_id);
        if (entry.actor_account_id) report += L" | actor #" + std::to_wstring(entry.actor_account_id);
        report += L"\r\n";
    }
    text = report + L"\r\n" + text;
    // Updating unchanged status must not repeatedly reset a Sysop's scroll or
    // text selection while they review history.
    if (window_text(state.events_view) != text) {
        DWORD start = 0, end = 0;
        SendMessageW(state.events_view,EM_GETSEL,reinterpret_cast<WPARAM>(&start),reinterpret_cast<LPARAM>(&end));
        const auto top = SendMessageW(state.events_view,EM_GETFIRSTVISIBLELINE,0,0);
        SetWindowTextW(state.events_view, text.c_str());
        SendMessageW(state.events_view,EM_SETSEL,start,end);
        SendMessageW(state.events_view,EM_LINESCROLL,0,top - SendMessageW(state.events_view,EM_GETFIRSTVISIBLELINE,0,0));
    }
}

const std::vector<app::AlphaNodeStatus>& displayed_node_rows(const AppState& state){
    return state.startup_pending?state.startup_nodes:state.status.nodes;
}

bool startup_surface_input(const AppState& state,UINT message){
    return state.startup_pending&&((message>=WM_MOUSEFIRST&&message<=WM_MOUSELAST)||
        (message>=WM_NCMOUSEMOVE&&message<=WM_NCXBUTTONDBLCLK)||message==WM_VSCROLL||message==WM_HSCROLL||
        (message>=WM_KEYFIRST&&message<=WM_KEYLAST)||message==WM_CONTEXTMENU||
        message==WM_SETFOCUS||message==WM_MOUSEACTIVATE);
}

void refresh_node_rows(AppState& state) {
    // Rows belong to stable node numbers, not caller assignments. Updating them
    // in place preserves native multi-selection, keyboard focus and Shift anchor.
    const auto& nodes=displayed_node_rows(state);
    bool same_rows = ListView_GetItemCount(state.node_list) == static_cast<int>(nodes.size());
    for (std::size_t i = 0; same_rows && i < nodes.size(); ++i) {
        LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = static_cast<int>(i);
        same_rows = ListView_GetItem(state.node_list, &item) &&
            item.lParam == nodes[i].number;
    }
    if (!same_rows) ListView_DeleteAllItems(state.node_list);
    state.indicators.observe(state.status, GetTickCount64());
    state.modem_indicators.observe(state.status, GetTickCount64());
    if (state.status.lifecycle.active_work.caller_sessions) SetTimer(state.window, kPulseTimer, 250, nullptr);
    else KillTimer(state.window, kPulseTimer);
    for (std::size_t index = 0; index < nodes.size(); ++index) {
        const auto& node = nodes[index];
        if (node.number < state.observed_screen_active.size()) {
            const bool ready = !node.screen_active && node.available && node.enabled &&
                node.state == sfx::core::NodeState::ready;
            if (ready && !state.observed_ready[node.number])
                state.node_view_interaction[node.number] = GetTickCount64();
            state.observed_screen_active[node.number] = node.screen_active;
            state.observed_ready[node.number] = ready;
        }
        const std::wstring node_label = L"Node " + std::to_wstring(node.number);
        const std::wstring state_label = state.startup_pending?node_state_text(sfx::core::NodeState::offline):!node.available ? L"NOT AVAILABLE" :
            (!node.enabled ? L"DISABLED" : node_state_text(node.state));
        const std::array<std::wstring, 6> columns{
            node_label, state_label, utf8_to_wide(node.caller_handle),
            utf8_to_wide(node.connection), utf8_to_wide(node.activity),
            utf8_to_wide(node.details)+(std::any_of(state.chat_status.pages.begin(),state.chat_status.pages.end(),[&](const auto& page){return page.caller.node==node.number&&page.caller.account==node.account_id;})?L" - Page requested":L"")};
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(index);
        item.lParam = node.number;
        item.pszText = const_cast<wchar_t*>(columns[0].c_str());
        if (!same_rows) ListView_InsertItem(state.node_list, &item);
        for (int column = 1; column < 6; ++column){
            const auto subitem=column+(state.preferences.modem_lights?1:0);wchar_t old_text[4096]{};
            ListView_GetItemText(state.node_list,static_cast<int>(index),subitem,old_text,4096);
            if(columns[static_cast<std::size_t>(column)]!=old_text)ListView_SetItemText(state.node_list,static_cast<int>(index),subitem,const_cast<wchar_t*>(columns[static_cast<std::size_t>(column)].c_str()));
        }
    }
}

void update_event_sounds(AppState& state){
    const auto batch=state.runtime.sound_poll(state.sound_cursor);
    if(!batch||!batch->owner){state.event_sound_player.stop();state.pending_sounds.clear();return;}
    if(state.sound_cursor.epoch!=batch->epoch){state.event_sound_player.stop();state.pending_sounds.clear();}
    state.sound_cursor.epoch=batch->epoch;state.sound_cursor.after=batch->through;
    state.sound_cursor.fresh_desktop_start=false;state.sound_mappings=batch->configuration;
    if(state.sound_preview_open){state.pending_sounds.clear();return;}
    const auto now=GetTickCount64();
    for(const auto& event:batch->events){
        if(state.pending_sounds.size()==16)state.pending_sounds.pop_front();
        state.pending_sounds.push_back(event);
    }
    if(state.event_sound_player.playing())return;
    state.event_sound_player.stop();
    while(!state.pending_sounds.empty()){
        const auto event=state.pending_sounds.front();state.pending_sounds.pop_front();
        if(event.at_ms>now||now-event.at_ms>3000)continue;
        const auto& path=state.sound_mappings.paths[static_cast<std::size_t>(event.event)];if(path.empty())continue;
        try{
            std::wstring detail;
            if(state.event_sound_player.preview(utf8_to_wide(path),detail)){state.sound_warning.clear();break;}
            state.sound_warning=L"Event sound unavailable: "+utf8_to_wide(std::string(app::kSoundEventNames[static_cast<std::size_t>(event.event)]))+L". Check BBS > Event Sounds. "+detail;
        }catch(...){state.sound_warning=L"Event sound unavailable. Check BBS > Event Sounds.";}
    }
}

void synchronize_ownership_menu(AppState& state,bool service){
    const auto bbs=GetSubMenu(GetMenu(state.window),1);if(!bbs)return;
    for(const auto item:{std::make_pair(ID_BBS_SERVICE_START,L"Start BBS (Windows Service)"),std::make_pair(ID_BBS_SERVICE_STOP,L"Stop BBS (Windows Service)...")}){
        const bool present=GetMenuState(bbs,item.first,MF_BYCOMMAND)!=static_cast<UINT>(-1);
        if(service&&!present)AppendMenuW(bbs,MF_STRING,item.first,item.second);
        else if(!service&&present)DeleteMenu(bbs,item.first,MF_BYCOMMAND);
    }
}
void update_service_button(AppState& state) {
    // The first window is created before ownership metadata is verified.
    // Publish the Service-only actions after handoff, and retire on mode change.
    synchronize_ownership_menu(state,state.runtime.is_service_mode());
    std::optional<win32::installation::ServiceState> actual;
    UINT command{};const wchar_t* label=L"Start BBS";
    if(state.runtime.is_service_mode()) {
        label=L"Service unavailable";
        try {
            actual=state.runtime.service_control_state();
            if(state.service_action_busy)label=state.service_action_start?L"Starting BBS...":L"Stopping BBS...";
            else if(actual->state==SERVICE_START_PENDING)label=L"Starting BBS...";
            else if(actual->state==SERVICE_STOP_PENDING)label=L"Stopping BBS...";
            else if(state.have_status&&actual->state==SERVICE_STOPPED&&actual->start_type!=SERVICE_DISABLED&&state.status.state==app::AlphaRuntimePresentationState::stopped){
                label=L"Start BBS";command=ID_BBS_SERVICE_START;
            }else if(state.have_status&&actual->state==SERVICE_RUNNING&&
                (state.status.state==app::AlphaRuntimePresentationState::ready||state.status.state==app::AlphaRuntimePresentationState::setup_required||
                 state.status.state==app::AlphaRuntimePresentationState::draining)){
                label=L"Stop BBS";command=ID_BBS_SERVICE_STOP;
            }
        }catch(...){actual.reset();}
    }
    const auto& old=state.service_button_state;
    if(command!=state.service_button_command||actual.has_value()!=old.has_value()||
       (actual&&old&&(actual->state!=old->state||actual->process_id!=old->process_id||actual->start_type!=old->start_type))||
       !(state.service_button_runtime==state.status.runtime_epoch)){
        if(++state.service_button_epoch==0)++state.service_button_epoch;
        state.service_button_valid_after=static_cast<DWORD>(GetTickCount64());
    }
    state.service_button_state=actual;state.service_button_command=command;state.service_button_runtime=state.status.runtime_epoch;
    (void)label; // Main surface monitors; service controls live in SFXConfig.

    const auto menu=GetMenu(state.window);
    EnableMenuItem(menu,ID_BBS_SERVICE_START,MF_BYCOMMAND|(command==ID_BBS_SERVICE_START?MF_ENABLED:MF_GRAYED));
    EnableMenuItem(menu,ID_BBS_SERVICE_STOP,MF_BYCOMMAND|(command==ID_BBS_SERVICE_STOP?MF_ENABLED:MF_GRAYED));
}
bool node_state_commands_ready(const AppState& state) {
    return !state.startup_pending && !state.closing && state.have_status &&
        state.status.state == app::AlphaRuntimePresentationState::ready;
}

bool can_set_all_nodes(const AppState& state, bool enabled) {
    return node_state_commands_ready(state) && std::any_of(state.status.nodes.begin(), state.status.nodes.end(),
        [enabled](const auto& node) { return node.available && node.enabled != enabled; });
}

void update_bulk_node_buttons(AppState& state) {
    const bool on_nodes = panel_page(state.tab) == 0;
    const bool reviewing = on_nodes && state.bulk_disable_review.has_value();
    const bool available = !state.service_action_busy && !state.bulk_node_change_pending && on_nodes;
    EnableWindow(state.disable_all_nodes_button, available && !reviewing && can_set_all_nodes(state, false));
    EnableWindow(state.enable_all_nodes_button, available && !reviewing && can_set_all_nodes(state, true));
    ShowWindow(state.bulk_node_status, on_nodes && !state.bulk_node_feedback.empty() ? SW_SHOWNA : SW_HIDE);
    ShowWindow(state.confirm_disable_all_button, reviewing ? SW_SHOWNA : SW_HIDE);
    ShowWindow(state.cancel_disable_all_button, reviewing ? SW_SHOWNA : SW_HIDE);
    EnableWindow(state.confirm_disable_all_button, available && reviewing && can_set_all_nodes(state, false));
    EnableWindow(state.cancel_disable_all_button, available && reviewing);
}

void update_status(AppState& state) {
    if(state.startup_pending)return;
#if defined(SFX_ALPHA_TEST_AUTOMATION) && defined(SFX_CONTROL_PANEL_SYNTHETIC_STATUS)
    // Standalone native UI fixture: retain its in-memory snapshot, never start Runtime.
    update_bulk_node_buttons(state);
#else
    auto current = state.runtime.status(state.displayed_node);
    if (!current || (state.have_status && !(state.status.runtime_epoch == current->runtime_epoch)))
        clear_node_presentation(state);
    const int selected_page = panel_page(state.tab);
    const bool nodes_page_active = selected_page == 0;
    const bool statistics_page_active = selected_page == 3;
    const bool node_view_active = selected_page >= 5;
    if (!current) {
        state.binkp.apply({});
        state.event_sound_player.stop();state.pending_sounds.clear();
        state.have_status = false;
        update_bulk_node_buttons(state);
        update_service_button(state);
        state.chat_status={};EnableMenuItem(GetMenu(state.window),ID_BBS_CHAT_AVAILABLE,MF_BYCOMMAND|MF_GRAYED);
        SetWindowTextW(state.botgate_view,L"BotGate status unavailable; Runtime connection lost. No live totals inferred.");
        update_scheduler_page(state);
        SetWindowTextW(state.subheading,state.runtime.is_service_mode()?L"Service control unavailable — check BBS status; reopen Control Panel":
            L"Runtime control lost — Desktop Runtime shuts down; reopen SFX after shutdown");
        state.status_text = L"FAULTED — runtime process/control channel unavailable";
        state.compact_status_text.clear();
        SendMessageW(state.status_bar, SB_SETTEXTW, SBT_OWNERDRAW,
            reinterpret_cast<LPARAM>(&state));
        if (nodes_page_active) InvalidateRect(state.node_list, nullptr, TRUE);
        return;
    }
    state.status = std::move(*current);
    if (state.log_cache.cursor().epoch != state.status.runtime_epoch){
        state.log_cache.bind(state.status.runtime_epoch);state.last_page_alert=0;
    }
    state.have_status = true;
    update_bulk_node_buttons(state);
    update_service_button(state);
    if(selected_page==-14&&!IsIconic(state.window))state.binkp.apply(state.runtime.ftn_runtime({}));
    update_event_sounds(state);
    state.chat_status=state.runtime.chat_operator({});
    CheckMenuItem(GetMenu(state.window),ID_BBS_CHAT_AVAILABLE,MF_BYCOMMAND|
        (state.chat_status.available?MF_CHECKED:MF_UNCHECKED));
    EnableMenuItem(GetMenu(state.window),ID_BBS_CHAT_AVAILABLE,MF_BYCOMMAND|
        (state.chat_status.code==app::ChatResult::ok?MF_ENABLED:MF_GRAYED));
    auto latest_page=state.last_page_alert;
    for(const auto& page:state.chat_status.pages)if(page.request>state.last_page_alert){
        latest_page=std::max(latest_page,page.request);FlashWindow(state.window,TRUE);
    }
    state.last_page_alert=latest_page;
    // Keep owner health/sounds/pages fresh while minimized, without hidden redraws.
    if(IsIconic(state.window))return;
    if(selected_page==1)update_scheduler_page(state);
    const std::wstring heading = utf8_to_wide(state.status.bbs_name) +
        (state.runtime.is_service_mode()?L"  •  Windows Service":L"  •  Desktop Mode")+
        L"  •  " + runtime_state_text(state.status.state) + L"  •  Callers " +
        std::to_wstring(state.status.lifecycle.active_work.caller_sessions) + L"/" +
        std::to_wstring(state.status.lifecycle.configured_nodes);
    BinkPConsole::text_if_changed(state.subheading,heading);
    const bool runtime_ready = state.status.state == app::AlphaRuntimePresentationState::ready;
    const std::wstring runtime = L"Runtime " +
        std::wstring(runtime_ready ? L"RUNNING" : runtime_state_text(state.status.state));
    const std::wstring telnet = L"Telnet " +
        std::wstring(state.status.telnet_active ? L"LISTENING on " : L"NOT LISTENING — ") +
        utf8_to_wide(state.status.telnet_bind) + L":" +
        std::to_wstring(state.status.telnet_port);
    const std::wstring ssh = L"SSH " + std::wstring(!state.status.ssh_configured ? L"DISABLED — " :
        state.status.ssh_active ? L"LISTENING on " : L"NOT LISTENING — ") +
        utf8_to_wide(state.status.ssh_bind) + L":" + std::to_wstring(state.status.ssh_port);
    const std::wstring status = runtime + L"   |   " + telnet + L"   |   " + ssh;
    state.status_text = (state.status.log_health ? L"LOG WARNING - see Live Logs | " :
        state.status.hostname_exhausted ? L"HOSTNAME LOOKUP UNAVAILABLE - see Live Logs | " : L"") + status;
    // Preserve the gold footer and detailed wording at normal widths. At the
    // minimum window width keep both complete endpoints visible (ON=listening).
    state.compact_status_text = (state.status.log_health ? L"LOG WARNING | " :
        state.status.hostname_exhausted ? L"HOSTNAME LOOKUP UNAVAILABLE | " : L"") + runtime + L" | Telnet " +
        utf8_to_wide(state.status.telnet_bind) + L":" + std::to_wstring(state.status.telnet_port) +
        (state.status.telnet_active ? L" ON" : L" DOWN") + L" | SSH " +
        utf8_to_wide(state.status.ssh_bind) + L":" + std::to_wstring(state.status.ssh_port) +
        (!state.status.ssh_configured ? L" OFF" : state.status.ssh_active ? L" ON" : L" DOWN");
    if(!state.sound_warning.empty()){
        state.status_text=state.sound_warning+L" | "+state.status_text;
        state.compact_status_text=L"SOUND WARNING - BBS > Event Sounds | "+state.compact_status_text;
    }
    if(state.rendered_status_text!=state.status_text||state.rendered_compact_status_text!=state.compact_status_text){
        SendMessageW(state.status_bar, SB_SETTEXTW, SBT_OWNERDRAW,reinterpret_cast<LPARAM>(&state));
        state.rendered_status_text=state.status_text;state.rendered_compact_status_text=state.compact_status_text;
    }
    const bool needs_setup = state.status.nodes.empty() &&
        (state.status.state == app::AlphaRuntimePresentationState::setup_required ||
        state.status.state == app::AlphaRuntimePresentationState::recovery_required);
    ShowWindow(state.setup_button, needs_setup ? SW_SHOW : SW_HIDE);
    SetWindowTextW(state.setup_button,
        state.status.state == app::AlphaRuntimePresentationState::recovery_required
            ? L"Recovery…" : L"First-Run Setup…");
    refresh_node_rows(state);

    refresh_live_logs(state);
    if(selected_page==4) {
        const auto gate=state.runtime.botgate_feed_status();
        if(!gate) SetWindowTextW(state.botgate_view,L"BotGate status unavailable; no totals inferred.");
        else {
            (void)state.log_time.configure(state.status.bbs_time_zone);
            const auto on=[](bool value){return value ? L"On" : L"Off";};
            std::wstring text=L"BOTGATE PROTECTION\r\nSettings: SFXConfig > Security > BotGate\r\n\r\nGlobal: "+std::wstring(on(gate->enabled))+
                L"   Telnet: "+on(gate->telnet)+L"   New SSH registration challenge: "+on(gate->ssh)+
                L"\r\nTotals since Runtime start: "+utf8_to_wide(state.log_time.format(static_cast<std::int64_t>(gate->protection_started)).value_or("unavailable"))+L"\r\n";
            const std::array<const wchar_t*,7> labels{{L"Connections checked",L"Challenge passed",L"Challenge failed",L"Challenge timed out",L"Blocked",L"Filter exemptions (challenge still required)",L"Challenges active now"}};
            for(unsigned i=0;i<labels.size();++i) text+=std::wstring(labels[i])+L": "+std::to_wstring(gate->protection_counts[i])+L"\r\n";
            text+=L"\r\nRules: "+utf8_to_wide(gate->rule_status)+(gate->rules_ready ? L" (last good rules active)" : L" (unavailable)")+L"\r\n";
            const std::array<const wchar_t*,6> rule_names{{L"Exempt",L"Manual IP",L"Silent IP",L"Verified hostname",L"Temporary entries (may include expired)",L"Feed exceptions"}};
            for(unsigned i=0;i<rule_names.size();++i) text+=std::wstring(rule_names[i])+L": "+std::to_wstring(gate->rule_counts[i])+L"   ";
            text+=L"\r\nGeo files: "+(gate->geo_summary.empty() ? std::wstring(L"none") : utf8_to_wide(gate->geo_summary));
            text+=L"\r\nDownloaded feed: "+std::to_wstring(gate->entries)+L" addresses; "+utf8_to_wide(gate->result)+
                (gate->available ? L"" : L" (DEGRADED: no valid cached feed)")+L"\r\nLast successful update: "+(gate->last_success ? utf8_to_wide(state.log_time.format(gate->last_success).value_or("unavailable")) : L"Never");
            text+=L"\r\n\r\nRECENT BOTGATE ACTIVITY (bounded Runtime notification tail; full history in Live Logs)\r\n";
            for(const auto& entry:state.status.logs) if(app::operational_filter_group(entry)==3) {
                // The latest feed attempt has one contiguous progress block
                // below, even after its start event leaves the bounded tail.
                const bool feed_event=entry.event && (entry.event->kind==app::OperationalKind::feed_update_started ||
                    entry.event->kind==app::OperationalKind::feed_update_completed ||
                    entry.event->kind==app::OperationalKind::feed_update_failed);
                if(feed_event && gate->last_attempt && entry.utc_seconds>=gate->last_attempt) continue;
                text+=utf8_to_wide(app::render_operational_line(entry,state.log_time.format(entry.utc_seconds)))+L"\r\n";
            }
            if(gate->last_attempt) text+=L"\r\n"+utf8_to_wide(app::render_botgate_feed_progress(*gate));
            SetWindowTextW(state.botgate_view,text.c_str());
        }
    }
    if (statistics_page_active) InvalidateRect(state.stats_view, nullptr, FALSE);
    if (node_view_active) {
        InvalidateRect(state.terminal_view, nullptr, FALSE);
        update_node_header(state);
        if (state.preferences.modem_lights) InvalidateRect(state.modem_strip, nullptr, FALSE);
    }
    if (nodes_page_active) InvalidateRect(state.node_list, nullptr, FALSE);
#endif
}

void sample_visible_monitor(AppState& state) {
    if (state.metric_sampler) state.metrics = state.metric_sampler->sample();
    const auto runtime_metrics = state.runtime.metrics();
    state.runtime_metrics = runtime_metrics.value_or(app::AlphaRuntimeMetricSnapshot{});
    state.sqlite_metrics = runtime_metrics
        ? runtime_metrics->sqlite : app::ControlPanelSqliteMetricSnapshot{};
    if (state.metrics.cpu_available)
        state.cpu_history.push(state.metrics.total_cpu_percent);
    if (state.metrics.memory_available && state.metrics.memory_total_bytes != 0)
        state.memory_history.push(100.0 * static_cast<double>(state.metrics.memory_used_bytes) /
            static_cast<double>(state.metrics.memory_total_bytes));
    if (state.metrics.storage_activity_available)
        state.disk_history.push(state.metrics.storage_read_bytes_per_second +
            state.metrics.storage_write_bytes_per_second);
    if (state.sqlite_metrics.available && state.sqlite_metrics.interval_available)
        state.sqlite_history.push(static_cast<double>(state.sqlite_metrics.page_reads_per_second +
            state.sqlite_metrics.page_writes_per_second));
    if (state.metrics.network_available)
        state.network_history.push(state.metrics.network_receive_bytes_per_second +
            state.metrics.network_send_bytes_per_second);
}

std::vector<std::wstring> installed_time_zone_keys() {
    constexpr wchar_t root_path[] =
        L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones";
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, root_path, 0, KEY_READ, &root) != ERROR_SUCCESS) return {};
    std::vector<std::wstring> result;
    for (DWORD index = 0;; ++index) {
        std::array<wchar_t, 256> name{};
        DWORD length = static_cast<DWORD>(name.size());
        const LSTATUS status = RegEnumKeyExW(root, index, name.data(), &length,
            nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status != ERROR_SUCCESS || length == 0 || length >= name.size()) {
            result.clear();
            break;
        }
        result.emplace_back(name.data(), length);
    }
    RegCloseKey(root);
    std::sort(result.begin(), result.end());
    return result;
}

std::wstring current_time_zone_key() {
    DYNAMIC_TIME_ZONE_INFORMATION value{};
    if (GetDynamicTimeZoneInformation(&value) == TIME_ZONE_ID_INVALID ||
        value.TimeZoneKeyName[0] == L'\0') return {};
    return value.TimeZoneKeyName;
}

void select_combo_text(const HWND combo, const std::wstring& requested) {
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0);
    for (LRESULT index = 0; index < count; ++index) {
        const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, index, 0);
        if (length <= 0) continue;
        std::wstring candidate(static_cast<std::size_t>(length) + 1U, L'\0');
        SendMessageW(combo, CB_GETLBTEXT, index, reinterpret_cast<LPARAM>(candidate.data()));
        candidate.resize(static_cast<std::size_t>(length));
        if (_wcsicmp(candidate.c_str(), requested.c_str()) == 0) {
            SendMessageW(combo, CB_SETCURSEL, index, 0);
            return;
        }
    }
}

struct SetupDialogContext { AppState* state{nullptr}; };

#ifdef SFX_ALPHA_TEST_AUTOMATION
bool capture_window_bmp(HWND window, const std::filesystem::path& path);
#endif

void open_sysop_wiki(HWND dialog) {
    try {
        const auto wiki = win32::alpha_installation_root() / L"Docs" / L"Sysop" / L"wiki.html";
        std::error_code error;
        if (!std::filesystem::is_regular_file(wiki, error)) {
            MessageBoxW(dialog, L"The SFX Wiki is missing or unavailable in this installation.\n\n"
                L"Run SFX Setup to restore Docs\\Sysop\\wiki.html, or check access to that file.",
                L"SFX Wiki", MB_OK | MB_ICONINFORMATION);
            return;
        }
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(dialog, L"open", wiki.c_str(), nullptr,
                wiki.parent_path().c_str(), SW_SHOWNORMAL)) <= 32) {
            MessageBoxW(dialog, L"Windows could not open the SFX Wiki.\n\n"
                L"Try opening Docs\\Sysop\\wiki.html from this SFX folder, or check the default app for HTML files.",
                L"SFX Wiki", MB_OK | MB_ICONINFORMATION);
        }
    } catch (...) {
        MessageBoxW(dialog, L"SFX could not locate the Wiki for this installation.\n\n"
            L"Open Docs\\Sysop\\wiki.html from your SFX folder.", L"SFX Wiki", MB_OK | MB_ICONINFORMATION);
    }
}

HMENU create_help_menu() {
    const auto help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, ID_HELP_ABOUT, L"About Spitfire-X BBS…");
    AppendMenuW(help, MF_STRING, ID_HELP_WIKI, L"&SFX Wiki");
    return help;
}

INT_PTR CALLBACK about_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        state = reinterpret_cast<AppState*>(lparam);
        if (state == nullptr || !state->about_image || state->about_image->handle == nullptr)
            return FALSE;
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        SetDlgItemTextW(dialog, IDC_ABOUT_VERSION,
            SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE);
        const int margin = scaled(dialog, 12);
        const int gap = scaled(dialog, 8);
        const int image_width = std::min<int>(622, scaled(dialog, 410));
        const int image_height = MulDiv(image_width, 620, 622);
        const int text_height = scaled(dialog, 24);
        const int button_width = scaled(dialog, 82);
        const int button_height = scaled(dialog, 27);
        const int client_width = image_width + margin * 2;
        const int client_height = margin + image_height + gap + text_height + gap +
            button_height + margin;
        RECT outer{0, 0, client_width, client_height};
        AdjustWindowRectEx(&outer, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_STYLE)),
            FALSE, static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE)));
        RECT owner{};
        GetWindowRect(state->window, &owner);
        const int width = outer.right - outer.left;
        const int height = outer.bottom - outer.top;
        const int left = owner.left + ((owner.right - owner.left) - width) / 2;
        const int top = owner.top + ((owner.bottom - owner.top) - height) / 2;
        SetWindowPos(dialog, HWND_TOP, left, top, width, height, SWP_NOACTIVATE);
        MoveWindow(GetDlgItem(dialog, IDC_ABOUT_IMAGE), margin, margin,
            image_width, image_height, TRUE);
        MoveWindow(GetDlgItem(dialog, IDC_ABOUT_VERSION), margin,
            margin + image_height + gap, image_width, text_height, TRUE);
        MoveWindow(GetDlgItem(dialog, IDOK), (client_width - button_width) / 2,
            client_height - margin - button_height, button_width, button_height, TRUE);
        SendDlgItemMessageW(dialog, IDC_ABOUT_VERSION, WM_SETFONT,
            reinterpret_cast<WPARAM>(state->ui_font), TRUE);
        SendDlgItemMessageW(dialog, IDOK, WM_SETFONT,
            reinterpret_cast<WPARAM>(state->ui_font), TRUE);
#ifdef SFX_ALPHA_TEST_AUTOMATION
        if (!state->automation.about_capture_path.empty() ||
            state->automation.about_close_method != 0U)
            SetTimer(dialog, kWizardAutomationTimer, 250, nullptr);
#endif
        return TRUE;
    }
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if (message == WM_TIMER && wparam == kWizardAutomationTimer) {
        KillTimer(dialog, kWizardAutomationTimer);
        const bool captured = state->automation.about_capture_path.empty() ||
            capture_window_bmp(dialog, state->automation.about_capture_path);
        state->automation.about_capture_path.clear();
        if (!captured) state->automation.exit_code = 76;
        const unsigned int close_method = state->automation.about_close_method;
        state->automation.about_close_method = 0U;
        if (!captured) EndDialog(dialog, IDCANCEL);
        else if (close_method == 1U)
            SendDlgItemMessageW(dialog, IDOK, BM_CLICK, 0, 0);
        else if (close_method == 2U)
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, 0), 0);
        else if (close_method == 3U)
            SendMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDCANCEL, 0), 0);
        else if (close_method == 4U)
            SendMessageW(dialog, WM_CLOSE, 0, 0);
        else EndDialog(dialog, IDOK);
        return TRUE;
    }
#endif
    if (state == nullptr) return FALSE;
    if (message == WM_COMMAND &&
        (LOWORD(wparam) == IDOK || LOWORD(wparam) == IDCANCEL)) {
        EndDialog(dialog, LOWORD(wparam));
        return TRUE;
    }
    if (message == WM_CLOSE) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message == WM_CTLCOLORDLG) return reinterpret_cast<INT_PTR>(state->background_brush);
    if (message == WM_CTLCOLORSTATIC) {
        SetTextColor(reinterpret_cast<HDC>(wparam), kGoldColor);
        SetBkColor(reinterpret_cast<HDC>(wparam), kBackgroundColor);
        return reinterpret_cast<INT_PTR>(state->background_brush);
    }
    if (message == WM_DRAWITEM) {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lparam);
        if (item == nullptr) return FALSE;
        if (item->CtlID == IDC_ABOUT_IMAGE && state->about_image) {
            fill_rectangle(item->hDC, item->rcItem, RGB(0, 0, 0));
            HDC source = CreateCompatibleDC(item->hDC);
            if (source != nullptr) {
                const HGDIOBJ previous = SelectObject(source, state->about_image->handle);
                const int available_width = item->rcItem.right - item->rcItem.left;
                const int available_height = item->rcItem.bottom - item->rcItem.top;
                const double factor = std::min({1.0,
                    static_cast<double>(available_width) / state->about_image->width,
                    static_cast<double>(available_height) / state->about_image->height});
                const int width = static_cast<int>(state->about_image->width * factor);
                const int height = static_cast<int>(state->about_image->height * factor);
                const int left = item->rcItem.left + (available_width - width) / 2;
                const int top = item->rcItem.top + (available_height - height) / 2;
                SetStretchBltMode(item->hDC, HALFTONE);
                SetBrushOrgEx(item->hDC, 0, 0, nullptr);
                StretchBlt(item->hDC, left, top, width, height, source, 0, 0,
                    static_cast<int>(state->about_image->width),
                    static_cast<int>(state->about_image->height), SRCCOPY);
                SelectObject(source, previous);
                DeleteDC(source);
            }
            return TRUE;
        }
        if (item->CtlID == IDOK) {
            const bool pressed = (item->itemState & ODS_SELECTED) != 0;
            fill_rectangle(item->hDC, item->rcItem,
                pressed ? kSelectionColor : kSurfaceColor);
            frame_rectangle(item->hDC, item->rcItem,
                (item->itemState & ODS_FOCUS) != 0 ? kGoldColor : kBorderColor);
            SetBkMode(item->hDC, TRANSPARENT);
            SetTextColor(item->hDC, kTextColor);
            const HGDIOBJ previous = SelectObject(item->hDC, state->ui_font);
            RECT text = item->rcItem;
            DrawTextW(item->hDC, L"Close", -1, &text,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(item->hDC, previous);
            return TRUE;
        }
    }
    return FALSE;
}

void show_about_dialog(AppState& state) {
    DialogBoxParamW(state.instance, MAKEINTRESOURCEW(IDD_ABOUT), state.window,
        about_dialog_proc, reinterpret_cast<LPARAM>(&state));
}

INT_PTR CALLBACK setup_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* context = reinterpret_cast<SetupDialogContext*>(GetWindowLongPtrW(dialog, DWLP_USER));
    if (message == WM_INITDIALOG) {
        context = reinterpret_cast<SetupDialogContext*>(lparam);
        SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(context));
        SetDlgItemTextW(dialog, IDC_BBS_NAME,
            context->state->have_status ? utf8_to_wide(context->state->status.bbs_name).c_str()
                                        : L"Spitfire-X BBS");
        SetDlgItemTextW(dialog, IDC_LOGICAL_NODES,
            context->state->have_status
                ? std::to_wstring(std::max<std::uint16_t>(
                    context->state->status.lifecycle.configured_nodes, 4)).c_str() : L"4");
        SetDlgItemTextW(dialog, IDC_TELNET_BIND,
            context->state->have_status
                ? utf8_to_wide(context->state->status.telnet_bind).c_str() : L"0.0.0.0");
        SetDlgItemTextW(dialog, IDC_TELNET_PORT,
            context->state->have_status
                ? std::to_wstring(context->state->status.telnet_port).c_str() : L"2323");
        const app::SshOperationalConfiguration ssh_defaults;
        CheckDlgButton(dialog, IDC_SETUP_SSH_ENABLED, ssh_defaults.enabled ? BST_CHECKED : BST_UNCHECKED);
        SetDlgItemTextW(dialog, IDC_SETUP_SSH_BIND, utf8_to_wide(ssh_defaults.bind_address).c_str());
        SetDlgItemTextW(dialog, IDC_SETUP_SSH_PORT, std::to_wstring(ssh_defaults.port).c_str());
        for (const auto& zone : installed_time_zone_keys())
            SendDlgItemMessageW(dialog, IDC_TIME_ZONE, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(zone.c_str()));
        const std::wstring configured = context->state->have_status
            ? utf8_to_wide(context->state->status.bbs_time_zone) : std::wstring{};
        select_combo_text(GetDlgItem(dialog, IDC_TIME_ZONE),
            configured.empty() ? current_time_zone_key() : configured);
        for (const int control : {IDC_BBS_NAME, IDC_SYSOP_NAME, IDC_ACCOUNT_PASSWORD,
                IDC_ACCOUNT_CONFIRM, IDC_SYSTEM_PASSWORD, IDC_SYSTEM_CONFIRM,
                IDC_TELNET_BIND, IDC_TELNET_PORT, IDC_LOGICAL_NODES,
                IDC_SETUP_SSH_BIND, IDC_SETUP_SSH_PORT}) {
            SendDlgItemMessageW(dialog, control, EM_SETLIMITTEXT, 128, 0);
        }
        for (const int control : {IDC_TELNET_BIND, IDC_SETUP_SSH_BIND})
            SendDlgItemMessageW(dialog, control, EM_SETLIMITTEXT, app::kTelnetBindMaximum, 0);
        for (const int control : {IDC_TELNET_PORT, IDC_SETUP_SSH_PORT})
            SendDlgItemMessageW(dialog, control, EM_SETLIMITTEXT, 5, 0);
#ifdef SFX_ALPHA_TEST_AUTOMATION
        if (context->state->automation.fixture_bootstrap) {
            if (!context->state->automation.multinode_directory.empty()) {
                std::ofstream defaults(context->state->automation.multinode_directory / L"setup-defaults.txt");
                defaults << (IsDlgButtonChecked(dialog, IDC_SETUP_SSH_ENABLED) == BST_CHECKED) << '\n'
                    << wide_to_utf8(window_text(GetDlgItem(dialog, IDC_SETUP_SSH_BIND))).value_or("") << '\n'
                    << GetDlgItemInt(dialog, IDC_SETUP_SSH_PORT, nullptr, FALSE) << '\n';
            }
            SetDlgItemTextW(dialog, IDC_BBS_NAME, L"Spitfire-X GUI QA");
            SetDlgItemTextW(dialog, IDC_SYSOP_NAME, L"Test Sysop");
            SetDlgItemTextW(dialog, IDC_ACCOUNT_PASSWORD, L"Acct-fixture-81a9");
            SetDlgItemTextW(dialog, IDC_ACCOUNT_CONFIRM, L"Acct-fixture-81a9");
            SetDlgItemTextW(dialog, IDC_SYSTEM_PASSWORD, L"SY-fixture-72f1");
            SetDlgItemTextW(dialog, IDC_SYSTEM_CONFIRM, L"SY-fixture-72f1");
            SetDlgItemTextW(dialog, IDC_LOGICAL_NODES, L"4");
            SetDlgItemTextW(dialog, IDC_TELNET_BIND, L"127.0.0.1");
            SetDlgItemTextW(dialog, IDC_TELNET_PORT, L"2323");
            SetDlgItemTextW(dialog, IDC_SETUP_SSH_BIND, L"127.0.0.1");
            if (context->state->automation.fixture_ssh_disabled)
                CheckDlgButton(dialog, IDC_SETUP_SSH_ENABLED, BST_UNCHECKED);
            SetTimer(dialog, kWizardAutomationTimer, 250, nullptr);
        }
#endif
        return TRUE;
    }
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if (message == WM_TIMER && wparam == kWizardAutomationTimer && context != nullptr) {
        KillTimer(dialog, kWizardAutomationTimer);
        if (!context->state->automation.wizard_capture_path.empty())
            (void)capture_window_bmp(dialog,
                context->state->automation.wizard_capture_path);
        PostMessageW(dialog, WM_COMMAND, MAKEWPARAM(IDOK, BN_CLICKED), 0);
        return TRUE;
    }
#endif
    if (message == WM_COMMAND && LOWORD(wparam) == IDCANCEL) {
        EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    if (message != WM_COMMAND || LOWORD(wparam) != IDOK || context == nullptr) return FALSE;

    std::wstring bbs = window_text(GetDlgItem(dialog, IDC_BBS_NAME));
    std::wstring sysop = window_text(GetDlgItem(dialog, IDC_SYSOP_NAME));
    std::wstring account_password = window_text(GetDlgItem(dialog, IDC_ACCOUNT_PASSWORD));
    std::wstring account_confirm = window_text(GetDlgItem(dialog, IDC_ACCOUNT_CONFIRM));
    std::wstring system_password = window_text(GetDlgItem(dialog, IDC_SYSTEM_PASSWORD));
    std::wstring system_confirm = window_text(GetDlgItem(dialog, IDC_SYSTEM_CONFIRM));
    const bool passwords_match = account_password == account_confirm &&
        system_password == system_confirm;
    if (!passwords_match) {
        MessageBoxW(dialog, L"Each password must exactly match its confirmation.",
            L"Spitfire-X BBS Setup", MB_OK | MB_ICONWARNING);
        erase(account_password); erase(account_confirm); erase(system_password); erase(system_confirm);
        return TRUE;
    }
    BOOL nodes_valid = FALSE, port_valid = FALSE, ssh_port_valid = FALSE;
    const UINT nodes = GetDlgItemInt(dialog, IDC_LOGICAL_NODES, &nodes_valid, FALSE);
    const UINT port = GetDlgItemInt(dialog, IDC_TELNET_PORT, &port_valid, FALSE);
    const UINT ssh_port = GetDlgItemInt(dialog, IDC_SETUP_SSH_PORT, &ssh_port_valid, FALSE);
    const LRESULT zone_index = SendDlgItemMessageW(dialog, IDC_TIME_ZONE, CB_GETCURSEL, 0, 0);
    if (!nodes_valid || nodes < 1 || nodes > app::kAlphaNodePoolCapacity || !port_valid || port < 1 || port > 65535 ||
        !ssh_port_valid || ssh_port < 1 || ssh_port > 65535 || zone_index == CB_ERR) {
        MessageBoxW(dialog, L"Select an installed time zone and enter nodes 1–128 and valid Telnet/SSH ports.",
            L"Spitfire-X BBS Setup", MB_OK | MB_ICONWARNING);
        erase(account_password); erase(account_confirm); erase(system_password); erase(system_confirm);
        return TRUE;
    }
    const LRESULT zone_length = SendDlgItemMessageW(dialog, IDC_TIME_ZONE,
        CB_GETLBTEXTLEN, zone_index, 0);
    std::wstring zone(static_cast<std::size_t>(zone_length) + 1U, L'\0');
    SendDlgItemMessageW(dialog, IDC_TIME_ZONE, CB_GETLBTEXT, zone_index,
        reinterpret_cast<LPARAM>(zone.data()));
    zone.resize(static_cast<std::size_t>(zone_length));
    const auto bbs_utf8 = wide_to_utf8(bbs);
    const auto sysop_utf8 = wide_to_utf8(sysop);
    auto account_utf8 = wide_to_utf8(account_password);
    auto system_utf8 = wide_to_utf8(system_password);
    const auto zone_utf8 = wide_to_utf8(zone);
    const auto bind_utf8 = wide_to_utf8(window_text(GetDlgItem(dialog, IDC_TELNET_BIND)));
    const auto ssh_bind_utf8 = wide_to_utf8(window_text(GetDlgItem(dialog, IDC_SETUP_SSH_BIND)));
    if (!bbs_utf8 || !sysop_utf8 || !account_utf8 || !system_utf8 || !zone_utf8 || !bind_utf8 || !ssh_bind_utf8) {
        MessageBoxW(dialog, L"One or more setup values cannot be encoded safely.",
            L"Spitfire-X BBS Setup", MB_OK | MB_ICONWARNING);
        erase(account_password); erase(account_confirm); erase(system_password); erase(system_confirm);
        if (account_utf8) erase_utf8(*account_utf8);
        if (system_utf8) erase_utf8(*system_utf8);
        return TRUE;
    }
    app::AlphaBootstrapRequest request;
    request.configuration.bbs_name = *bbs_utf8;
    request.configuration.bbs_time_zone = *zone_utf8;
    request.configuration.logical_nodes = static_cast<std::uint16_t>(nodes);
    request.configuration.telnet.enabled = true;
    request.configuration.telnet.bind_address = *bind_utf8;
    request.configuration.telnet.port = static_cast<std::uint16_t>(port);
    request.configuration.telnet.bot_gate_enabled = true;
    request.configuration.ssh.enabled = IsDlgButtonChecked(dialog, IDC_SETUP_SSH_ENABLED) == BST_CHECKED;
    request.configuration.ssh.bind_address = *ssh_bind_utf8;
    request.configuration.ssh.port = static_cast<std::uint16_t>(ssh_port);
    request.configuration.security_policy_revision = context->state->status.security_policy_revision;
    request.configuration.security_policy_hash = context->state->status.security_policy_hash;
    if (!app::validate_operational_configuration(request.configuration).valid()) {
        MessageBoxW(dialog, L"Enter a valid BBS name and IPv4 listener addresses. Enabled Telnet and SSH listeners must not overlap on the same port (including 0.0.0.0).",
            L"Spitfire-X BBS Setup", MB_OK | MB_ICONWARNING);
        erase(account_password); erase(account_confirm); erase(system_password); erase(system_confirm);
        erase_utf8(*account_utf8); erase_utf8(*system_utf8);
        return TRUE;
    }
    request.sysop_name = *sysop_utf8;
    request.sysop_password = *account_utf8;
    request.system_password = *system_utf8;
    erase_utf8(*account_utf8);
    erase_utf8(*system_utf8);
    const app::AlphaControlResponse response = context->state->runtime.bootstrap(request);
    app::erase_bootstrap_secrets(request);
    erase(account_password); erase(account_confirm); erase(system_password); erase(system_confirm);
    SetDlgItemTextW(dialog, IDC_ACCOUNT_PASSWORD, L"");
    SetDlgItemTextW(dialog, IDC_ACCOUNT_CONFIRM, L"");
    SetDlgItemTextW(dialog, IDC_SYSTEM_PASSWORD, L"");
    SetDlgItemTextW(dialog, IDC_SYSTEM_CONFIRM, L"");
    if (response.code != app::LocalControlResponseCode::ok) {
        update_status(*context->state);
        const auto saved = win32::read_operational_configuration_file(win32::alpha_installation_root());
        const bool readable = saved.state == win32::ConfigurationFileState::loaded && saved.configuration;
        const auto& reason = response.diagnostic_code;
        const bool listener_failure = reason.rfind("telnet_listener_", 0) == 0 ||
            reason == "configured_caller_listener_not_listening";
        const auto detail = std::wstring(readable ?
            L"The saved BBS configuration is readable. " : L"Setup did not finish saving a readable BBS configuration. ") +
            (listener_failure ? L"Runtime could not start a configured caller listener. Check that its bind address and port are available." :
                L"Runtime reported an error. Review BBS Health in SFXConfig.") +
            L"\n\nRuntime diagnostic: " + utf8_to_wide(reason);
        MessageBoxW(dialog, detail.c_str(), L"Spitfire-X BBS Setup", MB_OK | MB_ICONERROR);
        if (readable) EndDialog(dialog, IDCANCEL);
        return TRUE;
    }
    update_status(*context->state);
    auto& state = *context->state;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if (state.automation.fixture_bootstrap) {
        EndDialog(dialog, IDOK);
        return TRUE;
    }
#endif
    const auto configuration = state.runtime.configuration_snapshot();
    const bool ready = state.have_status && state.status.state == app::AlphaRuntimePresentationState::ready &&
        state.status.bootstrap_complete && state.status.configuration_loaded && configuration;
    const bool listening = ready &&
        (!configuration->active.telnet.enabled || state.status.telnet_active) &&
        (!configuration->active.ssh.enabled || state.status.ssh_active);
    if (ready) {
        auto detail = L"Board: " + utf8_to_wide(configuration->saved.bbs_name) +
            L"\n\nBoard settings and the initial System Sysop were saved. Runtime is ready and the saved configuration is readable.\n\n";
        detail += listening ? L"Your configured caller listeners are running." :
            L"Caller listener readiness could not be confirmed. Review Runtime status and BBS Health.";
        MessageBoxW(dialog, detail.c_str(), listening ? L"BBS setup complete" : L"Check caller startup",
            MB_OK | (listening ? MB_ICONINFORMATION : MB_ICONWARNING));
    } else {
        const auto diagnostic = !state.have_status ? "runtime_status_unavailable" :
            !configuration && state.status.state == app::AlphaRuntimePresentationState::ready ?
                "configuration_readback_unavailable" : state.status.diagnostic_code;
        const auto failed_checks = L"Runtime accepted the setup. The Control Panel could not confirm the saved settings and caller listener readiness. "
            L"Review BBS Health in SFXConfig.\n\nRuntime diagnostic: " +
            utf8_to_wide(diagnostic.empty() ? "setup_readiness_unavailable" : diagnostic);
        MessageBoxW(dialog, failed_checks.c_str(), L"Check BBS setup", MB_OK | MB_ICONWARNING);
    }
    EndDialog(dialog, IDOK);
    return TRUE;
}

void run_setup_dialog(AppState& state) {
    update_status(state);
    const auto failure = state.have_status ? app::bootstrap_prerequisite_failure(state.status) :
        std::optional<app::AlphaControlResponse>{{app::LocalControlResponseCode::internal_failure,
            "runtime_status_unavailable"}};
    if (failure) {
        const std::wstring detail = utf8_to_wide(std::string(app::alpha_recovery_guidance(
            failure->diagnostic_code))) + L"\n\nRuntime diagnostic: " +
            utf8_to_wide(failure->diagnostic_code);
        MessageBoxW(state.window, detail.c_str(), L"Spitfire-X BBS — Recovery Required", MB_OK | MB_ICONERROR);
        return;
    }
    SetupDialogContext context{&state};
    DialogBoxParamW(state.instance, MAKEINTRESOURCEW(IDD_FIRST_RUN), state.window,
        setup_dialog_proc, reinterpret_cast<LPARAM>(&context));
}

const app::AlphaNodeStatus* node_status(const AppState& state, const std::uint16_t number) {
    const auto found = std::find_if(state.status.nodes.begin(), state.status.nodes.end(),
        [number](const app::AlphaNodeStatus& node) { return node.number == number; });
    return found == state.status.nodes.end() ? nullptr : &*found;
}

bool task_choice(HWND parent, const wchar_t* title, const wchar_t* heading,
    const wchar_t* detail, const wchar_t* accept, const int accept_id) {
    const TASKDIALOG_BUTTON buttons[]{{accept_id, accept}, {IDCANCEL, L"Cancel"}};
    TASKDIALOGCONFIG config{sizeof(config)};
    config.hwndParent = parent;
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_POSITION_RELATIVE_TO_WINDOW;
    config.pszWindowTitle = title;
    config.pszMainIcon = TD_WARNING_ICON;
    config.pszMainInstruction = heading;
    config.pszContent = detail;
    config.cButtons = static_cast<UINT>(_countof(buttons));
    config.pButtons = buttons;
    config.nDefaultButton = IDCANCEL;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(parent, GWLP_USERDATA));
    if (state && state->automation.confirm_node_command) {
        state->automation.confirmation_heading = heading;
        state->automation.confirmation_detail = detail;
        config.lpCallbackData = accept_id;
        config.pfCallback = [](HWND dialog, UINT notification, WPARAM, LPARAM, LONG_PTR data) -> HRESULT {
            if (notification == TDN_CREATED) PostMessageW(dialog, TDM_CLICK_BUTTON, static_cast<WPARAM>(data), 0);
            return S_OK;
        };
    }
#endif
    int selected = IDCANCEL;
    return SUCCEEDED(TaskDialogIndirect(&config, &selected, nullptr, nullptr)) &&
        selected == accept_id;
}

void service_command(AppState& state,UINT command) {
    if(state.service_action_busy||!state.runtime.is_service_mode())return;
    update_status(state);
    if(command!=state.service_button_command)return;
    try {
        const auto before=state.runtime.service_control_state(true);
        const auto epoch=state.status.runtime_epoch;
        const bool start=command==ID_BBS_SERVICE_START;
        if(before.state!=static_cast<DWORD>(start?SERVICE_STOPPED:SERVICE_RUNNING))return;
        state.service_action_busy=true;state.service_action_start=start;
        struct Reset {AppState& state;~Reset(){state.service_action_busy=false;update_status(state);}} reset{state};
        update_service_button(state);UpdateWindow(state.service_button);
        if(!start&&!task_choice(state.window,L"Spitfire-X BBS",L"Stop BBS?",
            L"This explicitly stops the Windows service and its active caller, Door, transfer and background work. Closing this Control Panel instead leaves the BBS running.",
            L"Stop BBS",100))return;
        // A confirmation can outlive its service instance. Never apply its
        // decision to an externally restarted Runtime or changed selection.
        update_status(state);
        const auto after=state.runtime.service_control_state(true);
        if(after.state!=before.state||after.process_id!=before.process_id||(!start&&!(state.status.runtime_epoch==epoch)))return;
        std::string diagnostic;
        const bool complete=start?state.runtime.start_service(diagnostic):state.runtime.shutdown(diagnostic);
        if(!complete)MessageBoxW(state.window,
            ((start?std::wstring(L"The Windows service did not start.\n\n"):std::wstring(L"The service has not confirmed a completed stop.\n\n"))+utf8_to_wide(diagnostic)).c_str(),
            start?L"Start BBS":L"Stop BBS",MB_OK|MB_ICONERROR);
    }catch(const std::exception& error){
        update_status(state);MessageBoxW(state.window,(L"Service state could not be verified. Reopen Control Panel after resolving the installation state.\n\n"+
            utf8_to_wide(error.what())).c_str(),L"BBS Service",MB_OK|MB_ICONINFORMATION);
    }
}
void fill_rectangle(const HDC dc, const RECT& rectangle, const COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FillRect(dc, &rectangle, brush);
    DeleteObject(brush);
}

void frame_rectangle(const HDC dc, RECT rectangle, const COLORREF color) {
    const HBRUSH brush = CreateSolidBrush(color);
    FrameRect(dc, &rectangle, brush);
    DeleteObject(brush);
}

void draw_tab_rail(const HWND window, const HDC dc, AppState& state) {
    RECT client{};
    GetClientRect(window, &client);
    fill_rectangle(dc, client, kBackgroundColor);
    const HGDIOBJ previous_font = SelectObject(dc,
        state.ui_font != nullptr ? state.ui_font : GetStockObject(SYSTEM_FIXED_FONT));
    SetBkMode(dc, TRANSPARENT);
    const int selected = TabCtrl_GetCurSel(window);
    const int count = TabCtrl_GetItemCount(window);
    int strip_bottom = 0;
    for (int index = 0; index < count; ++index) {
        RECT item{};
        if (!TabCtrl_GetItemRect(window, index, &item)) continue;
        strip_bottom = std::max(strip_bottom, static_cast<int>(item.bottom));
        RECT visual = item;
        const bool active = index == selected;
        fill_rectangle(dc, visual, active ? kPanelColor : kSurfaceColor);
        frame_rectangle(dc, visual, active ? kGoldColor : kBorderColor);

        wchar_t stored_label[64]{};
        TCITEMW tab_item{};
        tab_item.mask = TCIF_TEXT;
        tab_item.pszText = stored_label;
        tab_item.cchTextMax = static_cast<int>(_countof(stored_label));
        (void)TabCtrl_GetItem(window, index, &tab_item);
        std::wstring visible_label = stored_label;
        if (index >= 6 && static_cast<std::size_t>(index - 6) < state.open_node_views.size()) {
            visible_label = L"Node " +
                std::to_wstring(state.open_node_views[static_cast<std::size_t>(index - 6)]);
        }
        RECT text = visual;
        text.left += 9;
        text.right -= index >= 6 ? 25 : 8;
        SetTextColor(dc, active ? kGoldColor : kMutedTextColor);
        DrawTextW(dc, visible_label.c_str(), -1, &text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        if (index >= 6) {
            const int center_x = visual.right - 12;
            const int center_y = (visual.top + visual.bottom) / 2;
            const HPEN pen = CreatePen(PS_SOLID, 2, active ? kGoldColor : kMutedTextColor);
            const HGDIOBJ previous_pen = SelectObject(dc, pen);
            MoveToEx(dc, center_x - 4, center_y - 4, nullptr);
            LineTo(dc, center_x + 5, center_y + 5);
            MoveToEx(dc, center_x + 4, center_y - 4, nullptr);
            LineTo(dc, center_x - 5, center_y + 5);
            SelectObject(dc, previous_pen);
            DeleteObject(pen);
        }
        if (active) {
            RECT underline{visual.left + 1, visual.bottom - 3, visual.right - 1, visual.bottom - 1};
            fill_rectangle(dc, underline, kGoldColor);
        }
    }
    if (strip_bottom > 0) {
        RECT divider{client.left, strip_bottom, client.right, strip_bottom + 1};
        fill_rectangle(dc, divider, kBorderColor);
    }
    SelectObject(dc, previous_font);
}

bool tab_close_hit_test(const HWND window, const POINT point, int& tab_index) {
    TCHITTESTINFO hit{};
    hit.pt = point;
    tab_index = TabCtrl_HitTest(window, &hit);
    if (tab_index < 6) return false;
    RECT item{};
    if (!TabCtrl_GetItemRect(window, tab_index, &item)) return false;
    RECT close{item.right - 25, item.top, item.right, item.bottom};
    return PtInRect(&close, point) != FALSE;
}

LRESULT CALLBACK tab_rail_proc(const HWND window, const UINT message,
    const WPARAM wparam, const LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
    if (state == nullptr || state->original_tab_proc == nullptr)
        return DefWindowProcW(window, message, wparam, lparam);
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            draw_tab_rail(window, dc, *state);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINT:
        case WM_PRINTCLIENT:
            draw_tab_rail(window, reinterpret_cast<HDC>(wparam), *state);
            return 0;
        case WM_LBUTTONDOWN: {
            const POINT point{static_cast<int>(static_cast<short>(LOWORD(lparam))),
                static_cast<int>(static_cast<short>(HIWORD(lparam)))};
            int tab_index = -1;
            if (tab_close_hit_test(window, point, tab_index)) {
                SendMessageW(GetParent(window), kCloseNodeTabMessage,
                    static_cast<WPARAM>(tab_index), 0);
                return 0;
            }
            break;
        }
        case WM_SETCURSOR: {
            POINT point{};
            GetCursorPos(&point);
            ScreenToClient(window, &point);
            int tab_index = -1;
            if (tab_close_hit_test(window, point, tab_index)) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        }
        default:
            break;
    }
    return CallWindowProcW(state->original_tab_proc, window, message, wparam, lparam);
}

void draw_list_header(const HWND window, const HDC dc, AppState& state) {
    RECT client{};
    GetClientRect(window, &client);
    fill_rectangle(dc, client, kSurfaceColor);
    const HGDIOBJ previous_font = SelectObject(dc,
        state.ui_font != nullptr ? state.ui_font : GetStockObject(SYSTEM_FIXED_FONT));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, kGoldColor);
    const int count = Header_GetItemCount(window);
    for (int index = 0; index < count; ++index) {
        RECT item{};
        if (!Header_GetItemRect(window, index, &item)) continue;
        wchar_t label[80]{};
        HDITEMW header_item{};
        header_item.mask = HDI_TEXT;
        header_item.pszText = label;
        header_item.cchTextMax = static_cast<int>(_countof(label));
        (void)Header_GetItem(window, index, &header_item);
        RECT text = item;
        text.left += 7;
        text.right -= 5;
        DrawTextW(dc, label, -1, &text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT separator{item.right - 1, item.top, item.right, item.bottom};
        fill_rectangle(dc, separator, kGridColor);
    }
    RECT bottom{client.left, client.bottom - 1, client.right, client.bottom};
    fill_rectangle(dc, bottom, kGoldColor);
    frame_rectangle(dc, client, kBorderColor);
    SelectObject(dc, previous_font);
}

LRESULT CALLBACK list_header_proc(const HWND window, const UINT message,
    const WPARAM wparam, const LPARAM lparam) {
    const HWND list = GetParent(window);
    auto* state = reinterpret_cast<AppState*>(
        GetWindowLongPtrW(GetParent(list), GWLP_USERDATA));
    if (state == nullptr || state->original_header_proc == nullptr)
        return DefWindowProcW(window, message, wparam, lparam);
    if(startup_surface_input(*state,message)){
        if(message==WM_SETFOCUS)SetFocus(state->window);
        return message==WM_MOUSEACTIVATE?MA_NOACTIVATEANDEAT:0;
    }
    switch (message) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            draw_list_header(window, dc, *state);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_PRINT:
        case WM_PRINTCLIENT:
            draw_list_header(window, reinterpret_cast<HDC>(wparam), *state);
            return 0;
        default:
            break;
    }
    return CallWindowProcW(state->original_header_proc, window, message, wparam, lparam);
}

void show_selected_page(AppState& state, bool interacted, bool refresh) {
    // Presentation bookkeeping (including tab switching, focus and scale menus)
    // is not interaction with a READY terminal. Only its input or a transition
    // into READY starts/restarts the per-node idle interval.
    (void)interacted;
    const int selected = panel_page(state.tab);
    const bool nodes = selected == 0;
    const bool node_view = selected >= 5 &&
        static_cast<std::size_t>(selected - 5) < state.open_node_views.size();
    const std::array<HWND, 20> page_controls{{state.binkp.window,state.node_list,
        state.open_node_button, state.notice_button, state.disable_all_nodes_button, state.enable_all_nodes_button,
        state.bulk_node_status, state.confirm_disable_all_button, state.cancel_disable_all_button,
        state.monitor_view, state.events_view,
        state.log_view, state.log_pause_button, state.log_clear_button,
        state.stats_view, state.terminal_view, state.node_header,
        state.modem_strip,state.log_filter,state.botgate_view}};
    const HWND captured = GetCapture();
    const HWND focused = GetFocus();
    bool focused_page_control = false;
    for (const HWND control : page_controls) {
        if (control == nullptr) continue;
        if (captured == control || IsChild(control, captured)) ReleaseCapture();
        if (focused == control || IsChild(control, focused)) focused_page_control = true;
        ShowWindow(control, SW_HIDE);
        EnableWindow(control, FALSE);
    }
    if (focused_page_control) SetFocus(state.tab);
    const auto activate = [](const HWND control) {
        if (control == nullptr) return;
        EnableWindow(control, TRUE);
        ShowWindow(control, SW_SHOW);
        SetWindowPos(control, HWND_TOP, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    };
    if (nodes) {
        activate(state.node_list);
        activate(state.open_node_button);
        activate(state.notice_button);
        activate(state.disable_all_nodes_button);
        activate(state.enable_all_nodes_button);
        update_bulk_node_buttons(state);
        if (state.preferences.system_monitor_visible) activate(state.monitor_view);
    } else if (selected == -14) {
        activate(state.binkp.window);
    } else if (selected == 1) {
        activate(state.events_view);
    } else if (selected == 2) {
        activate(state.log_view);
        activate(state.log_pause_button);
        activate(state.log_clear_button);
        activate(state.log_filter);
    } else if (selected == 3) {
        activate(state.stats_view);
    } else if (selected == 4) {
        activate(state.botgate_view);
    } else if (node_view) {
        activate(state.node_header);
        activate(state.terminal_view);
        if (state.preferences.modem_lights) activate(state.modem_strip);
    }
    state.displayed_node = node_view ? state.open_node_views[static_cast<std::size_t>(selected - 5)] : 0;
    update_node_header(state);
    if (node_view) {
        SetFocus(state.terminal_view);
    }
    if (selected == 2 && !state.logs_paused) {
        state.log_view_initialized = false;
    }
    if (refresh) update_status(state);
    InvalidateRect(state.window, nullptr, TRUE);
}

std::optional<app::ExternalTelnetClientProfile> telnet_client_for_next_logon(AppState& state) {
    auto profile = read_telnet_client_profile();
    if (profile && app::external_telnet_client_executable_is_usable(*profile)) return profile;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if (g_suppress_external_client_warning) return std::nullopt;
#endif
    MessageBoxW(state.window,
        profile
            ? L"The saved Telnet client is no longer usable. Open BBS > SFX Config > External Client to update or clear it. No node was reserved."
            : L"No default Telnet client is selected. Open BBS > SFX Config > External Client to select one. No node was reserved.",
        L"Default Telnet Client", MB_OK | MB_ICONWARNING);
    return std::nullopt;
}

std::vector<std::uint16_t> selected_node_numbers(const AppState& state) {
    std::vector<std::uint16_t> numbers;
    if (!IsWindow(state.node_list)) return numbers;
    for (int i = ListView_GetNextItem(state.node_list, -1, LVNI_SELECTED); i >= 0;
            i = ListView_GetNextItem(state.node_list, i, LVNI_SELECTED)) {
        LVITEMW item{}; item.mask = LVIF_PARAM; item.iItem = i;
        if (ListView_GetItem(state.node_list, &item) && item.lParam >= 1 &&
                item.lParam <= sfx::core::kMaximumLogicalNodes &&
                node_status(state, static_cast<std::uint16_t>(item.lParam)))
            numbers.push_back(static_cast<std::uint16_t>(item.lParam));
    }
    return numbers;
}

std::vector<std::uint16_t> node_command_targets(const AppState& state) {
    // A Node View acts on its displayed node without changing the Nodes-page
    // selection. Snapshot identifiers before opening tabs or sending commands.
    if (state.displayed_node && node_status(state, state.displayed_node)) return {state.displayed_node};
    return selected_node_numbers(state);
}

void set_node_states(AppState& state, const std::vector<std::uint16_t>& targets, bool enabled) {
    std::wstring failures;
    for (const auto number : targets) {
        const auto result = state.runtime.command_node({number,
            enabled ? app::AlphaNodeCommandKind::enable : app::AlphaNodeCommandKind::disable});
        if (result.code != app::LocalControlResponseCode::ok)
            failures += L"Node " + std::to_wstring(number) + L": " + utf8_to_wide(result.diagnostic_code) + L"\r\n";
    }
    update_status(state);
    if (!failures.empty()) MessageBoxW(state.window,
        (L"These nodes could not be changed. Other completed changes remain in effect.\r\n" + failures).c_str(),
        L"Spitfire-X BBS", MB_OK | MB_ICONERROR);
}

AppState::BulkDisableReview observed_bulk_disable(const AppState& state) {
    AppState::BulkDisableReview review{state.status.runtime_epoch, {}, 0};
    for (const auto& node : state.status.nodes) {
        if (!node.available || !node.enabled) continue;
        review.nodes.emplace_back(node.number, node.caller_connected ? node.assignment_sequence : 0);
        if (node.caller_connected) ++review.callers;
    }
    return review;
}

void show_bulk_node_feedback(AppState& state, std::wstring detail) {
    state.bulk_node_feedback = std::move(detail);
    SetWindowTextW(state.bulk_node_status, state.bulk_node_feedback.c_str());
    RECT area{};
    GetClientRect(state.window, &area);
    layout(state, area.right, area.bottom);
    update_bulk_node_buttons(state);
}

void review_bulk_disable(AppState& state, AppState::BulkDisableReview review, bool changed) {
    auto detail = (changed ? std::wstring(L"Callers changed. Review again. ") : std::wstring{}) +
        L"Disable All Nodes? " + std::to_wstring(review.callers) +
        (review.callers == 1 ? L" caller will disconnect." : L" callers will disconnect.") +
        L" Runtime stays running.";
    state.bulk_disable_review = std::move(review);
    show_bulk_node_feedback(state, std::move(detail));
    SetFocus(state.cancel_disable_all_button);
}

void apply_bulk_disable(AppState& state, const AppState::BulkDisableReview& review) {
    state.bulk_disable_review.reset();
    state.bulk_node_change_pending = true;
    update_bulk_node_buttons(state);
    std::wstring failure;
    std::size_t completed{};
    for (const auto& [number, generation] : review.nodes) {
        const auto result = state.runtime.command_node({number, app::AlphaNodeCommandKind::disable_observed,
            review.epoch, generation});
        if (result.code != app::LocalControlResponseCode::ok) {
            failure = result.diagnostic_code == "node_caller_changed"
                ? L"A caller changed. Choose Disable All Nodes to review the remaining nodes."
                : L"Could not disable Node " + std::to_wstring(number) + L": " + utf8_to_wide(result.diagnostic_code);
            break;
        }
        ++completed;
    }
    if (!failure.empty() && completed)
        failure += L" " + std::to_wstring(completed) + (completed == 1 ? L" node is disabled." : L" nodes are disabled.");
    state.bulk_node_change_pending = false;
    update_status(state);
    show_bulk_node_feedback(state, std::move(failure));
    SetFocus(IsWindowEnabled(state.disable_all_nodes_button) ? state.disable_all_nodes_button : state.enable_all_nodes_button);
}

void confirm_bulk_disable(AppState& state) {
    if (!state.bulk_disable_review || state.service_action_busy || state.bulk_node_change_pending || panel_page(state.tab) != 0) return;
    update_status(state);
    if (!can_set_all_nodes(state, false)) {
        state.bulk_disable_review.reset();
        show_bulk_node_feedback(state, L"Availability changed. No further nodes were disabled.");
        return;
    }
    auto current = observed_bulk_disable(state);
    if (!(current.epoch == state.bulk_disable_review->epoch) || current.nodes != state.bulk_disable_review->nodes) {
        review_bulk_disable(state, std::move(current), true);
        return;
    }
    apply_bulk_disable(state, current);
}

void cancel_bulk_disable(AppState& state) {
    if (!state.bulk_disable_review) return;
    state.bulk_disable_review.reset();
    show_bulk_node_feedback(state, {});
    SetFocus(IsWindowEnabled(state.disable_all_nodes_button) ? state.disable_all_nodes_button : state.node_list);
}

void set_all_nodes(AppState& state, bool enabled) {
    if (state.bulk_node_change_pending || state.service_action_busy || panel_page(state.tab) != 0 ||
        state.bulk_disable_review || !can_set_all_nodes(state, enabled)) return;
    update_status(state);
    if (!can_set_all_nodes(state, enabled)) return;
    if (!enabled) {
        auto review = observed_bulk_disable(state);
        if (review.callers) review_bulk_disable(state, std::move(review), false);
        else apply_bulk_disable(state, review);
        return;
    }
    state.bulk_node_change_pending = true;
    struct Reset {
        AppState& state;
        ~Reset() { state.bulk_node_change_pending = false; update_bulk_node_buttons(state); }
    } reset{state};
    update_bulk_node_buttons(state);
    show_bulk_node_feedback(state, {});
    std::vector<std::uint16_t> targets;
    for (const auto& node : state.status.nodes) if (node.available) targets.push_back(node.number);
    set_node_states(state, targets, true);
}

void open_node_view(AppState& state, const std::uint16_t number) {
    auto found = std::find(state.open_node_views.begin(), state.open_node_views.end(), number);
    int tab_index = 0;
    if (found == state.open_node_views.end()) {
        state.open_node_views.push_back(number);
        TCITEMW item{};
        item.mask = TCIF_TEXT;
        // The native tab control sizes items from their stored text.  Reserve explicit
        // width for the independently painted close affordance, then draw the concise
        // public label in draw_tab_rail().
        std::wstring label = L"Node " + std::to_wstring(number) + L" Close";
        item.pszText = label.data();
        tab_index = TabCtrl_InsertItem(state.tab,
            5 + static_cast<int>(state.open_node_views.size()), &item);
    } else {
        tab_index = 6 + static_cast<int>(std::distance(state.open_node_views.begin(), found));
    }
    TabCtrl_SetCurSel(state.tab, tab_index);
    InvalidateRect(state.tab, nullptr, TRUE);
    show_selected_page(state);
}

void open_selected_node(AppState& state) {
    if(state.startup_pending)return;
    const auto numbers = selected_node_numbers(state);
    for (const auto number : numbers) open_node_view(state, number);
}

bool node_ready_for_local_logon(const AppState& state, const std::uint16_t number) {
    const auto* node = node_status(state, number);
    return state.have_status && state.status.state == app::AlphaRuntimePresentationState::ready &&
        node && node->enabled && node->available &&
        node->state == sfx::core::NodeState::ready;
}

bool launch_external_telnet_client(
    const app::ExternalTelnetClientProfile& profile,
    const app::ExternalTelnetClientLaunch& launch) {
    std::vector<wchar_t> command(launch.command_line.begin(), launch.command_line.end());
    command.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const auto working = std::filesystem::path(profile.executable).parent_path();
    const BOOL started = CreateProcessW(profile.executable.c_str(), command.data(),
        nullptr, nullptr, FALSE, 0, nullptr, working.c_str(), &startup, &process);
    if (!started) return false;
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
}

void logon_to_node(AppState& state, const std::uint16_t number) {
    // The saved client selection and all profile validation happen before
    // Runtime owns a node. A missing or unusable profile creates no reservation.
    auto profile = telnet_client_for_next_logon(state);
    if (!profile) return;
    auto prepared = app::prepare_external_telnet_client_launch(
        *profile, L"127.0.0.1", 1);
    if (!prepared || !app::external_telnet_client_executable_is_usable(*profile)) {
        MessageBoxW(state.window, L"The saved Telnet client is no longer usable. Choose it again before trying to log on.",
            L"Logon to Node", MB_OK | MB_ICONWARNING);
        return;
    }

    update_status(state);
    if (!state.have_status || !node_ready_for_local_logon(state, number)) {
        MessageBoxW(state.window, L"That node is no longer READY. No local logon was started.",
            L"Logon to Node", MB_OK | MB_ICONINFORMATION);
        return;
    }
    const auto reservation = state.runtime.start_local_session(number);
    if (reservation.response.code != app::LocalControlResponseCode::ok) {
        MessageBoxW(state.window, L"The selected node could not be reserved. Refresh the node list and try again.",
            L"Logon to Node", MB_OK | MB_ICONWARNING);
        update_status(state);
        return;
    }

    // Revalidate after reservation to contain a moved/replaced executable race.
    prepared = app::prepare_external_telnet_client_launch(
        *profile, utf8_to_wide(reservation.host), reservation.port);
    if (!prepared || !app::external_telnet_client_executable_is_usable(*profile)) {
        (void)state.runtime.cancel_local_session({reservation.reservation,
            app::AlphaLocalSessionCancelReason::profile_invalid});
        MessageBoxW(state.window,
            L"The Telnet client changed after the node was reserved. The reservation was released; choose the client again.",
            L"Logon to Node",
            MB_OK | MB_ICONWARNING);
        update_status(state);
        return;
    }
    if (!launch_external_telnet_client(*profile, *prepared)) {
        const auto cancelled = state.runtime.cancel_local_session({reservation.reservation,
            app::AlphaLocalSessionCancelReason::client_launch_failed});
        const wchar_t* message = cancelled.code == app::LocalControlResponseCode::ok
            ? L"Windows could not start the selected Telnet client. The node reservation was released."
            : L"Windows could not start the selected Telnet client. Runtime will release the one-use reservation at its bounded timeout.";
        MessageBoxW(state.window, message, L"Logon to Node", MB_OK | MB_ICONERROR);
        update_status(state);
        return;
    }
    update_status(state);
}

bool handle_node_page_key(AppState& state, HWND target, UINT message, WPARAM key) {
    if(state.startup_pending)return false;
    if (message == WM_KEYDOWN && panel_page(state.tab) == 2) {
        const std::array<HWND, 5> order{state.tab, state.log_view, state.log_pause_button,
            state.log_clear_button, state.log_filter};
        const auto found = std::find(order.begin(), order.end(), target);
        if (target != state.window && found == order.end()) return false;
        if (key == VK_TAB) {
            const auto focused = std::find(order.begin(), order.end(), GetFocus());
            const auto index = focused == order.end() ? 0U :
                (static_cast<unsigned>(focused - order.begin()) +
                    ((GetKeyState(VK_SHIFT) & 0x8000) ? 4U : 1U)) % 5U;
            SetFocus(order[index]);
            return true;
        }
        if (key == VK_RETURN && (target == state.log_pause_button || target == state.log_clear_button)) {
            SendMessageW(state.window, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(target), BN_CLICKED),
                reinterpret_cast<LPARAM>(target));
            return true;
        }
        return false;
    }
    if (message != WM_KEYDOWN || panel_page(state.tab) != 0) return false;
    const std::array<HWND, 8> order{state.tab, state.node_list, state.open_node_button, state.notice_button,
        state.disable_all_nodes_button, state.enable_all_nodes_button, state.confirm_disable_all_button,
        state.cancel_disable_all_button};
    if (target != state.window && std::find(order.begin(), order.end(), target) == order.end()) return false;
    if (key == VK_ESCAPE && state.bulk_disable_review) { cancel_bulk_disable(state); return true; }
    if (key == VK_TAB) {
        // Page activation raises children in paint order, so HWND z-order is
        // not a stable keyboard traversal order. Follow the visible layout.
        const auto found = std::find(order.begin(), order.end(), GetFocus());
        const auto step = (GetKeyState(VK_SHIFT) & 0x8000) ? order.size() - 1 : 1;
        auto index = found == order.end() ? 0 : (static_cast<std::size_t>(found - order.begin()) + step) % order.size();
        for (std::size_t visited = 0; visited < order.size(); ++visited, index = (index + step) % order.size()) {
            if (order[index] && IsWindowVisible(order[index]) && IsWindowEnabled(order[index])) { SetFocus(order[index]); break; }
        }
        return true;
    }
    if(target==state.notice_button&&key==VK_RETURN){open_notice_dialog(state);return true;}
    if (key == VK_RETURN && (target == state.disable_all_nodes_button || target == state.enable_all_nodes_button ||
        target == state.confirm_disable_all_button || target == state.cancel_disable_all_button)) {
        if (IsWindowEnabled(target)) SendMessageW(target, BM_CLICK, 0, 0);
        return true;
    }
    if (target == state.open_node_button && key == VK_RETURN) {
        open_selected_node(state);
        return true;
    }
    return false;
}

void relay_modem_mouse(AppState*, HWND, UINT, WPARAM, LPARAM);

void select_context_node(HWND list, const int clicked) {
    if (clicked < 0) return;
    // Right-click inside a selected group changes focus, not the command targets.
    if (!(ListView_GetItemState(list, clicked, LVIS_SELECTED) & LVIS_SELECTED))
        ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
    ListView_SetItemState(list, -1, 0, LVIS_FOCUSED);
    ListView_SetItemState(list, clicked, LVIS_SELECTED | LVIS_FOCUSED,
        LVIS_SELECTED | LVIS_FOCUSED);
    ListView_SetSelectionMark(list, clicked);
    SetFocus(list);
}

LRESULT CALLBACK node_list_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(GetParent(window), GWLP_USERDATA));
    if (!state || !state->original_node_list_proc) return DefWindowProcW(window, message, wparam, lparam);
    if(startup_surface_input(*state,message)||(state->startup_pending&&
        (message==LVM_SETITEMSTATE||message==LVM_SETSELECTIONMARK))){
        if(message==WM_SETFOCUS)SetFocus(state->window);
        return message==WM_MOUSEACTIVATE?MA_NOACTIVATEANDEAT:0;
    }
    relay_modem_mouse(state, window, message, wparam, lparam);
    if (message == WM_RBUTTONDOWN) {
        LVHITTESTINFO hit{};
        hit.pt = {static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        const int clicked = ListView_HitTest(window, &hit);
        if (clicked >= 0 && (hit.flags & LVHT_ONITEM)) {
            select_context_node(window, clicked);
            return 0; // prevent native mouse-down selection from collapsing the group
        }
    }
    if (message == WM_RBUTTONUP) {
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        ClientToScreen(window, &point);
        SendMessageW(GetParent(window), WM_CONTEXTMENU, reinterpret_cast<WPARAM>(window),
            MAKELPARAM(point.x, point.y));
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000)) {
        ListView_SetItemState(window, -1, LVIS_SELECTED, LVIS_SELECTED);
        if (ListView_GetNextItem(window, -1, LVNI_FOCUSED) < 0 && ListView_GetItemCount(window)) {
            ListView_SetItemState(window, 0, LVIS_FOCUSED, LVIS_FOCUSED);
            ListView_SetSelectionMark(window, 0);
        }
        return 0;
    }
    if (message == WM_KEYDOWN && wparam == VK_RETURN) {
        open_selected_node(*state);
        return 0;
    }
    return CallWindowProcW(state->original_node_list_proc, window, message, wparam, lparam);
}

void close_node_view_at(AppState& state, const int tab_index) {
    if (tab_index < 6 ||
        static_cast<std::size_t>(tab_index - 6) >= state.open_node_views.size()) return;
    const int selected = TabCtrl_GetCurSel(state.tab);
    TabCtrl_DeleteItem(state.tab, tab_index);
    state.open_node_views.erase(state.open_node_views.begin() + (tab_index - 6));
    int next = selected;
    if (state.open_node_views.empty()) next = 0;
    else if (selected == tab_index)
        next = std::min(tab_index, 5 + static_cast<int>(state.open_node_views.size()));
    else if (selected > tab_index) --next;
    TabCtrl_SetCurSel(state.tab, next);
    InvalidateRect(state.tab, nullptr, TRUE);
    show_selected_page(state, false);
    RECT area{};
    GetClientRect(state.window, &area);
    layout(state, area.right, area.bottom);
}

void close_selected_node_view(AppState& state) {
    close_node_view_at(state, TabCtrl_GetCurSel(state.tab));
}

COLORREF dos_color(const std::uint8_t value) {
    const auto& color = app::kControlPanelMoebiusEgaPalette[value & 0x0fU];
    return RGB(app::control_panel_vga_channel(color[0]),
        app::control_panel_vga_channel(color[1]),
        app::control_panel_vga_channel(color[2]));
}

std::uint32_t dos_dib_color(const std::uint8_t value) {
    const COLORREF color = dos_color(value);
    // A 32-bit BI_RGB DIB stores B, G, R bytes, while COLORREF stores R, G, B.
    return (static_cast<std::uint32_t>(GetRValue(color)) << 16U) |
        (static_cast<std::uint32_t>(GetGValue(color)) << 8U) |
        static_cast<std::uint32_t>(GetBValue(color));
}

std::string ascii_lowercase(std::string value) {
    for (char& character : value) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    return value;
}

bool draw_moebius_cp437_terminal(const HDC dc,
    const app::ControlPanelTerminalSnapshot& screen,
    const EmbeddedCp437Font& font, const app::NodeViewPixelSize target,
    const app::ScreenSaverPosition origin = {}, const bool saver = false) {
    if (target.width <= 0 || target.height <= 0) return false;
    constexpr int native_cell_width = 8;
    constexpr int native_cell_height = 16;
    constexpr int cell_width = native_cell_width, cell_height = native_cell_height;
    const std::size_t columns = saver ? 36U : 80U, rows = saver ? 5U : 25U;
    const int width = static_cast<int>(columns) * cell_width;
    const int height = static_cast<int>(rows) * cell_height;
    std::vector<std::uint32_t> pixels(static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height), dos_dib_color(0));
    for (std::size_t row = 0; row < rows; ++row) {
        for (std::size_t column = 0; column < columns; ++column) {
            const auto& cell = screen.cells[row * app::kControlPanelTerminalColumns + column];
            const auto foreground = dos_dib_color(cell.attribute);
            const auto background = dos_dib_color(
                static_cast<std::uint8_t>((cell.attribute >> 4U)&7U));
            const int left = static_cast<int>(column) * cell_width;
            const int top = static_cast<int>(row) * cell_height;
            for (int glyph_y = 0; glyph_y < native_cell_height; ++glyph_y) {
                const std::uint8_t bits = font[static_cast<std::size_t>(cell.character) *
                    native_cell_height + static_cast<std::size_t>(glyph_y)];
                for (int glyph_x = 0; glyph_x < native_cell_width; ++glyph_x) {
                    const std::uint32_t color = (bits & (0x80U >> glyph_x)) != 0U
                        ? foreground : background;
                    pixels[static_cast<std::size_t>(top + glyph_y) * width + left + glyph_x] = color;
                }
            }
        }
    }
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = width;
    information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    const int previous = SetStretchBltMode(dc, COLORONCOLOR);
    const int result = StretchDIBits(dc, origin.x, origin.y, target.width, target.height,
        0, 0, width, height, pixels.data(), &information, DIB_RGB_COLORS, SRCCOPY);
    if (previous != 0) SetStretchBltMode(dc, previous);
    return result != 0 && result != GDI_ERROR;
}

app::ControlPanelTerminalSnapshot idle_screen(AppState& state,
    const app::AlphaNodeStatus& node) {
    app::ControlPanelTerminalSnapshot screen;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    // A visual fixture selects the exact authored grid through the real paint
    // path. Live substitutions are captured and checked separately.
    if (state.automation.authored_ansi_fixture && state.owner_node_view)
        return *state.owner_node_view;
#endif
    for (auto& cell : screen.cells) cell = {0x20, 0x07};
    if (!state.owner_node_view) return screen;
    app::ControlPanelNodeViewFields fields;
    fields.abbreviated_state = !node.available ? "N/A" : !node.enabled ? "OFF" :
        node.state == sfx::core::NodeState::ready ? "RDY" :
        node.state == sfx::core::NodeState::connecting ? "CON" :
        node.state == sfx::core::NodeState::online ? "ONL" : "BSY";
    const std::string root = ascii_lowercase(
        win32::alpha_installation_root().make_preferred().u8string());
    fields.work_path = "not available - not configured";
    fields.display_path = root + "\\displays";
    fields.message_path = "not available - not configured";
    fields.protocol_path = root + "\\work\\sexyz";
    const bool actual_fault = state.status.state == app::AlphaRuntimePresentationState::faulted ||
        state.status.state == app::AlphaRuntimePresentationState::recovery_required;
    fields.configuration_result = state.status.configuration_loaded ? "ok" : "not ready";
    const bool database_ready =
        state.status.storage.state == app::StorageRuntimeState::ready;
    fields.database_result = database_ready ? "ok" : "not ready";
    const bool node_ready = node.enabled && node.state == sfx::core::NodeState::ready;
    fields.node_readiness_result = !node.available ? "not available" :
        !node.enabled ? "disabled" : node_ready ? "ok" : "busy";
    fields.listener_result = "Telnet tcp/" + std::to_string(state.status.telnet_port) +
        (state.status.telnet_active ? " ON" : " DOWN") + "; SSH tcp/" +
        std::to_string(state.status.ssh_port) + (!state.status.ssh_configured ? " OFF" :
            state.status.ssh_active ? " ON" : " DOWN");
    const bool overall_ready = state.status.configuration_loaded && database_ready &&
        node_ready && state.status.telnet_active &&
        (!state.status.ssh_configured || state.status.ssh_active);
    fields.health_summary_result = overall_ready ? "OK" :
        actual_fault ? "FAULT" : "NOT RDY";
    fields.ready_result = node_ready ? "Spitfire-X ready for use" :
        !node.available ? "Node unavailable; see Runtime diagnostic" :
        !node.enabled ? "Node disabled by Sysop" : "Node is busy";
#ifdef SFX_ALPHA_TEST_AUTOMATION
    state.automation.last_live_fields = fields;
#endif
    return app::compose_control_panel_node_view(*state.owner_node_view, fields);
}

bool screen_saver_active(const AppState& state, const app::AlphaNodeStatus& node,
    const ULONGLONG now) {
    return node.number < state.node_view_interaction.size() && !node.screen_active &&
        node.available && node.enabled &&
        node.state == sfx::core::NodeState::ready &&
        (
#ifdef SFX_ALPHA_TEST_AUTOMATION
        state.automation.saver_steps[node.number].has_value() ||
#endif
        now - state.node_view_interaction[node.number] >= 180000);
}

void draw_terminal(HWND window, AppState& state, HDC dc) {
    RECT area{}; GetClientRect(window, &area);
    FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    const auto* node = node_status(state, state.displayed_node);
    if (node == nullptr) return;
    const bool caller_screen = node->screen_active;
    if (caller_screen && !node->screen_included) {
        SetTextColor(dc, RGB(220, 220, 220)); SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, L"Waiting for the selected node screen...", -1, &area, DT_LEFT | DT_TOP | DT_NOPREFIX);
        return;
    }
    app::ControlPanelTerminalSnapshot screen = caller_screen ? node->screen : idle_screen(state, *node);
    const ULONGLONG now = GetTickCount64();
    const bool saver = screen_saver_active(state, *node, now);
    const int available_width = area.right - area.left;
    const int available_height = area.bottom - area.top;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    state.automation.painted_canvas = {};
    state.automation.painted_art = {};
    state.automation.painted_saver = saver;
#endif
    if (state.terminal_font) {
        const auto canvas = app::node_view_pixel_size(available_width, available_height, state.node_view_scale);
        if (!canvas) return;
        auto target = *canvas;
        app::ScreenSaverPosition origin;
        if (saver && state.owner_screen_saver) {
            std::uint64_t step =
                (now - state.node_view_interaction[node->number] - 180000) / 30000;
#ifdef SFX_ALPHA_TEST_AUTOMATION
            if (state.automation.saver_steps[node->number]) step = *state.automation.saver_steps[node->number];
#endif
            // Extract the exact authored 36x5 artwork, then place its scaled
            // pixels anywhere in THIS child viewport, not in an 80x25 motion grid.
            screen = app::compose_control_panel_screen_saver(*state.owner_screen_saver,
                step + node->number - 1U, 36, 5).screen;
            target = {static_cast<int>((static_cast<std::int64_t>(canvas->width) * 36 + 79) / 80), canvas->height / 5};
            origin = state.saver_placement[node->number].place(
                available_width - target.width, available_height - target.height,
                state.node_view_interaction[node->number] + step);
        }
#ifdef SFX_ALPHA_TEST_AUTOMATION
        state.automation.last_painted_terminal = screen;
        state.automation.painted_canvas = {0,0,canvas->width,canvas->height};
        state.automation.painted_art = {origin.x,origin.y,origin.x+target.width,origin.y+target.height};
        state.automation.painted_saver_color = static_cast<unsigned>(screen.sequence - 1U) % 3U;
#endif
        (void)draw_moebius_cp437_terminal(dc, screen, *state.terminal_font, target, origin, saver);
        return;
    }
    if (saver && state.owner_screen_saver) {
        const std::uint64_t step = (now - state.node_view_interaction[node->number] - 180000) / 30000;
        screen = app::compose_control_panel_screen_saver(*state.owner_screen_saver,
            step + node->number - 1U).screen;
    }
    HFONT font = nullptr;
    int cell_width = 0, cell_height = 0;
    HGDIOBJ old_font = nullptr;
    std::optional<app::ControlPanelTerminalGridLayout> grid_layout;
    const int maximum_height = std::max(6, available_height / 25);
    for (int requested = maximum_height; requested >= 6; --requested) {
        HFONT candidate = CreateFontW(requested, 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, OEM_CHARSET, OUT_RASTER_PRECIS,
            CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
            FIXED_PITCH | FF_MODERN, L"Terminal");
        if (candidate == nullptr) continue;
        const HGDIOBJ previous = SelectObject(dc, candidate);
        TEXTMETRICW metrics{};
        SIZE glyph{};
        const bool measured = GetTextMetricsW(dc, &metrics) != FALSE &&
            GetTextExtentPoint32W(dc, L"M", 1, &glyph) != FALSE;
        const int measured_width = measured ? glyph.cx : 0;
        const int measured_height = measured
            ? metrics.tmHeight + metrics.tmExternalLeading : 0;
        const auto candidate_layout = measured
            ? app::control_panel_terminal_grid_layout(available_width,
                available_height, measured_width, metrics.tmHeight,
                metrics.tmExternalLeading)
            : std::nullopt;
        if (candidate_layout) {
            font = candidate; old_font = previous;
            cell_width = measured_width; cell_height = measured_height;
            grid_layout = candidate_layout;
            break;
        }
        SelectObject(dc, previous);
        DeleteObject(candidate);
    }
    if (font == nullptr || !grid_layout) return;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    state.automation.last_painted_terminal = screen;
#endif
    // The owner-authored ANSI is a top-left anchored 80x25 canvas. Spare pane
    // space remains outside that canvas instead of re-centering its composition.
    const int origin_x = area.left;
    const int origin_y = area.top;
    SetBkMode(dc, OPAQUE);
    for (std::size_t row = 0; row < 25; ++row) {
        for (std::size_t column = 0; column < 80; ++column) {
            const auto& cell = screen.cells[row * 80 + column];
            const char byte = static_cast<char>(cell.character);
            wchar_t character = L' ';
            (void)MultiByteToWideChar(437, 0, &byte, 1, &character, 1);
            SetTextColor(dc, dos_color(cell.attribute));
            SetBkColor(dc, dos_color(static_cast<std::uint8_t>((cell.attribute >> 4U)&7U)));
            RECT cell_area{origin_x + static_cast<int>(column) * cell_width,
                origin_y + static_cast<int>(row) * cell_height,
                origin_x + static_cast<int>(column + 1U) * cell_width,
                origin_y + static_cast<int>(row + 1U) * cell_height};
            ExtTextOutW(dc, cell_area.left, cell_area.top, ETO_OPAQUE | ETO_CLIPPED,
                &cell_area, &character, 1, nullptr);
        }
    }
    SelectObject(dc, old_font);
    DeleteObject(font);
}

LRESULT CALLBACK terminal_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = reinterpret_cast<AppState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state != nullptr && message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        draw_terminal(window, *state, dc);
#ifdef SFX_ALPHA_TEST_AUTOMATION
        ++state->automation.native_terminal_paints;
        const auto* painted_node = node_status(*state, state->displayed_node);
        state->automation.native_saver_painted = painted_node &&
            screen_saver_active(*state, *painted_node, GetTickCount64());
#endif
        EndPaint(window, &paint);
        return 0;
    }
    if (state != nullptr && message == WM_PRINTCLIENT) {
        draw_terminal(window, *state, reinterpret_cast<HDC>(wparam));
        return 0;
    }
    if (state != nullptr && (message == WM_KEYDOWN || message == WM_LBUTTONDOWN)) {
        const auto* node = node_status(*state, state->displayed_node);
        if (node != nullptr && screen_saver_active(*state, *node, GetTickCount64()) &&
            (message != WM_KEYDOWN || wparam != VK_HOME)) return 0;
        if (message == WM_LBUTTONDOWN) SetFocus(window);
        if (state->displayed_node < state->node_view_interaction.size())
            state->node_view_interaction[state->displayed_node] = GetTickCount64();
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

std::wstring statistic_text(const app::AlphaStatisticValue* const statistic,
    const bool today, const bool duration) {
    if (statistic == nullptr || !statistic->available) return L"Not Available";
    const std::uint64_t value = today ? statistic->today : statistic->total;
    if (!duration) return std::to_wstring(value);
    return std::to_wstring(value / 3600U) + L"h " +
        std::to_wstring((value % 3600U) / 60U) + L"m " +
        std::to_wstring(value % 60U) + L"s";
}

void draw_statistics(HWND window, AppState& state, HDC dc) {
    RECT client{}; GetClientRect(window, &client);
    FillRect(dc, &client, state.panel_brush);
    SetBkMode(dc, TRANSPARENT);
    const HGDIOBJ previous_font = SelectObject(dc, state.ui_font);
    const int padding = scaled(window, 12);
    const int width = std::max(1,
        static_cast<int>(client.right - client.left) - padding * 2);
    const int header_height = scaled(window, 32);
    constexpr std::size_t row_count = 3;
    const int available_row_height = std::max(1,
        (static_cast<int>(client.bottom - client.top) - header_height - padding) /
            12);
    const int row_height = std::min(scaled(window, 26),
        std::max(scaled(window, 19), available_row_height));
    const int left = client.left + padding;
    const int right = client.right - padding;
    const int statistic_right = left + width / 2;
    const int today_right = statistic_right + width / 4;
    RECT statistic_header{left + scaled(window, 7), client.top,
        statistic_right - scaled(window, 5), client.top + header_height};
    RECT today_header{statistic_right + scaled(window, 5), client.top,
        today_right - scaled(window, 8), client.top + header_height};
    RECT total_header{today_right + scaled(window, 5), client.top,
        right - scaled(window, 8), client.top + header_height};
    SetTextColor(dc, kGoldColor);
    DrawTextW(dc, L"STATISTIC", -1, &statistic_header,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(dc, L"TODAY", -1, &today_header,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    DrawTextW(dc, L"TOTAL", -1, &total_header,
        DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    RECT separator{left, client.top + header_height - 1, right,
        client.top + header_height};
    fill_rectangle(dc, separator, kGoldColor);

    struct Row {
        const wchar_t* label;
        const app::AlphaStatisticValue* value;
        bool duration;
    };
    const std::array<Row, row_count> rows{{
        {L"Successful logons", &state.status.statistics.successful_logons, false},
        {L"Caller time", &state.status.statistics.caller_seconds, true},
        {L"New users", &state.status.statistics.new_users, false},
    }};
    for (std::size_t index = 0; index < rows.size(); ++index) {
        const int top = client.top + header_height + static_cast<int>(index) * row_height;
        RECT row{left, top, right, top + row_height};
        if ((index & 1U) != 0U) fill_rectangle(dc, row, kAlternateSurfaceColor);
        RECT label{left + scaled(window, 7), top,
            statistic_right - scaled(window, 5), top + row_height};
        RECT today{statistic_right + scaled(window, 5), top,
            today_right - scaled(window, 8), top + row_height};
        RECT total{today_right + scaled(window, 5), top,
            right - scaled(window, 8), top + row_height};
        SetTextColor(dc, rows[index].value == nullptr ? kMutedTextColor : kTextColor);
        DrawTextW(dc, rows[index].label, -1, &label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const std::wstring today_text = statistic_text(
            rows[index].value, true, rows[index].duration);
        const std::wstring total_text = statistic_text(
            rows[index].value, false, rows[index].duration);
        DrawTextW(dc, today_text.c_str(), -1, &today,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextW(dc, total_text.c_str(), -1, &total,
            DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        RECT row_line{left, top + row_height - 1, right, top + row_height};
        fill_rectangle(dc, row_line, kGridColor);
    }
    RECT vertical_one{statistic_right, client.top, statistic_right + 1,
        client.top + header_height + static_cast<int>(rows.size()) * row_height};
    RECT vertical_two{today_right, client.top, today_right + 1,
        vertical_one.bottom};
    fill_rectangle(dc, vertical_one, kGridColor);
    fill_rectangle(dc, vertical_two, kGridColor);
    const int heading_top = vertical_one.bottom + scaled(window,8);
    RECT runtime_heading{left,heading_top,right,heading_top + header_height};
    SetTextColor(dc,kGoldColor);
    DrawTextW(dc,L"RECORDED THIS RUNTIME — resets at restart",-1,&runtime_heading,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    constexpr auto counter_count = static_cast<std::size_t>(app::OperationalCounter::count);
    constexpr auto counter_rows = (counter_count + 1) / 2;
    for (std::size_t index = 0; index < counter_count; ++index) {
        const int column = static_cast<int>(index / counter_rows);
        const int column_left = left + column * width / 2;
        const int column_right = column_left + width / 2 - scaled(window,5);
        const int top = runtime_heading.bottom + static_cast<int>(index % counter_rows) * row_height;
        RECT row{column_left,top,column_right,top + row_height};
        if (index % counter_rows % 2) fill_rectangle(dc,row,kAlternateSurfaceColor);
        RECT label{column_left + scaled(window,7),top,column_left + width / 3,top + row_height};
        RECT value{label.right + scaled(window,5),top,column_right - scaled(window,5),top + row_height};
        const auto name = utf8_to_wide(std::string(app::operational_counter_label(static_cast<app::OperationalCounter>(index))));
        const auto count = state.have_status && state.status.reporting.available
            ? std::to_wstring(state.status.reporting.counts[index]) : L"N/A";
        SetTextColor(dc,kTextColor);
        DrawTextW(dc,name.c_str(),-1,&label,DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawTextW(dc,count.c_str(),-1,&value,DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    const int footer_top = runtime_heading.bottom + static_cast<int>(counter_rows) * row_height + scaled(window,8);
    RECT footer{left,footer_top,right,client.bottom};
    SetTextColor(dc,kMutedTextColor);
    DrawTextW(dc,state.status.log_health ? L"Recording is degraded; counts may be incomplete. See Events / Live Logs." :
        L"Today/Total are persisted accounting; Runtime counters are not all-time totals.",-1,&footer,
        DT_LEFT | DT_TOP | DT_WORDBREAK);
    SelectObject(dc, previous_font);
}

LRESULT CALLBACK statistics_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = reinterpret_cast<AppState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state != nullptr && message == WM_PAINT) {
        PAINTSTRUCT paint{}; const HDC dc = BeginPaint(window, &paint);
        draw_statistics(window, *state, dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (state != nullptr && message == WM_PRINTCLIENT) {
        draw_statistics(window, *state, reinterpret_cast<HDC>(wparam));
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void graph(HDC dc, const RECT& area, const std::vector<double>& values, const double maximum,
    const COLORREF color) {
    if (area.right <= area.left || area.bottom <= area.top) return;
    fill_rectangle(dc, area, g_appearance.theme() == win32::DesktopTheme::dark_retro
        ? RGB(18, 22, 25) : kBackgroundColor);
    frame_rectangle(dc, area, kBorderColor);
    double scale_maximum = maximum;
    if (scale_maximum <= 0) {
        for (const double value : values) scale_maximum = std::max(scale_maximum, value);
    }
    const int baseline = area.bottom - 2;
    HPEN baseline_pen = CreatePen(PS_SOLID, 1, kGridColor);
    const HGDIOBJ previous_baseline = SelectObject(dc, baseline_pen);
    MoveToEx(dc, area.left + 1, baseline, nullptr);
    LineTo(dc, area.right - 1, baseline);
    SelectObject(dc, previous_baseline);
    DeleteObject(baseline_pen);
    if (values.size() < 2) return;
    if (scale_maximum <= 0) scale_maximum = 1.0;
    HPEN pen = CreatePen(PS_SOLID, 2, color);
    const HGDIOBJ old = SelectObject(dc, pen);
    for (std::size_t index = 1; index < values.size(); ++index) {
        const int inner_width = std::max(1,
            static_cast<int>(area.right - area.left) - 3);
        const int x1 = area.left + 1 + static_cast<int>((index - 1U) *
            static_cast<std::size_t>(inner_width) / (values.size() - 1U));
        const int x2 = area.left + 1 + static_cast<int>(index *
            static_cast<std::size_t>(inner_width) / (values.size() - 1U));
        const int graph_height = std::max(1,
            static_cast<int>(area.bottom - area.top) - 3);
        const int y1 = baseline - static_cast<int>(
            std::min(scale_maximum, values[index - 1U]) / scale_maximum * graph_height);
        const int y2 = baseline - static_cast<int>(
            std::min(scale_maximum, values[index]) / scale_maximum * graph_height);
        MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2);
    }
    SelectObject(dc, old); DeleteObject(pen);
}

std::wstring compact_bytes(const double bytes) {
    const wchar_t* suffix = L"B";
    double value = std::max(0.0, bytes);
    if (value >= 1024.0 * 1024.0 * 1024.0) { value /= 1024.0 * 1024.0 * 1024.0; suffix = L"G"; }
    else if (value >= 1024.0 * 1024.0) { value /= 1024.0 * 1024.0; suffix = L"M"; }
    else if (value >= 1024.0) { value /= 1024.0; suffix = L"K"; }
    wchar_t text[32]{};
    swprintf_s(text, value == 0.0 || value >= 10.0 ? L"%.0f%s" : L"%.1f%s",
        value, suffix);
    return text;
}

void draw_monitor_icon(HDC dc, const RECT& area, const int kind, const COLORREF color) {
    HPEN pen = CreatePen(PS_SOLID, kind == 5 ? 1 : 2, color);
    const HGDIOBJ previous_pen = SelectObject(dc, pen);
    const HGDIOBJ previous_brush = SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    const int cx = (area.left + area.right) / 2;
    const int cy = (area.top + area.bottom) / 2;
    if (kind == 0) {
        Rectangle(dc, cx - 9, cy - 9, cx + 10, cy + 10);
        for (int offset = -6; offset <= 6; offset += 6) {
            MoveToEx(dc, cx + offset, cy - 14, nullptr); LineTo(dc, cx + offset, cy - 9);
            MoveToEx(dc, cx + offset, cy + 10, nullptr); LineTo(dc, cx + offset, cy + 15);
            MoveToEx(dc, cx - 14, cy + offset, nullptr); LineTo(dc, cx - 9, cy + offset);
            MoveToEx(dc, cx + 10, cy + offset, nullptr); LineTo(dc, cx + 15, cy + offset);
        }
    } else if (kind == 1) {
        Rectangle(dc, cx - 14, cy - 8, cx + 15, cy + 9);
        for (int x = cx - 9; x <= cx + 7; x += 8) Rectangle(dc, x, cy - 4, x + 5, cy + 4);
        for (int x = cx - 10; x <= cx + 10; x += 5) {
            MoveToEx(dc, x, cy + 9, nullptr); LineTo(dc, x, cy + 13);
        }
    } else if (kind == 2) {
        Ellipse(dc, cx - 14, cy - 14, cx + 15, cy + 15);
        Ellipse(dc, cx - 4, cy - 4, cx + 5, cy + 5);
        MoveToEx(dc, cx, cy, nullptr); LineTo(dc, cx + 10, cy - 8);
    } else if (kind == 3) {
        Ellipse(dc, cx - 13, cy - 13, cx + 14, cy - 4);
        MoveToEx(dc, cx - 13, cy - 9, nullptr); LineTo(dc, cx - 13, cy + 11);
        MoveToEx(dc, cx + 13, cy - 9, nullptr); LineTo(dc, cx + 13, cy + 11);
        Arc(dc, cx - 13, cy + 2, cx + 14, cy + 12, cx + 14, cy + 7, cx - 13, cy + 7);
        Arc(dc, cx - 13, cy + 7, cx + 14, cy + 17, cx + 14, cy + 12, cx - 13, cy + 12);
    } else if (kind == 4) {
        MoveToEx(dc, cx, cy + 13, nullptr); LineTo(dc, cx, cy - 4);
        MoveToEx(dc, cx - 7, cy + 13, nullptr); LineTo(dc, cx + 7, cy + 13);
        Arc(dc, cx - 9, cy - 11, cx + 10, cy + 8, cx + 10, cy - 1, cx - 9, cy - 1);
        Arc(dc, cx - 15, cy - 17, cx + 16, cy + 14, cx + 16, cy - 2, cx - 15, cy - 2);
    } else {
        // Simple native one-pixel clock face, two hands and an upward arrow.
        // Keep the silhouette clear at the compact card's actual pixel size.
        const int unit = std::max(1L, (area.right - area.left) / 30);
        const auto line = [&](int x1, int y1, int x2, int y2) {
            MoveToEx(dc, cx + x1 * unit, cy + y1 * unit, nullptr);
            LineTo(dc, cx + x2 * unit, cy + y2 * unit);
        };
        // Adjacent single-pixel strokes give the small face/hands a bold,
        // crisp silhouette without scaling a bitmap or adding dial detail.
        Ellipse(dc, cx - 14 * unit, cy - 8 * unit, cx + 10 * unit, cy + 16 * unit);
        Ellipse(dc, cx - 14 * unit + 1, cy - 8 * unit + 1, cx + 10 * unit - 1, cy + 16 * unit - 1);
        line(-2,4,-2,-4); line(-2,4,5,4);
        line(-1,4,-1,-4); line(-2,3,5,3);
        line(11,-5,11,-15); line(7,-11,11,-15); line(11,-15,15,-11);
    }
    SelectObject(dc, previous_brush);
    SelectObject(dc, previous_pen);
    DeleteObject(pen);
}

void draw_monitor(HWND window, AppState& state, HDC dc) {
#ifdef SFX_ALPHA_TEST_AUTOMATION
    ++state.automation.monitor_paints;
    std::ostringstream observation;
    observation << std::setprecision(std::numeric_limits<double>::max_digits10);
#endif
    RECT client{}; GetClientRect(window, &client);
    FillRect(dc, &client, state.panel_brush);
    SelectObject(dc, state.ui_font); SetBkMode(dc, TRANSPARENT);
    const int margin = scaled(window, 4);
    RECT header_window{};
    const int title_height = state.list_header != nullptr &&
            GetWindowRect(state.list_header, &header_window)
        ? std::max(1, static_cast<int>(header_window.bottom - header_window.top))
        : scaled(window, 24);
    RECT header{0, 0, client.right, title_height};
#ifdef SFX_ALPHA_TEST_AUTOMATION
    observation << "canvas\t" << client.right << '\t' << client.bottom << '\t' << title_height << '\n';
#endif
    fill_rectangle(dc, header, kSurfaceColor);
    frame_rectangle(dc, header, kBorderColor);
    SetTextColor(dc, kGoldColor);
    RECT title{scaled(window, 7), 0, client.right - scaled(window, 5), title_height};
    DrawTextW(dc, L"System Monitor", -1, &title,
        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    std::array<COLORREF, 6> colors{{
        RGB(82, 202, 222), RGB(205, 94, 193), RGB(104, 202, 126),
        RGB(221, 162, 62), RGB(70, 180, 211), RGB(205, 94, 193)}};
    if (g_appearance.theme() == win32::DesktopTheme::classic)
        colors = {{RGB(0,105,127), RGB(142,44,131), RGB(24,111,43),
            RGB(132,82,0), RGB(0,99,124), RGB(142,44,131)}};
    if (state.preferences.system_monitor_grayscale) colors.fill(
        g_appearance.theme() == win32::DesktopTheme::dark_retro ? RGB(153,153,153) : kMutedTextColor);
    const int gap = scaled(window, 5);
    // The monitor owns its entire allocated sidebar, including unused space.
    // Keep the accepted card geometry while repainting the lower area too.
    const int cards_height = std::max(5,
        std::min(static_cast<int>(client.bottom), scaled(window, 399)) - title_height - gap - margin);
    const int card_height = std::max(1, (cards_height - gap * 5) / 6);
    const int icon_width = scaled(window, 38);
    const int graph_width = scaled(window, 62);
    const int text_gap = scaled(window, 4);
    const auto one_decimal_gib = [](const std::uint64_t bytes) {
        wchar_t text[24]{};
        swprintf_s(text, L"%.1f", static_cast<double>(bytes) /
            (1024.0 * 1024.0 * 1024.0));
        return std::wstring(text);
    };
    for (int index = 0; index < 6; ++index) {
        const int top = title_height + gap + index * (card_height + gap);
        RECT card{margin, top, client.right - margin,
            std::min(static_cast<int>(client.bottom), top + card_height)};
        fill_rectangle(dc, card, kSurfaceColor);
        frame_rectangle(dc, card, colors[static_cast<std::size_t>(index)]);
        RECT icon{card.left + scaled(window, 3), card.top + scaled(window, 3),
            card.left + icon_width, card.bottom - scaled(window, 3)};
        draw_monitor_icon(dc, icon, index, colors[static_cast<std::size_t>(index)]);
        RECT spark{card.right - graph_width - scaled(window, 5),
            card.top + scaled(window, 8), card.right - scaled(window, 5),
            card.bottom - scaled(window, 8)};
        const int text_left = card.left + icon_width + text_gap;
        RECT label{text_left, card.top + scaled(window, 3), spark.left - text_gap,
            card.top + scaled(window, 20)};
        RECT primary{text_left, card.top + scaled(window, 19), spark.left - text_gap,
            card.top + scaled(window, 36)};
        RECT secondary{text_left, card.top + scaled(window, 35), spark.left - text_gap,
            card.bottom - scaled(window, 2)};
        if (card_height < scaled(window, 48)) {
            // At the existing minimum window height, use three adjacent text
            // rows instead of clipping the sixth card. Face, size and colors
            // stay unchanged; the normal-size five existing cards are exact.
            const int inset = scaled(window, 1);
            const int row = std::max(1, (card_height - 2 * inset) / 3);
            label.top = card.top + inset; label.bottom = label.top + row;
            primary.top = label.bottom; primary.bottom = primary.top + row;
            secondary.top = primary.bottom; secondary.bottom = card.bottom - inset;
        }
        if (index >= 5) {
            // Uptime has values, not a rolling graph.
            spark = {};
            label.right = primary.right = secondary.right = card.right - scaled(window, 5);
        }
        const auto os_uptime = index == 5 ? GetTickCount64() : 0;
        std::wstring heading, first, second;
        const std::vector<double>* history = nullptr;
        double maximum = 100.0;
        if (index == 0) {
            heading = L"CPU"; history = &state.cpu_history.values();
            first = state.metrics.cpu_available
                ? std::to_wstring(static_cast<int>(std::lround(state.metrics.total_cpu_percent))) + L"%"
                : L"Unavailable";
            second = state.metrics.cpu_available
                ? std::to_wstring(state.metrics.per_core_cpu_percent.size()) + L" logical CPUs" : L"";
        } else if (index == 1) {
            heading = L"Memory"; history = &state.memory_history.values();
            if (state.metrics.memory_available && state.metrics.memory_total_bytes != 0) {
                const auto percent_used = static_cast<int>(std::lround(100.0 *
                    static_cast<double>(state.metrics.memory_used_bytes) /
                    static_cast<double>(state.metrics.memory_total_bytes)));
                first = one_decimal_gib(state.metrics.memory_used_bytes) + L"/" +
                    one_decimal_gib(state.metrics.memory_total_bytes) + L" GiB";
                second = std::to_wstring(percent_used) + L"% used";
            } else first = L"Unavailable";
        } else if (index == 2) {
            history = &state.disk_history.values();
            maximum = app::disk_activity_graph_maximum(*history);
            heading = L"Disks 0-" + compact_bytes(maximum) + L"/s";
            first = state.metrics.storage_activity_available
                ? L"R+W " + compact_bytes(state.metrics.storage_read_bytes_per_second +
                    state.metrics.storage_write_bytes_per_second) + L"/s" : L"Unavailable";
            if (state.metrics.storage_activity_available)
                second = L"R" + compact_bytes(state.metrics.storage_read_bytes_per_second) +
                    L" W" + compact_bytes(state.metrics.storage_write_bytes_per_second) + L"/s";
        } else if (index == 3) {
            heading = L"SQLite"; history = &state.sqlite_history.values();
            maximum = app::sqlite_activity_graph_maximum(*history);
            if (!state.sqlite_metrics.available) first = L"Unavailable";
            else if (!state.sqlite_metrics.interval_available) first = L"Sampling…";
            else {
                first = L"R " + std::to_wstring(state.sqlite_metrics.page_reads_per_second) +
                    L"/s W " + std::to_wstring(state.sqlite_metrics.page_writes_per_second) + L"/s";
                second = state.sqlite_metrics.cache_hit_available
                    ? L"Cache " + std::to_wstring(
                        state.sqlite_metrics.cache_hit_basis_points / 100U) + L"%" : L"Cache —";
            }
        } else if (index == 4) {
            heading = L"Network (host)"; history = &state.network_history.values(); maximum = 0;
            first = state.metrics.network_available
                ? L"R" + compact_bytes(state.metrics.network_receive_bytes_per_second) +
                    L" S" + compact_bytes(state.metrics.network_send_bytes_per_second) + L" /s"
                : L"Unavailable";
            if (state.metrics.network_available)
                second = L"T " + compact_bytes(static_cast<double>(state.metrics.network_received_bytes)) +
                    L"/" + compact_bytes(static_cast<double>(state.metrics.network_sent_bytes));
        } else if (index == 5) {
            heading = L"Uptime";
            first = state.runtime_metrics.uptime_available
                ? L"BBS " + utf8_to_wide(app::compact_uptime(state.runtime_metrics.uptime_milliseconds))
                : L"BBS unavailable";
            second = L"OS  " + utf8_to_wide(app::compact_uptime(os_uptime));
        }
#ifdef SFX_ALPHA_TEST_AUTOMATION
        observation << "card\t" << index;
        for (const auto& rect : {card, icon, spark, label, primary, secondary})
            observation << '\t' << rect.left << '\t' << rect.top << '\t' << rect.right << '\t' << rect.bottom;
        observation << '\n' << "text\t" << wide_to_utf8(heading).value_or("") << '\t'
            << wide_to_utf8(first).value_or("") << '\t' << wide_to_utf8(second).value_or("") << '\n';
        observation << "history\t" << maximum;
        if (history != nullptr) for (const auto value : *history) observation << '\t' << value;
        observation << '\n';
#endif
        SetTextColor(dc, colors[static_cast<std::size_t>(index)]);
        DrawTextW(dc, heading.c_str(), -1, &label,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        const COLORREF reading_color = first == L"Unavailable" ? kMutedTextColor : kTextColor;
        SetTextColor(dc, reading_color);
        if (index == 5) {
            RECT value = primary; value.left += scaled(window, 32);
            DrawTextW(dc, L"BBS", -1, &primary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, first.c_str() + 4, -1, &value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else DrawTextW(dc, first.c_str(), -1, &primary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SetTextColor(dc, kMutedTextColor);
        if (index == 5) {
            RECT value = secondary; value.left += scaled(window, 32);
            DrawTextW(dc, L"OS", -1, &secondary, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            DrawTextW(dc, second.c_str() + 4, -1, &value, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        } else DrawTextW(dc, second.c_str(), -1, &secondary,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (history != nullptr) graph(dc, spark, *history, maximum,
            state.preferences.system_monitor_grayscale ? reading_color : colors[static_cast<std::size_t>(index)]);
#ifdef SFX_ALPHA_TEST_AUTOMATION
        if (index == 5) {
            observation << "duration\t" << state.runtime_metrics.uptime_available << '\t'
                << state.runtime_metrics.uptime_milliseconds << '\t' << os_uptime << '\n';
        }
#endif
    }
#ifdef SFX_ALPHA_TEST_AUTOMATION
    state.automation.monitor_content = observation.str();
#endif
}

LRESULT CALLBACK monitor_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = reinterpret_cast<AppState*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state != nullptr && message == WM_ERASEBKGND) {
        RECT area{}; GetClientRect(window, &area);
        fill_rectangle(reinterpret_cast<HDC>(wparam), area, kPanelColor);
        return 1;
    }
    if (state != nullptr && message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        draw_monitor(window, *state, dc);
        EndPaint(window, &paint);
        return 0;
    }
    if (state != nullptr && message == WM_PRINTCLIENT) {
        draw_monitor(window, *state, reinterpret_cast<HDC>(wparam));
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

void draw_owner_control(const DRAWITEMSTRUCT& item, AppState& state) {
    RECT rectangle = item.rcItem;
    if (item.hwndItem == state.status_bar) {
        fill_rectangle(item.hDC, rectangle, kPanelColor);
        RECT accent{rectangle.left, rectangle.top, rectangle.right, rectangle.top + 1};
        fill_rectangle(item.hDC, accent, kGoldColor);
        RECT text = rectangle;
        text.left += 8;
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, kTextColor);
        const HGDIOBJ previous_font = SelectObject(item.hDC, state.ui_font);
        SIZE extent{};
        GetTextExtentPoint32W(item.hDC, state.status_text.c_str(),
            static_cast<int>(state.status_text.size()), &extent);
        const auto& status = extent.cx > text.right - text.left && !state.compact_status_text.empty()
            ? state.compact_status_text : state.status_text;
        DrawTextW(item.hDC, status.c_str(), -1, &text,
            DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        SelectObject(item.hDC, previous_font);
        return;
    }

    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    fill_rectangle(item.hDC, rectangle,
        pressed ? kAlternateSurfaceColor : kSurfaceColor);
    frame_rectangle(item.hDC, rectangle,
        disabled ? kBorderColor : kGoldColor);
    wchar_t label[96]{};
    GetWindowTextW(item.hwndItem, label, static_cast<int>(_countof(label)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? kMutedTextColor :
        pressed ? g_appearance.palette().selected_text : kGoldColor);
    const HGDIOBJ previous_font = SelectObject(item.hDC, state.ui_font);
    if (pressed) OffsetRect(&rectangle, 1, 1);
    DrawTextW(item.hDC, label, -1, &rectangle,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if ((item.itemState & ODS_FOCUS) != 0) {
        RECT focus = item.rcItem;
        InflateRect(&focus, -3, -3);
        DrawFocusRect(item.hDC, &focus);
    }
    SelectObject(item.hDC, previous_font);
}

void draw_workspace(const HWND window, const HDC dc) {
    RECT client{};
    GetClientRect(window, &client);
    fill_rectangle(dc, client, kBackgroundColor);
    RECT banner{0, 0, client.right, scaled(window, 62)};
    fill_rectangle(dc, banner, g_appearance.palette().banner);
    RECT divider{0, banner.bottom - scaled(window, 2), client.right, banner.bottom};
    fill_rectangle(dc, divider, g_appearance.palette().banner_gold);
}

#ifdef SFX_ALPHA_TEST_AUTOMATION
bool capture_terminal_cells(const AppState& state, const std::filesystem::path& path) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    for (const auto& cell : state.automation.last_painted_terminal.cells) {
        stream.put(static_cast<char>(cell.character));
        stream.put(static_cast<char>(cell.attribute));
    }
    return stream.good();
}

bool capture_live_fields(const AppState& state, const std::filesystem::path& path) {
    // Record actual producer text before composition, never inferred colors or
    // trimmed cell contents. This preserves written spaces and clipped lengths.
    const auto& fields = state.automation.last_live_fields;
    const std::array<std::string_view, 11> values{{
        fields.abbreviated_state, fields.work_path, fields.display_path,
        fields.message_path, fields.protocol_path, fields.configuration_result,
        fields.database_result, fields.node_readiness_result, fields.listener_result,
        fields.health_summary_result, fields.ready_result}};
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream << "field\tvalue_hex\n";
    constexpr char hex[] = "0123456789ABCDEF";
    for (std::size_t index = 0; index < values.size(); ++index) {
        stream << index << '\t';
        for (const unsigned char byte : values[index])
            stream << hex[byte >> 4U] << hex[byte & 0x0fU];
        stream << '\n';
    }
    return stream.good();
}

bool capture_window_bmp(const HWND window, const std::filesystem::path& path) {
    ShowWindow(window, SW_SHOW);
    SetWindowPos(window, HWND_TOP, 0, 0, 0, 0,
        SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
    SetForegroundWindow(window);
    RECT rectangle{};
    if (!GetWindowRect(window, &rectangle)) return false;
    std::array<wchar_t, 128> class_name{};
    const bool owned_painted_child = GetClassNameW(window, class_name.data(),
        static_cast<int>(class_name.size())) != 0 &&
        (wcscmp(class_name.data(), kTerminalClass) == 0 ||
         wcscmp(class_name.data(), kMonitorClass) == 0);
    using DwmGetWindowAttributeFunction = HRESULT (WINAPI*)(HWND, DWORD, PVOID, DWORD);
    HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
    if (dwm != nullptr) {
        const auto get_attribute = reinterpret_cast<DwmGetWindowAttributeFunction>(
            GetProcAddress(dwm, "DwmGetWindowAttribute"));
        RECT visible{};
        constexpr DWORD extended_frame_bounds = 9;
        if (get_attribute != nullptr && SUCCEEDED(get_attribute(window,
                extended_frame_bounds, &visible, sizeof(visible)))) rectangle = visible;
        FreeLibrary(dwm);
    }
    const int width = rectangle.right - rectangle.left;
    const int height = rectangle.bottom - rectangle.top;
    HDC screen = GetDC(nullptr);
    HDC memory = CreateCompatibleDC(screen);
    HBITMAP bitmap = CreateCompatibleBitmap(screen, width, height);
    HGDIOBJ previous = SelectObject(memory, bitmap);
    RedrawWindow(window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    GdiFlush();
    // Render the owned window directly so deterministic review evidence cannot be
    // contaminated by another foreground window covering it during automation.
    constexpr UINT render_full_content = 0x00000002U;
    bool painted = false;
    if (owned_painted_child) {
        // The owned child's WM_PRINTCLIENT handler paints the entire client into
        // this fresh unclipped DC. Its output does not depend on the current
        // WM_PAINT update region or PrintWindow's child-window dispatch.
        SendMessageW(window, WM_PRINTCLIENT, reinterpret_cast<WPARAM>(memory),
            PRF_CLIENT | PRF_ERASEBKGND);
        painted = GdiFlush() != FALSE;
    } else {
        painted = PrintWindow(window, memory, render_full_content) != FALSE;
    }
    if (!painted) {
        painted = BitBlt(memory, 0, 0, width, height, screen,
            rectangle.left, rectangle.top, SRCCOPY | CAPTUREBLT) != FALSE;
    }
    BITMAPINFO information{};
    information.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    information.bmiHeader.biWidth = width;
    information.bmiHeader.biHeight = -height;
    information.bmiHeader.biPlanes = 1;
    information.bmiHeader.biBitCount = 32;
    information.bmiHeader.biCompression = BI_RGB;
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) * 4U);
    const bool copied = painted && GetDIBits(memory, bitmap, 0, static_cast<UINT>(height),
        pixels.data(), &information, DIB_RGB_COLORS) != 0;
    SelectObject(memory, previous);
    DeleteObject(bitmap);
    DeleteDC(memory);
    ReleaseDC(nullptr, screen);
    if (!copied) return false;
    BITMAPFILEHEADER header{};
    header.bfType = 0x4d42;
    header.bfOffBits = sizeof(header) + sizeof(BITMAPINFOHEADER);
    header.bfSize = header.bfOffBits + static_cast<DWORD>(pixels.size());
    const std::filesystem::path pending = path.native() + L".pending";
    const HANDLE file = CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool written = write_exact(file, &header, sizeof(header)) &&
        write_exact(file, &information.bmiHeader, sizeof(information.bmiHeader)) &&
        write_exact(file, pixels.data(), static_cast<DWORD>(pixels.size())) &&
        FlushFileBuffers(file) != FALSE;
    CloseHandle(file);
    if (!written) { DeleteFileW(pending.c_str()); return false; }
    // A consumer can never observe a partially written final BMP pathname.
    return MoveFileExW(pending.c_str(), path.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
}

bool monitor_appearance_menu_matches(const AppState& state, const bool grayscale) {
    const HMENU appearance = state.monitor_appearance_menu;
    if (GetMenuItemCount(appearance) != 2) return false;
    const bool color_checked = (GetMenuState(appearance, ID_VIEW_MONITOR_COLOR, MF_BYCOMMAND) & MF_CHECKED) != 0;
    const bool gray_checked = (GetMenuState(appearance, ID_VIEW_MONITOR_GRAYSCALE, MF_BYCOMMAND) & MF_CHECKED) != 0;
    MENUITEMINFOW selected{sizeof(selected)}; selected.fMask = MIIM_FTYPE;
    return color_checked == !grayscale && gray_checked == grayscale &&
        GetMenuItemInfoW(appearance, grayscale ? ID_VIEW_MONITOR_GRAYSCALE : ID_VIEW_MONITOR_COLOR,
            FALSE, &selected) && (selected.fType & MFT_RADIOCHECK) != 0;
}

bool capture_monitor_appearances(AppState& state) {
    const auto& directory = state.automation.owner_review_capture_directory;
    const bool startup_grayscale = state.preferences.system_monitor_grayscale;
    if (!monitor_appearance_menu_matches(state, startup_grayscale)) return false;
    const auto select = [&state](const bool grayscale) {
        const auto before = state.automation.monitor_paints;
        SendMessageW(state.window, WM_COMMAND,
            grayscale ? ID_VIEW_MONITOR_GRAYSCALE : ID_VIEW_MONITOR_COLOR, 0);
        const auto persisted = win32::read_control_panel_ui_preferences(win32::alpha_installation_root());
        return state.preferences.system_monitor_grayscale == grayscale &&
            monitor_appearance_menu_matches(state, grayscale) && state.automation.monitor_paints > before &&
            persisted && app::serialize_control_panel_ui_preferences(*persisted) ==
                app::serialize_control_panel_ui_preferences(state.preferences);
    };
    std::string color_content;
    // Synchronous commands/paints hold the same real metric snapshot and history;
    // no timer polling or synthetic metric data is introduced between modes.
    for (const bool grayscale : {false, true}) {
        if (!select(grayscale)) return false;
        const auto name = grayscale ? L"02b-monitor-grayscale" : L"02a-monitor-color";
        if (!capture_window_bmp(state.monitor_view, directory / (std::wstring(name) + L".bmp"))) return false;
        if (!grayscale) color_content = state.automation.monitor_content;
        else if (color_content != state.automation.monitor_content) return false;
        std::ofstream content(directory / (std::wstring(name) + L".content.tsv"), std::ios::binary);
        content << state.automation.monitor_content;
        std::ofstream preferences(directory / (std::wstring(name) + L".preferences.txt"), std::ios::binary);
        preferences << app::serialize_control_panel_ui_preferences(state.preferences);
        if (!content.good() || !preferences.good()) return false;
    }
    if (state.automation.final_monitor_color && !select(false)) return false;
    std::ofstream result(directory / L"MONITOR_APPEARANCE_TESTS.txt");
    result << "PASS: menu hierarchy; exactly one radio; immediate paint; immediate persisted read-back\n"
        << "PASS: metric text, histories, graph maxima and every drawing rectangle identical between modes\n"
        << "startup_grayscale=" << startup_grayscale << "\n"
        << "expected_startup=" << state.automation.expected_monitor_appearance << "\n"
        << "final_grayscale=" << state.preferences.system_monitor_grayscale << "\n";
    return result.good();
}
#endif

void apply_node_column_widths(AppState& state, const int available_width) {
    const UINT dpi = window_dpi(state.window);
    constexpr std::array<int, 6> minimum_logical{{64, 120, 80, 80, 80, 80}};
    std::array<int, 6> physical{};
    int fixed_total = state.preferences.modem_lights ? scaled(state.window, 126) : 0;
    for (std::size_t index = 0; index < physical.size(); ++index) {
        physical[index] = std::max(MulDiv(minimum_logical[index], static_cast<int>(dpi), 96),
            MulDiv(state.preferences.node_column_widths[index], static_cast<int>(dpi), 96));
        if (index + 1U < physical.size()) fixed_total += physical[index];
    }
    const int minimum_details = MulDiv(minimum_logical.back(), static_cast<int>(dpi), 96);
    const int remainder = std::max(minimum_details, available_width - fixed_total - 2);
    physical.back() = available_width >= fixed_total + physical.back()
        ? std::max(physical.back(), remainder) : remainder;
    state.applying_node_columns = true;
    for (std::size_t index = 0; index < physical.size(); ++index)
        ListView_SetColumnWidth(state.node_list, static_cast<int>(index) +
            (state.preferences.modem_lights && index > 0 ? 1 : 0), physical[index]);
    if (state.preferences.modem_lights) ListView_SetColumnWidth(state.node_list, 1, scaled(state.window, 126));
    state.applying_node_columns = false;
}

void rebuild_ui_fonts(AppState& state, const UINT dpi) {
    HFONT heading = CreateFontW(-MulDiv(22, static_cast<int>(dpi), 96), 0, 0, 0,
        FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
        L"Segoe UI");
    HFONT regular = CreateFontW(-MulDiv(16, static_cast<int>(dpi), 96), 0, 0, 0,
        FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    HFONT bold = CreateFontW(-MulDiv(16, static_cast<int>(dpi), 96), 0, 0, 0,
        FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
    if (heading == nullptr || regular == nullptr || bold == nullptr) {
        if (heading != nullptr) DeleteObject(heading);
        if (regular != nullptr) DeleteObject(regular);
        if (bold != nullptr) DeleteObject(bold);
        return;
    }
    const HFONT previous_heading = state.heading_font;
    const HFONT previous_regular = state.ui_font;
    const HFONT previous_bold = state.bold_font;
    state.heading_font = heading;
    state.ui_font = regular;
    state.bold_font = bold;
    state.binkp.font(regular);
    SendMessageW(state.heading, WM_SETFONT, reinterpret_cast<WPARAM>(heading), TRUE);
    for (HWND control : {state.subheading, state.tab, state.node_list,
            state.open_node_button, state.notice_button, state.disable_all_nodes_button, state.enable_all_nodes_button,
            state.bulk_node_status, state.confirm_disable_all_button, state.cancel_disable_all_button, state.events_view,
            state.log_view, state.log_pause_button, state.log_clear_button,state.log_filter,
            state.stats_view, state.botgate_view, state.setup_button, state.service_button, state.node_header, state.status_bar})
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(regular), TRUE);
    if (state.list_header != nullptr)
        SendMessageW(state.list_header, WM_SETFONT,
            reinterpret_cast<WPARAM>(regular), TRUE);
    if (previous_heading != nullptr) DeleteObject(previous_heading);
    if (previous_regular != nullptr) DeleteObject(previous_regular);
    if (previous_bold != nullptr) DeleteObject(previous_bold);
}

void layout(AppState& state, const int width, const int height) {
    SendMessageW(state.status_bar, SB_SETMINHEIGHT,
        static_cast<WPARAM>(scaled(state.window, 20)), 0);
    SendMessageW(state.status_bar, WM_SIZE, 0, 0);
    RECT status_rectangle{};
    GetWindowRect(state.status_bar, &status_rectangle);
    const int status_height = status_rectangle.bottom - status_rectangle.top;
    MoveWindow(state.heading, scaled(state.window, 18), scaled(state.window, 9),
        width - scaled(state.window, 202), scaled(state.window, 28), TRUE);
    MoveWindow(state.subheading, scaled(state.window, 20), scaled(state.window, 38),
        width - scaled(state.window, 190), scaled(state.window, 20), TRUE);
    MoveWindow(state.setup_button, width - scaled(state.window, 166),
        scaled(state.window, 40), scaled(state.window, 146), scaled(state.window, 23), TRUE);

    const int tab_top = scaled(state.window, 68);
    const int tab_height = scaled(state.window, 31);
    MoveWindow(state.tab, scaled(state.window, 12), tab_top,
        width - scaled(state.window, 24), tab_height, TRUE);
    RECT page{scaled(state.window, 16), tab_top + tab_height + scaled(state.window, 6),
        width - scaled(state.window, 16), height - status_height - scaled(state.window, 8)};
    const int monitor_width = state.preferences.system_monitor_visible ?
        std::min(scaled(state.window, state.preferences.system_monitor_width),
            std::max(scaled(state.window, 251), static_cast<int>(page.right-page.left)/2)) : 0;
    const int button_height = scaled(state.window, 28);
    MoveWindow(state.open_node_button, page.left, page.bottom - button_height,
        scaled(state.window, 150), button_height, TRUE);
    MoveWindow(state.notice_button,page.left+scaled(state.window,160),page.bottom-button_height,scaled(state.window,210),button_height,TRUE);
    const int all_nodes_button_width = scaled(state.window, 180);
    MoveWindow(state.disable_all_nodes_button, page.left + scaled(state.window, 380), page.bottom - button_height,
        all_nodes_button_width, button_height, TRUE);
    MoveWindow(state.enable_all_nodes_button, page.left + scaled(state.window, 390) + all_nodes_button_width,
        page.bottom - button_height, all_nodes_button_width, button_height, TRUE);
    const int review_height = scaled(state.window, 48);
    const int review_top = page.bottom - button_height - review_height;
    const int cancel_width = scaled(state.window, 96);
    const int confirm_width = scaled(state.window, 157);
    const int button_gap = scaled(state.window, 8);
    const int confirm_left = page.right - cancel_width - button_gap - confirm_width;
    const int description_right = state.bulk_disable_review ? confirm_left - button_gap : page.right;
    MoveWindow(state.bulk_node_status, page.left, review_top + scaled(state.window, 4),
        description_right - page.left, review_height - scaled(state.window, 8), TRUE);
    MoveWindow(state.confirm_disable_all_button, confirm_left, review_top + scaled(state.window, 5),
        confirm_width, button_height, TRUE);
    MoveWindow(state.cancel_disable_all_button, page.right - cancel_width, review_top + scaled(state.window, 5),
        cancel_width, button_height, TRUE);
    const int node_list_width = std::max(scaled(state.window, 100),
        static_cast<int>(page.right - page.left) - monitor_width);
    const int node_content_height = page.bottom - page.top - button_height - scaled(state.window, 7) -
        (state.bulk_node_feedback.empty() ? 0 : review_height);
    MoveWindow(state.node_list, page.left, page.top, node_list_width,
        node_content_height, TRUE);
    apply_node_column_widths(state, node_list_width);
    MoveWindow(state.monitor_view, page.right - monitor_width, page.top,
        monitor_width, node_content_height, TRUE);
    MoveWindow(state.events_view, page.left, page.top, page.right - page.left,
        page.bottom - page.top, TRUE);
    MoveWindow(state.binkp.window,page.left,page.top,page.right-page.left,page.bottom-page.top,TRUE);
    MoveWindow(state.log_view, page.left, page.top, page.right - page.left,
        page.bottom - page.top - button_height - scaled(state.window, 7), TRUE);
    MoveWindow(state.log_pause_button, page.left, page.bottom - button_height,
        scaled(state.window, 90), button_height, TRUE);
    MoveWindow(state.log_clear_button, page.left + scaled(state.window, 98),
        page.bottom - button_height, scaled(state.window, 116), button_height, TRUE);
    MoveWindow(state.log_filter,page.left+scaled(state.window,230),page.bottom-button_height,
        scaled(state.window,165),scaled(state.window,180),TRUE);
    MoveWindow(state.stats_view, page.left, page.top, page.right - page.left,
        page.bottom - page.top, TRUE);
    MoveWindow(state.botgate_view,page.left,page.top,page.right-page.left,page.bottom-page.top,TRUE);
    MoveWindow(state.node_header, page.left, page.top, page.right - page.left,
        scaled(state.window, 24), TRUE);
    MoveWindow(state.terminal_view, page.left, page.top + scaled(state.window, 24),
        page.right - page.left,
        page.bottom - page.top - scaled(state.window, 24) -
            (state.preferences.modem_lights ? scaled(state.window, 27) : 0), TRUE);
    MoveWindow(state.modem_strip, page.left,
        page.bottom - scaled(state.window, 24), scaled(state.window, 126), scaled(state.window, 24), TRUE);
}

void launch_configuration(AppState& state,bool botgate=false,app::AccountId account_id=0,bool fileman=false,bool ftn=false) {
    try {
        const auto root = win32::alpha_installation_root();
        // Re-verify the complete protected generation immediately before the
        // fixed-path launch.  Bin is never resolved through PATH or the CWD.
        sfx::platform::win32::installation::verify_runtime_installation(
            root, win32::current_process_user_sid_string());
        const auto executable = root / L"Bin" / (fileman ? L"SFXFileManager.exe" : L"SFXConfig.exe");
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        auto command=L"\""+executable.native()+L"\""+(botgate ? L" --botgate" : ftn?L" --ftn":L"");
        if(account_id>0) command+=L" --edit-user "+std::to_wstring(account_id);
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                0, nullptr, root.c_str(), &startup, &process)) {
            throw std::runtime_error("configuration_launch_failed");
        }
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    } catch (const std::exception& error) {
        MessageBoxW(state.window,
            ((fileman ? std::wstring(L"SFX Fileman could not be opened.\n\n") : std::wstring(L"SFX Config could not be opened.\n\n")) +
             L"Close SFX and run Repair from the matching SFX Setup package. "
             L"The current Runtime has not been disturbed.\n\nDiagnostic: " +
             utf8_to_wide(error.what())).c_str(),
            fileman ? L"SFX Fileman" : L"SFX Config", MB_OK | MB_ICONERROR);
    }
}

void restart_runtime(AppState& state) {
    update_status(state);
    if (!state.have_status ||
        state.status.state != app::AlphaRuntimePresentationState::ready) {
        MessageBoxW(state.window,
            L"Runtime is not ready for an orderly restart. The saved configuration is unchanged.",
            L"Restart Spitfire-X BBS", MB_OK | MB_ICONWARNING);
        return;
    }
    const auto callers = state.status.lifecycle.active_work.caller_sessions;
    const auto detail =
        L"Continuing stops both listeners, disconnects " +
        std::to_wstring(callers) +
        (callers == 1 ? L" active caller, " : L" active callers, ") +
        L"and restarts the Runtime using the saved configuration.";
    if (!task_choice(state.window, L"Spitfire-X BBS",
            L"Restart Spitfire-X BBS?", detail.c_str(), L"Continue", 100)) return;
    std::string diagnostic;
    if (!state.runtime.restart(diagnostic)) {
        MessageBoxW(state.window,
            (L"Runtime restart failed. The Control Panel will continue reporting the "
             L"actual Runtime state.\n\nDiagnostic: " + utf8_to_wide(diagnostic)).c_str(),
            L"Restart Spitfire-X BBS", MB_OK | MB_ICONERROR);
        update_status(state);
        return;
    }
    update_status(state);
    if (state.status.diagnostic_code == "settings_activation_failed_restored") {
        MessageBoxW(state.window,
            L"The saved listener endpoint could not be activated. The last working endpoint was restored.",
            L"Restart Spitfire-X BBS", MB_OK | MB_ICONERROR);
    }
}

void update_startup_dots(AppState& state, unsigned count) {
    state.startup_dot_count = count;
    const auto text = L"SFX: opening Runtime — checking configuration and protection" +
        std::wstring(count, L'.');
    SetWindowTextW(state.subheading, text.c_str());
}

void show_startup_activity(AppState& state, bool active) {
    if (active) {
        update_startup_dots(state, 1);
        SetTimer(state.window, kStartupActivityTimer, kStartupActivityIntervalMs, nullptr);
    } else if (state.startup_dot_count) {
        KillTimer(state.window, kStartupActivityTimer);
        state.startup_dot_count = 0;
    }
}

bool close_runtime(AppState& state, const bool confirm) {
    if (state.closing) return false;
    if (!state.startup_pending && confirm && !state.runtime.is_service_mode()) {
        const wchar_t* detail = state.have_status &&
            state.status.lifecycle.active_work.caller_sessions != 0
            ? L"Continuing will terminate active caller work, shut down the Runtime, and close the Control Panel."
            : L"Continuing will shut down Spitfire-X BBS and close the Control Panel.";
        if (!task_choice(state.window, L"Spitfire-X BBS",
                L"Shut down Spitfire-X BBS?", detail, L"Continue", 100)) return false;
        if (state.closing) return false;
    }
    // The confirmation pumps messages and can finish the metrics worker.
    if(state.startup&&state.startup->thread.joinable()){
        state.startup_close=true;state.startup->cancelled.store(true);
        show_startup_activity(state, false);
        SetWindowTextW(state.subheading,L"Cancelling startup — waiting for owned work to stop...");
        return false;
    }
    if(state.startup_pending){
        if(state.startup&&state.startup->runtime.child_alive()&&!state.startup->runtime.is_service_mode()){
            state.startup_close=true;begin_startup(state,StartupWork::Phase::cleanup);return false;
        }
        DestroyWindow(state.window);return true;
    }
    state.closing = true;
    KillTimer(state.window, kRefreshTimer);
    KillTimer(state.window, kPulseTimer);
    WINDOWPLACEMENT placement{sizeof(placement)};
    KillTimer(state.window, kLogPageTimer);
    const bool have_placement = state.full_screen
        ? (placement = state.windowed_placement, true)
        : GetWindowPlacement(state.window, &placement) != FALSE;
    if (have_placement &&
        placement.showCmd != SW_SHOWMINIMIZED) {
        const RECT rectangle = placement.rcNormalPosition;
        state.preferences.left = rectangle.left;
        state.preferences.top = rectangle.top;
        state.preferences.width = rectangle.right - rectangle.left;
        state.preferences.height = rectangle.bottom - rectangle.top;
    }
    (void)win32::publish_control_panel_ui_preferences(win32::alpha_installation_root(),
        win32::current_process_user_sid_string(), state.preferences);
    std::string diagnostic;
    if (!state.runtime.is_service_mode()&&state.runtime.child_alive() && !state.runtime.shutdown(diagnostic)) {
        state.closing = false;
        SetTimer(state.window, kRefreshTimer, 1000, nullptr);
        const std::wstring detail = L"The runtime did not complete orderly shutdown: " +
            utf8_to_wide(diagnostic);
        MessageBoxW(state.window, detail.c_str(), L"Spitfire-X BBS", MB_OK | MB_ICONERROR);
        return false;
    }
    DestroyWindow(state.window);
    return true;
}

#include "startup.inc"

bool editable_caller(const AppState& state,const app::AlphaNodeStatus* node) {
    return state.have_status && node && node->caller_connected && node->account_id>0 &&
        (node->state==sfx::core::NodeState::online || node->state==sfx::core::NodeState::door || node->state==sfx::core::NodeState::draining) &&
        (state.status.state==app::AlphaRuntimePresentationState::ready || state.status.state==app::AlphaRuntimePresentationState::draining);
}
bool same_edit_caller(const AppState& state,const app::AlphaNodeStatus* node) {
    return editable_caller(state,node) && state.menu_edit_account==node->account_id &&
        state.menu_edit_assignment==node->screen_owner;
}
void populate_node_menu(const HMENU menu, AppState& state) {
    while (GetMenuItemCount(menu) > 0) DeleteMenu(menu, 0, MF_BYPOSITION);
    const auto targets = node_command_targets(state);
    const bool valid = !targets.empty();
    state.menu_edit_account=0;state.menu_edit_assignment={};
    if(targets.size()==1) {
        const auto* node=node_status(state,targets.front());
        if(editable_caller(state,node)) {state.menu_edit_account=node->account_id;state.menu_edit_assignment=node->screen_owner;}
    }
    AppendMenuW(menu, MF_STRING | (valid ? MF_ENABLED : MF_GRAYED),
        ID_NODE_OPEN, L"Open Selected");
    if (targets.size() == 1U) {
        const auto number = targets.front();
        const std::wstring label = L"Logon to Node " + std::to_wstring(number);
        AppendMenuW(menu, MF_STRING |
            (node_ready_for_local_logon(state, number) ? MF_ENABLED : MF_GRAYED),
            ID_NODE_LOGON, label.c_str());
    }
    AppendMenuW(menu,MF_STRING|(state.menu_edit_account ? MF_ENABLED : MF_GRAYED),ID_NODE_EDIT_USER,L"Edit User...");
    AppendMenuW(menu,MF_STRING|(state.menu_edit_account?MF_ENABLED:MF_GRAYED),ID_NODE_NOTICE,L"Send System Notice...");
    const bool paged=targets.size()==1&&std::any_of(state.chat_status.pages.begin(),state.chat_status.pages.end(),[&](const auto& p){return p.caller.node==targets.front();});
    AppendMenuW(menu,MF_STRING|(state.menu_edit_account&&paged?MF_ENABLED:MF_GRAYED),ID_NODE_ANSWER_PAGE,L"Answer Page");
    AppendMenuW(menu,MF_STRING|(state.menu_edit_account?MF_ENABLED:MF_GRAYED),ID_NODE_CHAT,L"Chat with Caller");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (!valid) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
            L"Node control unavailable — select a node");
        return;
    }
    const bool ready = state.have_status && state.status.state == app::AlphaRuntimePresentationState::ready;
    const bool draining = state.have_status && state.status.state == app::AlphaRuntimePresentationState::draining;
    bool can_enable = false, can_disable = false, unavailable = false;
    for (const auto number : targets) {
        const auto* node = node_status(state, number);
        if (!node || !node->available) { unavailable = true; continue; }
        can_enable = can_enable || (ready && !node->enabled);
        can_disable = can_disable || (ready && node->enabled);
    }
    AppendMenuW(menu, MF_STRING | (can_enable ? MF_ENABLED : MF_GRAYED), ID_NODE_ENABLE,
        state.displayed_node ? L"Enable Node" : L"Enable Selected Nodes");
    AppendMenuW(menu, MF_STRING | (can_disable ? MF_ENABLED : MF_GRAYED), ID_NODE_DISABLE,
        state.displayed_node ? L"Disable Node" : L"Disable Selected Nodes");
    bool can_disconnect = false;
    for (const auto number : targets) {
        const auto* node = node_status(state, number);
        can_disconnect = can_disconnect || ((ready || draining) && node && node->available &&
            node->assignment_sequence != 0 && (node->caller_connected ||
                node->state == sfx::core::NodeState::connecting));
    }
    AppendMenuW(menu, MF_STRING | (can_disconnect ? MF_ENABLED : MF_GRAYED),
        ID_NODE_DISCONNECT, state.displayed_node ? L"Disconnect Caller..." : L"Disconnect Selected Callers...");
    bool can_terminate_door=false;
    for(const auto number:targets){const auto* node=node_status(state,number);can_terminate_door=can_terminate_door||((ready||draining)&&node&&node->caller_connected&&node->door_id&&node->door_launch_sequence);}
    AppendMenuW(menu,MF_STRING|(can_terminate_door?MF_ENABLED:MF_GRAYED),ID_NODE_TERMINATE_DOOR,
        state.displayed_node?L"Terminate Door...":L"Terminate Selected Doors...");
    if (!ready) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, draining ?
            L"Enable/disable unavailable during Graceful Shut Down" :
            L"Node control unavailable — Runtime is not ready");
    }
    if (unavailable) {
        AppendMenuW(menu, MF_STRING | MF_GRAYED, 0,
            L"Unavailable nodes cannot be changed — check Runtime diagnostics");
    }
}

#include "alpha_full_screen.inl"

#ifdef SFX_ALPHA_TEST_AUTOMATION
#include "alpha_multinode_automation.inl"
#endif

void apply_desktop_theme(AppState& state, const win32::DesktopPalette& previous) {
    DeleteObject(state.background_brush);
    DeleteObject(state.panel_brush);
    state.background_brush = CreateSolidBrush(kBackgroundColor);
    state.panel_brush = CreateSolidBrush(kPanelColor);
    g_appearance.check_menu(state.theme_menu);
    ListView_SetBkColor(state.node_list, kPanelColor);
    ListView_SetTextBkColor(state.node_list, kPanelColor);
    ListView_SetTextColor(state.node_list, kTextColor);
    ListView_SetExtendedListViewStyleEx(state.node_list, LVS_EX_GRIDLINES,
        g_appearance.theme() == win32::DesktopTheme::classic ? LVS_EX_GRIDLINES : 0);
    SendMessageW(state.status_bar, SB_SETBKCOLOR, 0, kPanelColor);
    if (state.rich_edit_module && state.log_view) {
        // Recolor the existing buffer, including Pause, without fetching logs,
        // changing the inspection position or touching Runtime state.
        CHARRANGE selected{}; POINT scroll{};
        SendMessageW(state.log_view, EM_EXGETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        SendMessageW(state.log_view, EM_GETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        SendMessageW(state.log_view, WM_SETREDRAW, FALSE, 0);
        SendMessageW(state.log_view, EM_SETBKGNDCOLOR, 0, kPanelColor);
        CHARFORMAT2W format{}; format.cbSize = sizeof(format);
        format.dwMask = CFM_COLOR; format.crTextColor = kTextColor;
        SendMessageW(state.log_view, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
        for (auto& span : state.rendered_log_colors) {
            span.second = span.second == previous.error ? kErrorColor :
                span.second == previous.amber ? kAmberColor : kTextColor;
            format.crTextColor = span.second;
            SendMessageW(state.log_view, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&span.first));
            SendMessageW(state.log_view, EM_SETCHARFORMAT, SCF_SELECTION, reinterpret_cast<LPARAM>(&format));
        }
        SendMessageW(state.log_view, EM_EXSETSEL, 0, reinterpret_cast<LPARAM>(&selected));
        SendMessageW(state.log_view, EM_SETSCROLLPOS, 0, reinterpret_cast<LPARAM>(&scroll));
        SendMessageW(state.log_view, WM_SETREDRAW, TRUE, 0);
    }
    RedrawWindow(state.window, nullptr, nullptr,
        RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}

#include "alpha_modem_lights.inl"

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = reinterpret_cast<AppState*>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    const bool startup_paint=message==WM_NOTIFY&&lparam&&
        reinterpret_cast<NMHDR*>(lparam)->hwndFrom==state->node_list&&
        reinterpret_cast<NMHDR*>(lparam)->code==NM_CUSTOMDRAW;
    if(state->startup_pending&&(message==WM_COMMAND||(message==WM_NOTIFY&&!startup_paint)||
        message==WM_CONTEXTMENU||message==kConfigurationAdminMessage||message==kOpenSetupMessage)){
        if(message==WM_COMMAND&&LOWORD(wparam)==ID_FILE_EXIT)(void)close_runtime(*state,false);
        return 0;
    }
    if ((message == win32::DesktopAppearance::changed_message() && message != 0) ||
            (message == WM_ACTIVATEAPP && wparam)) {
        const auto previous = g_appearance.palette();
        if (g_appearance.reload()) apply_desktop_theme(*state, previous);
    }
    switch (message) {
        case kStartupNodesMessage:
            if(state->startup_pending&&!state->startup_close&&wparam<=app::kAlphaNodePoolCapacity){
                state->startup_nodes.clear();
                for(std::uint16_t n=1;n<=wparam;++n){app::AlphaNodeStatus node;node.number=n;state->startup_nodes.push_back(std::move(node));}
                refresh_node_rows(*state);InvalidateRect(state->node_list,nullptr,TRUE);
            }
            return 0;
        case kStartupMessage: complete_startup(*state);return 0;
        case WM_CREATE: {
            state->background_brush = CreateSolidBrush(kBackgroundColor);
            state->panel_brush = CreateSolidBrush(kPanelColor);
            state->heading = CreateWindowExW(0, L"STATIC",
                SFX_PRODUCT_CONTROL_PANEL_DISPLAY_VERSION_WIDE,
                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_MAIN_HEADING), state->instance, nullptr);
            state->subheading = CreateWindowExW(0, L"STATIC", L"SFX: checking program files...",
                WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_MAIN_SUBHEADING), state->instance, nullptr);
            state->setup_button = CreateWindowExW(0, L"BUTTON", L"First-Run Setup…",
                WS_CHILD | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_MAIN_SETUP), state->instance, nullptr);

            state->tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_TABSTOP,
                0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_MAIN_TAB), state->instance, nullptr);
            state->original_tab_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                state->tab, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(tab_rail_proc)));
            SendMessageW(state->tab, TCM_SETPADDING, 0, MAKELPARAM(17, 4));
            const std::array<const wchar_t*, 6> tabs{L"Nodes",L"BinkP", L"Events", L"Live Logs", L"Statistics", L"BotGate"};
            for (std::size_t index = 0; index < tabs.size(); ++index) {
                TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(tabs[index]);
                TabCtrl_InsertItem(state->tab, static_cast<int>(index), &item);
            }
            state->node_list = CreateWindowExW(0, WC_LISTVIEWW, L"Nodes",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP |
                    LVS_REPORT | LVS_SHOWSELALWAYS,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_NODE_LIST), state->instance, nullptr);
            if(!state->binkp.create(window,state->instance))return -1;
            state->original_node_list_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                state->node_list, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(node_list_proc)));
            ListView_SetExtendedListViewStyle(state->node_list,
                LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
            const std::array<const wchar_t*, 6> columns{{
                L"Node", L"Status", L"Caller-Handle", L"Connection", L"Activity", L"Details"}};
            for (std::size_t index = 0; index < columns.size(); ++index) {
                LVCOLUMNW column{}; column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.pszText = const_cast<wchar_t*>(columns[index]);
                column.cx = MulDiv(state->preferences.node_column_widths[index],
                    static_cast<int>(window_dpi(window)), 96);
                ListView_InsertColumn(state->node_list, static_cast<int>(index), &column);
            }
            ListView_SetBkColor(state->node_list, kPanelColor);
            ListView_SetTextBkColor(state->node_list, kPanelColor);
            ListView_SetTextColor(state->node_list, kTextColor);
            state->open_node_button = CreateWindowExW(0, L"BUTTON", L"Open Selected",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_OPEN_NODE), state->instance, nullptr);
            state->notice_button=CreateWindowExW(0,L"BUTTON",L"Send System Notice",WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_TABSTOP|BS_OWNERDRAW,0,0,0,0,window,reinterpret_cast<HMENU>(IDC_SEND_NOTICE),state->instance,nullptr);
            state->disable_all_nodes_button = CreateWindowExW(0, L"BUTTON", L"Disable All Nodes",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_DISABLE_ALL_NODES), state->instance, nullptr);
            state->enable_all_nodes_button = CreateWindowExW(0, L"BUTTON", L"Enable All Nodes",
                WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_ENABLE_ALL_NODES), state->instance, nullptr);
            state->bulk_node_status = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_CLIPSIBLINGS | SS_LEFT | SS_NOPREFIX, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_BULK_NODE_STATUS), state->instance, nullptr);
            state->confirm_disable_all_button = CreateWindowExW(0, L"BUTTON", L"Confirm Disable",
                WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_CONFIRM_DISABLE_ALL), state->instance, nullptr);
            state->cancel_disable_all_button = CreateWindowExW(0, L"BUTTON", L"Cancel",
                WS_CHILD | WS_DISABLED | WS_CLIPSIBLINGS | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_CANCEL_DISABLE_ALL), state->instance, nullptr);
            state->modem_strip = CreateWindowExW(0, L"STATIC", L"Node modem indicators",
                WS_CHILD | WS_CLIPSIBLINGS | SS_OWNERDRAW, 0, 0, 0, 0, window,
                nullptr, state->instance, nullptr);
            state->events_view = CreateWindowExW(0, L"EDIT",
                L"Built-in job: Backup\r\n\r\nScheduler: Starting",
                WS_CHILD | WS_CLIPSIBLINGS | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | WS_VSCROLL,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_EVENTS_VIEW), state->instance, nullptr);
            state->rich_edit_module = LoadLibraryW(L"Msftedit.dll");
            const wchar_t* log_class = state->rich_edit_module != nullptr ? MSFTEDIT_CLASS : L"EDIT";
            state->log_view = CreateWindowExW(0, log_class, L"",
                WS_CHILD | WS_CLIPSIBLINGS | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | WS_VSCROLL,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_LOG_VIEW), state->instance, nullptr);
            if (state->rich_edit_module != nullptr)
                SendMessageW(state->log_view, EM_SETBKGNDCOLOR, 0, kPanelColor);
            // 10,000 * (640 message characters + bounded timestamp/level/CRLF), plus notices.
            // This is a separate, explicit presentation bound, never a wire-frame limit.
            SendMessageW(state->log_view, EM_LIMITTEXT,
                app::kOperationalBacklogEntries * (app::kOperationalPresentationMaximum + 128) + 65536, 0);
            state->log_pause_button = CreateWindowExW(0, L"BUTTON", L"Pause",
                WS_CHILD | WS_CLIPSIBLINGS | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_LOG_PAUSE), state->instance, nullptr);
            state->log_clear_button = CreateWindowExW(0, L"BUTTON", L"Clear View",
                WS_CHILD | WS_CLIPSIBLINGS | BS_OWNERDRAW, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_LOG_CLEAR), state->instance, nullptr);
            state->log_filter=CreateWindowExW(0,L"COMBOBOX",L"",WS_CHILD|WS_TABSTOP|CBS_DROPDOWNLIST|WS_VSCROLL,
                0,0,0,0,window,reinterpret_cast<HMENU>(IDC_LOG_FILTER),state->instance,nullptr);
            for (const auto* name:{L"All events",L"Connections",L"Callers",L"BotGate",L"System"})
                SendMessageW(state->log_filter,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(name));
            SendMessageW(state->log_filter,CB_SETCURSEL,0,0);
            state->stats_view = CreateWindowExW(0, kStatisticsClass, L"",
                WS_CHILD | WS_CLIPSIBLINGS,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(IDC_STATS_VIEW), state->instance, state);
            state->botgate_view=CreateWindowExW(0,L"EDIT",L"BotGate status unavailable.",WS_CHILD|WS_TABSTOP|ES_READONLY|ES_MULTILINE|WS_VSCROLL,
                0,0,0,0,window,nullptr,state->instance,nullptr);
            state->terminal_view = CreateWindowExW(0, kTerminalClass, L"",
                WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_TERMINAL_VIEW), state->instance, state);
            state->node_header = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_CLIPSIBLINGS | SS_LEFTNOWORDWRAP, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_NODE_HEADER), state->instance, nullptr);
            state->monitor_view = CreateWindowExW(0, kMonitorClass, L"",
                WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_MONITOR_VIEW), state->instance, state);
            state->status_bar = CreateWindowExW(0, STATUSCLASSNAMEW, L"Starting…",
                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(IDC_MAIN_STATUS), state->instance, nullptr);
            SendMessageW(state->status_bar, SB_SETBKCOLOR, 0, kPanelColor);
            state->list_header = ListView_GetHeader(state->node_list);
            initialize_modem_lights(*state);
            ListView_SetExtendedListViewStyleEx(state->node_list, LVS_EX_GRIDLINES,
                g_appearance.theme() == win32::DesktopTheme::classic ? LVS_EX_GRIDLINES : 0);
            rebuild_ui_fonts(*state, window_dpi(window));
            if (state->list_header != nullptr) {
                state->original_header_proc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(
                    state->list_header, GWLP_WNDPROC,
                    reinterpret_cast<LONG_PTR>(list_header_proc)));
            }
            SendMessageW(state->status_bar, SB_SETTEXTW, SBT_OWNERDRAW,
                reinterpret_cast<LPARAM>(state));
            HMENU menu = CreateMenu();
            HMENU file = CreatePopupMenu();
            AppendMenuW(file, MF_STRING, ID_FILE_EXIT, L"Exit");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file), L"File");
            HMENU bbs = CreatePopupMenu();
            AppendMenuW(bbs, MF_STRING, ID_BBS_CONFIGURE, L"SFX Config");
            AppendMenuW(bbs, MF_STRING, ID_BBS_FILEMAN, L"SFX Fileman");
            AppendMenuW(bbs,MF_STRING,ID_BBS_NOTICE,L"Send System Notice...");
            AppendMenuW(bbs,MF_STRING,ID_BBS_CHAT_AVAILABLE,L"Sysop Available for Chat");
            AppendMenuW(bbs,MF_STRING,ID_BBS_EVENT_SOUNDS,L"Event Sounds...");
            AppendMenuW(bbs, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(bbs, MF_STRING, ID_BBS_RESTART, L"Restart BBS Now");
            if(state->runtime.is_service_mode()) {
                AppendMenuW(bbs,MF_STRING,ID_BBS_SERVICE_START,L"Start BBS (Windows Service)");
                AppendMenuW(bbs,MF_STRING,ID_BBS_SERVICE_STOP,L"Stop BBS (Windows Service)...");
            }
            const HMENU drain = CreatePopupMenu();
            AppendMenuW(drain, MF_STRING, ID_BBS_DRAIN_1, L"Up to 1 minute...");
            AppendMenuW(drain, MF_STRING, ID_BBS_DRAIN_5, L"Up to 5 minutes...");
            AppendMenuW(drain, MF_STRING, ID_BBS_DRAIN_15, L"Up to 15 minutes...");
            AppendMenuW(bbs, MF_POPUP, reinterpret_cast<UINT_PTR>(drain), L"Graceful Shut Down");
            AppendMenuW(bbs, MF_STRING, ID_BBS_CANCEL_DRAIN, L"Cancel Graceful Shut Down");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(bbs), L"BBS");
            HMENU view = CreatePopupMenu();
            AppendMenuW(view, MF_STRING | (state->preferences.system_monitor_visible ? MF_CHECKED : 0),
                ID_VIEW_MONITOR, L"System Monitor");
            HMENU appearance = CreatePopupMenu();
            state->monitor_appearance_menu = appearance;
            AppendMenuW(appearance, MF_STRING, ID_VIEW_MONITOR_COLOR, L"Color");
            AppendMenuW(appearance, MF_STRING, ID_VIEW_MONITOR_GRAYSCALE, L"Grayscale");
            CheckMenuRadioItem(appearance, ID_VIEW_MONITOR_COLOR, ID_VIEW_MONITOR_GRAYSCALE,
                state->preferences.system_monitor_grayscale
                    ? ID_VIEW_MONITOR_GRAYSCALE : ID_VIEW_MONITOR_COLOR, MF_BYCOMMAND);
            AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(appearance),
                L"System Monitor Appearance");
            state->monitor_width_menu = CreatePopupMenu();
            for (const auto choice : {std::make_pair(ID_VIEW_MONITOR_COMPACT,L"Compact"),
                    std::make_pair(ID_VIEW_MONITOR_MEDIUM,L"Medium"),
                    std::make_pair(ID_VIEW_MONITOR_WIDE,L"Wide")})
                AppendMenuW(state->monitor_width_menu, MF_STRING, choice.first, choice.second);
            CheckMenuRadioItem(state->monitor_width_menu, ID_VIEW_MONITOR_COMPACT, ID_VIEW_MONITOR_WIDE,
                state->preferences.system_monitor_width >= 400 ? ID_VIEW_MONITOR_WIDE :
                state->preferences.system_monitor_width >= 320 ? ID_VIEW_MONITOR_MEDIUM : ID_VIEW_MONITOR_COMPACT,
                MF_BYCOMMAND);
            AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(state->monitor_width_menu),
                L"System Monitor Width");
            state->theme_menu = g_appearance.append_menu(view);
            AppendMenuW(view, MF_STRING | (state->preferences.modem_lights ? MF_CHECKED : 0),
                ID_VIEW_MODEM_LIGHTS, L"Modem Lights");
            state->scale_menu = CreatePopupMenu();
            constexpr std::array<const wchar_t*, 5> scale_labels{{L"Auto Fit", L"100%", L"125%", L"150%", L"200%"}};
            for (UINT index = 0; index < scale_labels.size(); ++index) {
                AppendMenuW(state->scale_menu, MF_STRING |
                    (state->node_view_scale == kNodeViewScales[index] ? MF_CHECKED : 0),
                    ID_VIEW_SCALE_AUTO + index, scale_labels[index]);
            }
            AppendMenuW(view, MF_POPUP, reinterpret_cast<UINT_PTR>(state->scale_menu), L"Node View Scale");
            AppendMenuW(view, MF_SEPARATOR, 0, nullptr);
            AppendMenuW(view, MF_STRING, ID_VIEW_FULL_SCREEN, L"Full Screen\tAlt+Enter");
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(view), L"View");
            HMENU node = CreatePopupMenu();
            state->node_menu = node;
            populate_node_menu(node, *state);
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(node), L"Node");
            HMENU help = create_help_menu();
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(help), L"Help");
            SetMenu(window, menu);
            show_selected_page(*state);
            SetTimer(window, kRefreshTimer, 1000, nullptr);
#ifdef SFX_ALPHA_TEST_AUTOMATION
            if (!state->automation.multinode_directory.empty()) SetTimer(window, 19, 100, nullptr);
#endif
            return 0;
        }
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lparam)->ptMinTrackSize = {
                scaled(window, 810), scaled(window, 520)};
            return 0;
        case WM_SIZE:
            if(wparam!=SIZE_MINIMIZED)PostMessageW(window,WM_TIMER,kRefreshTimer,0);
            layout(*state, LOWORD(lparam), HIWORD(lparam));
            return 0;
        case WM_DPICHANGED: {
            rebuild_ui_fonts(*state, LOWORD(wparam));
            if (state->restoring_placement) return 0;
            if (state->full_screen) { (void)full_screen_bounds(*state); return 0; }
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            if (suggested != nullptr) SetWindowPos(window, nullptr, suggested->left, suggested->top,
                suggested->right - suggested->left, suggested->bottom - suggested->top,
                SWP_NOACTIVATE | SWP_NOZORDER);
            return 0;
        }
        case WM_DISPLAYCHANGE:
            if (state->full_screen) { (void)full_screen_bounds(*state); return 0; }
            break;
        case WM_SYSCOMMAND:
            if (state->full_screen && (wparam & 0xfff0U) == SC_KEYMENU) return 0;
            break;
        case WM_INITMENUPOPUP:
            if (reinterpret_cast<HMENU>(wparam) == state->node_menu) {
                populate_node_menu(state->node_menu, *state);
                return 0;
            }
            break;
        case WM_TIMER:
            if (wparam == kStartupActivityTimer) {
                if (state->startup_dot_count)
                    update_startup_dots(*state, state->startup_dot_count % 3 + 1);
                return 0;
            }
            if(state->startup_pending)return 0;
            if (wparam == kLogPageTimer) { refresh_live_logs(*state); return 0; }
#ifdef SFX_ALPHA_TEST_AUTOMATION
            if (wparam == 19) { run_multinode_automation(*state); return 0; }
#endif
            if (wparam == kRefreshTimer) {
                if (!state->runtime.child_alive()) {
                    state->status.state = app::AlphaRuntimePresentationState::faulted;
                    state->status.diagnostic_code = "runtime_child_exited";
                }
                update_status(*state);
                if (state->close_after_drain && state->have_status &&
                    state->status.lifecycle.state == app::RuntimeState::stopped) {
                    (void)close_runtime(*state, false);
                    return 0;
                }
                if (!IsIconic(window) && app::control_panel_monitor_should_poll(panel_page(state->tab),
                        state->preferences.system_monitor_visible) && state->metric_sampler) {
                    sample_visible_monitor(*state);
                    InvalidateRect(state->monitor_view, nullptr, FALSE);
                }
                if (state->displayed_node != 0) InvalidateRect(state->terminal_view, nullptr, FALSE);
#ifdef SFX_ALPHA_TEST_AUTOMATION
            } else if (wparam == kAutomationTimer) {
                KillTimer(window, kAutomationTimer);
                if (!state->automation.owner_review_capture_directory.empty()) {
                    const auto file = [&state](const wchar_t* name) {
                        return state->automation.owner_review_capture_directory / name;
                    };
                    const auto fail = [&state](const int code) {
                        state->automation.exit_code = code;
                        (void)close_runtime(*state, false);
                    };
                    switch (state->automation.suite_step++) {
                        case 0: {
                            if (state->automation.capture_monitor_appearance &&
                                (!monitor_appearance_menu_matches(*state, state->preferences.system_monitor_grayscale) ||
                                 (state->automation.expected_monitor_appearance >= 0 &&
                                  state->preferences.system_monitor_grayscale !=
                                      (state->automation.expected_monitor_appearance == 1)))) {
                                fail(89); return 0;
                            }
                            if (state->automation.expect_persisted_columns &&
                                state->preferences.node_column_widths !=
                                    std::array<std::int32_t, 6>{{90, 150, 170, 120, 150, 220}}) {
                                fail(60); return 0;
                            }
                            SetWindowPos(window, nullptr, 0, 0,
                                scaled(window, 1080), scaled(window, 700),
                                SWP_NOMOVE | SWP_NOZORDER);
                            state->preferences.system_monitor_visible = false;
                            CheckMenuItem(GetMenu(window), ID_VIEW_MONITOR, MF_BYCOMMAND | MF_UNCHECKED);
                            select_panel_page(state->tab, 0); show_selected_page(*state);
                            RECT area{}; GetClientRect(window, &area);
                            layout(*state, area.right, area.bottom);
                            if (!capture_window_bmp(window, file(L"01-nodes-default.bmp"))) {
                                fail(61); return 0;
                            }
                            break;
                        }
                        case 1: {
                            state->preferences.system_monitor_visible = true;
                            CheckMenuItem(GetMenu(window), ID_VIEW_MONITOR, MF_BYCOMMAND | MF_CHECKED);
                            show_selected_page(*state);
                            RECT area{}; GetClientRect(window, &area);
                            layout(*state, area.right, area.bottom);
                            sample_visible_monitor(*state);
                            break;
                        }
                        case 2:
                            update_status(*state);
                            sample_visible_monitor(*state);
                            InvalidateRect(state->monitor_view, nullptr, FALSE);
                            UpdateWindow(state->monitor_view);
                            if (!capture_window_bmp(window, file(L"02-five-card-monitor.bmp"))) {
                                fail(62); return 0;
                            }
                            if (state->automation.capture_monitor_appearance && !capture_monitor_appearances(*state)) {
                                fail(90); return 0;
                            }
                            state->preferences.system_monitor_visible = false;
                            CheckMenuItem(GetMenu(window), ID_VIEW_MONITOR, MF_BYCOMMAND | MF_UNCHECKED);
                            show_selected_page(*state);
                            { RECT area{}; GetClientRect(window, &area);
                              layout(*state, area.right, area.bottom); }
                            ListView_SetItemState(state->node_list, 0,
                                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                            open_selected_node(*state);
                            break;
                        case 3:
                            SetWindowPos(window, nullptr, 0, 0,
                                scaled(window, 1380), scaled(window, 800),
                                SWP_NOMOVE | SWP_NOZORDER);
                            { RECT area{}; GetClientRect(window, &area);
                              layout(*state, area.right, area.bottom); }
                            state->node_view_interaction[1] = GetTickCount64();
                            state->automation.authored_ansi_fixture = false;
                            InvalidateRect(state->terminal_view, nullptr, FALSE);
                            UpdateWindow(state->terminal_view);
                            if (!capture_window_bmp(state->terminal_view,
                                    file(L"03b-node-view-live-application.bmp")) ||
                                !capture_terminal_cells(*state, file(L"03b-node-live.cells")) ||
                                !capture_live_fields(*state, file(L"03b-node-live-fields.tsv"))) {
                                fail(86); return 0;
                            }
                            state->automation.authored_ansi_fixture = true;
                            InvalidateRect(state->terminal_view, nullptr, FALSE);
                            UpdateWindow(state->terminal_view);
                            if (!capture_window_bmp(window, file(L"03-node-view-idle-1380x800.bmp")) ||
                                !capture_window_bmp(state->terminal_view,
                                    file(L"03a-node-view-idle-application.bmp")) ||
                                !capture_terminal_cells(*state, file(L"03a-node-authored.cells"))) {
                                fail(63); return 0;
                            }
                            // Repaint the exact same model repeatedly; neither the
                            // glyph table nor GDI resource count may change.
                            {
                                const auto font_before = *state->terminal_font;
                                const DWORD gdi_before = GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS);
                                for (int iteration = 0; iteration < 64; ++iteration) {
                                    InvalidateRect(state->terminal_view, nullptr, FALSE);
                                    UpdateWindow(state->terminal_view);
                                }
                                if (font_before != *state->terminal_font ||
                                    gdi_before != GetGuiResources(GetCurrentProcess(), GR_GDIOBJECTS)) {
                                    fail(87); return 0;
                                }
                            }
                            break;
                        case 4:
                        case 5:
                        case 6: {
                            const std::size_t color = state->automation.suite_step - 5U;
                            state->node_view_interaction[1] = GetTickCount64() -
                                (181000ULL + color * 30000ULL);
                            InvalidateRect(state->terminal_view, nullptr, FALSE);
                            UpdateWindow(state->terminal_view);
                            const std::array<const wchar_t*, 3> names{{
                                L"04-screen-saver-red.bmp", L"05-screen-saver-white.bmp",
                                L"06-screen-saver-blue.bmp"}};
                            const std::array<const wchar_t*, 3> terminal_names{{
                                L"04a-screen-saver-red-application.bmp",
                                L"05a-screen-saver-white-application.bmp",
                                L"06a-screen-saver-blue-application.bmp"}};
                            if (!capture_window_bmp(window, file(names[color])) ||
                                !capture_window_bmp(state->terminal_view,
                                    file(terminal_names[color])) ||
                                !capture_terminal_cells(*state,
                                    file((std::wstring(terminal_names[color]) + L".cells").c_str()))) {
                                fail(64 + static_cast<int>(color)); return 0;
                            }
                            break;
                        }
                        case 7: {
                            state->automation.authored_ansi_fixture = false;
                            const ULONGLONG before = state->node_view_interaction[1];
                            SendMessageW(state->terminal_view, WM_KEYDOWN, 'A', 0);
                            if (state->node_view_interaction[1] != before) {
                                fail(73); return 0;
                            }
                            SendMessageW(state->terminal_view, WM_KEYDOWN, VK_HOME, 0);
                            const auto* node = node_status(*state, 1);
                            if (state->node_view_interaction[1] == before || node == nullptr ||
                                screen_saver_active(*state, *node, GetTickCount64())) {
                                fail(74); return 0;
                            }
                            SetWindowPos(window, nullptr, 0, 0,
                                scaled(window, 820), scaled(window, 560),
                                SWP_NOMOVE | SWP_NOZORDER);
                            if (!capture_window_bmp(window, file(L"07-node-view-idle-820x560.bmp"))) {
                                fail(67); return 0;
                            }
                            break;
                        }
                        case 8:
                            select_panel_page(state->tab, 3); show_selected_page(*state);
                            if (!capture_window_bmp(window, file(L"08-statistics-820x560.bmp"))) {
                                fail(68); return 0;
                            }
                            break;
                        case 9:
                            SetWindowPos(window, nullptr, 0, 0,
                                scaled(window, 1200), scaled(window, 760),
                                SWP_NOMOVE | SWP_NOZORDER);
                            if (!capture_window_bmp(window, file(L"09-statistics-1200x760.bmp"))) {
                                fail(69); return 0;
                            }
                            break;
                        case 10:
                            select_panel_page(state->tab, 2); show_selected_page(*state);
                            if (!capture_window_bmp(window, file(L"10-live-logs-concise.bmp"))) {
                                fail(70); return 0;
                            }
                            break;
                        case 11: {
                            constexpr std::array<std::int32_t, 6> custom{{90, 150, 170, 120, 150, 220}};
                            select_panel_page(state->tab, 0); show_selected_page(*state);
                            for (int column = 0; column < 6; ++column) {
                                ListView_SetColumnWidth(state->node_list, column,
                                    MulDiv(custom[static_cast<std::size_t>(column)],
                                        static_cast<int>(window_dpi(window)), 96));
                                NMHEADERW notification{};
                                notification.hdr.hwndFrom = state->list_header;
                                notification.hdr.code = HDN_ENDTRACKW;
                                notification.iItem = column;
                                SendMessageW(window, WM_NOTIFY, 0,
                                    reinterpret_cast<LPARAM>(&notification));
                            }
                            if (state->preferences.node_column_widths != custom) {
                                fail(75); return 0;
                            }
                            close_node_view_at(*state, 4);
                            ListView_SetItemState(state->node_list, 0,
                                LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                            open_selected_node(*state);
                            select_panel_page(state->tab, 0); show_selected_page(*state);
                            state->preferences.system_monitor_visible = true;
                            RECT area{}; GetClientRect(window, &area);
                            layout(*state, area.right, area.bottom);
                            select_panel_page(state->tab, 1); show_selected_page(*state);
                            select_panel_page(state->tab, 0); show_selected_page(*state);
                            state->preferences.system_monitor_visible = false;
                            layout(*state, area.right, area.bottom);
                            const int expected_status = MulDiv(150,
                                static_cast<int>(window_dpi(window)), 96);
                            if (ListView_GetColumnWidth(state->node_list, 1) != expected_status ||
                                !capture_window_bmp(window, file(L"11-nodes-custom-columns.bmp"))) {
                                fail(71); return 0;
                            }
                            break;
                        }
                        case 12: {
                            select_panel_page(state->tab, 2);
                            show_selected_page(*state);
                            const std::wstring before = window_text(state->log_view);
                            SetFocus(state->log_view);
                            SetCapture(state->log_view);
                            select_panel_page(state->tab, 0);
                            show_selected_page(*state);
                            if (GetCapture() == state->log_view ||
                                GetFocus() == state->log_view ||
                                IsWindowVisible(state->log_view) ||
                                IsWindowEnabled(state->log_view)) {
                                fail(76); return 0;
                            }
                            const auto disabled = state->runtime.command_node(
                                {1, app::AlphaNodeCommandKind::disable});
                            const auto enabled = state->runtime.command_node(
                                {1, app::AlphaNodeCommandKind::enable});
                            update_status(*state);
                            if (disabled.code != app::LocalControlResponseCode::ok ||
                                enabled.code != app::LocalControlResponseCode::ok ||
                                window_text(state->log_view) != before ||
                                !state->runtime.child_alive()) {
                                fail(77); return 0;
                            }
                            select_panel_page(state->tab, 2);
                            show_selected_page(*state);
                            if (window_text(state->log_view) == before ||
                                !IsWindowVisible(state->log_view) ||
                                !IsWindowEnabled(state->log_view)) {
                                fail(78); return 0;
                            }
                            break;
                        }
                        case 13:
                            state->automation.about_capture_path =
                                file(L"13-about-dialog.bmp");
                            state->automation.about_close_method = 1U;
                            show_about_dialog(*state);
                            if (state->automation.exit_code != 0 ||
                                !state->automation.about_capture_path.empty() ||
                                state->automation.about_close_method != 0U ||
                                !state->runtime.child_alive()) {
                                fail(79); return 0;
                            }
                            break;
                        case 14:
                        case 15:
                        case 16:
                            state->automation.about_close_method =
                                state->automation.suite_step - 13U;
                            show_about_dialog(*state);
                            if (state->automation.about_close_method != 0U ||
                                !state->runtime.child_alive()) {
                                fail(80 + static_cast<int>(state->automation.suite_step));
                                return 0;
                            }
                            break;
                        case 17:
                            if (!state->runtime.child_alive()) { fail(72); return 0; }
                            (void)close_runtime(*state, false);
                            return 0;
                    }
                    SetTimer(window, kAutomationTimer, 1050, nullptr);
                    return 0;
                }
                if (!state->automation.correction_capture_directory.empty()) {
                    const auto file = [&state](const wchar_t* name) {
                        return state->automation.correction_capture_directory / name;
                    };
                    const auto fail = [&state](const int code) {
                        state->automation.exit_code = code;
                        (void)close_runtime(*state, false);
                    };
                    switch (state->automation.suite_step++) {
                        case 0:
                            select_panel_page(state->tab, 0);
                            show_selected_page(*state);
                            InvalidateRect(state->tab, nullptr, TRUE);
                            if (!capture_window_bmp(window, file(L"01-nodes-dark-retro.bmp"))) {
                                fail(30);
                                return 0;
                            }
                            break;
                        case 1:
                            ListView_SetItemState(state->node_list, -1, 0,
                                LVIS_SELECTED | LVIS_FOCUSED);
                            ListView_SetItemState(state->node_list, 0,
                                LVIS_SELECTED | LVIS_FOCUSED,
                                LVIS_SELECTED | LVIS_FOCUSED);
                            open_selected_node(*state);
                            ListView_SetItemState(state->node_list, -1, 0,
                                LVIS_SELECTED | LVIS_FOCUSED);
                            ListView_SetItemState(state->node_list, 1,
                                LVIS_SELECTED | LVIS_FOCUSED,
                                LVIS_SELECTED | LVIS_FOCUSED);
                            open_selected_node(*state);
                            if (state->open_node_views.size() != 2 ||
                                TabCtrl_GetItemCount(state->tab) != 7 ||
                                !capture_window_bmp(window,
                                    file(L"02-two-node-tabs-unavailable.bmp"))) {
                                fail(31);
                                return 0;
                            }
                            break;
                        case 2: {
                            RECT tab{};
                            if (!TabCtrl_GetItemRect(state->tab, 6, &tab)) {
                                fail(32);
                                return 0;
                            }
                            const int x = tab.right - 12;
                            const int y = (tab.top + tab.bottom) / 2;
                            SendMessageW(state->tab, WM_LBUTTONDOWN, MK_LBUTTON,
                                MAKELPARAM(x, y));
                            if (state->open_node_views.size() != 1 ||
                                TabCtrl_GetItemCount(state->tab) != 6 ||
                                panel_page(state->tab) != 5 ||
                                !state->runtime.child_alive() ||
                                !capture_window_bmp(window,
                                    file(L"03-idle-node-after-close.bmp"))) {
                                fail(33);
                                return 0;
                            }
                            break;
                        }
                        case 3: {
                            RECT tab{};
                            if (!TabCtrl_GetItemRect(state->tab, 5, &tab)) {
                                fail(34);
                                return 0;
                            }
                            const int x = tab.right - 12;
                            const int y = (tab.top + tab.bottom) / 2;
                            SendMessageW(state->tab, WM_LBUTTONDOWN, MK_LBUTTON,
                                MAKELPARAM(x, y));
                            bool primary_close_target = false;
                            for (int index = 0; index < 5; ++index) {
                                RECT primary{};
                                if (!TabCtrl_GetItemRect(state->tab, index, &primary)) {
                                    primary_close_target = true;
                                    break;
                                }
                                const POINT point{primary.right - 12,
                                    (primary.top + primary.bottom) / 2};
                                int hit = -1;
                                if (tab_close_hit_test(state->tab, point, hit)) {
                                    primary_close_target = true;
                                    break;
                                }
                            }
                            if (state->open_node_views.size() != 0 ||
                                TabCtrl_GetItemCount(state->tab) != 5 ||
                                primary_close_target || !state->runtime.child_alive() ||
                                !capture_window_bmp(window,
                                    file(L"04-primary-tabs-after-close.bmp"))) {
                                fail(35);
                                return 0;
                            }
                            break;
                        }
                        case 4:
                            (void)close_runtime(*state, false);
                            return 0;
                    }
                    SetTimer(window, kAutomationTimer, 650, nullptr);
                    return 0;
                }
                if (!state->automation.capture_suite_directory.empty()) {
                    const auto file = [&state](const wchar_t* name) {
                        return state->automation.capture_suite_directory / name;
                    };
                    switch (state->automation.suite_step++) {
                        case 0:
                            select_panel_page(state->tab, 0); show_selected_page(*state);
                            (void)capture_window_bmp(window, file(L"01-nodes.bmp"));
                            state->preferences.system_monitor_visible = true;
                            CheckMenuItem(GetMenu(window), ID_VIEW_MONITOR,
                                MF_BYCOMMAND | MF_CHECKED);
                            show_selected_page(*state);
                            { RECT area{}; GetClientRect(window, &area);
                              layout(*state, area.right, area.bottom); }
                            break;
                        case 1:
                            sample_visible_monitor(*state);
                            InvalidateRect(state->monitor_view, nullptr, FALSE);
                            UpdateWindow(state->monitor_view);
                            (void)capture_window_bmp(window, file(L"02-system-monitor.bmp"));
                            select_panel_page(state->tab, 1); show_selected_page(*state);
                            break;
                        case 2:
                            (void)capture_window_bmp(window, file(L"03-events.bmp"));
                            select_panel_page(state->tab, 2); show_selected_page(*state);
                            break;
                        case 3:
                            (void)capture_window_bmp(window, file(L"04-live-logs.bmp"));
                            select_panel_page(state->tab, 3); show_selected_page(*state);
                            break;
                        case 4:
                            (void)capture_window_bmp(window, file(L"05-statistics.bmp"));
                            select_panel_page(state->tab, 0); show_selected_page(*state);
                            ListView_SetItemState(state->node_list, 0, LVIS_SELECTED | LVIS_FOCUSED,
                                LVIS_SELECTED | LVIS_FOCUSED);
                            open_selected_node(*state);
                            break;
                        case 5:
                            (void)capture_window_bmp(window, file(L"06-node-view.bmp"));
                            state->node_view_interaction[1] = GetTickCount64() - 181000;
                            InvalidateRect(state->terminal_view, nullptr, FALSE);
                            break;
                        case 6:
                            (void)capture_window_bmp(window, file(L"07-screen-saver.bmp"));
                            SetWindowPos(window, nullptr, 0, 0, 820, 560,
                                SWP_NOMOVE | SWP_NOZORDER);
                            break;
                        case 7:
                            (void)capture_window_bmp(window, file(L"08-resized-node-view.bmp"));
                            break;
                        case 8:
                            (void)close_runtime(*state, false);
                            return 0;
                    }
                    SetTimer(window, kAutomationTimer, 650, nullptr);
                    return 0;
                }
                if (!state->automation.capture_path.empty())
                    (void)capture_window_bmp(window, state->automation.capture_path);
                if (state->automation.auto_close) (void)close_runtime(*state, false);
#endif
            } else if (wparam == kPulseTimer) {
                if (panel_page(state->tab) == 0)
                    InvalidateRect(state->node_list, nullptr, FALSE);
                if (state->preferences.modem_lights && state->displayed_node)
                    InvalidateRect(state->modem_strip, nullptr, FALSE);
            }
            return 0;
        case kConfigurationAdminMessage: {
            const auto exchange = state->configuration_admin.begin(wparam);
            if (!exchange) return 0;
            struct CompleteExchange {
                std::shared_ptr<ConfigurationAdminExchange> value;
                ~CompleteExchange() { value->finish(); }
            } completion{exchange};
            const auto respond = [&](const app::LocalControlMessageType type,
                                     std::vector<std::uint8_t> payload) {
                exchange->response.type = type;
                exchange->response.request_id = exchange->request.request_id;
                exchange->response.payload = std::move(payload);
                exchange->handled = !exchange->response.payload.empty();
            };
            if (exchange->request.type == app::LocalControlMessageType::throttle_clear_request) {
                const auto command = app::decode_throttle_clear_request(exchange->request.payload);
                const auto result = command ? state->runtime.clear_throttle(*command) :
                    app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"throttle_clear_invalid"};
                respond(app::LocalControlMessageType::throttle_clear_response,app::encode_control_response(result));
            } else if(exchange->request.type==app::LocalControlMessageType::message_maintenance_query_request){
                const auto kind=app::decode_message_maintenance_kind(exchange->request.payload);
                const auto result=kind?state->runtime.message_maintenance_status(*kind):app::MessageMaintenanceStatus{};
                respond(app::LocalControlMessageType::message_maintenance_query_response,app::encode_message_maintenance_status(result));
            } else if(exchange->request.type==app::LocalControlMessageType::message_maintenance_schedule_request){
                const auto settings=app::decode_message_maintenance_schedule(exchange->request.payload);
                const auto result=settings?state->runtime.configure_message_schedule(*settings):app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"message_schedule_invalid"};
                respond(app::LocalControlMessageType::message_maintenance_schedule_response,app::encode_control_response(result));
            } else if(exchange->request.type==app::LocalControlMessageType::message_maintenance_run_request){
                const auto kind=app::decode_message_maintenance_kind(exchange->request.payload);
                const auto result=kind?state->runtime.run_message_maintenance_now(*kind):app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"message_job_unknown"};
                respond(app::LocalControlMessageType::message_maintenance_run_response,app::encode_control_response(result));
            } else if(exchange->request.type==app::LocalControlMessageType::qwk_policy_query_request){
                const auto result=exchange->request.payload.empty()?state->runtime.qwk_policy_snapshot():app::LocalQwkPolicyResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::qwk_policy_query_response,app::encode_qwk_policy_result(result));
            } else if(exchange->request.type==app::LocalControlMessageType::qwk_policy_change_request){
                const auto change=app::decode_qwk_policy_change(exchange->request.payload);
                const auto result=change?state->runtime.change_qwk_policy(*change):app::LocalQwkPolicyResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::qwk_policy_change_response,app::encode_qwk_policy_result(result));
            } else if(exchange->request.type==app::LocalControlMessageType::file_management_query_request){
                const auto query=app::decode_file_management_read_request(exchange->request.payload);const auto result=query?state->runtime.query_file_management(*query):app::FileManagementReadSnapshot{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::file_management_query_response,app::encode_file_management_read_snapshot(result));
            } else if(app::file_management_control_request(exchange->request.type)){
                const auto result=state->runtime.file_management(exchange->request.type,exchange->request.payload);
                const auto response_type=exchange->request.type==app::LocalControlMessageType::file_management_start_request?app::LocalControlMessageType::file_management_start_response:exchange->request.type==app::LocalControlMessageType::file_management_status_request?app::LocalControlMessageType::file_management_status_response:app::LocalControlMessageType::file_management_cancel_response;
                respond(response_type,app::encode_file_management_status(result));
            } else if(exchange->request.type==app::LocalControlMessageType::clamav_manual_notice_request){
                const auto notice=app::decode_manual_clamav_notice(exchange->request.payload);
                const auto result=notice?state->runtime.manual_clamav_notice(*notice):app::AlphaControlResponse{app::LocalControlResponseCode::invalid_request,"clamav_notice_invalid"};
                respond(app::LocalControlMessageType::clamav_manual_notice_response,app::encode_control_response(result));
            } else if(exchange->request.type==app::LocalControlMessageType::ftn_hatch_catalog_request){
                const auto query=app::decode_ftn_configuration_request(exchange->request.payload);const auto result=query?state->runtime.ftn_hatch_catalog(*query):app::FtnConfigurationResponse{app::FtnConfigurationCode::invalid};respond(app::LocalControlMessageType::ftn_hatch_catalog_response,app::encode_ftn_configuration_response(result));
            } else if(exchange->request.type==app::LocalControlMessageType::ftn_held_request){
                const auto query=app::decode_ftn_held_request(exchange->request.payload);auto result=query?state->runtime.ftn_held(*query):app::FtnHeldControlResponse{};if(!query)result.code=app::FtnConfigurationCode::invalid;respond(app::LocalControlMessageType::ftn_held_response,app::encode_ftn_held_response(result));
            } else if(exchange->request.type==app::LocalControlMessageType::ftn_configuration_request){
                const auto query=app::decode_ftn_configuration_request(exchange->request.payload);
                const auto result=query?state->runtime.ftn_configuration(*query):app::FtnConfigurationResponse{app::FtnConfigurationCode::invalid};
                respond(app::LocalControlMessageType::ftn_configuration_response,app::encode_ftn_configuration_response(result));
            } else if(exchange->request.type==app::LocalControlMessageType::chat_configuration_query_request){
                const auto result=exchange->request.payload.empty()?state->runtime.chat_configuration():app::ChatConfigurationResult{app::ChatResult::invalid};
                respond(app::LocalControlMessageType::chat_configuration_query_response,app::encode_chat_result(result));
            } else if(exchange->request.type==app::LocalControlMessageType::chat_configuration_change_request){
                const auto value=app::decode_chat_configuration(exchange->request.payload);
                const auto result=value?state->runtime.change_chat_configuration(*value):app::ChatConfigurationResult{app::ChatResult::invalid};
                respond(app::LocalControlMessageType::chat_configuration_change_response,app::encode_chat_result(result));
            } else if(exchange->request.type==app::LocalControlMessageType::door_configuration_query_request){
                const auto query=app::decode_door_configuration_query(exchange->request.payload);
                const auto result=query?state->runtime.door_configuration_snapshot(*query):app::LocalDoorSnapshot{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::door_configuration_query_response,app::encode_door_configuration_snapshot(result));
            } else if(exchange->request.type==app::LocalControlMessageType::door_configuration_change_request){
                const auto change=app::decode_door_configuration_change(exchange->request.payload);
                const auto result=change?state->runtime.change_door_configuration(*change):app::LocalDoorResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::door_configuration_change_response,app::encode_door_configuration_result(result));
            } else if(exchange->request.type==app::LocalControlMessageType::door_bulletin_query_request){
                const auto query=app::decode_door_bulletin_query(exchange->request.payload);
                const auto result=query?state->runtime.door_bulletin_snapshot(*query):app::LocalDoorSnapshot{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::door_bulletin_query_response,app::encode_door_configuration_snapshot(result));
            } else if(exchange->request.type==app::LocalControlMessageType::door_bulletin_change_request){
                const auto change=app::decode_door_bulletin_change(exchange->request.payload);
                const auto result=change?state->runtime.change_door_bulletin(*change):app::LocalDoorResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::door_bulletin_change_response,app::encode_door_configuration_result(result));
            } else if(exchange->request.type==app::LocalControlMessageType::file_configuration_query_request){
                const auto query=app::decode_file_configuration_query(exchange->request.payload);
                const auto result=query?state->runtime.file_configuration_snapshot(*query):app::LocalFileConfigurationSnapshot{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::file_configuration_query_response,app::encode_file_configuration_snapshot(result));
            } else if(exchange->request.type==app::LocalControlMessageType::file_configuration_change_request){
                const auto change=app::decode_file_configuration_change(exchange->request.payload);
                const auto result=change?state->runtime.change_file_configuration(*change):app::LocalFileConfigurationResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::file_configuration_change_response,app::encode_file_configuration_result(result));
            } else if (exchange->request.type == app::LocalControlMessageType::message_configuration_query_request) {
                const auto query=app::decode_message_configuration_query(exchange->request.payload);
                const auto result=query?state->runtime.message_configuration_snapshot(*query):app::LocalMessageConfigurationSnapshot{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::message_configuration_query_response,app::encode_message_configuration_snapshot(result));
            } else if (exchange->request.type == app::LocalControlMessageType::message_configuration_change_request) {
                const auto change=app::decode_message_configuration_change(exchange->request.payload);
                const auto result=change?state->runtime.change_message_configuration(*change):app::LocalMessageConfigurationResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::message_configuration_change_response,app::encode_message_configuration_result(result));
            } else if (exchange->request.type == app::LocalControlMessageType::classic_information_query_request) {
                const auto query=app::decode_classic_information_query(exchange->request.payload);
                const auto result=query?state->runtime.classic_information_snapshot(*query):app::LocalClassicInformationSnapshot{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::classic_information_query_response,app::encode_classic_information_snapshot(result));
            } else if (exchange->request.type == app::LocalControlMessageType::classic_information_change_request) {
                const auto change=app::decode_classic_information_change(exchange->request.payload);
                const auto result=change?state->runtime.change_classic_information(*change):app::LocalAccountResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::classic_information_change_response,app::encode_account_result(result));
            } else if (exchange->request.type == app::LocalControlMessageType::security_query_request) {
                const auto query=app::decode_account_query(exchange->request.payload);
                const auto result=query && !query->account_id && query->name_prefix.empty() ? state->runtime.security_snapshot(*query) : app::LocalSecuritySnapshot{};
                respond(app::LocalControlMessageType::security_query_response,app::encode_security_snapshot(result));
            } else if (exchange->request.type == app::LocalControlMessageType::security_change_request) {
                const auto change=app::decode_security_change(exchange->request.payload);
                const auto result=change ? state->runtime.change_security(*change) : app::LocalAccountResult{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::security_change_response,app::encode_account_result(result));
            } else if (exchange->request.type == app::LocalControlMessageType::account_query_request) {
                const auto query = app::decode_account_query(exchange->request.payload);
                const auto result = query ? state->runtime.account_snapshot(*query) :
                    app::LocalAccountSnapshot{app::LocalAccountResultCode::invalid_request};
                respond(app::LocalControlMessageType::account_query_response,
                    app::encode_account_snapshot(result));
            } else if (exchange->request.type == app::LocalControlMessageType::account_command_request) {
                auto command = app::decode_account_request(exchange->request.payload);
                if (!exchange->request.payload.empty()) SecureZeroMemory(
                    exchange->request.payload.data(), exchange->request.payload.size());
                exchange->request.payload.clear();
                const auto result = command ? state->runtime.administer_account(*command) :
                    app::LocalAccountResult{app::LocalAccountResultCode::invalid_request};
                if (command) app::erase_local_account_secrets(*command);
                respond(app::LocalControlMessageType::account_command_response,
                    app::encode_account_result(result));
                update_status(*state);
            } else if (exchange->request.type==app::LocalControlMessageType::botgate_feed_status_request && exchange->request.payload.empty()) {
                if (const auto snapshot=state->runtime.botgate_feed_status())
                    respond(app::LocalControlMessageType::botgate_feed_status_response,app::encode_botgate_feed_status(*snapshot));
            } else if (exchange->request.type==app::LocalControlMessageType::botgate_feed_update_request && exchange->request.payload.empty()) {
                respond(app::LocalControlMessageType::botgate_feed_update_response,app::encode_control_response(state->runtime.update_botgate_feed()));
            } else if (exchange->request.type ==
                    app::LocalControlMessageType::configuration_snapshot_request &&
                exchange->request.payload.empty()) {
                if (const auto snapshot = state->runtime.configuration_snapshot())
                    respond(app::LocalControlMessageType::configuration_snapshot_response,
                        app::encode_configuration_snapshot(*snapshot));
            } else if (exchange->request.type ==
                    app::LocalControlMessageType::configuration_update_request) {
                const auto request = app::decode_configuration_update_request(
                    exchange->request.payload);
                if (request) {
                    const auto result = state->runtime.update_configuration(*request);
                    respond(app::LocalControlMessageType::configuration_update_response,
                        app::encode_configuration_update_result(result));
                    update_status(*state);
                }
            } else if (exchange->request.type == app::LocalControlMessageType::status_request) {
                const auto query = app::decode_status_request(exchange->request.payload);
                if (query && query->screen_node == 0) {
                    bool captured = true;
                    if (query->snapshot_sequence == 0) {
                        const auto status = state->runtime.status();
                        captured = status && state->configuration_status_cache.capture(*status, 0);
                    }
                    if (captured) respond(app::LocalControlMessageType::status_response,
                        state->configuration_status_cache.chunk(*query));
                }
            } else if (exchange->request.type ==
                    app::LocalControlMessageType::scheduler_settings_request) {
                const auto request = app::decode_scheduler_settings_request(
                    exchange->request.payload);
                if (request) {
                    const auto result = state->runtime.configure_scheduler(*request);
                    respond(app::LocalControlMessageType::scheduler_settings_response,
                        app::encode_control_response(result));
                    update_status(*state);
                }
            } else if (exchange->request.type ==
                    app::LocalControlMessageType::scheduler_run_request) {
                const auto request = app::decode_scheduler_run_request(
                    exchange->request.payload);
                if (request) {
                    const auto result = state->runtime.run_scheduler_now();
                    respond(app::LocalControlMessageType::scheduler_run_response,
                        app::encode_control_response(result));
                    update_status(*state);
                }
            }
            return 0;
        }
        case kOpenSetupMessage:
            if (state->have_status &&
                (state->status.state == app::AlphaRuntimePresentationState::setup_required ||
                 state->status.state == app::AlphaRuntimePresentationState::recovery_required))
                run_setup_dialog(*state);
#ifdef SFX_ALPHA_TEST_AUTOMATION
            if (!state->automation.capture_path.empty() || state->automation.auto_close ||
                !state->automation.capture_suite_directory.empty() ||
                !state->automation.correction_capture_directory.empty() ||
                !state->automation.owner_review_capture_directory.empty())
                SetTimer(window, kAutomationTimer,
                    state->automation.completion_delay_millis, nullptr);
#endif
            return 0;
        case kCloseNodeTabMessage:
            close_node_view_at(*state, static_cast<int>(wparam));
            return 0;
        case kServiceButtonMessage:
            if(state->service_action_busy)return 0;
            update_status(*state);
            if(wparam==state->service_button_epoch&&static_cast<UINT>(lparam)==state->service_button_command&&state->service_button_command)
                SendMessageW(window,WM_COMMAND,state->service_button_command,0);
            return 0;
        case WM_COMMAND:
            if(state->service_action_busy)return 0;
            switch (LOWORD(wparam)) {
                case IDC_SERVICE_BUTTON:
                    if(HIWORD(wparam)==BN_CLICKED&&reinterpret_cast<HWND>(lparam)==state->service_button&&
                        state->service_button_command&&IsWindowEnabled(state->service_button)&&
                        (InSendMessage()||static_cast<LONG>(static_cast<DWORD>(GetMessageTime())-state->service_button_valid_after)>=0))
                        PostMessageW(window,kServiceButtonMessage,state->service_button_epoch,state->service_button_command);
                    return 0;
                case IDC_MAIN_SETUP: run_setup_dialog(*state); return 0;
                case IDC_SEND_NOTICE:case ID_BBS_NOTICE:open_notice_dialog(*state);return 0;
                case IDC_DISABLE_ALL_NODES:
                case IDC_ENABLE_ALL_NODES: {
                    const bool enable = LOWORD(wparam) == IDC_ENABLE_ALL_NODES;
                    const auto button = enable ? state->enable_all_nodes_button : state->disable_all_nodes_button;
                    if (HIWORD(wparam) == BN_CLICKED && reinterpret_cast<HWND>(lparam) == button && IsWindowEnabled(button))
                        set_all_nodes(*state, enable);
                    return 0;
                }
                case IDC_CONFIRM_DISABLE_ALL:
                case IDC_CANCEL_DISABLE_ALL: {
                    const bool confirm = LOWORD(wparam) == IDC_CONFIRM_DISABLE_ALL;
                    const auto button = confirm ? state->confirm_disable_all_button : state->cancel_disable_all_button;
                    if (HIWORD(wparam) == BN_CLICKED && reinterpret_cast<HWND>(lparam) == button && IsWindowEnabled(button)) {
                        if (confirm) confirm_bulk_disable(*state);
                        else cancel_bulk_disable(*state);
                    }
                    return 0;
                }
                case ID_BBS_CHAT_AVAILABLE:{
                    auto current=state->runtime.chat_operator({});
                    if(current.code==app::ChatResult::ok){app::ChatOperatorRequest request;request.action=app::ChatOperatorAction::availability;request.available=!current.available;state->chat_status=state->runtime.chat_operator(request);update_status(*state);}
                    return 0;
                }
                case ID_NODE_ANSWER_PAGE:case ID_NODE_CHAT:{
                    const auto targets=node_command_targets(*state);
                    if(targets.size()==1&&state->menu_edit_account){const auto* node=node_status(*state,targets.front());
                        if(same_edit_caller(*state,node))open_operator_chat(*state,state->menu_edit_assignment,LOWORD(wparam)==ID_NODE_ANSWER_PAGE);}
                    return 0;
                }
                case ID_NODE_NOTICE:{const auto targets=node_command_targets(*state);if(targets.size()==1&&state->menu_edit_account){const auto* node=node_status(*state,targets.front());if(same_edit_caller(*state,node))open_notice_dialog(*state,state->menu_edit_assignment,L"Node "+std::to_wstring(targets.front())+L": "+utf8_to_wide(node->caller_handle));}return 0;}
                case IDC_OPEN_NODE:
                case ID_NODE_OPEN:
                    if (state->displayed_node) open_node_view(*state, state->displayed_node);
                    else open_selected_node(*state);
                    return 0;
                case ID_NODE_LOGON: {
                    const auto targets = node_command_targets(*state);
                    if (targets.size() == 1U) logon_to_node(*state, targets.front());
                    return 0;
                }
                case ID_NODE_EDIT_USER: {
                    const auto targets=node_command_targets(*state);
                    if(targets.size()!=1 || !state->menu_edit_account) return 0;
                    update_status(*state);
                    if(same_edit_caller(*state,node_status(*state,targets.front())))
                        launch_configuration(*state,false,state->menu_edit_account);
                    else MessageBoxW(window,L"That caller is no longer on this node. Select the caller again.",L"Edit User",MB_OK|MB_ICONINFORMATION);
                    return 0;
                }
                case IDC_CLOSE_NODE: close_selected_node_view(*state); return 0;
                case ID_BBS_CONFIGURE: launch_configuration(*state); return 0;
                case BinkPConsole::Configure:launch_configuration(*state,false,0,false,true);return 0;
                case BinkPConsole::Sessions:state->binkp.select_session();return 0;
                case BinkPConsole::Scan:{app::FtnRuntimeRequest request;request.action=app::FtnRuntimeAction::scan;const auto result=state->runtime.ftn_runtime(request);state->binkp.apply(result);MessageBoxW(window,result.code==app::FtnConfigurationCode::ok?L"Outbound scan requested. Queued mail still needs a successful session and peer acknowledgement.":L"Outbound scan is unavailable. Check FTN status and settings.",L"Scan Outbound",MB_OK|MB_ICONINFORMATION);return 0;}
                case BinkPConsole::Activity:state->binkp.show_activity();return 0;
                case BinkPConsole::Poll:state->binkp.poll_now([&](const app::FtnRuntimeRequest& request){return state->runtime.ftn_runtime(request);});return 0;
                case BinkPConsole::Queues:{app::FtnRuntimeRequest query;query.action=app::FtnRuntimeAction::queues;query.offset=state->binkp.queue_offset;const auto result=state->runtime.ftn_runtime(query);state->binkp.show_queues(result);return 0;}
                case ID_BBS_FILEMAN: launch_configuration(*state,false,0,true); return 0;
                case ID_BBS_EVENT_SOUNDS: open_event_sounds(*state); return 0;
                case ID_BBS_RESTART: restart_runtime(*state); return 0;
                case ID_BBS_SERVICE_START:case ID_BBS_SERVICE_STOP:
                    service_command(*state,LOWORD(wparam));return 0;
                case ID_FILE_EXIT: (void)close_runtime(*state, true); return 0;
                case ID_BBS_DRAIN_1:
                case ID_BBS_DRAIN_5:
                case ID_BBS_DRAIN_15:
                case ID_BBS_CANCEL_DRAIN: {
                    const bool cancel = LOWORD(wparam) == ID_BBS_CANCEL_DRAIN;
                    const std::uint32_t seconds = cancel ? 0U : LOWORD(wparam) == ID_BBS_DRAIN_1
                        ? 60U : LOWORD(wparam) == ID_BBS_DRAIN_5 ? 300U : 900U;
                    if (!cancel && !task_choice(window, L"Spitfire-X BBS", L"Graceful Shut Down?",
                        (L"New callers and new scheduled jobs will be refused. Existing work may finish for up to " +
                            std::to_wstring(seconds / 60) + L" minute(s). At the deadline, remaining callers "
                            L"will be disconnected and Runtime will shut down. You can cancel graceful shutdown or "
                            L"use File > Exit to shut down now.").c_str(), L"Graceful Shut Down", 100)) return 0;
                    const auto result = state->runtime.command_lifecycle({cancel
                        ? app::AlphaLifecycleCommandKind::cancel_drain
                        : app::AlphaLifecycleCommandKind::begin_drain, seconds});
                    if (result.code == app::LocalControlResponseCode::ok) state->close_after_drain = !cancel;
                    update_status(*state);
                    if (result.code != app::LocalControlResponseCode::ok)
                        MessageBoxW(window, utf8_to_wide(result.diagnostic_code).c_str(),
                            L"Graceful Shut Down result", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                case ID_HELP_ABOUT: show_about_dialog(*state); return 0;
                case ID_HELP_WIKI: open_sysop_wiki(window); return 0;
                case ID_VIEW_FULL_SCREEN:
                    request_full_screen(*state);
                    return 0;
                case ID_VIEW_SCALE_AUTO: case ID_VIEW_SCALE_100: case ID_VIEW_SCALE_125:
                case ID_VIEW_SCALE_150: case ID_VIEW_SCALE_200: {
                    const auto next = kNodeViewScales[LOWORD(wparam) - ID_VIEW_SCALE_AUTO];
                    if (!save_node_view_scale(next)) {
                        MessageBoxW(window, L"The Node View Scale could not be saved for this Windows user. The current scale is unchanged.",
                            L"Spitfire-X BBS", MB_OK | MB_ICONWARNING);
                        return 0;
                    }
                    state->node_view_scale = next;
                    CheckMenuRadioItem(state->scale_menu, ID_VIEW_SCALE_AUTO, ID_VIEW_SCALE_200,
                        LOWORD(wparam), MF_BYCOMMAND);
                    InvalidateRect(state->terminal_view, nullptr, FALSE);
                    return 0;
                }
                case win32::DesktopAppearance::classic_command:
                case win32::DesktopAppearance::dark_command: {
                    const auto previous = g_appearance.palette();
                    const auto theme = LOWORD(wparam) == win32::DesktopAppearance::classic_command
                        ? win32::DesktopTheme::classic : win32::DesktopTheme::dark_retro;
                    if (g_appearance.save(theme)) apply_desktop_theme(*state, previous);
                    else MessageBoxW(window, L"The theme could not be saved. The previous theme remains selected.",
                        L"Spitfire-X BBS", MB_OK | MB_ICONWARNING);
                    return 0;
                }
                case ID_VIEW_MONITOR_COLOR:
                case ID_VIEW_MONITOR_GRAYSCALE:
                    state->preferences.system_monitor_grayscale =
                        LOWORD(wparam) == ID_VIEW_MONITOR_GRAYSCALE;
                    CheckMenuRadioItem(state->monitor_appearance_menu,
                        ID_VIEW_MONITOR_COLOR, ID_VIEW_MONITOR_GRAYSCALE,
                        LOWORD(wparam), MF_BYCOMMAND);
                    InvalidateRect(state->monitor_view, nullptr, FALSE);
                    UpdateWindow(state->monitor_view);
                    if (!win32::publish_control_panel_ui_preferences(
                        win32::alpha_installation_root(), win32::current_process_user_sid_string(),
                        state->preferences).succeeded())
                        MessageBoxW(window, L"The appearance changed for this session but could not be saved.",
                            L"Spitfire-X BBS", MB_OK | MB_ICONWARNING);
                    return 0;
                case ID_VIEW_MONITOR_COMPACT:
                case ID_VIEW_MONITOR_MEDIUM:
                case ID_VIEW_MONITOR_WIDE: {
                    const auto old = state->preferences.system_monitor_width;
                    state->preferences.system_monitor_width = LOWORD(wparam) == ID_VIEW_MONITOR_WIDE ? 400 :
                        LOWORD(wparam) == ID_VIEW_MONITOR_MEDIUM ? 320 : 251;
                    if (!win32::publish_control_panel_ui_preferences(win32::alpha_installation_root(),
                            win32::current_process_user_sid_string(), state->preferences)) {
                        state->preferences.system_monitor_width = old;
                        MessageBoxW(window,L"The monitor width could not be saved.",L"Spitfire-X BBS",MB_OK|MB_ICONWARNING);
                    } else CheckMenuRadioItem(state->monitor_width_menu, ID_VIEW_MONITOR_COMPACT,
                        ID_VIEW_MONITOR_WIDE, LOWORD(wparam), MF_BYCOMMAND);
                    RECT area{}; GetClientRect(window,&area); layout(*state,area.right,area.bottom);
                    return 0;
                }
                case ID_VIEW_MONITOR:
                    state->preferences.system_monitor_visible =
                        !state->preferences.system_monitor_visible;
                    CheckMenuItem(GetMenu(window), ID_VIEW_MONITOR, MF_BYCOMMAND |
                        (state->preferences.system_monitor_visible ? MF_CHECKED : MF_UNCHECKED));
                    show_selected_page(*state);
                    { RECT area{}; GetClientRect(window, &area); layout(*state, area.right, area.bottom); }
                    if (app::control_panel_monitor_should_poll(panel_page(state->tab),
                            state->preferences.system_monitor_visible)) {
                        sample_visible_monitor(*state);
                        InvalidateRect(state->monitor_view, nullptr, FALSE);
                    }
                    return 0;
                case IDC_LOG_PAUSE:
                    state->logs_paused = !state->logs_paused;
                    if (state->logs_paused) KillTimer(window, kLogPageTimer);
                    SetWindowTextW(state->log_pause_button, state->logs_paused ? L"Resume" : L"Pause");
                    if (!state->logs_paused) {
                        state->log_view_initialized = false;
                        update_status(*state);
                    }
                    return 0;
                case IDC_LOG_FILTER:
                    if (HIWORD(wparam)==CBN_SELCHANGE) {
                        state->selected_log_filter=static_cast<int>(SendMessageW(state->log_filter,CB_GETCURSEL,0,0));
                        state->log_view_initialized=false;
                        if (!state->logs_paused) refresh_live_logs(*state);
                    }
                    return 0;
                case IDC_LOG_CLEAR: {
                    update_status(*state); // obtain an authoritative presentation cutoff
                    if (!state->status.logs.empty())
                        state->logs_cleared_through = state->status.logs.back().sequence;
                    const auto cleared = std::wstring(L"View cleared. Persistent operational logs were not deleted.\r\n") +
                        log_health_notice(state->status.log_health, state->status.hostname_exhausted);
                    SetWindowTextW(state->log_view, cleared.c_str());
                    state->rendered_log_colors.clear();
                    if (state->status.log_health)
                        state->rendered_log_colors.push_back({{0, log_selection_position(cleared)}, kAmberColor});
                    if (state->rich_edit_module != nullptr) {
                        CHARFORMAT2W format{}; format.cbSize = sizeof(format);
                        format.dwMask = CFM_COLOR;
                        format.crTextColor = state->status.log_health ? kAmberColor : kTextColor;
                        SendMessageW(state->log_view, EM_SETCHARFORMAT, SCF_ALL, reinterpret_cast<LPARAM>(&format));
                    }
                    state->rendered_log_first_sequence = state->log_cache.backlog().oldest();
                    state->rendered_log_last_sequence = state->log_cache.backlog().newest();
                    state->rendered_log_count = state->log_cache.backlog().entries().size();
                    state->rendered_log_clear_through = state->logs_cleared_through;
                    state->log_view_initialized = true;
                    return 0;
                }
                case ID_VIEW_MODEM_LIGHTS:
                    toggle_modem_lights(*state);
                    return 0;
                case ID_NODE_DISCONNECT: {
                    std::vector<app::AlphaNodeCommandRequest> captured;
                    std::wstring description;
                    for (const auto number : node_command_targets(*state)) {
                        const auto* node = node_status(*state, number);
                        if (!node || !node->available || !node->assignment_sequence ||
                            (!node->caller_connected && node->state != sfx::core::NodeState::connecting)) continue;
                        captured.push_back({number, app::AlphaNodeCommandKind::disconnect,
                            state->status.runtime_epoch, node->assignment_sequence});
                        description += L"Node " + std::to_wstring(number) + L"\r\n";
                    }
                    if (captured.empty() || !task_choice(window, L"Spitfire-X BBS",
                        L"Disconnect these caller assignments?",
                        (description + L"Current work will end with normal accounting and cleanup. "
                            L"Nodes remain enabled. Replacement callers are not affected.").c_str(),
                        L"Disconnect", 100)) return 0;
                    std::wstring failures;
                    for (const auto& request : captured) {
                        const auto result = state->runtime.command_node(request);
                        if (result.code != app::LocalControlResponseCode::ok)
                            failures += L"Node " + std::to_wstring(request.node_number) + L": " +
                                utf8_to_wide(result.diagnostic_code) + L"\r\n";
                    }
                    update_status(*state);
                    if (!failures.empty()) MessageBoxW(window, failures.c_str(),
                        L"Caller disconnect result", MB_OK | MB_ICONINFORMATION);
                    return 0;
                }
                case ID_NODE_TERMINATE_DOOR: {
                    std::vector<app::AlphaNodeCommandRequest> captured;std::wstring description;
                    for(const auto number:node_command_targets(*state)){
                        const auto* node=node_status(*state,number);if(!node||!node->caller_connected||!node->door_id||!node->door_launch_sequence)continue;
                        captured.push_back({number,app::AlphaNodeCommandKind::terminate_door,state->status.runtime_epoch,node->assignment_sequence,node->door_launch_sequence});
                        description+=L"Node "+std::to_wstring(number)+L", Door "+std::to_wstring(node->door_id)+L"\r\n";
                    }
                    if(captured.empty()||!task_choice(window,L"Spitfire-X BBS",L"Terminate these Door sessions?",
                        (description+L"The remote Door connection will close. Callers return to their launching Door Menus and remain logged in. Replacement Door sessions are not affected.").c_str(),L"Terminate Door",100))return 0;
                    std::wstring failures;for(const auto& command:captured){const auto result=state->runtime.command_node(command);if(result.code!=app::LocalControlResponseCode::ok)failures+=L"Node "+std::to_wstring(command.node_number)+L": "+utf8_to_wide(result.diagnostic_code)+L"\r\n";}
                    update_status(*state);if(!failures.empty())MessageBoxW(window,failures.c_str(),L"Terminate Door result",MB_OK|MB_ICONINFORMATION);return 0;
                }
                case ID_NODE_ENABLE:
                case ID_NODE_DISABLE: {
                    if (node_state_commands_ready(*state))
                        set_node_states(*state, node_command_targets(*state), LOWORD(wparam) == ID_NODE_ENABLE);
                    return 0;
                }
                default: break;
            }
            break;
        case WM_NOTIFY: {
            const auto* header = reinterpret_cast<NMHDR*>(lparam);
            if (header->hwndFrom == state->modem_tooltip && header->code == TTN_GETDISPINFOW) {
                state->modem_tooltip_text = modem_tooltip_at_cursor(*state);
                reinterpret_cast<NMTTDISPINFOW*>(lparam)->lpszText = state->modem_tooltip_text.data();
                return 0;
            }
            if (header->hwndFrom == state->list_header && state->preferences.modem_lights &&
                (header->code == HDN_BEGINTRACKW || header->code == HDN_BEGINTRACKA) &&
                reinterpret_cast<NMHEADERW*>(lparam)->iItem == 1) return TRUE;
            if (header->hwndFrom == state->list_header &&
                (header->code == HDN_ENDTRACKW || header->code == HDN_ENDTRACKA) &&
                !state->applying_node_columns) {
                const auto* notification = reinterpret_cast<NMHEADERW*>(lparam);
                const int base_column = notification->iItem -
                    (state->preferences.modem_lights && notification->iItem > 0 ? 1 : 0);
                if (base_column >= 0 && base_column < 6 &&
                    !(state->preferences.modem_lights && notification->iItem == 1)) {
                    const int physical = ListView_GetColumnWidth(
                        state->node_list, notification->iItem);
                    const int logical = std::max(40, MulDiv(physical, 96,
                        static_cast<int>(window_dpi(window))));
                    state->preferences.node_column_widths[
                        static_cast<std::size_t>(base_column)] = logical;
                }
                return 0;
            }
            if (header->hwndFrom == state->tab && header->code == TCN_SELCHANGE) {
                InvalidateRect(state->tab, nullptr, TRUE);
                show_selected_page(*state);
                RECT area{}; GetClientRect(window, &area); layout(*state, area.right, area.bottom);
                return 0;
            }
            if (header->hwndFrom == state->node_list && header->code == NM_DBLCLK) {
                open_selected_node(*state);
                return 0;
            }
            if (header->hwndFrom == state->node_list && header->code == NM_CUSTOMDRAW) {
                auto* draw = reinterpret_cast<NMLVCUSTOMDRAW*>(lparam);
                if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) return CDRF_NOTIFYSUBITEMDRAW;
                if (draw->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) &&
                    draw->nmcd.dwItemSpec < displayed_node_rows(*state).size()) {
                    const auto& node = displayed_node_rows(*state)[draw->nmcd.dwItemSpec];
                    const bool selected = (ListView_GetItemState(state->node_list,
                        static_cast<int>(draw->nmcd.dwItemSpec), LVIS_SELECTED) &
                        LVIS_SELECTED) != 0;
                    draw->clrTextBk = selected ? kSelectionColor :
                        g_appearance.theme() == win32::DesktopTheme::classic && draw->nmcd.dwItemSpec % 2 == 0
                            ? kAlternateSurfaceColor : kPanelColor;
                    draw->clrText = selected ? g_appearance.palette().selected_text : kTextColor;
                    if (state->preferences.modem_lights && draw->iSubItem == 1) {
                        RECT area{};
                        ListView_GetSubItemRect(state->node_list, static_cast<int>(draw->nmcd.dwItemSpec),
                            1, LVIR_BOUNDS, &area);
                        draw_modem_lights(*state, draw->nmcd.hdc, area, &node, draw->clrTextBk, true);
                        return CDRF_SKIPDEFAULT;
                    }
                    const int base_column = draw->iSubItem -
                        (state->preferences.modem_lights && draw->iSubItem > 0 ? 1 : 0);
                    if (base_column == 1) {
                        if(state->startup_pending)draw->clrText=kMutedTextColor;
                        else if (!node.available || !node.enabled) draw->clrText = kAmberColor;
                        else if (node.state == sfx::core::NodeState::ready ||
                            node.state == sfx::core::NodeState::online) draw->clrText = kGreenColor;
                    } else if (!node.available && base_column >= 4) {
                        draw->clrText = kMutedTextColor;
                    }
                    const bool bold = draw->iSubItem == 0 &&
                        state->indicators.bold(node.number, GetTickCount64());
                    // The list-view DC is reused across subitems/rows. Always
                    // select this cell's font, so one node's pulse cannot leak.
                    SelectObject(draw->nmcd.hdc, bold ? state->bold_font : state->ui_font);
#ifdef SFX_ALPHA_TEST_AUTOMATION
                    if (node.number <= 4 && draw->iSubItem >= 0 && draw->iSubItem < 6) {
                        LOGFONTW actual{};
                        GetObjectW(GetCurrentObject(draw->nmcd.hdc, OBJ_FONT), sizeof(actual), &actual);
                        state->automation.painted_bold[node.number - 1][draw->iSubItem] = actual.lfWeight == FW_BOLD;
                    }
#endif
                    return CDRF_NEWFONT;
                }
                return CDRF_DODEFAULT;
            }
            break;
        }
        case WM_DRAWITEM: {
            const auto* item = reinterpret_cast<DRAWITEMSTRUCT*>(lparam);
            if (item && item->hwndItem == state->modem_strip) {
                draw_modem_lights(*state, item->hDC, item->rcItem,
                    node_status(*state, state->displayed_node), kBackgroundColor);
                return TRUE;
            }
            if (item != nullptr && (item->hwndItem == state->setup_button || item->hwndItem==state->service_button ||
                item->hwndItem == state->open_node_button || item->hwndItem == state->notice_button ||
                item->hwndItem == state->disable_all_nodes_button || item->hwndItem == state->enable_all_nodes_button ||
                item->hwndItem == state->confirm_disable_all_button || item->hwndItem == state->cancel_disable_all_button ||
                item->hwndItem == state->log_pause_button ||
                item->hwndItem == state->log_clear_button ||
                item->hwndItem == state->status_bar)) {
                draw_owner_control(*item, *state);
                return TRUE;
            }
            break;
        }
        case WM_CONTEXTMENU:
            if (reinterpret_cast<HWND>(wparam) == state->node_list) {
                POINT point{static_cast<int>(static_cast<short>(LOWORD(lparam))),
                    static_cast<int>(static_cast<short>(HIWORD(lparam)))};
                if (point.x != -1) {
                    LVHITTESTINFO hit{};
                    hit.pt = point;
                    ScreenToClient(state->node_list, &hit.pt);
                    const int clicked = ListView_HitTest(state->node_list, &hit);
                    if (clicked >= 0 && (hit.flags & LVHT_ONITEM)) {
                        select_context_node(state->node_list, clicked);
                    }
                }
                HMENU popup = CreatePopupMenu();
                populate_node_menu(popup, *state);
                if (point.x == -1) {
                    const int focused = ListView_GetNextItem(state->node_list, -1, LVNI_FOCUSED);
                    RECT item{};
                    if (focused >= 0 && ListView_GetItemRect(state->node_list, focused, &item, LVIR_BOUNDS))
                        point = {item.left, item.bottom};
                    else point = {0, 0};
                    ClientToScreen(state->node_list, &point);
                }
                TrackPopupMenu(popup, TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
                DestroyMenu(popup);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC: {
            const HWND control = reinterpret_cast<HWND>(lparam);
            if (control == state->heading || control == state->subheading) {
                SetTextColor(reinterpret_cast<HDC>(wparam), control == state->heading
                    ? g_appearance.palette().banner_gold : g_appearance.palette().banner_text);
                SetBkColor(reinterpret_cast<HDC>(wparam), g_appearance.palette().banner);
                return reinterpret_cast<LRESULT>(g_appearance.banner_brush());
            }
            SetTextColor(reinterpret_cast<HDC>(wparam),
                control == state->heading || control == state->node_header
                    ? kGoldColor : kTextColor);
            SetBkColor(reinterpret_cast<HDC>(wparam),
                control == state->heading || control == state->subheading
                    ? kPanelColor : kBackgroundColor);
            return reinterpret_cast<LRESULT>(
                control == state->heading || control == state->subheading
                    ? state->panel_brush : state->background_brush);
        }
        case WM_CTLCOLOREDIT:
            SetTextColor(reinterpret_cast<HDC>(wparam), kTextColor);
            SetBkColor(reinterpret_cast<HDC>(wparam), kPanelColor);
            return reinterpret_cast<LRESULT>(state->panel_brush);
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(window, &paint);
            draw_workspace(window, dc);
            EndPaint(window, &paint);
            return 0;
        }
        case WM_ERASEBKGND: {
            RECT area{}; GetClientRect(window, &area);
            FillRect(reinterpret_cast<HDC>(wparam), &area, state->background_brush);
            return 1;
        }
        case WM_CLOSE:
            if(state->service_action_busy)return 0;
            (void)close_runtime(*state, true);
            return 0;
        case WM_DESTROY:
            // Detached menus are ours; attached menus remain Windows-owned.
            // This also handles a partially restored presentation without
            // double-destroying a menu after its window disappears.
            if (state->windowed_menu && GetMenu(window) != state->windowed_menu)
                DestroyMenu(state->windowed_menu);
            state->windowed_menu = nullptr;
#ifdef SFX_ALPHA_TEST_AUTOMATION
            PostQuitMessage(state->automation.exit_code);
#else
            PostQuitMessage(0);
#endif
            return 0;
        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

AutomationOptions parse_automation_options() {
    AutomationOptions result;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) return result;
    for (int index = 1; index < count; ++index) {
        const std::wstring argument = arguments[index];
        if (argument == L"--fixture-bootstrap") result.fixture_bootstrap = true;
        else if (argument == L"--fixture-ssh-disabled") result.fixture_ssh_disabled = true;
        else if (argument.rfind(L"--multinode-qa=", 0) == 0)
            result.multinode_directory = argument.substr(15);
        else if (argument == L"--auto-close") result.auto_close = true;
        else if (argument.rfind(L"--capture-review=", 0) == 0)
            result.capture_path = argument.substr(17);
        else if (argument.rfind(L"--capture-wizard=", 0) == 0)
            result.wizard_capture_path = argument.substr(17);
        else if (argument.rfind(L"--capture-suite=", 0) == 0)
            result.capture_suite_directory = argument.substr(16);
        else if (argument.rfind(L"--capture-correction=", 0) == 0)
            result.correction_capture_directory = argument.substr(21);
        else if (argument.rfind(L"--capture-owner-review=", 0) == 0)
            result.owner_review_capture_directory = argument.substr(23);
        else if (argument == L"--expect-persisted-columns")
            result.expect_persisted_columns = true;
        else if (argument == L"--capture-monitor-appearance") result.capture_monitor_appearance = true;
        else if (argument == L"--expect-monitor-color") result.expected_monitor_appearance = 0;
        else if (argument == L"--expect-monitor-grayscale") result.expected_monitor_appearance = 1;
        else if (argument == L"--monitor-final-color") result.final_monitor_color = true;
        else if (argument.rfind(L"--automation-dpi=", 0) == 0) {
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(argument.c_str() + 17, &end, 10);
            if (end != argument.c_str() + 17 && *end == L'\0' &&
                value >= 96 && value <= 240) result.dpi_override = static_cast<UINT>(value);
        }
        else if (argument.rfind(L"--completion-delay-ms=", 0) == 0) {
            wchar_t* end = nullptr;
            const unsigned long value = wcstoul(argument.c_str() + 22, &end, 10);
            if (end != argument.c_str() + 22 && *end == L'\0' &&
                value >= 100 && value <= 60000) {
                result.completion_delay_millis = static_cast<UINT>(value);
            }
        }
    }
    LocalFree(arguments);
#endif
    return result;
}

#ifdef SFX_ALPHA_TEST_AUTOMATION
std::optional<int> run_f06_external_client_automation() {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) return 94;
    constexpr std::wstring_view prefix = L"--f06-client-qa=";
    std::filesystem::path directory;
    for (int index = 1; index < count; ++index) {
        const std::wstring argument = arguments[index];
        if (argument.rfind(prefix, 0) == 0) directory = argument.substr(prefix.size());
    }
    LocalFree(arguments);
    if (directory.empty()) return std::nullopt;

    std::wifstream profile_file(directory / L"client-profile.txt");
    std::wstring executable, output, release, label;
    std::wifstream endpoint_file(directory / L"endpoint.txt");
    std::wstring host, port_text;
    bool ok = static_cast<bool>(std::getline(profile_file, executable) &&
        std::getline(profile_file, output) && std::getline(profile_file, release) &&
        std::getline(profile_file, label) && std::getline(endpoint_file, host) &&
        std::getline(endpoint_file, port_text));
    wchar_t* end = nullptr;
    const auto port = std::wcstoul(port_text.c_str(), &end, 10);
    ok = ok && end && *end == L'\0' && host == L"127.0.0.1" &&
        port > 0 && port <= 65535;
    app::ExternalTelnetClientProfile profile;
    profile.type = app::ExternalTelnetClientType::custom;
    profile.executable = std::move(executable);
    profile.custom_arguments = {L"--output=" + output, L"--release=" + release,
        L"--label=" + label, L"--host={HOST}", L"--port={PORT}"};
    std::optional<app::ExternalTelnetClientLaunch> prepared;
    if (ok && save_telnet_client_profile(profile)) {
        const auto loaded = read_telnet_client_profile();
        if (loaded) prepared = app::prepare_external_telnet_client_launch(
            *loaded, host, static_cast<std::uint16_t>(port));
        ok = loaded.has_value() && prepared.has_value();
    } else {
        ok = false;
    }
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    const HANDLE sentinel = ok ? CreateEventW(&security, TRUE, FALSE, nullptr) : nullptr;
    ok = ok && sentinel != nullptr;
    if (ok) {
        const auto value = std::to_wstring(reinterpret_cast<std::uintptr_t>(sentinel));
        ok = SetEnvironmentVariableW(L"SFX_F06_INHERITABLE_HANDLE", value.c_str()) != FALSE &&
            launch_external_telnet_client(profile, *prepared);
        (void)SetEnvironmentVariableW(L"SFX_F06_INHERITABLE_HANDLE", nullptr);
    }
    if (sentinel) CloseHandle(sentinel);
    ok = clear_telnet_client_profile() && ok;
    std::ofstream complete(directory / L"complete.txt", std::ios::binary);
    complete << (ok ? "PASS\r\n" : "FAIL\r\n");
    complete.close();
    return ok && !complete.fail() ? 0 : 94;
}
#endif

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR command_line, int show) {
    if(const auto worker=win32::portable_preparation_worker(command_line?command_line:L""))return *worker;
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if (const auto result = run_f06_external_client_automation()) return *result;
#endif
#ifndef SFX_ALPHA_TEST_AUTOMATION
    if (command_line != nullptr && *command_line != L'\0') {
        MessageBoxW(nullptr, L"This production checkpoint accepts no command-line options.",
            L"Spitfire-X BBS", MB_OK | MB_ICONERROR);
        return 2;
    }
#else
    (void)command_line;
#endif
    INITCOMMONCONTROLSEX controls{sizeof(controls),
        ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES};
    if (!InitCommonControlsEx(&controls)) return 3;
    AppState state;
    state.startup_pending=true;
    state.instance = instance;
    state.automation = parse_automation_options();
#ifdef SFX_ALPHA_TEST_AUTOMATION
    g_automation_dpi = state.automation.dpi_override;
#endif
    state.owner_node_view = load_owner_ansi(instance, IDR_OWNER_NODE_VIEW_ANSI);
    state.owner_screen_saver = load_owner_ansi(instance, IDR_OWNER_SCREEN_SAVER_ANSI);
    state.node_view_scale = read_node_view_scale();
    LARGE_INTEGER visual_seed{}; QueryPerformanceCounter(&visual_seed);
    for (std::size_t node = 1; node < state.saver_placement.size(); ++node)
        state.saver_placement[node].seed(static_cast<std::uint64_t>(visual_seed.QuadPart) ^
            (static_cast<std::uint64_t>(GetCurrentProcessId()) << 32U) ^ (node * 0x9e3779b97f4a7c15ULL));
    state.terminal_font = load_embedded_cp437_font(instance, IDR_MOEBIUS_CP437_FONT);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    state.about_image = load_embedded_png(instance, IDR_OWNER_ABOUT_ARTWORK);
    if (SUCCEEDED(com)) CoUninitialize();
    if (!state.owner_node_view || !state.owner_screen_saver || !state.about_image ||
        !state.terminal_font) {
        MessageBoxW(nullptr, L"The embedded Control Panel visual resources are invalid.",
            L"Spitfire-X BBS", MB_OK | MB_ICONERROR);
        return 7;
    }
    if (const auto saved = win32::read_control_panel_ui_preferences(
            win32::alpha_installation_root())) state.preferences = *saved;

    win32::WindowIcon large_icon(instance, IDI_CONTROL_PANEL,
        GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON));
    win32::WindowIcon small_icon(instance, IDI_CONTROL_PANEL,
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
    if (!RegisterClassExW(&window_class)) return 5;
    WNDCLASSEXW terminal_class{sizeof(terminal_class)};
    terminal_class.style = CS_HREDRAW | CS_VREDRAW;
    terminal_class.lpfnWndProc = terminal_proc;
    terminal_class.hInstance = instance;
    terminal_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    terminal_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    terminal_class.lpszClassName = kTerminalClass;
    if (!RegisterClassExW(&terminal_class)) return 5;
    WNDCLASSEXW monitor_class{sizeof(monitor_class)};
    monitor_class.style = CS_HREDRAW | CS_VREDRAW;
    monitor_class.lpfnWndProc = monitor_proc;
    monitor_class.hInstance = instance;
    monitor_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    monitor_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    monitor_class.lpszClassName = kMonitorClass;
    if (!RegisterClassExW(&monitor_class)) return 5;
    WNDCLASSEXW statistics_class{sizeof(statistics_class)};
    statistics_class.style = CS_HREDRAW | CS_VREDRAW;
    statistics_class.lpfnWndProc = statistics_proc;
    statistics_class.hInstance = instance;
    statistics_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    statistics_class.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    statistics_class.lpszClassName = kStatisticsClass;
    if (!RegisterClassExW(&statistics_class)) return 5;
    RECT preferred{state.preferences.left, state.preferences.top,
        state.preferences.left + state.preferences.width,
        state.preferences.top + state.preferences.height};
    if (MonitorFromRect(&preferred, MONITOR_DEFAULTTONULL) == nullptr) {
        state.preferences.left = 80;
        state.preferences.top = 80;
    }
    const HWND window = CreateWindowExW(0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        state.preferences.left, state.preferences.top,
        state.preferences.width, state.preferences.height,
        nullptr, nullptr, instance, &state);
    if (window == nullptr) return 6;
    ShowWindow(window, show);
    UpdateWindow(window);
    begin_startup(state,StartupWork::Phase::verification);
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        bool chat_message=false;
        for(const auto chat_window:std::vector<HWND>(state.operator_windows))if(IsWindow(chat_window)&&IsDialogMessageW(chat_window,&message)){chat_message=true;break;}
        if(chat_message)continue;
        if (!state.startup_pending&&handle_full_screen_key(state, message)) continue;
        if (handle_node_page_key(state, message.hwnd, message.message, message.wParam)) continue;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    state.configuration_admin.stop();
    if (state.rich_edit_module != nullptr) FreeLibrary(state.rich_edit_module);
#ifdef SFX_ALPHA_TEST_AUTOMATION
    if (message.wParam == 0 && !state.automation.owner_review_capture_directory.empty()) {
        std::ofstream completion(state.automation.owner_review_capture_directory /
            L"CAPTURE_COMPLETE.txt", std::ios::trunc);
        completion << "PASS: all capture steps and functional checks exited successfully\n"
            << "dpi=" << state.automation.dpi_override << "\n"
            << "authored_cell_pixels=16x32; live_substitution_capture_is_separate\n";
        if (!completion.good()) return 88;
    }
#endif
    return static_cast<int>(message.wParam);
}
