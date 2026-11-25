#include <winsock2.h>
#include <ws2tcpip.h>

#include "alpha_runtime_host.hpp"
#include "alpha_ssh_stream.hpp"

#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "../platform/installed_qualification_fixture.hpp"

#include <windows.h>
#include <sddl.h>
#include <Aclapi.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <array>
#include <chrono>
#include <future>
#include <thread>
#include <stdexcept>
#include <string>
#include <sstream>

namespace {

namespace app = sfx::application;
namespace runtime = sfx::apps::runtime;
namespace win = sfx::platform::win32;
std::atomic<unsigned> assertions{0};

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
    ++assertions;
}

sfx::test::InstalledQualificationFixture* installed_fixture = nullptr;
std::filesystem::path fresh_case(const std::string& name) {
    require(installed_fixture != nullptr, "genuine installed qualification fixture required");
    std::cout << "CASE " << name << std::endl;
    const auto root = installed_fixture->next(name);
    // Filtering Off does not stop the independent feed updater. Supply a
    // fresh, protected synthetic cache in its normal feed directory
    // so isolated lifecycle cases do not depend on external HTTPS downloads.
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto text = "# SFX BotGate cache v1 updated=" + std::to_string(now) + "\n192.0.2.1\n";
    const auto owner = win::current_process_user_sid_string();
    const auto feeds = root / L"BotGate" / L"feeds";
    require(std::filesystem::create_directory(feeds) &&
        win::WindowsStorageDaclPolicy(owner).apply(feeds,win::StorageDaclObjectKind::directory).succeeded(),
        "create only the new fixture feed directory with its exact protected policy");
    const auto path = feeds / L"BotGate-feed.ipv4";
    const auto descriptor = win::installation::mutable_descriptor(owner);
    win::installation::create_file(path, {text.begin(), text.end()}, descriptor.get());
    require(win::WindowsStorageDaclPolicy(owner).apply(path,win::StorageDaclObjectKind::file).succeeded(),
        "protect current synthetic feed cache for isolated Runtime tests");
    return root;
}

app::AlphaBootstrapRequest request(
    const std::string& bbs_name,
    const std::uint16_t telnet_port,
    const std::string& system_password = "SY-fixture-72f1") {
    app::AlphaBootstrapRequest value;
    value.configuration.bbs_name = bbs_name;
    value.configuration.bbs_time_zone = "Pacific Standard Time";
    value.configuration.logical_nodes = 4;
    value.configuration.telnet.enabled = true;
    value.configuration.telnet.bind_address = "127.0.0.1";
    value.configuration.telnet.port = telnet_port;
    value.configuration.telnet.bot_gate_enabled = false;
    value.configuration.botgate.enabled = false; // Filtering off; fresh_case supplies an offline feed cache.
    // Historical Telnet fixtures must not expose a default SSH port on the LAN.
    value.configuration.ssh.enabled = false;
    value.sysop_name = "Alpha Sysop";
    value.sysop_password = "Acct-fixture-81a9";
    value.system_password = system_password;
    return value;
}

class Winsock final {
public:
    Winsock() {
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "start Winsock for listener test");
    }
    ~Winsock() { WSACleanup(); }
};

