// Test-only host for external-client interoperability. Never linked into SFXRuntime.
#include "alpha_runtime_host.hpp"
#include "alpha_ssh_stream.hpp"
#include "alpha_hostname_lookup.hpp"
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <atomic>
#include <stdexcept>
#include <string>

namespace app = sfx::application;
namespace runtime = sfx::apps::runtime;

int main(int argc, char** argv) {
    try {
        if (argc < 4 || argc > 6) return 2;
        const bool allow_new_users = argc >= 5 && std::string(argv[4]) == "registration";
        // Explicit one-configured-node regression fixture, not a production cap.
        // Stage 3 matrix requests four through this test-only argument.
        const auto nodes = argc == 6 ? std::stoul(argv[5]) : 1UL;
        if (nodes < 1 || nodes > 4) return 2;
        const auto root = std::filesystem::absolute(argv[1]).lexically_normal();
        const auto permitted = std::filesystem::path(SFX_SSH_TEST_SCRATCH).lexically_normal();
        if (root.parent_path() != permitted || root.filename().wstring().find(L"ssh-qa-") != 0) return 3;
        const auto telnet_port = std::stoul(argv[2]);
        const auto ssh_port = std::stoul(argv[3]);
        if (!telnet_port || telnet_port > 65535 || !ssh_port || ssh_port > 65535 ||
            ssh_port == telnet_port) return 4;
        std::atomic<int> failure_boundary{-1};
        runtime::AlphaRuntimeHost host(root, [&](runtime::AlphaCallerBoundary point) {
            int expected = static_cast<int>(point);
            if (failure_boundary.compare_exchange_strong(expected, -1))
                throw std::runtime_error("SFX-stage2-injected-secret-must-not-escape");
        });
        auto result = host.start();
        if (result.code == app::LocalControlResponseCode::setup_required) {
            app::AlphaBootstrapRequest bootstrap;
            bootstrap.configuration.bbs_name = "SFX SSH QA";
            bootstrap.configuration.allow_new_users = allow_new_users;
            bootstrap.configuration.logical_nodes = static_cast<std::uint16_t>(nodes);
            bootstrap.configuration.bbs_time_zone = "Pacific Standard Time";
            bootstrap.configuration.telnet = {true, "127.0.0.1",
                static_cast<std::uint16_t>(telnet_port), false};
            bootstrap.configuration.botgate.enabled=false; // This legacy matrix predates BotGate.
            bootstrap.configuration.ssh = {true, "127.0.0.1", static_cast<std::uint16_t>(ssh_port)};
            bootstrap.sysop_name = "SSH Fixture";
            bootstrap.sysop_password = "Sfx-test-password-123";
            bootstrap.system_password = "Sfx-system-password-456";
            result = host.bootstrap(bootstrap);
            app::erase_bootstrap_secrets(bootstrap);
        }
        if (result.code != app::LocalControlResponseCode::ok || host.status().bbs_name != "SFX SSH QA") {
            std::cout << "FAILED " << result.diagnostic_code << std::endl;
            (void)host.shutdown();
            return 5;
        }
        std::cout << "READY" << std::endl;
        std::string command;
        while (std::getline(std::cin, command)) {
            if (command == "stop") break;
            if (command.size() == 6 && command.compare(0, 5, "fail ") == 0 &&
                command[5] >= '0' && command[5] <= '3') {
                failure_boundary.store(command[5] - '0');
                std::cout << "ARMED" << std::endl;
            }
            if (command == "backend") {
                const auto counts = runtime::AlphaSshBackend::statistics();
                std::cout << "BACKEND " << counts.registration_attempts << ' '
                    << counts.key_generation_attempts << ' ' << counts.key_load_attempts << ' '
                    << counts.live_sessions << std::endl;
            }
            if (command == "resources") {
                const auto counts = runtime::alpha_caller_resource_counts();
                std::cout << "RESOURCES " << counts.workers << ' ' << counts.contexts << ' '
                    << counts.sockets << std::endl;
            }
            if (command == "registration 0" || command == "registration 1") {
                const auto status = host.status();
                const auto saved = host.stage_settings({status.bbs_name, status.telnet_bind,
                    status.telnet_port, status.ssh_configured, status.ssh_bind, status.ssh_port,
                    command.back() == '1', status.hostname_lookup});
                std::cout << "SETTINGS " << (saved.code == app::LocalControlResponseCode::ok) << std::endl;
            }
            if (command == "disable" || command == "enable") {
                const auto response = host.command_node({1, command == "enable"
                    ? app::AlphaNodeCommandKind::enable : app::AlphaNodeCommandKind::disable});
                std::cout << "NODE " << (response.code == app::LocalControlResponseCode::ok) << std::endl;
            }
            if (command.size() == 8 && command.compare(0, 5, "node ") == 0 &&
                command[5] >= '1' && command[5] <= '4' && command[6] == ' ' &&
                (command[7] == '0' || command[7] == '1')) {
                const auto response = host.command_node({static_cast<std::uint16_t>(command[5] - '0'),
                    command[7] == '1' ? app::AlphaNodeCommandKind::enable : app::AlphaNodeCommandKind::disable});
                std::cout << "NODE " << (response.code == app::LocalControlResponseCode::ok) << std::endl;
            }
            if (command == "nodes") {
                // Exercise the unchanged real Control Panel status codec with
                // four complete screens, rather than only inspecting host objects.
                const auto decoded = app::decode_runtime_status(app::encode_runtime_status(host.status()));
                if (!decoded) throw std::runtime_error("four-node status codec rejected live snapshot");
                const auto& status = *decoded;
                std::cout << "{\"active\":" << status.lifecycle.active_work.caller_sessions
                    << ",\"diagnostic\":" << std::quoted(status.diagnostic_code) << ",\"nodes\":[";
                bool first = true;
                for (const auto& node : status.nodes) {
                    if (!first) std::cout << ',';
                    first = false;
                    std::string screen, attributes;
                    constexpr char hex[] = "0123456789abcdef";
                    for (const auto& cell : node.screen.cells) {
                        const auto byte = static_cast<unsigned char>(cell.character);
                        screen += hex[byte >> 4]; screen += hex[byte & 15];
                        attributes += hex[cell.attribute >> 4]; attributes += hex[cell.attribute & 15];
                    }
                    std::cout << "{\"node\":" << node.number << ",\"generation\":" << node.assignment_sequence
                        << ",\"enabled\":" << node.enabled << ",\"available\":" << node.available
                        << ",\"active\":" << node.screen_active << ",\"columns\":" << node.terminal_columns
                        << ",\"rows\":" << node.terminal_rows << ",\"caller\":" << std::quoted(node.caller_handle)
                        << ",\"connection\":" << std::quoted(node.connection) << ",\"activity\":" << std::quoted(node.activity)
                        << ",\"source\":" << std::quoted(node.source_ip) << ",\"hostname\":" << std::quoted(node.hostname)
                        << ",\"details\":" << std::quoted(node.details) << ",\"screen\":" << std::quoted(screen)
                        << ",\"attributes\":" << std::quoted(attributes) << '}';
                }
                std::cout << "]}" << std::endl;
            }
            if (command == "audit") {
                const auto status = host.status();
                std::string visible;
                for (const auto& node : status.nodes) {
                    visible += node.caller_handle + node.details;
                    for (const auto& cell : node.screen.cells)
                        visible.push_back(static_cast<char>(cell.character));
                }
                for (const auto& log : status.logs) visible += log.level + log.component + log.message +
                    app::encode_operational_record(log);
                bool clean = true;
                for (const char* secret : {"Registration-actual-password-789", "discard-this-ssh-password",
                    "Sfx-test-password-123", "Sfx-system-password-456"})
                    clean = clean && visible.find(secret) == std::string::npos;
                std::cout << "AUDIT " << clean << std::endl;
            }
            if (command == "status") {
                const auto status = host.status();
                const auto& node = status.nodes.front();
                std::cout << "STATUS " << status.telnet_active << ' ' << status.ssh_active << ' '
                    << status.lifecycle.active_work.caller_sessions << ' ' << (node.connection.empty() ? "-" : node.connection) << ' '
                    << static_cast<unsigned>(node.state) << ' ' << node.terminal_columns << ' '
                    << node.terminal_rows << ' ' << status.statistics.successful_logons.total << std::endl;
            }
            if (command == "inspect") {
                const auto status = host.status();
                const auto& node = status.nodes.front();
                std::string screen;
                constexpr char hex[] = "0123456789abcdef";
                for (const auto& cell : node.screen.cells) {
                    const auto byte = static_cast<unsigned char>(cell.character);
                    screen += hex[byte >> 4]; screen += hex[byte & 15];
                }
                bool higher_unavailable = true;
                for (const auto& other : status.nodes) if (other.number > 1)
                    higher_unavailable = higher_unavailable && !other.available && !other.enabled;
                std::cout << "{\"assignment\":" << node.assignment_sequence
                    << ",\"active\":" << status.lifecycle.active_work.caller_sessions
                    << ",\"screen_active\":" << node.screen_active
                    << ",\"higher_unavailable\":" << higher_unavailable
                    << ",\"caller\":" << std::quoted(node.caller_handle)
                    << ",\"source\":" << std::quoted(node.source_ip)
                    << ",\"connection\":" << std::quoted(node.connection)
                    << ",\"activity\":" << std::quoted(node.activity)
                    << ",\"details\":" << std::quoted(node.details)
                    << ",\"diagnostic\":" << std::quoted(status.diagnostic_code)
                    << ",\"screen\":" << std::quoted(screen) << "}" << std::endl;
            }
        }
        result = host.shutdown();
        runtime::close_alpha_hostname_process();
        const auto resources = runtime::alpha_caller_resource_counts();
        const auto ssh_resources = runtime::AlphaSshBackend::statistics();
        if (resources.workers || resources.contexts || resources.sockets || ssh_resources.live_sessions ||
            runtime::alpha_hostname_worker_count()) {
            std::cout << "FAILED leaked_caller_resources" << std::endl;
            return 8;
        }
        std::cout << "STOPPED " << (result.code == app::LocalControlResponseCode::ok) << std::endl;
        return result.code == app::LocalControlResponseCode::ok ? 0 : 6;
    } catch (...) {
        std::cout << "FAILED test_host_exception" << std::endl;
        return 7;
    }
}
