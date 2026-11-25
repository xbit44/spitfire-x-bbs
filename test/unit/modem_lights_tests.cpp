#include "sfx/application/local_control_protocol.hpp"
#include <iostream>
#include <stdexcept>

namespace app = sfx::application;
unsigned checks = 0;
void require(bool value, const char* label) { ++checks; if (!value) throw std::runtime_error(label); }

int main() {
    try {
        {
            app::AlphaRuntimeStatus status;
            status.runtime_epoch.bytes[0] = 1;
            app::AlphaNodeStatus node; node.number = 1;
            status.nodes.push_back(node);
            app::AlphaNodeIndicators labels;
            labels.observe(status, 0);
            require(!labels.bold(1, 0), "ready node never blinks");
            auto& caller = status.nodes[0];
            caller.caller_connected = true; caller.assignment_sequence = 1;
            labels.observe(status, 1000); // Before a screen or authenticated account exists.
            for (std::uint64_t time = 1000; time <= 2300; time += 25) {
                ++caller.screen.sequence; ++caller.receive_activity; ++caller.send_activity;
                caller.screen_active = time >= 1400;
                caller.activity = std::to_string(time);
                labels.observe(status, time);
                const bool expected = time < 2000 && ((time - 1000) / 250) % 2 == 0;
                require(labels.bold(1, time) == expected,
                    "exactly two 250ms blinks despite continuous traffic/auth/status updates");
            }
            caller.caller_connected = false; labels.observe(status, 2400);
            require(!labels.bold(1, 2400), "disconnect clears label pulse");
            caller.caller_connected = true; ++caller.assignment_sequence;
            labels.observe(status, 2500);
            require(labels.bold(1, 2500) && !labels.bold(1, 2750) &&
                labels.bold(1, 3000) && !labels.bold(1, 3250) && !labels.bold(1, 3500),
                "reconnect starts exactly two new blinks");
        }
        app::AlphaRuntimeStatus status;
        status.runtime_epoch.bytes[0] = 1;
        status.telnet_active = true;
        for (unsigned number : {1U, 12U, 128U}) {
            app::AlphaNodeStatus node; node.number = static_cast<std::uint16_t>(number);
            node.available = node.enabled = true; node.state = sfx::core::NodeState::ready;
            status.nodes.push_back(node);
        }
        app::AlphaModemIndicators view;
        view.observe(status, 1000);
        for (const auto& node : status.nodes) {
            const auto lights = view.lights(node, 1000);
            require(lights.aa && lights.mr && !lights.cd && !lights.rd && !lights.sd && !lights.sc,
                "ready nodes 1/12/128 show AA/MR only");
        }
        auto& caller = status.nodes[0];
        caller.state = sfx::core::NodeState::connecting; caller.caller_connected = true;
        caller.connection = "Telnet"; caller.assignment_sequence = 1;
        view.observe(status, 1100);
        auto lights = view.lights(caller, 1100);
        require(lights.cd && lights.mr && !lights.aa && !lights.sc && !lights.rd && !lights.sd,
            "connected prelogin Telnet is not ready or encrypted");
        ++caller.receive_activity;
        view.observe(status, 1200);
        require(view.lights(caller, 1200).rd && !view.lights(caller, 1200).sd, "receive only");
        view.observe(status, 1300);
        require(view.lights(caller, 1900).rd && !view.lights(caller, 1950).rd, "unchanged polling never extends pulse");
        ++caller.send_activity; view.observe(status, 2000);
        require(!view.lights(caller, 2000).rd && view.lights(caller, 2000).sd, "send only");
        for (std::size_t index = 1; index < status.nodes.size(); ++index)
            require(!view.lights(status.nodes[index], 2000).rd && !view.lights(status.nodes[index], 2000).sd,
                "traffic cannot leak across node rows");
        caller.activity = "Other activity"; ++caller.screen.sequence;
        view.observe(status, 2800);
        require(!view.lights(caller, 2800).rd && !view.lights(caller, 2800).sd,
            "screen and activity changes never invent traffic");
        for (std::uint64_t now = 3000; now < 4500; now += 100) {
            ++caller.send_activity; view.observe(status, now);
            require(view.lights(caller, now).sd, "sustained successful output stays visible");
        }
        ++caller.assignment_sequence; caller.connection = "SSH";
        caller.receive_activity = caller.send_activity = 0;
        view.observe(status, 4500);
        require(!view.lights(caller, 4500).sc && !view.lights(caller, 4500).sd, "reuse clears pulse; SSH negotiation is not encryption");
        caller.ssh_encrypted = true; view.observe(status, 4600);
        require(view.lights(caller, 4600).sc && caller.caller_handle.empty(), "SSH encryption does not imply authenticated user");
        caller.caller_connected = caller.ssh_encrypted = false; view.observe(status, 4700);
        require(!view.lights(caller, 4700).cd && !view.lights(caller, 4700).sc && !view.lights(caller, 4700).sd,
            "disconnect clears connection/security/traffic");
        caller.state = sfx::core::NodeState::offline; caller.enabled = false;
        view.observe(status, 4800); lights = view.lights(caller, 4800);
        require(!lights.aa && !lights.cd && !lights.rd && !lights.sd && !lights.sc && !lights.mr, "down node is all dim");
        status.telnet_active = false; view.observe(status, 4900);
        require(!view.lights(status.nodes[1], 4900).aa && view.lights(status.nodes[1], 4900).mr,
            "operational node without listening transport cannot advertise auto answer");
        caller.enabled = caller.caller_connected = true; caller.state = sfx::core::NodeState::connecting;
        caller.send_activity = 8; view.clear(); view.observe(status, 5000);
        require(!view.lights(caller, 5000).sd, "first snapshot does not replay historical traffic");
        status.runtime_epoch.bytes[0] = 2; ++caller.send_activity; view.observe(status, 5100);
        require(!view.lights(caller, 5100).sd, "Runtime epoch reset clears old pulses");
        std::cout << "PASS modem lights: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL modem lights: " << error.what() << '\n'; return 1;
    }
}