std::uint16_t available_loopback_port() {
    const SOCKET probe = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(probe != INVALID_SOCKET, "create loopback port probe");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    require(bind(probe, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
        "bind loopback port probe");
    int size = sizeof(address);
    require(getsockname(probe, reinterpret_cast<sockaddr*>(&address), &size) == 0,
        "read selected loopback port");
    const std::uint16_t port = ntohs(address.sin_port);
    closesocket(probe);
    return port;
}

std::uint16_t distinct_loopback_port(const std::uint16_t excluded) {
    for (int attempt = 0; attempt < 32; ++attempt) {
        const std::uint16_t candidate = available_loopback_port();
        if (candidate != excluded) return candidate;
    }
    throw std::runtime_error("select distinct loopback port");
}

SOCKET bind_loopback_listener(const std::uint16_t port) {
    const SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(listener != INVALID_SOCKET, "create loopback settings-activation blocker");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    if (bind(listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(listener, 1) != 0) {
        closesocket(listener);
        throw std::runtime_error("bind loopback settings-activation blocker");
    }
    return listener;
}

SOCKET connect_loopback(const std::uint16_t port) {
    const SOCKET client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(client != INVALID_SOCKET, "create Alpha Telnet client");
    DWORD timeout = 30000;
    require(setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0 &&
            setsockopt(client, SOL_SOCKET, SO_SNDTIMEO,
                reinterpret_cast<const char*>(&timeout), sizeof(timeout)) == 0,
        "configure bounded Telnet client I/O");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(port);
    require(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0,
        "connect to real Alpha Telnet listener");
    return client;
}

void send_text(const SOCKET client, const std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const int sent = send(client, text.data() + offset,
            static_cast<int>(text.size() - offset), 0);
        require(sent > 0, "send scripted Alpha Telnet input");
        offset += static_cast<std::size_t>(sent);
    }
}

std::string without_sgr(const std::string_view wire) {
    std::string text;
    for (std::size_t n = 0; n < wire.size();) {
        if (wire[n] == '\x1b' && n + 1 < wire.size() && wire[n + 1] == '[') {
            auto end = n + 2;
            while (end < wire.size() && ((wire[end] >= '0' && wire[end] <= '9') || wire[end] == ';')) ++end;
            if (end < wire.size() && wire[end] == 'm') { n = end + 1; continue; }
        }
        text += wire[n++];
    }
    return text;
}

std::string receive_until(const SOCKET client, const std::string_view expected, bool visible = false) {
    std::string output;
    output.reserve(4096);
    std::array<char, 2048> block{};
    while ((visible ? without_sgr(output) : output).find(expected) == std::string::npos) {
        const int received = recv(client, block.data(), static_cast<int>(block.size()), 0);
        if (received <= 0) {
            // Keep a bounded wire diagnostic for these synthetic callers; a
            // stale prompt must not appear as an unexplained socket failure.
            std::string escaped;
            constexpr char hex[] = "0123456789ABCDEF";
            for (const unsigned char byte : output.substr(0, 8192)) {
                if (byte >= 32 && byte < 127) escaped += static_cast<char>(byte);
                else { escaped += "\\x"; escaped += hex[byte >> 4]; escaped += hex[byte & 15]; }
            }
            throw std::runtime_error("receive expected Alpha Telnet output: expected=" +
                std::string(expected) + "; recv=" + std::to_string(received) +
                "; error=" + std::to_string(WSAGetLastError()) + "; wire=" + escaped);
        }
        ++assertions;
        output.append(block.data(), static_cast<std::size_t>(received));
        require(output.size() <= 1024U * 1024U, "bound Alpha Telnet test transcript");
    }
    return output;
}

std::string terminal_text(const app::ControlPanelTerminalSnapshot& screen) {
    std::string result;
    result.reserve(app::kControlPanelTerminalCells + app::kControlPanelTerminalRows);
    for (std::size_t row = 0; row < app::kControlPanelTerminalRows; ++row) {
        for (std::size_t column = 0; column < app::kControlPanelTerminalColumns; ++column) {
            result.push_back(static_cast<char>(screen.cells[
                row * app::kControlPanelTerminalColumns + column].character));
        }
        result.push_back('\n');
    }
    return result;
}

void qualify_real_telnet_login(
    runtime::AlphaRuntimeHost& host,
    const std::uint16_t port) {
    const SOCKET client = connect_loopback(port);
    auto banner = receive_until(client,
        "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    const std::string exact_banner = "Do you want ANSI COLOR GRAPHICS? [Y/n] ";
    require(banner.find("Your Sysop is Alpha Sysop\r\n") == 12 &&
            banner.find("\r\n\r\n" + exact_banner) != std::string::npos &&
            banner.find('\x1b') == std::string::npos,
        "accepted Row1 identity follows 12-byte Telnet negotiation including terminal-type request; blank row before ANSI question");
    const std::string negotiation_and_answer{"\xff\xfb\x01Y\r", 5};
    send_text(client, negotiation_and_answer);
    auto ansi_first_name = receive_until(client, "Enter your first name: ");
    require(ansi_first_name.find("Your Sysop") == std::string::npos &&
            ansi_first_name.find("\x1b[0;37;40mEnter your first name: ") != std::string::npos,
        "identity is not repeated after graphics choice; generated prompt remains VGA 07 before VGA 0B input");
    send_text(client, "Alpha\r");
    (void)receive_until(client, "Enter your last name: ");
    send_text(client, "Sysop\r");
    (void)receive_until(client, "Enter your password: ");
    constexpr std::string_view account_before_backspace{"Acct-fixture-81aX"};
    send_text(client, "Acct-fixture-81aX\b9\r");
    auto system_prompt = receive_until(client, "su password: ");
    const std::string expected_account_mask =
        std::string(account_before_backspace.size(), '*') + "\b \b*\x1b[0;37;40m\r\n";
    require(system_prompt.find(expected_account_mask) != std::string::npos &&
            system_prompt.find(account_before_backspace) == std::string::npos,
        "account password masks every accepted byte and backspace erases only the visible mask");
    auto password_view = host.status();
    std::string password_screen = terminal_text(password_view.nodes[0].screen);
    const ULONGLONG password_view_deadline = GetTickCount64() + 2000ULL;
    while (password_screen.find("su password: ") == std::string::npos &&
            GetTickCount64() < password_view_deadline) {
        Sleep(10);
        password_view = host.status();
        password_screen = terminal_text(password_view.nodes[0].screen);
    }
    require(password_view.nodes[0].screen_active &&
            password_view.nodes[0].assignment_sequence != 0 &&
            password_view.nodes[0].source_ip == "127.0.0.1",
        "live Node View snapshot carries real connection assignment telemetry");
    require(password_screen.find("su password: ") != std::string::npos,
        "live Node View snapshot mirrors the real System Password prompt");
    require(password_screen.find(std::string(account_before_backspace.size(), '*')) !=
            std::string::npos,
        "live Node View snapshot mirrors the caller-visible account-password mask");
    require(password_screen.find(account_before_backspace) == std::string::npos &&
            password_screen.find("Acct-fixture-81a9") == std::string::npos,
        "live Node View snapshot excludes account-password input");
    constexpr std::string_view system_before_backspace{"SY-fixture-72fX"};
    send_text(client, "SY-fixture-72fX\b1\r");
    auto authenticated = receive_until(client, "Press <ENTER> to continue.", true);
    const std::string expected_system_mask =
        std::string(system_before_backspace.size(), '*') + "\b \b*\x1b[0;37;40m\r\n";
    require(authenticated.find(expected_system_mask + "\x1b[0;37;40m\r\nGood ") !=
                std::string::npos &&
            authenticated.find("Alpha Sysop, welcome to Spitfire-X Alpha Runtime QA.\r\n") !=
                std::string::npos &&
            authenticated.find(system_before_backspace) == std::string::npos,
        "System Password masking is exact and a blank line separates authentication from the welcome");
    require(authenticated.find("There have been 0 calls today.") != std::string::npos &&
            authenticated.find("You have called 1 time.") != std::string::npos &&
            authenticated.find("You have spent less than one minute online.") != std::string::npos &&
            authenticated.find("Caller:") == std::string::npos &&
            authenticated.find("Authenticated session seconds:") == std::string::npos &&
            without_sgr(authenticated).find("Press <ENTER> to continue.") != std::string::npos,
        "first Sysop login is excluded from public daily calls, retains personal totals and shows the current continuation prompt");
    send_text(client, "\r");
    const auto bulletins = receive_until(client, "< ENTER > To Continue? ");
    require(bulletins.find("Bulletin <#>") != std::string::npos,
        "current installed Bulletin artwork has its real continuation prompt");
    send_text(client, "\r");
    const auto scan = receive_until(client, "<Enter> to continue ");
    require(scan.find("No new messages addressed to you.") != std::string::npos,
        "fresh caller sees the real empty message scan before Main");
    send_text(client, "\r");
    auto menu = receive_until(client, "]? ", true);
    require(menu.find("\x1b[0m\x1b[2J\x1b[H") != std::string::npos &&
            menu.find("Message Menu") != std::string::npos &&
            menu.find("Goodbye") != std::string::npos &&
            without_sgr(menu).find("MAIN MENU - [") != std::string::npos,
        "continuation Enter resets ANSI styling and reaches installed Main artwork and current footer");
    auto online_view = host.status();
    std::string online_screen = terminal_text(online_view.nodes[0].screen);
    const ULONGLONG online_view_deadline = GetTickCount64() + 2000ULL;
    while (online_screen.find("Message Menu") == std::string::npos &&
            GetTickCount64() < online_view_deadline) {
        Sleep(10);
        online_view = host.status();
        online_screen = terminal_text(online_view.nodes[0].screen);
    }
    require(online_view.nodes[0].state == sfx::core::NodeState::online &&
            online_view.nodes[0].caller_handle == "Alpha Sysop" &&
            online_view.nodes[0].activity == "Main Menu" &&
            online_screen.find("Message Menu") != std::string::npos &&
            online_screen.find("Goodbye") != std::string::npos &&
            online_screen.find("Acct-fixture-81a9") == std::string::npos &&
            online_screen.find("SY-fixture-72f1") == std::string::npos,
        "live Node View snapshot reports the authenticated caller and real main-menu screen safely");
    send_text(client, "M\r");
    const auto messages = receive_until(client, "]? ", true);
    require(without_sgr(messages).find("MESSAGE MENU - [") != std::string::npos &&
            messages.find("QWK / Offline Mail") != std::string::npos &&
            messages.find("Current Message Conference is #") != std::string::npos,
        "Message choice enters the implemented Message Menu with actual conference context");
    send_text(client, "Q\r");
    require(without_sgr(receive_until(client, "]? ", true)).find("MAIN MENU - [") != std::string::npos,
        "Message Menu returns to Main through its real Q command");
    send_text(client, "G\r");
    (void)receive_until(client, "Logoff the BBS? [Y/n]", true);
    send_text(client, "Y\r");
    auto goodbye = receive_until(client, "Goodbye from Spitfire-X BBS.");
    require(goodbye.find("Goodbye from Spitfire-X BBS.") != std::string::npos,
        "Goodbye closes the authenticated session cleanly");
    shutdown(client, SD_BOTH);
    closesocket(client);

    const auto retired_by = GetTickCount64() + 5000;
    while (host.status().lifecycle.active_work.caller_sessions && GetTickCount64() < retired_by) Sleep(1);
    require(!host.status().lifecycle.active_work.caller_sessions, "normal logoff completes cleanup before exact Node 1 banner reuse");
    const SOCKET second = connect_loopback(port);
    auto second_banner = receive_until(second,
        "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    send_text(second, "N\r");
    auto ascii_first_name = receive_until(second, "Enter your first name: ");
    require(second_banner.find(exact_banner) != std::string::npos &&
            ascii_first_name.find("\x1b[") == std::string::npos,
        "ASCII selection keeps the post-choice caller presentation free of ANSI sequences");
    shutdown(second, SD_BOTH);
    closesocket(second);
    const auto second_retired_by = GetTickCount64() + 5000;
    while (host.status().lifecycle.active_work.caller_sessions && GetTickCount64() < second_retired_by) Sleep(1);
    require(!host.status().lifecycle.active_work.caller_sessions,
        "ASCII disconnect cleanup is quiescent before sampling unchanged pager counters");
    require(host.status().telnet_active,
        "listener remains available after an authenticated caller disconnects");
}

void qualify_shutdown_with_idle_caller(
    runtime::AlphaRuntimeHost& host,
    const std::uint16_t port) {
    const SOCKET idle = connect_loopback(port);
    (void)receive_until(idle,
        "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    const auto started = std::chrono::steady_clock::now();
    require(host.shutdown().code == app::LocalControlResponseCode::ok,
        "runtime checkpoints and stops orderly with an idle Telnet caller");
    require(std::chrono::steady_clock::now() - started < std::chrono::seconds(5),
        "runtime cancellation interrupts an idle Telnet read promptly");
    std::array<char, 8> block{};
    const int received = recv(idle, block.data(), static_cast<int>(block.size()), 0);
    require(received <= 0, "runtime shutdown closes the idle caller socket");
    closesocket(idle);
}

void qualify_disable_with_active_caller(
    runtime::AlphaRuntimeHost& host,
    const std::uint16_t port) {
    const SOCKET caller = connect_loopback(port);
    (void)receive_until(caller, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    const auto assigned = host.status();
    require(assigned.nodes[0].state == sfx::core::NodeState::connecting &&
            assigned.nodes[0].source_ip == "127.0.0.1",
        "Control Panel sees real assigned Node 1 telemetry for an active caller");
    require(host.command_node({1, app::AlphaNodeCommandKind::disable}).code ==
            app::LocalControlResponseCode::ok,
        "confirmed Node 1 disable succeeds while a caller is active");
    std::array<char, 8> block{};
    const int received = recv(caller, block.data(), static_cast<int>(block.size()), 0);
    require(received <= 0, "disabling Node 1 terminates its active caller socket");
    closesocket(caller);
    const auto disabled = host.status();
    require(disabled.telnet_active && !disabled.nodes[0].enabled,
        "active-caller disable is reflected in Control Panel status");
    require(host.command_node({1, app::AlphaNodeCommandKind::enable}).code ==
            app::LocalControlResponseCode::ok && host.status().telnet_active,
        "Node 1 can be explicitly restored after active-caller disable");
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "open bounded runtime artifact for audit");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void qualify_complete_restart_and_fail_closed_config(const std::filesystem::path& root) {
    const std::uint16_t port = available_loopback_port();
    auto bootstrap = request("Spitfire-X Alpha Runtime QA", port);
    {
        runtime::AlphaRuntimeHost host(root);
        const auto started = host.start();
        if (started.code != app::LocalControlResponseCode::setup_required) {
            throw std::runtime_error("clean baseline start: " + started.diagnostic_code);
        }
        const auto setup_status = host.status();
        require(setup_status.state == app::AlphaRuntimePresentationState::setup_required &&
                setup_status.nodes.empty() && !setup_status.telnet_active,
            "pre-bootstrap status has no synthetic ready nodes or listener");

        runtime::AlphaRuntimeHost duplicate(root);
        require(duplicate.start().code == app::LocalControlResponseCode::recovery_required,
            "second runtime for the same installation is rejected");

        const auto bootstrapped = host.bootstrap(bootstrap);
        if (bootstrapped.code != app::LocalControlResponseCode::ok) {
            throw std::runtime_error("runtime completes first-run bootstrap: " +
                bootstrapped.diagnostic_code);
        }
        const auto ready = host.status();
        const auto feed = host.botgate_feed_status();
        require(feed.available && feed.entries == 1 && !feed.last_attempt && !feed.updating,
            "current synthetic cache loads without an external feed update");
        require(ready.state == app::AlphaRuntimePresentationState::ready &&
                ready.bootstrap_complete && ready.configuration_loaded &&
                ready.storage.state == app::StorageRuntimeState::ready &&
                ready.bbs_name == "Spitfire-X Alpha Runtime QA" &&
                ready.telnet_configured && ready.telnet_bind == "127.0.0.1" &&
                ready.telnet_port == port && !ready.telnet_bot_gate_enabled &&
                ready.telnet_active,
            "ready status is runtime-derived and truthfully reports Telnet listening");
        require(ready.nodes.size() == 4 && ready.nodes[0].available && ready.nodes[0].enabled &&
                ready.nodes[1].available && ready.nodes[2].available && ready.nodes[3].available &&
                ready.nodes[3].enabled && ready.nodes[3].details == "Ready for connection",
            "all four configured nodes are backed by real idle workers");
        require(!ready.logs.empty(), "runtime exposes real bounded operational listener logs");
        require(host.command_node({5, app::AlphaNodeCommandKind::disable}).code ==
                app::LocalControlResponseCode::invalid_request,
            "out-of-range logical nodes cannot be controlled");
        require(host.command_node({1, app::AlphaNodeCommandKind::disable}).code ==
                app::LocalControlResponseCode::ok && host.status().telnet_active &&
                !host.status().nodes[0].enabled,
            "disabling Node 1 preserves shared listeners and reports disabled");
        require(host.command_node({1, app::AlphaNodeCommandKind::enable}).code ==
                app::LocalControlResponseCode::ok && host.status().telnet_active &&
                host.status().nodes[0].enabled,
            "enabling Node 1 restores its listener and persisted state");
        qualify_disable_with_active_caller(host, port);
        const auto pager_baseline = host.metrics();
        require(pager_baseline.sqlite.available &&
                !pager_baseline.sqlite.interval_available,
            "first visible-monitor request establishes the Runtime SQLite pager baseline");
        for (int index = 0; index < 5; ++index) (void)host.status();
        Sleep(10);
        const auto idle_pager = host.metrics();
        require(idle_pager.sqlite.interval_available &&
                idle_pager.sqlite.page_reads_per_second == 0 &&
                idle_pager.sqlite.page_writes_per_second == 0 &&
                host.status().security_policy_revision == ready.security_policy_revision &&
                host.status().security_policy_hash == ready.security_policy_hash,
            "idle status refreshes use the loaded policy cache and leave SQLite pager rates at zero");
        qualify_real_telnet_login(host, port);
        const auto pager_activity = host.metrics();
        require(pager_activity.sqlite.interval_available &&
                pager_activity.sqlite.page_reads_per_second > 0 &&
                pager_activity.sqlite.page_writes_per_second > 0,
            "known real login activity advances read-only Runtime pager rates");
        Sleep(10);
        const auto pager_settled = host.metrics();
        require(pager_settled.sqlite.interval_available &&
                pager_settled.sqlite.page_reads_per_second == 0 &&
                pager_settled.sqlite.page_writes_per_second == 0,
            "unchanged Runtime pager counters settle without exposing database content");
        const auto observed = host.status();
        require(observed.statistics.successful_logons.available &&
                observed.statistics.successful_logons.today == 0 &&
                observed.statistics.successful_logons.total == 0 &&
                observed.statistics.new_users.available && observed.statistics.new_users.today == 1 &&
                observed.statistics.new_users.total == 1,
            "Control Panel excludes the Sysop session from public calls while counting its actual account");
        require(std::filesystem::is_regular_file(root / L"Logs" / L"SFXRuntime.log"),
            "real bounded operational log is retained independently of the viewer");
        const win::WindowsStorageDaclPolicy log_policy(win::current_process_user_sid_string());
        require(log_policy.assess(root / L"Logs" / L"SFXRuntime.log",
                win::StorageDaclObjectKind::file).status.succeeded(),
            "persistent operational log inherits the exact protected file DACL");
        qualify_shutdown_with_idle_caller(host, port);
        require(host.status().lifecycle.state == app::RuntimeState::stopped,
            "runtime lifecycle reaches stopped");
    }
    const std::string operational_log = read_file(root / L"Logs" / L"SFXRuntime.log");
    std::vector<app::AlphaOperationalLogEntry> events;
    std::istringstream lines(operational_log);
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto event = app::decode_operational_record(line, events.size() + 1);
        require(event && event->event, "new runtime writes validated typed records, not rendered sentences");
        events.push_back(std::move(*event));
    }
    const auto has = [&](app::OperationalKind kind) {
        return std::any_of(events.begin(), events.end(), [&](const auto& e) { return e.event->kind == kind; });
    };
    require(!has(app::OperationalKind::feed_update_started),
        "isolated Runtime lifecycle made no external feed request");
    require(has(app::OperationalKind::listener_ready) && has(app::OperationalKind::connection_assigned) &&
            has(app::OperationalKind::authenticated) && has(app::OperationalKind::connection_ended) &&
            has(app::OperationalKind::assignment_released) && has(app::OperationalKind::listener_stopped),
        "persistent operational log records listener, node, login, release, and shutdown outcomes");
    for (const auto& e : events) if (e.event->kind == app::OperationalKind::authenticated) {
        const auto& c = e.event->context;
        require(c.epoch.valid() && c.node && c.generation && c.account_id && c.attempt &&
            c.durable_session_id.valid() && c.correlation_id.valid() && !c.caller.empty(),
            "fully authenticated records retain exact assignment, account and durable correlation");
    }
    require(operational_log.find(bootstrap.sysop_password) == std::string::npos &&
            operational_log.find(bootstrap.system_password) == std::string::npos,
        "persistent operational log never retains authentication secrets");
    require(std::filesystem::is_regular_file(root / L"Data" / L"SFX.DB") &&
            !std::filesystem::exists(root / L"Data" /
                std::filesystem::path{std::wstring{L"S"} + L"FNG.DB"}) &&
            std::filesystem::is_regular_file(root / L"Config" / L"SFX.conf"),
        "bootstrap uses only the clean SFX.DB baseline and canonical configuration path");
    const std::string config = read_file(root / L"Config" / L"SFX.conf");
    require(config.find(bootstrap.sysop_password) == std::string::npos &&
            config.find(bootstrap.system_password) == std::string::npos,
        "human-readable configuration contains no bootstrap secrets");
    {
        runtime::AlphaRuntimeHost restarted(root);
        require(restarted.start().code == app::LocalControlResponseCode::ok &&
                restarted.status().state == app::AlphaRuntimePresentationState::ready,
            "completed installation restarts ready without repeating setup");
        const auto restarted_metrics = restarted.metrics();
        require(restarted_metrics.sqlite.available &&
                !restarted_metrics.sqlite.interval_available,
            "restarted SQLite connections establish a fresh no-spike metric baseline");
        const auto restarted_status = restarted.status();
        require(std::none_of(restarted_status.logs.begin(), restarted_status.logs.end(),
                [](const app::AlphaOperationalLogEntry& entry) {
                    return entry.event && entry.event->kind == app::OperationalKind::authenticated;
                }) && read_file(root / L"Logs" / L"SFXRuntime.log").rfind(operational_log, 0) == 0,
            "new Live Logs epoch does not replay old authenticated events; physical history is preserved byte-for-byte");
        require(restarted.shutdown().code == app::LocalControlResponseCode::ok,
            "restarted runtime stops orderly");
    }

    const HANDLE corrupt = CreateFileW((root / L"Config" / L"SFX.conf").c_str(),
        GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(corrupt != INVALID_HANDLE_VALUE, "open configuration for corruption test");
    constexpr char invalid[] = "format_version=999\n";
    DWORD written = 0;
    const bool wrote = WriteFile(corrupt, invalid, sizeof(invalid) - 1U, &written, nullptr) != FALSE;
    CloseHandle(corrupt);
    require(wrote && written == sizeof(invalid) - 1U,
        "write intentionally invalid strict configuration");
    runtime::AlphaRuntimeHost rejected(root);
    require(rejected.start().code == app::LocalControlResponseCode::recovery_required &&
            rejected.status().state == app::AlphaRuntimePresentationState::recovery_required,
        "invalid completed configuration fails closed into explicit recovery");
    require(rejected.shutdown().code == app::LocalControlResponseCode::ok,
        "recovery runtime still closes storage orderly");
}

void qualify_settings_activation_and_fallback() {
    {
        const auto case_root = fresh_case("activation");
        const std::uint16_t active_port = available_loopback_port();
        const std::uint16_t candidate_port = distinct_loopback_port(active_port);
        auto bootstrap = request("SFX Settings Active QA", active_port);
        {
            runtime::AlphaRuntimeHost host(case_root);
            require(host.start().code == app::LocalControlResponseCode::setup_required &&
                    host.bootstrap(bootstrap).code == app::LocalControlResponseCode::ok,
                "settings activation case bootstraps cleanly");
            const app::AlphaSettingsRequest settings{
                "SFX Settings Updated QA", "127.0.0.1", candidate_port, false, "0.0.0.0", 2222, false, false};
            require(host.stage_settings(settings).code == app::LocalControlResponseCode::ok,
                "valid settings are staged through the runtime boundary");
            const auto staged = host.status();
            require(staged.listener_restart_required && staged.telnet_port == active_port &&
                    staged.allow_new_users && staged.pending_settings && !staged.pending_settings->allow_new_users &&
                    staged.hostname_lookup && !staged.pending_settings->hostname_lookup &&
                    staged.bbs_name == "SFX Settings Active QA" && staged.telnet_active,
                "staging preserves the active listener/configuration until restart");
            require(host.shutdown().code == app::LocalControlResponseCode::ok,
                "settings activation case stops before restart");
        }
        runtime::AlphaRuntimeHost restarted(case_root);
        require(restarted.start().code == app::LocalControlResponseCode::ok,
            "staged settings activate on restart after endpoint validation");
        const auto activated = restarted.status();
        require(!activated.listener_restart_required && activated.telnet_active &&
                !activated.allow_new_users && !activated.hostname_lookup && !activated.pending_settings &&
                activated.telnet_port == candidate_port &&
                activated.bbs_name == "SFX Settings Updated QA",
            "successful activation publishes only the validated candidate settings");
        const SOCKET caller = connect_loopback(candidate_port);
        const auto banner = receive_until(caller, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
        require(banner.find("SFX Settings Updated QA") == std::string::npos,
            "caller identity remains the locked Slice A banner after a Control Panel name update");
        shutdown(caller, SD_BOTH);
        closesocket(caller);
        require(restarted.shutdown().code == app::LocalControlResponseCode::ok,
            "activated-settings runtime stops cleanly");
    }
    {
        const auto case_root = fresh_case("fallback");
        const std::uint16_t active_port = available_loopback_port();
        const std::uint16_t candidate_port = distinct_loopback_port(active_port);
        auto bootstrap = request("SFX Settings Fallback QA", active_port);
        {
            runtime::AlphaRuntimeHost host(case_root);
            require(host.start().code == app::LocalControlResponseCode::setup_required &&
                    host.bootstrap(bootstrap).code == app::LocalControlResponseCode::ok,
                "settings fallback case bootstraps cleanly");
            const app::AlphaSettingsRequest settings{
                "SFX Settings Rejected QA", "127.0.0.1", candidate_port, false, "0.0.0.0", 2222, false, false};
            require(host.stage_settings(settings).code == app::LocalControlResponseCode::ok &&
                    host.status().listener_restart_required,
                "fallback candidate is durably staged before restart");
            require(host.shutdown().code == app::LocalControlResponseCode::ok,
                "settings fallback case stops before restart");
        }
        const SOCKET blocker = bind_loopback_listener(candidate_port);
        runtime::AlphaRuntimeHost restarted(case_root);
        const auto start = restarted.start();
        require(start.code == app::LocalControlResponseCode::ok &&
                start.diagnostic_code == "settings_activation_failed_restored",
            "unavailable candidate endpoint restores the last active settings instead of faulting");
        const auto restored = restarted.status();
        require(restored.state == app::AlphaRuntimePresentationState::ready &&
                restored.telnet_active && restored.telnet_port == active_port &&
                restored.bbs_name == "SFX Settings Fallback QA" &&
                restored.allow_new_users && restored.hostname_lookup && !restored.pending_settings &&
                !restored.listener_restart_required,
            "restored status truthfully reports the last active endpoint and BBS name");
        const auto disk = win::read_operational_configuration_file(case_root);
        require(disk.state == win::ConfigurationFileState::loaded && disk.configuration &&
                disk.configuration->telnet.port == active_port &&
                disk.configuration->bbs_name == "SFX Settings Fallback QA" &&
                disk.configuration->hostname_lookup &&
                win::read_pending_operational_configuration_file(case_root).state ==
                    win::ConfigurationFileState::missing,
            "failed activation leaves the last active file authoritative and removes the candidate");
        const SOCKET caller = connect_loopback(active_port);
        (void)receive_until(caller, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
        shutdown(caller, SD_BOTH);
        closesocket(caller);
        require(restarted.shutdown().code == app::LocalControlResponseCode::ok,
            "restored runtime stops cleanly");
        closesocket(blocker);
    }
}

void qualify_ssh_fail_closed() {
    for (const auto* fault : {"corrupt", "insecure", "unreadable", "port"}) {
        const auto case_root = fresh_case(std::string("ssh-") + fault);
        const auto telnet = available_loopback_port();
        const auto ssh = distinct_loopback_port(telnet);
        auto bootstrap = request("SFX SSH Failure QA", telnet);
        bootstrap.configuration.ssh = {true, "127.0.0.1", ssh};
        {
            runtime::AlphaRuntimeHost host(case_root);
            require(host.start().code == app::LocalControlResponseCode::setup_required,
                "isolated SSH failure fixture starts in setup state");
            require(host.bootstrap(bootstrap).code == app::LocalControlResponseCode::ok &&
                    host.status().telnet_active && host.status().ssh_active,
                "both isolated listeners start with a newly generated host key");
            require(host.shutdown().code == app::LocalControlResponseCode::ok,
                "SSH failure fixture stops before fault injection");
        }
        const auto key = case_root / L"Config" / L"ssh_host_ed25519.pem";
        const win::WindowsStorageDaclPolicy policy(win::current_process_user_sid_string());
        const auto assessment = policy.assess(key, win::StorageDaclObjectKind::file);
        require(assessment.status.succeeded() && assessment.private_access && assessment.effective_access,
            "generated host key has the exact protected installation-owner policy");
        HANDLE locked_key = INVALID_HANDLE_VALUE;
        SOCKET blocker = INVALID_SOCKET;
        if (std::string(fault) == "corrupt") {
            std::ofstream stream(key, std::ios::binary | std::ios::trunc);
            stream << "deliberately invalid test key\n";
            require(stream.good(), "write bounded malformed fixture key");
        } else if (std::string(fault) == "insecure") {
            const auto sddl = L"D:P(A;;FA;;;SY)(A;;FA;;;BA)(A;;FA;;;" +
                win::current_process_user_sid_string() + L")(A;;FR;;;WD)";
            PSECURITY_DESCRIPTOR descriptor = nullptr;
            require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),
                SDDL_REVISION_1, &descriptor, nullptr) != FALSE, "create insecure test-only key DACL");
            const bool applied = SetFileSecurityW(key.c_str(),
                DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != FALSE;
            LocalFree(descriptor);
            require(applied, "apply unexpected Everyone grant to isolated fixture key only");
        } else if (std::string(fault) == "unreadable") {
            locked_key = CreateFileW(key.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            require(locked_key != INVALID_HANDLE_VALUE, "hold exclusive fixture key handle");
        } else {
            blocker = bind_loopback_listener(ssh);
        }
        runtime::AlphaRuntimeHost restarted(case_root);
        const auto result = restarted.start();
        require(result.code == app::LocalControlResponseCode::recovery_required &&
                !restarted.status().telnet_active && !restarted.status().ssh_active,
            "bad SSH key or occupied enabled port fails closed without a partial ready state");
        require(result.diagnostic_code.find(std::string(fault) == "port" ?
                "ssh_bind_or_listen_failed" : "ssh_host_key_") != std::string::npos,
            "SSH startup failure reports its bounded truthful cause");
        require(restarted.shutdown().code == app::LocalControlResponseCode::ok,
            "faulted SSH runtime stops cleanly");
        if (locked_key != INVALID_HANDLE_VALUE) CloseHandle(locked_key);
        if (blocker != INVALID_SOCKET) closesocket(blocker);
        if (std::string(fault) == "corrupt") {
            std::ifstream stream(key, std::ios::binary);
            std::string text((std::istreambuf_iterator<char>(stream)), {});
            require(text == "deliberately invalid test key\n", "malformed existing key is never regenerated");
        }
    }
    const auto case_root = fresh_case("disabled-settings");
    const auto telnet = available_loopback_port();
    const auto ssh = distinct_loopback_port(telnet);
    {
        runtime::AlphaRuntimeHost host(case_root);
        auto bootstrap = request("SFX Disabled Settings QA", telnet);
        require(host.start().code == app::LocalControlResponseCode::setup_required &&
                host.bootstrap(bootstrap).code == app::LocalControlResponseCode::ok &&
                host.command_node({1, app::AlphaNodeCommandKind::disable}).code ==
                    app::LocalControlResponseCode::ok,
            "disabled-node SSH settings fixture initializes");
        require(host.stage_settings({"SFX Must Not Publish", "127.0.0.1", telnet,
                true, "127.0.0.1", ssh}).code == app::LocalControlResponseCode::ok &&
                host.shutdown().code == app::LocalControlResponseCode::ok,
            "SSH candidate settings stage while the shared node is disabled");
    }
    const SOCKET blocker = bind_loopback_listener(ssh);
    runtime::AlphaRuntimeHost restarted(case_root);
    require(restarted.start().diagnostic_code == "settings_activation_failed_restored" &&
            restarted.status().bbs_name == "SFX Disabled Settings QA" &&
            !restarted.status().ssh_configured && !restarted.status().nodes[0].enabled,
        "occupied SSH port prevents publishing staged settings even with Node 1 disabled");
    require(restarted.shutdown().code == app::LocalControlResponseCode::ok,
        "disabled-node settings fallback stops cleanly");
    closesocket(blocker);
}

void qualify_resume_after_sysop_stage(const std::filesystem::path& root) {
    const std::uint16_t port = available_loopback_port();
    auto incomplete = request("Spitfire-X Sysop Resume QA", port, "x");
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::setup_required,
            "sysop-resume case starts clean");
        require(host.bootstrap(incomplete).code == app::LocalControlResponseCode::invalid_request,
            "invalid SY password interrupts after durable initial-Sysop staging");
        require(host.shutdown().code == app::LocalControlResponseCode::ok,
            "interrupted runtime stops cleanly");
    }
    auto resume = request("Spitfire-X Sysop Resume QA", port);
    runtime::AlphaRuntimeHost restarted(root);
    require(restarted.start().code == app::LocalControlResponseCode::recovery_required &&
            restarted.status().diagnostic_code == "configuration_missing",
        "durable Sysop-only state requires recovery, not a new questionnaire");
    require(restarted.bootstrap(resume).code == app::LocalControlResponseCode::recovery_required,
        "matching credentials cannot repurpose first-run setup as recovery");
    require(restarted.shutdown().code == app::LocalControlResponseCode::ok,
        "resumed runtime stops cleanly");
}

void qualify_resume_after_system_and_configuration(const std::filesystem::path& root) {
    const std::uint16_t port = available_loopback_port();
    auto bootstrap = request("Spitfire-X Publication Resume QA", port);
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::setup_required,
            "publication-resume case starts clean");
        require(CreateDirectoryW((root / L"Config" / L"SFX.conf.pending").c_str(), nullptr) != FALSE,
            "install deterministic configuration-publication obstruction");
        require(host.bootstrap(bootstrap).code == app::LocalControlResponseCode::configuration_failure,
            "publication failure interrupts after durable Sysop and SY credentials");
        require(host.status().state == app::AlphaRuntimePresentationState::recovery_required,
            "publication failure is visibly recoverable, never reported ready");
        require(host.shutdown().code == app::LocalControlResponseCode::ok,
            "publication-interrupted runtime stops cleanly");
    }
    require(RemoveDirectoryW((root / L"Config" / L"SFX.conf.pending").c_str()) != FALSE,
        "remove deterministic publication obstruction");
    const std::wstring sid = win::current_process_user_sid_string();
    require(win::publish_operational_configuration_file(root, sid,
            bootstrap.configuration).succeeded(),
        "publish configuration to model interruption before completion marker");

    runtime::AlphaRuntimeHost restarted(root);
    require(restarted.start().code == app::LocalControlResponseCode::recovery_required &&
            restarted.status().configuration_loaded &&
            !restarted.status().bootstrap_complete,
        "published-config/incomplete-database boundary is detected explicitly");
    require(restarted.bootstrap(bootstrap).code == app::LocalControlResponseCode::recovery_required &&
            !restarted.status().bootstrap_complete,
        "published-config interruption cannot accept replacement first-run credentials");
    require(restarted.shutdown().code == app::LocalControlResponseCode::ok,
        "configuration-boundary resume stops cleanly");
}

void qualify_alpha_listener_policy() {
    const std::uint16_t port = available_loopback_port();
    {
        runtime::AlphaRuntimeHost host(fresh_case("node-count"));
        require(host.start().code == app::LocalControlResponseCode::setup_required, "node-count fixture initializes");
        auto setup = request("Stage 3 node count QA", port);
        setup.configuration.logical_nodes = 129;
        require(host.bootstrap(setup).diagnostic_code == "alpha_node_count_requires_1_to_128" &&
            host.status().state == app::AlphaRuntimePresentationState::setup_required,
            "Alpha rejects unsupported configured count before creating account/configuration, never clamps");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "node-count fixture stops cleanly");
    }
    {
        runtime::AlphaRuntimeHost host(fresh_case("disabled"));
        require(host.start().code == app::LocalControlResponseCode::setup_required,
            "disabled-listener policy case starts clean");
        auto disabled = request("Spitfire-X Disabled Listener QA", port);
        disabled.configuration.telnet.enabled = false;
        require(host.bootstrap(disabled).code == app::LocalControlResponseCode::configuration_failure,
            "Alpha setup rejects a disabled Telnet listener before publishing state");
        require(host.status().state == app::AlphaRuntimePresentationState::setup_required,
            "disabled-listener rejection leaves clean setup resumable");
        require(host.shutdown().code == app::LocalControlResponseCode::ok,
            "disabled-listener policy case stops cleanly");
    }
    {
        runtime::AlphaRuntimeHost host(fresh_case("bot-gate"));
        require(host.start().code == app::LocalControlResponseCode::setup_required,
            "Bot-Gate policy case starts clean");
        auto bot_gate = request("Spitfire-X Bot-Gate QA", available_loopback_port());
        bot_gate.configuration.telnet.bot_gate_enabled = true;
        require(host.bootstrap(bot_gate).code == app::LocalControlResponseCode::ok &&
            host.status().state == app::AlphaRuntimePresentationState::ready && host.status().telnet_bot_gate_enabled,
            "implemented Telnet participation persists; global Off still controls enforcement");
        require(host.shutdown().code == app::LocalControlResponseCode::ok,
            "Bot-Gate policy case stops cleanly");
    }
}

void qualify_provisioning_diagnostic(const std::filesystem::path& root) {
    std::filesystem::create_directory(root);
    const auto sid = win::current_process_user_sid_string();
    win::WindowsStorageDaclPolicy policy(sid);
    require(policy.apply(root, win::StorageDaclObjectKind::directory).succeeded(),
        "protect diagnostic fixture root");
    { std::ofstream file(root / "Bin"); file << "deliberately not a directory"; }
    runtime::AlphaRuntimeHost host(root);
    const auto failed = host.start();
    std::cout << "Malformed Bin startup diagnostic: " << failed.diagnostic_code << std::endl;
    require(failed.code == app::LocalControlResponseCode::recovery_required &&
        failed.diagnostic_code == "preparation_incomplete_open_control_panel_or_config_to_retry",
        "unreadable preparation marker through a file-shaped Bin refuses before installation provisioning");
    const auto status = host.status();
    require(status.security_policy_revision == 0 && status.security_policy_hash.empty(),
        "failed provisioning must not fabricate a bootstrap security policy");
    const auto blocked = app::bootstrap_prerequisite_failure(status);
    require(blocked && blocked->diagnostic_code == failed.diagnostic_code,
        "Control Panel setup gate preserves Runtime's provisioning diagnostic");
    auto setup = request("SFX provisioning failure QA", available_loopback_port());
    require(host.bootstrap(setup).diagnostic_code == failed.diagnostic_code,
        "direct bootstrap attempt also preserves original failure, not storage_unavailable");
    app::erase_bootstrap_secrets(setup);
    require(!std::filesystem::exists(root / "Config") && !std::filesystem::exists(root / "Data"),
        "failed directory correction stops before mutable provisioning");
    (void)host.shutdown();
}

void qualify_ssh_startup_diagnostic_priority(const std::filesystem::path& root) {
    const auto telnet = available_loopback_port();
    const auto ssh = distinct_loopback_port(telnet);
    auto setup = request("SFX SSH diagnostic priority QA", telnet);
    setup.configuration.ssh = {true, "127.0.0.1", ssh};
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::setup_required &&
            host.bootstrap(setup).code == app::LocalControlResponseCode::ok, "SSH diagnostic fixture initializes");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "diagnostic fixture stops before corruption");
    }
    app::erase_bootstrap_secrets(setup);
    auto saved = win::read_operational_configuration_file(root);
    require(saved.configuration.has_value(), "read diagnostic fixture configuration");
    saved.configuration->security_policy_hash.assign(64, 'a');
    require(win::publish_operational_configuration_file(root, win::current_process_user_sid_string(),
        *saved.configuration).succeeded(), "publish deliberately mismatched policy fixture");
    { std::ofstream key(root / "Config" / "ssh_host_ed25519.pem"); key << "invalid fixture key"; require(key.good(), "corrupt fixture key"); }
    const auto loads = runtime::AlphaSshBackend::statistics().key_load_attempts;
    runtime::AlphaRuntimeHost host(root);
    const auto failed = host.start();
    require(failed.code == app::LocalControlResponseCode::recovery_required &&
        failed.diagnostic_code == "security_snapshot_mismatch_or_publication_failed",
        "security snapshot binding diagnostic wins before SSH initialization");
    require(runtime::AlphaSshBackend::statistics().key_load_attempts == loads,
        "SSH backend not acquired when prior Runtime prerequisites failed");
    require(!host.status().telnet_active && !host.status().ssh_active, "no partial listeners on prerequisite failure");
    (void)host.shutdown();
}

void qualify_observable_shutdown(const std::filesystem::path& root) {
    std::promise<void> entered, release;
    std::atomic<unsigned> entered_count{0};
    auto waiting = entered.get_future();
    auto permit = release.get_future().share();
    runtime::AlphaRuntimeHost host(root, [&](runtime::AlphaCallerBoundary point) {
        if (point != runtime::AlphaCallerBoundary::accepted) return;
        if (++entered_count == 4) entered.set_value();
        if (permit.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
            throw std::runtime_error("bounded runner interlock timeout");
    });
    const auto port = available_loopback_port();
    auto setup = request("Stage 3 shutdown fixture", port);
    require(host.start().code == app::LocalControlResponseCode::setup_required &&
        host.bootstrap(setup).code == app::LocalControlResponseCode::ok, "shutdown fixture initializes");
    app::erase_bootstrap_secrets(setup);
    std::array<SOCKET, 4> callers{};
    for (auto& caller : callers) caller = connect_loopback(port);
    require(waiting.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
        "four independent workers simultaneously hold four accepted connections");
    auto stopping = std::async(std::launch::async, [&] { return host.shutdown(); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (host.status().state != app::AlphaRuntimePresentationState::stopping &&
        std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const auto repeated = host.shutdown();
    const auto pending = host.status();
    release.set_value(); // unblock the owning runner before making test assertions
    require(repeated.code == app::LocalControlResponseCode::shutting_down &&
        repeated.diagnostic_code == "shutdown_in_progress", "pending cleanup is never reported complete");
    require(pending.nodes.size() == 4 && pending.lifecycle.active_work.caller_sessions == 4,
        "in-progress shutdown retains real four-caller status until workers finish cleanup");
    require(stopping.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
        stopping.get().code == app::LocalControlResponseCode::ok, "cleanup joins before successful shutdown response");
    for (auto caller : callers) closesocket(caller);
    require(host.shutdown().diagnostic_code == "shutdown_already_complete", "only completed shutdown is idempotent success");
    const auto counts = runtime::alpha_caller_resource_counts();
    require(!counts.workers && !counts.contexts && !counts.sockets, "all four workers, contexts and accepted sockets gone after shutdown");
    std::cout << "PASS bounded four-worker concurrent shutdown: pending cleanup stays in-progress until every runner joins; zero worker/context/socket resources\n";
}

void qualify_exact_node_local_logon(const std::filesystem::path& root) {
    const auto public_port = available_loopback_port();
    runtime::AlphaRuntimeHost host(root);
    auto setup = request("F-06 exact-node local logon", public_port);
    require(host.start().code == app::LocalControlResponseCode::setup_required &&
        host.bootstrap(setup).code == app::LocalControlResponseCode::ok,
        "F-06 local-logon fixture initializes");
    app::erase_bootstrap_secrets(setup);

    const auto local = host.start_local_session({3});
    require(local.response.code == app::LocalControlResponseCode::ok &&
        local.host == "127.0.0.1" && local.port != 0 &&
        local.reservation.node == 3 && local.reservation.runtime_epoch == host.runtime_epoch() &&
        local.timeout_milliseconds == app::kAlphaLocalSessionTimeoutMilliseconds,
        "Runtime publishes one bounded loopback endpoint only after exact Node 3 reservation");
    const auto reserved = host.status();
    require(reserved.lifecycle.active_work.caller_sessions == 1 &&
        reserved.nodes[2].screen_active && reserved.nodes[2].state == sfx::core::NodeState::connecting &&
        reserved.nodes[2].source_ip == "127.0.0.1" &&
        reserved.nodes[2].assignment_sequence == local.reservation.generation &&
        reserved.nodes[0].state == sfx::core::NodeState::ready,
        "reserved exact node is visible without consuming a lower READY node");
    require(host.start_local_session({3}).response.diagnostic_code == "node_not_ready",
        "second reservation against occupied exact node fails without fallback");

    const SOCKET local_client = connect_loopback(local.port);
    const auto local_banner = receive_until(local_client, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    require(local_banner.find("Your Sysop is Alpha Sysop - Node 3\r\n") == 12,
        "one-use local endpoint enters the ordinary Telnet login on exact Node 3");
    const SOCKET second = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    require(second != INVALID_SOCKET, "create second local endpoint contender");
    sockaddr_in second_address{};
    second_address.sin_family = AF_INET;
    second_address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    second_address.sin_port = htons(local.port);
    const int second_result = connect(second,
        reinterpret_cast<const sockaddr*>(&second_address), sizeof(second_address));
    require(second_result == SOCKET_ERROR,
        "first accepted connection consumes the one-use local endpoint");
    closesocket(second);

    const SOCKET public_client = connect_loopback(public_port);
    const auto public_banner = receive_until(public_client, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    require(public_banner.find("Your Sysop is Alpha Sysop\r\n") == 12,
        "public admission remains lowest-free while exact Node 3 is occupied");
    closesocket(public_client);
    closesocket(local_client);
    const auto release_deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < release_deadline) {
        const auto current = host.status();
        if (current.nodes[0].state == sfx::core::NodeState::ready &&
            current.nodes[2].state == sfx::core::NodeState::ready) break;
        Sleep(10);
    }
    require(host.status().nodes[0].state == sfx::core::NodeState::ready &&
        host.status().nodes[2].state == sfx::core::NodeState::ready,
        "both public and exact-node sessions return their own nodes after cleanup");

    const auto cancelled = host.start_local_session({2});
    require(cancelled.response.code == app::LocalControlResponseCode::ok,
        "second exact-node reservation succeeds after independent cleanup");
    require(host.cancel_local_session({cancelled.reservation,
            app::AlphaLocalSessionCancelReason::client_launch_failed}).code ==
            app::LocalControlResponseCode::ok &&
        host.status().nodes[1].state == sfx::core::NodeState::ready,
        "client launch failure synchronously closes endpoint and returns Node 2 to READY");
    require(host.cancel_local_session({cancelled.reservation,
            app::AlphaLocalSessionCancelReason::client_launch_failed}).diagnostic_code ==
            "local_reservation_stale",
        "stale cancellation cannot affect a released or reassigned node");
    const auto events = host.status().logs;
    const auto has_kind = [&](const app::OperationalKind kind) {
        return std::any_of(events.begin(), events.end(), [&](const auto& entry) {
            return entry.event && entry.event->kind == kind &&
                entry.event->context.node == 2;
        });
    };
    require(has_kind(app::OperationalKind::local_session_requested) &&
        has_kind(app::OperationalKind::local_session_reserved) &&
        has_kind(app::OperationalKind::local_session_launch_failed),
        "local reservation lifecycle is visible as typed bounded operational events");
    require(host.shutdown().code == app::LocalControlResponseCode::ok,
        "F-06 local-logon fixture shuts down cleanly");
    const auto counts = runtime::alpha_caller_resource_counts();
    require(!counts.workers && !counts.contexts && !counts.sockets,
        "exact-node accept, cancellation and shutdown leave zero owned caller resources");
    std::cout << "PASS F-06 exact-node one-use loopback reservation, ordinary Telnet login, public isolation, stale cancel and zero resources\n";
}

void qualify_worker_and_assignment_boundaries() {
    for (unsigned fail_at = 1; fail_at <= 4; ++fail_at) {
        unsigned starts = 0;
        runtime::AlphaRuntimeHost host(fresh_case("partial-start-" + std::to_string(fail_at)),
            [&](runtime::AlphaCallerBoundary point) {
                if (point == runtime::AlphaCallerBoundary::worker_start && ++starts == fail_at)
                    throw std::runtime_error("private worker-start failure text");
            });
        auto setup = request("Stage 3 worker start failure", available_loopback_port());
        require(host.start().code == app::LocalControlResponseCode::setup_required, "partial start fixture initializes");
        require(host.bootstrap(setup).code == app::LocalControlResponseCode::recovery_required &&
            host.status().diagnostic_code == "telnet_listener_node_worker_start_failed",
            "injected partial worker startup fails closed with typed diagnostic");
        const auto counts = runtime::alpha_caller_resource_counts();
        require(!counts.workers && !counts.contexts && !counts.sockets, "partial startup joins every earlier worker before returning");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "partial worker start shuts down cleanly");
    }
    for (const bool ssh : {false, true}) for (const bool stop : {false, true}) {
        std::promise<void> entered, release;
        auto waiting = entered.get_future();
        auto permit = release.get_future().share();
        std::atomic<bool> held{false};
        runtime::AlphaRuntimeHost host(fresh_case(std::string(ssh ? "ssh-" : "telnet-") +
            (stop ? "stop-before-assignment" : "disable-before-assignment")),
            [&](runtime::AlphaCallerBoundary point) {
                if (point != runtime::AlphaCallerBoundary::before_assignment || held.exchange(true)) return;
                entered.set_value();
                if (permit.wait_for(std::chrono::seconds(10)) != std::future_status::ready)
                    throw std::runtime_error("bounded acceptor barrier expired");
            });
        const auto port = available_loopback_port();
        const auto ssh_port = distinct_loopback_port(port);
        auto setup = request("Stage 3 assignment race", port);
        setup.configuration.ssh = {ssh, "127.0.0.1", ssh_port};
        require(host.start().code == app::LocalControlResponseCode::setup_required &&
            host.bootstrap(setup).code == app::LocalControlResponseCode::ok, "assignment race fixture initializes");
        const auto caller = connect_loopback(ssh ? ssh_port : port);
        require(waiting.wait_for(std::chrono::seconds(5)) == std::future_status::ready,
            "raw accepted socket is deterministically held before pool assignment");
        if (stop) {
            auto stopping = std::async(std::launch::async, [&] { return host.shutdown(); });
            const auto deadline = GetTickCount64() + 5000;
            while (host.status().state != app::AlphaRuntimePresentationState::stopping &&
                GetTickCount64() < deadline) Sleep(1);
            release.set_value();
            require(stopping.wait_for(std::chrono::seconds(5)) == std::future_status::ready &&
                stopping.get().code == app::LocalControlResponseCode::ok, "stop wins before assignment without leaked accepted socket");
        } else {
            const auto disabled = host.command_node({1, app::AlphaNodeCommandKind::disable});
            release.set_value();
            require(disabled.code == app::LocalControlResponseCode::ok, "disable linearizes before pending accept");
            if (ssh) (void)receive_until(caller, "SSH-2.0-");
            else {
                const auto question = receive_until(caller, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
                require(question.find("Your Sysop is Alpha Sysop - Node 2\r\n") == 12 && question.find('\x1b') == std::string::npos,
                    "accepted Row1 identity identifies actual node 2 before the plain graphics question");
                send_text(caller, "N\r");
                const auto banner = receive_until(caller, "Enter your first name: ");
                require(banner.find("Your Sysop is") == std::string::npos, "identity is not repeated after graphics choice");
            }
            require(!host.status().nodes[0].screen_active && host.status().nodes[1].screen_active,
                "pending accept atomically chooses lowest remaining enabled node 2 on either transport");
            require(host.shutdown().code == app::LocalControlResponseCode::ok, "disabled-assignment race joins cleanly");
        }
        closesocket(caller);
        const auto counts = runtime::alpha_caller_resource_counts();
        require(!counts.workers && !counts.contexts && !counts.sockets, "assignment race releases all owned resources");
    }
    std::cout << "PASS four partial worker-start failures and four deterministic Telnet/SSH disable-before-assignment/stop-before-assignment races\n";
}

void qualify_log_write_failure(const std::filesystem::path& root) {
    const auto port = available_loopback_port();
    auto setup = request("Spitfire-X Alpha Runtime QA", port);
    {
        runtime::AlphaRuntimeHost initial(root);
        require(initial.start().code == app::LocalControlResponseCode::setup_required &&
            initial.bootstrap(setup).code == app::LocalControlResponseCode::ok, "logging failure fixture initializes");
        require(initial.shutdown().code == app::LocalControlResponseCode::ok,
            "retire cached journal writer before installing deliberate sharing obstruction");
    }
    const auto path = root / L"Logs" / L"SFXRuntime.log";
    // The rotating journal intentionally retains its writer for its lifetime.
    // Establish this real OS sharing failure before the next Runtime acquires it.
    const HANDLE held = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    require(held != INVALID_HANDLE_VALUE, "hold real operational log against write access");
    struct CloseFile { HANDLE value; ~CloseFile() { CloseHandle(value); } } close_file{held};
    const auto before = read_file(path);
    runtime::AlphaRuntimeHost host(root);
    require(host.start().code == app::LocalControlResponseCode::ok,
        "logging obstruction does not prevent ordinary caller handling on restart");
    const HANDLE denied = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (denied != INVALID_HANDLE_VALUE) CloseHandle(denied);
    require(denied == INVALID_HANDLE_VALUE && GetLastError() == ERROR_SHARING_VIOLATION,
        "actual log-write attempt fails with sharing violation, not a skipped fixture");
    qualify_real_telnet_login(host, port);
    require((host.status().log_health & app::operational_log_write_failed) != 0 &&
        !app::operational_log_health_text(host.status().log_health).empty(),
        "real log write obstruction is clearly reported without faulting caller service");
    require(host.shutdown().code == app::LocalControlResponseCode::ok,
        "real login/finalization/shutdown survive failed operational logging");
    const auto counts = runtime::alpha_caller_resource_counts();
    require(!counts.workers && !counts.contexts && !counts.sockets,
        "log failure cannot leak workers, contexts or accepted sockets");
    require(read_file(path) == before, "injected log obstruction actually prevented every append");
    std::cout << "PASS real log sharing failure: authenticated login, finalization and shutdown succeed; zero resources; blocked file unchanged\n";
}

using ProtectedSnapshot = std::map<std::filesystem::path, std::string>;
ProtectedSnapshot protected_snapshot(const std::filesystem::path& root) {
    ProtectedSnapshot result;
    for (const auto* leaf : {L"Config", L"Data", L"Backup", L"Logs"})
        for (const auto& entry : std::filesystem::recursive_directory_iterator(root / leaf))
            if (entry.is_regular_file() && entry.path().filename() != L"SFXRuntime.lock")
                result.emplace(entry.path(), read_file(entry.path()));
    return result;
}

void write_fixture(const std::filesystem::path& path, const std::string& contents) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    require(file.good(), "open isolated recovery fixture for writing");
    file.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    file.close();
    require(!file.fail(), "write isolated recovery fixture");
}

void assert_preserved_recovery(const std::filesystem::path& root, const std::string& diagnostic,
    const bool lock_configuration = false) {
    const auto before = protected_snapshot(root);
    {
        const HANDLE locked = lock_configuration ? CreateFileW((root / L"Config" / L"SFX.conf").c_str(),
            GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) : INVALID_HANDLE_VALUE;
        require(!lock_configuration || locked != INVALID_HANDLE_VALUE, "hold config against startup read");
        struct Close { HANDLE h; ~Close() { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); } } close{locked};
        runtime::AlphaRuntimeHost host(root);
        const auto response = host.start();
        const auto status = host.status();
        require(response.code == app::LocalControlResponseCode::recovery_required &&
            response.diagnostic_code == diagnostic && status.diagnostic_code == diagnostic,
            "recovery retains the exact missing/invalid/inspection diagnostic");
        require(status.state == app::AlphaRuntimePresentationState::recovery_required &&
            !status.configuration_loaded && !status.bootstrap_complete && status.nodes.empty() &&
            !status.telnet_active && !status.ssh_active && status.security_policy_revision == 0,
            "recovery happens before storage/policy/listeners/assignments are initialized");
        const auto wire = app::decode_runtime_status(app::encode_runtime_status(status));
        require(wire && wire->diagnostic_code == diagnostic &&
            app::bootstrap_prerequisite_failure(*wire)->diagnostic_code == diagnostic,
            "matched Control Panel round trip blocks setup without masking the diagnostic");
        auto attempted = request("Never create replacement credentials", available_loopback_port());
        const auto rejected = host.bootstrap(attempted);
        app::erase_bootstrap_secrets(attempted);
        require(rejected.code == app::LocalControlResponseCode::recovery_required &&
            rejected.diagnostic_code == diagnostic, "direct bootstrap cannot bypass recovery");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "recovery shuts down cleanly");
    }
    require(protected_snapshot(root) == before,
        "every protected config/data/backup/log file retains exact bytes and no new identity file appears");
    const auto resources = runtime::alpha_caller_resource_counts();
    require(!resources.workers && !resources.contexts && !resources.sockets,
        "recovery leaves no workers, contexts or sockets");
}

void qualify_configuration_recovery() {
    const auto sid = win::current_process_user_sid_string();
    const auto root = fresh_case("configuration-recovery");
    auto setup = request("C1 configuration QA", available_loopback_port());
    setup.configuration.ssh.enabled = true;
    setup.configuration.ssh.bind_address = "127.0.0.1";
    setup.configuration.ssh.port = distinct_loopback_port(setup.configuration.telnet.port);
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::setup_required &&
            host.bootstrap(setup).code == app::LocalControlResponseCode::ok,
            "C1 genuine fresh setup creates one working protected installation");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "C1 fresh setup joins");
    }
    const auto config = root / L"Config" / L"SFX.conf";
    const auto canonical = read_file(config);
    const auto key = read_file(root / L"Config" / L"ssh_host_ed25519.pem");
    std::string crlf;
    for (char c : canonical) { if (c == '\n') crlf += '\r'; crlf += c; }
    write_fixture(config, crlf);
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok &&
            host.status().telnet_active && host.status().ssh_active,
            "editor no-content-change CRLF round trip restarts both actual listeners");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "CRLF runtime joins");
    }
    require(read_file(config) == crlf && read_file(root / L"Config" / L"ssh_host_ed25519.pem") == key,
        "startup neither rewrites the edited configuration nor replaces the shared host key");
    win::WindowsStorageDaclPolicy policy(sid);
    require(policy.assess(config, win::StorageDaclObjectKind::file).status.succeeded() &&
        win::assess_alpha_bin_directory(root, sid).succeeded(), "CRLF retains protected config and exact Bin RX");
    require(win::publish_operational_configuration_file(root, sid,
        *win::read_operational_configuration_file(root).configuration).succeeded() &&
        read_file(config) == canonical, "protected republishing normalizes editor CRLF to canonical LF");

    std::filesystem::rename(config, root / L"Config" / L"SFX.conf.retained");
    assert_preserved_recovery(root, "configuration_missing");
    std::filesystem::rename(root / L"Config" / L"SFX.conf.retained", config);
    for (const auto& malformed : {std::string("format_version=999\r\n"),
            std::string("bbs_name=bad\rrecord\n"), std::string("bad\0data\n", 9)}) {
        write_fixture(config, malformed);
        assert_preserved_recovery(root, "configuration_invalid");
    }
    write_fixture(config, std::string(app::kOperationalConfigurationMaximumBytes + 1, 'x'));
    assert_preserved_recovery(root, "configuration_invalid_file");
    write_fixture(config, canonical);
    assert_preserved_recovery(root, "configuration_open_failed", true);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    require(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:P(A;;FA;;;WD)", SDDL_REVISION_1, &descriptor, nullptr) != FALSE,
        "build isolated overly broad config DACL fixture");
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    const bool have_dacl = GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != FALSE;
    const auto security_result = have_dacl && present ? SetNamedSecurityInfoW(
        const_cast<wchar_t*>(config.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, dacl, nullptr)
        : ERROR_INVALID_SECURITY_DESCR;
    LocalFree(descriptor);
    require(security_result == ERROR_SUCCESS, "apply overly broad fixture DACL without elevation");
    assert_preserved_recovery(root, "configuration_security_invalid");
    require(policy.apply(config, win::StorageDaclObjectKind::file).succeeded(), "restore isolated config protection");
    const auto db = root / L"Data" / L"SFX.DB";
    std::filesystem::rename(db, root / L"Data" / L"SFX.DB.retained");
    assert_preserved_recovery(root, "installation_database_missing");
    require(!std::filesystem::exists(db), "missing initialized database is never recreated");
    write_fixture(db, "");
    assert_preserved_recovery(root, "installation_database_invalid");
    require(std::filesystem::remove(db), "remove only empty disposable database fixture before restoring original");
    std::filesystem::rename(root / L"Data" / L"SFX.DB.retained", db);

    unsigned identities = 0;
    for (const auto* relative : {L"Config/ssh_host_ed25519.pem", L"Config/ssh_host_ed25519.pending",
            L"Config/SFX.conf.renamed", L"Config/SFX.pending.conf", L"Data/SFX.DB",
            L"Data/SFX.DB-wal", L"Backup/retained.backup", L"Logs/SFXRuntime.log"}) {
        const auto isolated = fresh_case("identity-only-" + std::to_string(++identities));
        require(win::provision_alpha_installation_tree(isolated, sid).succeeded(), "protect identity-only fixture");
        write_fixture(isolated / relative, "");
        require(policy.apply(isolated / relative, win::StorageDaclObjectKind::file).succeeded(),
            "protect identity evidence even when empty");
        assert_preserved_recovery(isolated, "configuration_missing");
    }
    const auto unfinished = fresh_case("setup-cancelled");
    require(win::provision_alpha_installation_tree(unfinished, sid).succeeded(), "protect empty-directory fixture");
    require(win::publish_control_panel_ui_preferences(unfinished, sid, {}).succeeded(),
        "UI preferences alone do not establish a BBS identity");
    {
        runtime::AlphaRuntimeHost host(unfinished);
        require(host.start().code == app::LocalControlResponseCode::setup_required,
            "empty protected directories plus UI-only preferences permit genuinely fresh setup");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "cancelled fresh setup shuts down");
    }
    assert_preserved_recovery(unfinished, "configuration_missing");
    std::cout << "PASS C1 recovery: CRLF real-listener restart and canonical protected publication; "
        "18 exact protected-tree preservation cases (excluding the empty coordination lock), "
        "blocked bootstrap and diagnostic wire round trips; "
        "fresh UI-only versus interrupted setup; unchanged shared host-key bytes and exact Bin RX\n";
}

void qualify_c2_settings(const std::filesystem::path& root) {
    const app::OperationalConfiguration defaults;
    require(defaults.allow_new_users && defaults.ssh.enabled && defaults.ssh.bind_address == "0.0.0.0" &&
        defaults.ssh.port == 2222, "C2 retains registration-On and enabled wildcard SSH port 2222 defaults");
    auto setup = request("C2 Settings QA", available_loopback_port());
    setup.configuration.ssh = {true, "127.0.0.1", distinct_loopback_port(setup.configuration.telnet.port)};
    const auto sid = win::current_process_user_sid_string();
    win::WindowsStorageDaclPolicy policy(sid);
    const auto key_path = root / L"Config" / L"ssh_host_ed25519.pem";
    const auto pending_path = root / L"Config" / L"SFX.pending.conf";
    std::string key;
    app::AlphaSettingsRequest active{setup.configuration.bbs_name, setup.configuration.telnet.bind_address,
        setup.configuration.telnet.port, true, setup.configuration.ssh.bind_address, setup.configuration.ssh.port, true};
    auto closed = active; closed.allow_new_users = false; closed.hostname_lookup = false;
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::setup_required, "C2 starts fresh setup");
        const auto before = host.status().storage.committed_operations;
        for (unsigned test = 0; test < 7; ++test) {
            auto invalid = setup;
            if (test < 3) {
                invalid.configuration.ssh.port = invalid.configuration.telnet.port;
                if (test == 1) invalid.configuration.ssh.bind_address = "0.0.0.0";
                if (test == 2) invalid.configuration.telnet.bind_address = "0.0.0.0";
            } else if (test == 3) invalid.configuration.ssh.bind_address = "127.0.0.999";
            else if (test == 4) invalid.configuration.ssh.port = 0;
            else if (test == 5) invalid.configuration.telnet.bind_address = "localhost";
            else invalid.configuration.telnet.port = 0;
            require(host.bootstrap(invalid).code == app::LocalControlResponseCode::configuration_failure &&
                host.status().storage.committed_operations == before &&
                host.status().state == app::AlphaRuntimePresentationState::setup_required &&
                !std::filesystem::exists(root / L"Config" / L"SFX.conf") && !std::filesystem::exists(key_path),
                "bad endpoints/equal or wildcard overlap reject before account/config/key commit");
            app::erase_bootstrap_secrets(invalid);
        }
        auto disabled = setup.configuration;
        disabled.ssh.enabled = false; disabled.ssh.port = disabled.telnet.port;
        disabled.security_policy_revision = host.status().security_policy_revision;
        disabled.security_policy_hash = host.status().security_policy_hash;
        require(app::validate_operational_configuration(disabled).valid(), "disabled SSH endpoint does not conflict");
        auto distinct = disabled; distinct.ssh.enabled = true; distinct.ssh.bind_address = "127.0.0.2";
        require(app::validate_operational_configuration(distinct).valid(), "same port on distinct specific IPv4 binds is not a conflict");
        require(host.bootstrap(setup).code == app::LocalControlResponseCode::ok &&
            host.status().telnet_active && host.status().ssh_active, "valid custom first-run endpoints commit and both start");
        key = read_file(key_path);
        const auto canonical = read_file(root / L"Config" / L"SFX.conf");
        require(host.stage_settings(closed).code == app::LocalControlResponseCode::ok, "stage registration Off through Runtime");
        const auto pending = host.status();
        const auto decoded = app::decode_runtime_status(app::encode_runtime_status(pending));
        require(decoded && decoded->allow_new_users && decoded->pending_settings &&
            decoded->hostname_lookup && !decoded->pending_settings->hostname_lookup &&
            !decoded->pending_settings->allow_new_users && decoded->listener_restart_required &&
            decoded->telnet_active && decoded->ssh_active, "active On and saved Off remain distinct over matched protocol");
        require(read_file(root / L"Config" / L"SFX.conf") == canonical && read_file(key_path) == key &&
            policy.assess(pending_path, win::StorageDaclObjectKind::file).status.succeeded() &&
            !win::read_pending_operational_configuration_file(root).configuration->allow_new_users &&
            !win::read_pending_operational_configuration_file(root).configuration->hostname_lookup,
            "pending publication is protected and does not alter active configuration or key");
        const auto pending_bytes = read_file(pending_path);
        const HANDLE pinned = CreateFileW(pending_path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        require(pinned != INVALID_HANDLE_VALUE, "pin isolated pending file against replacement/deletion");
        const auto cancel_failed = host.stage_settings(active);
        auto edited = closed; edited.bbs_name = "C2 blocked pending replacement";
        const auto replace_failed = host.stage_settings(edited);
        CloseHandle(pinned);
        require(cancel_failed.code == app::LocalControlResponseCode::configuration_failure &&
            replace_failed.code == app::LocalControlResponseCode::configuration_failure &&
            host.status().listener_restart_required && host.status().pending_settings &&
            host.status().pending_settings->bbs_name == active.bbs_name && read_file(pending_path) == pending_bytes,
            "failed pending removal/replacement never falsely reports cancellation or replaces saved snapshot");
        require(host.stage_settings(active).diagnostic_code == "settings_unchanged" &&
            !host.status().pending_settings && !host.status().listener_restart_required && !std::filesystem::exists(pending_path),
            "explicitly saving active values cancels pending only after durable removal succeeds");
        require(host.stage_settings(closed).code == app::LocalControlResponseCode::ok &&
            host.shutdown().code == app::LocalControlResponseCode::ok, "stage Off again, then orderly restart boundary");
    }
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok && !host.status().allow_new_users &&
            !host.status().hostname_lookup &&
            !host.status().pending_settings && host.status().ssh_active && read_file(key_path) == key,
            "restart activates registration Off and retains shared SSH identity");
        active.ssh_port = distinct_loopback_port(active.telnet_port);
        require(host.stage_settings(active).code == app::LocalControlResponseCode::ok &&
            !host.status().allow_new_users && host.status().pending_settings->allow_new_users &&
            !host.status().hostname_lookup && host.status().pending_settings->hostname_lookup,
            "stage On plus endpoint change without applying either early");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "C2 joins before activating On");
    }
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok && host.status().allow_new_users && host.status().hostname_lookup &&
            host.status().ssh_port == active.ssh_port && read_file(key_path) == key, "On/custom SSH endpoint survive restart with identical key");
        auto disabled = active; disabled.ssh_enabled = false; disabled.ssh_port = disabled.telnet_port;
        require(host.stage_settings(disabled).code == app::LocalControlResponseCode::ok && host.status().ssh_active,
            "SSH disable remains pending and may retain an overlapping inactive endpoint");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "C2 joins before SSH disable");
    }
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok && !host.status().ssh_active &&
            host.status().telnet_active && read_file(key_path) == key, "disabled SSH preserves Telnet and retained identity");
        require(host.stage_settings(active).code == app::LocalControlResponseCode::ok &&
            host.shutdown().code == app::LocalControlResponseCode::ok, "stage shared-identity SSH re-enable");
    }
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok && host.status().ssh_active &&
            read_file(key_path) == key, "SSH re-enable loads the same installation key");
        require(host.shutdown().code == app::LocalControlResponseCode::ok, "C2 final shutdown joins");
    }
    std::cout << "PASS C2: seven pre-commit endpoint rejections, custom dual-listener setup, protected active/pending policy, "
        "pending removal/replacement failures, cancellation, On/Off activation, SSH disable/re-enable and unchanged key\n";
}

void qualify_section3_controls(const std::filesystem::path& root) {
    const auto exact_target = std::filesystem::path(L"C:\\xtest-sfx-v002-installer-cr3-20260914");
    require(std::filesystem::equivalent(root, exact_target), "controls fixture is limited to the approved disposable target");
    const auto owner = win::current_process_user_sid_string();
    win::installation::verify_runtime_installation(root, owner);
    const auto receipt = win::installation::read_receipt(root, owner);
    require(receipt.id == L"42DFE5EEBF865D9C6B75FF83BEBAB6FA", "controls fixture verifies exact disposable receipt identity");
    const auto configured = win::read_operational_configuration_file(root);
    require(configured.configuration && configured.configuration->telnet.enabled &&
        configured.configuration->telnet.bind_address == "127.0.0.1" &&
        (!configured.configuration->ssh.enabled || configured.configuration->ssh.bind_address == "127.0.0.1") &&
        configured.configuration->logical_nodes == 4, "controls fixture requires existing four-node loopback configuration");
    const auto controls = win::load_alpha_node_controls(root, owner);
    require(controls && controls.value && std::all_of(controls.value->begin(), controls.value->end(),
        [](bool enabled) { return enabled; }), "controls fixture preserves existing enabled-node configuration");
    const auto port = configured.configuration->telnet.port;
    runtime::AlphaRuntimeHost host(root);
    const auto started = host.start();
    if (started.code != app::LocalControlResponseCode::ok)
        throw std::runtime_error("installed controls start: " + started.diagnostic_code);
    const SOCKET first = connect_loopback(port);
    (void)receive_until(first, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    auto state = host.status();
    const app::AlphaNodeCommandRequest captured{1, app::AlphaNodeCommandKind::disconnect,
        state.runtime_epoch, state.nodes[0].assignment_sequence};
    auto wrong_epoch = captured; wrong_epoch.expected_epoch.bytes[0] ^= 0xff;
    require(host.command_node(wrong_epoch).code == app::LocalControlResponseCode::invalid_request &&
        host.status().lifecycle.active_work.caller_sessions == 1, "wrong Runtime epoch cannot disconnect a caller");
    require(host.command_node(captured).code == app::LocalControlResponseCode::ok,
        "captured caller disconnect completes normal cleanup");
    closesocket(first);
    require(host.status().nodes[0].enabled && host.status().nodes[0].state == sfx::core::NodeState::ready,
        "disconnect does not disable the node");
    const SOCKET replacement = connect_loopback(port);
    (void)receive_until(replacement, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    require(host.command_node(captured).diagnostic_code == "caller_assignment_changed" &&
        host.status().lifecycle.active_work.caller_sessions == 1,
        "stale disconnect leaves replacement caller untouched");
    const auto reserved = host.start_local_session({3});
    require(reserved.response.code == app::LocalControlResponseCode::ok, "reserve local node before drain");
    require(host.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain, 60}).code ==
        app::LocalControlResponseCode::ok, "finite drain begins with live caller");
    state = host.status();
    require(state.state == app::AlphaRuntimePresentationState::draining &&
        state.lifecycle.state == app::RuntimeState::draining &&
        state.lifecycle.active_work.caller_sessions == 1 && !state.nodes[2].screen_active &&
        app::decode_runtime_status(app::encode_runtime_status(state)).has_value(),
        "drain is wire-visible, preserves caller and retires pending local reservation");
    require(host.start_local_session({2}).response.code == app::LocalControlResponseCode::invalid_request &&
        host.command_node({2, app::AlphaNodeCommandKind::disable}).code == app::LocalControlResponseCode::invalid_request,
        "drain refuses new local admission and unrelated node mutations");
    const SOCKET refused = connect_loopback(port);
    const auto refusal = receive_until(refused, "\r\n");
    require(refusal.find("Do you want ANSI") == std::string::npos &&
        host.status().lifecycle.active_work.caller_sessions == 1, "public drain refusal allocates no logical node");
    closesocket(refused);
    require(host.command_lifecycle({app::AlphaLifecycleCommandKind::cancel_drain, 0}).code ==
        app::LocalControlResponseCode::ok && host.status().state == app::AlphaRuntimePresentationState::ready,
        "cancel drain restores admission without dropping original caller");
    const SOCKET resumed = connect_loopback(port);
    (void)receive_until(resumed, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
    require(host.status().lifecycle.active_work.caller_sessions == 2, "public caller admitted after cancellation");
    require(host.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain, 1}).code ==
        app::LocalControlResponseCode::ok, "short real drain starts");
    // Deliberately no status requests while the real deadline elapses: Runtime
    // must enforce this independently of the Control Panel's polling.
    Sleep(1800);
    const auto deadline = GetTickCount64() + 10000;
    do { state = host.status(); if (state.lifecycle.state == app::RuntimeState::stopped) break; Sleep(10); }
        while (GetTickCount64() < deadline);
    require(state.lifecycle.state == app::RuntimeState::stopped &&
        state.lifecycle.active_work.caller_sessions == 0 && !state.telnet_active,
        "real deadline ends both callers and shuts down without requiring a status poll");
    closesocket(replacement); closesocket(resumed);
    require(host.command_lifecycle({app::AlphaLifecycleCommandKind::cancel_drain, 0}).code ==
        app::LocalControlResponseCode::invalid_request && host.shutdown().code == app::LocalControlResponseCode::ok,
        "completed shutdown cannot be cancelled and acknowledgement is idempotent");
    const auto counts = runtime::alpha_caller_resource_counts();
    require(!counts.workers && !counts.contexts && !counts.sockets,
        "drain leaves zero owned caller workers, contexts and sockets");
    std::cout << "PASS Section 3 disconnect/reuse, admission, cancellation and real deadline shutdown\n";
}

void qualify_section3_password_policy(const std::filesystem::path& root) {
    require(std::filesystem::equivalent(root,std::filesystem::path(L"C:\\xtest-sfx-v002-installer-cr3-20260914")),
        "password-policy fixture is restricted to the authorized disposable target");
    const auto owner = win::current_process_user_sid_string();
    win::installation::verify_runtime_installation(root,owner);
    require(win::installation::read_receipt(root,owner).id == L"42DFE5EEBF865D9C6B75FF83BEBAB6FA",
        "password-policy fixture verifies existing exact installation identity");
    require(win::installation::file_sha256(root / L"Backup" / L"SFX-setup-backup-20260920-105512.zip") ==
        "C31A685D0C0E3F67ABD3F85A7B8CA229F5DD47A255ED65C7D27A71F6B20EB826",
        "verified retained recovery archive precedes policy mutation");
    const auto before = win::read_operational_configuration_file(root);
    require(before.configuration && before.configuration->password_mode == app::PasswordMode::standard &&
        before.configuration->telnet.bind_address == "127.0.0.1" && before.configuration->ssh.bind_address == "127.0.0.1",
        "existing Standard/loopback policy is required, not overwritten if unexpected");
    const auto original = *before.configuration;
    {
        runtime::AlphaRuntimeHost host(root);
        const auto started = host.start();
        require(started.code == app::LocalControlResponseCode::ok,"verified policy Runtime starts");
        win::OfflineInstallationGuard offline;
        require(!offline.acquire(root,owner),"active Runtime denies a second offline configuration owner");
        auto changed = original; ++changed.configuration_revision; changed.password_mode = app::PasswordMode::strong;
        const app::AlphaConfigurationUpdateRequest update{original.configuration_revision,changed};
        const auto result = host.update_configuration(update);
        require(result.code == app::LocalControlResponseCode::ok && result.activation == app::AlphaConfigurationActivation::restart_required,
            "running Strong selection is saved as restart-required");
        const auto snapshot = host.configuration_snapshot();
        require(snapshot && snapshot->active.password_mode == app::PasswordMode::standard &&
            snapshot->saved.password_mode == app::PasswordMode::strong,"active and saved password policies are truthful");
        require(host.update_configuration(update).code == app::LocalControlResponseCode::invalid_request,
            "stale duplicate configuration request refuses");
        require(host.shutdown().code == app::LocalControlResponseCode::ok,"policy test shutdown joins normally");
    }
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok,"Strong policy restarts with embedded blocklist");
        const auto active = host.configuration_snapshot();
        require(active && active->active.password_mode == app::PasswordMode::strong &&
            active->activation == app::AlphaConfigurationActivation::applied,"Strong is active only after restart");
        auto restored = original; restored.configuration_revision = active->saved.configuration_revision + 1;
        const auto result = host.update_configuration({active->saved.configuration_revision,restored});
        require(result.code == app::LocalControlResponseCode::ok && result.activation == app::AlphaConfigurationActivation::restart_required,
            "explicit restore of original Standard policy is restart-required too");
        require(host.shutdown().code == app::LocalControlResponseCode::ok,"Strong test host stops before restoring activation");
    }
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok,"restored Standard host starts");
        const auto final = host.configuration_snapshot();
        auto expected = original; expected.configuration_revision += 2;
        require(final && final->active.password_mode == original.password_mode &&
            final->active.configuration_revision == original.configuration_revision+2 &&
            app::equivalent_operational_configuration(final->active,expected),
            "original semantic configuration restored with honest monotonic revision");
        require(host.shutdown().code == app::LocalControlResponseCode::ok,"final policy host closes normally");
    }
    const auto counts = runtime::alpha_caller_resource_counts();
    require(!counts.workers && !counts.contexts && !counts.sockets,"policy restart sequence leaves no owned caller resources");
    require(win::installation::file_sha256(root / L"Backup" / L"SFX-setup-backup-20260920-105512.zip") ==
        "C31A685D0C0E3F67ABD3F85A7B8CA229F5DD47A255ED65C7D27A71F6B20EB826","recovery archive remains exact");
}

void qualify_section3_drain_completion(const std::filesystem::path& root) {
    for (const std::string mode : {"idle", "natural", "escalate"}) {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code == app::LocalControlResponseCode::ok, "restart after completed drain");
        SOCKET caller = INVALID_SOCKET;
        if (mode != "idle") {
            caller = connect_loopback(host.status().telnet_port);
            (void)receive_until(caller, "Do you want ANSI COLOR GRAPHICS? [Y/n] ");
        }
        require(host.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain, 60}).code ==
            app::LocalControlResponseCode::ok, "begin idle/natural/escalation drain");
        if (mode == "natural") { closesocket(caller); caller = INVALID_SOCKET; }
        if (mode == "escalate") require(host.shutdown().code == app::LocalControlResponseCode::ok,
            "explicit shutdown escalates a drain immediately with orderly cleanup");
        const auto deadline = GetTickCount64() + 5000;
        while (host.status().lifecycle.state != app::RuntimeState::stopped && GetTickCount64() < deadline) Sleep(10);
        require(host.status().lifecycle.state == app::RuntimeState::stopped,
            "idle/natural/escalated drain completes before its sixty-second deadline");
        if (caller != INVALID_SOCKET) closesocket(caller);
        const auto counts = runtime::alpha_caller_resource_counts();
        require(!counts.workers && !counts.contexts && !counts.sockets,
            "each drain completion reaps all owned caller resources");
    }
    std::cout << "PASS Section 3 idle/natural/escalated drain and restart\n";
}

void qualify_section3_reporting(const std::filesystem::path& root) {
    require(std::filesystem::equivalent(root,std::filesystem::path(L"C:\\xtest-sfx-v002-installer-cr3-20260914")),
        "reporting fixture is restricted to the authorized disposable target");
    const auto owner=win::current_process_user_sid_string();
    win::installation::verify_runtime_installation(root,owner);
    require(win::installation::read_receipt(root,owner).id==L"42DFE5EEBF865D9C6B75FF83BEBAB6FA",
        "reporting fixture verifies exact installation receipt");
    const auto recovery=root / L"Backup" / L"SFX-setup-backup-20260920-105512.zip";
    require(win::installation::file_sha256(recovery)==
        "C31A685D0C0E3F67ABD3F85A7B8CA229F5DD47A255ED65C7D27A71F6B20EB826","reporting recovery archive verified");
    const auto configuration=win::read_operational_configuration_file(root);
    require(configuration.configuration && configuration.configuration->telnet.enabled &&
        configuration.configuration->telnet.bind_address=="127.0.0.1" &&
        (!configuration.configuration->ssh.enabled || configuration.configuration->ssh.bind_address=="127.0.0.1"),
        "reporting test will never expose a public listener");
    const auto prior_config=read_file(root / L"Config" / L"SFX.conf");
    app::Identifier128 cleared_audit;
    {
        runtime::AlphaRuntimeHost host(root);
        require(host.start().code==app::LocalControlResponseCode::ok,"reporting host starts verified target");
        auto state=host.status();
        require(state.reporting.available && state.administration_history.available &&
            !state.administration_history.recent.empty(),"read-only Events includes actual persisted account/security history");
        require(app::decode_runtime_status(app::encode_runtime_status(state)).has_value(),"real idle status with reporting encodes");
        const auto idle=[&] {
            (void)host.status(); (void)host.metrics();
            for (unsigned i=0;i<20;++i) { Sleep(10); (void)host.status(); }
            const auto metrics=host.metrics();
            require(metrics.sqlite.interval_available && metrics.sqlite.page_reads_per_second==0 &&
                metrics.sqlite.page_writes_per_second==0,"repeated idle reporting polls cause zero SQLite page reads/writes");
        };
        idle();
        require(host.clear_throttle({app::RateLimitScope::source,"127.0.0.254"}).code==app::LocalControlResponseCode::ok,
            "owner clears only one explicit unused fixture source bucket");
        state=host.status();
        require(state.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::throttle_clears)]==1,
            "successful clear produces exactly one Runtime counter");
        const auto audit=std::find_if(state.administration_history.recent.begin(),state.administration_history.recent.end(),
            [](const auto& value) { return value.authentication &&
                value.kind==static_cast<unsigned>(app::AuthenticationAuditEventKind::throttling_cleared); });
        require(audit!=state.administration_history.recent.end(),"writer commit refresh exposes the actual durable throttle audit");
        cleared_audit=audit->event_id;
        idle();
        const SOCKET caller=connect_loopback(state.telnet_port);
        (void)receive_until(caller,"Do you want ANSI COLOR GRAPHICS? [Y/n] ");
        require(host.command_lifecycle({app::AlphaLifecycleCommandKind::begin_drain,60}).code==app::LocalControlResponseCode::ok &&
            host.command_lifecycle({app::AlphaLifecycleCommandKind::cancel_drain,0}).code==app::LocalControlResponseCode::ok,
            "held real caller exercises drain/cancel reporting without completing shutdown");
        state=host.status();
        require(state.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::drains_started)]==1 &&
            state.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::drains_cancelled)]==1 &&
            state.reporting.counts[0]==1,"accepted connection and lifecycle transitions counted once");
        closesocket(caller);
        const auto disconnected=GetTickCount64()+5000;
        do { state=host.status(); if (!state.lifecycle.active_work.caller_sessions) break; Sleep(10); }
        while (GetTickCount64()<disconnected);
        require(!state.lifecycle.active_work.caller_sessions,"fixture caller completely retires before backup");
        require(state.scheduler.configuration_loaded && !state.scheduler.active && !state.scheduler.queued,
            "no unexpected existing backup overlaps reporting test");
        require(host.run_scheduler_now({std::string(app::kBackupJobId)}).code==app::LocalControlResponseCode::ok,
            "real managed backup queued through Runtime");
        const auto deadline=GetTickCount64()+30000;
        do {
            state=host.status();
            if (state.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::backups_completed)] ||
                state.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::backups_failed)]) break;
            Sleep(20);
        } while (GetTickCount64()<deadline);
        if (state.scheduler.last_result!=app::ScheduledJobResult::success)
            throw std::runtime_error("reporting backup failed: "+state.scheduler.last_detail);
        require(state.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::backups_completed)]==1 &&
            state.reporting.counts[static_cast<std::size_t>(app::OperationalCounter::backups_failed)]==0 &&
            !state.scheduler.active,"only completed verified backup increments success counter");
        require(app::decode_runtime_status(app::encode_runtime_status(state)).has_value(),"real changed reporting status remains bounded and decodable");
        std::cout << "Reporting verified backup: " << state.scheduler.last_detail << '\n';
        idle();
        require(host.shutdown().code==app::LocalControlResponseCode::ok,"reporting host shutdown succeeds");
    }
    {
        runtime::AlphaRuntimeHost restarted(root);
        require(restarted.start().code==app::LocalControlResponseCode::ok,"reporting restart succeeds");
        const auto state=restarted.status();
        require(state.reporting.available && std::all_of(state.reporting.counts.begin(),state.reporting.counts.end(),
            [](auto count) { return count==0; }),"new Runtime counters reset without replaying durable events");
        require(state.administration_history.available &&
            std::any_of(state.administration_history.recent.begin(),state.administration_history.recent.end(),
                [&](const auto& row) { return row.event_id==cleared_audit; }),"actual SQLite audit remains after restart");
        require(state.scheduler.last_result==app::ScheduledJobResult::success,"backup result remains durable independently of reset counters");
        require(restarted.shutdown().code==app::LocalControlResponseCode::ok,"restarted reporting host shuts down");
    }
    require(read_file(root / L"Config" / L"SFX.conf")==prior_config,"reporting test leaves configuration byte-identical");
    require(win::installation::file_sha256(recovery)==
        "C31A685D0C0E3F67ABD3F85A7B8CA229F5DD47A255ED65C7D27A71F6B20EB826","retained recovery backup unchanged");
    const auto resources=runtime::alpha_caller_resource_counts();
    require(!resources.workers && !resources.contexts && !resources.sockets,"reporting fixture leaves zero owned caller resources");
}

#include "live_security_host_cases.inl"
}  // namespace

int main(int argc, char** argv) {
    try {
        Winsock winsock;
        if (argc == 3 && std::string(argv[1]) == "--provisioning-diagnostic-only") {
            const auto root = std::filesystem::absolute(argv[2]);
            const auto build = std::filesystem::weakly_canonical(SFX_QUALIFICATION_BUILD_ROOT);
            require(root.parent_path() == build && !std::filesystem::exists(root),
                "provisioning diagnostic uses a new direct child of this configured build");
            qualify_provisioning_diagnostic(root);
            std::cout << "PASS isolated malformed-Bin diagnostic and bootstrap refusal\n";
            return 0;
        }
        if (argc==3 && std::string(argv[1])=="--installed-reporting") {
            qualify_section3_reporting(std::filesystem::absolute(argv[2]));
            std::cout << "SFX installed Section 3 reporting PASS: " << assertions << " assertions; no credential or configuration changes\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--installed-password-policy") {
            qualify_section3_password_policy(std::filesystem::absolute(argv[2]));
            std::cout << "SFX installed password-policy PASS: " << assertions << " assertions; no credential changes\n";
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--installed-controls") {
            qualify_section3_controls(std::filesystem::absolute(argv[2]));
            qualify_section3_drain_completion(std::filesystem::absolute(argv[2]));
            std::cout << "SFX Section 3 operating controls PASS: " << assertions << " assertions\n";
            return 0; // never remove installed data or change its credentials
        }
        require(argc == 2, "Alpha runtime host tests require an output directory");
        const std::filesystem::path output = std::filesystem::absolute(argv[1]);
        require(!std::filesystem::exists(output), "new retained Runtime evidence directory required");
        std::filesystem::create_directories(output);
        sfx::test::InstalledQualificationFixture fixture(output / "installed-cases");
        installed_fixture = &fixture;
        qualify_complete_restart_and_fail_closed_config(fresh_case("complete"));
        qualify_resume_after_sysop_stage(fresh_case("sysop-resume"));
        qualify_resume_after_system_and_configuration(fresh_case("config-resume"));
        qualify_alpha_listener_policy();
        qualify_settings_activation_and_fallback();
        qualify_ssh_fail_closed();
        qualify_provisioning_diagnostic(output / "provisioning-diagnostic");
        qualify_ssh_startup_diagnostic_priority(fresh_case("ssh-diagnostic-priority"));
        qualify_observable_shutdown(fresh_case("observable-shutdown"));
        qualify_exact_node_local_logon(fresh_case("exact-node-local-logon"));
        qualify_worker_and_assignment_boundaries();
        qualify_log_write_failure(fresh_case("logging-failure"));
        qualify_configuration_recovery();
        qualify_c2_settings(fresh_case("c2-settings"));
        qualify_live_security_administration(fresh_case("live-security-administration"));
        fixture.finish();
        installed_fixture = nullptr;
        std::cout << "SFX Alpha runtime-host integration tests PASS: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Alpha runtime-host integration tests FAIL: " << error.what() << '\n';
        return 1;
    }
}
