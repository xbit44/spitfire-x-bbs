#pragma once
#include "linux_caller_listener.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace sfx::apps::runtime::web {
struct BrowserTerminalView {
    std::string handle;
    application::NodeAssignmentIdentity identity;
    application::ControlPanelTerminalSnapshot screen;
    std::uint64_t next_input{1};
    bool connecting{};
    std::uint16_t cursor_column{},cursor_row{};
};
inline bool browser_node_ready(const application::AlphaNodeStatus& node) noexcept {
    return node.enabled&&node.available&&!node.caller_connected&&node.state==core::NodeState::ready;
}
// Every transport is owned by one authenticated browser session. Handles and
// exact assignment generations prevent stale pages from controlling successors.
class BrowserTerminals final {
public:
    struct Ports {
        application::IClock& clock;
        std::function<LinuxBrowserNodeStart(std::uint16_t)> open;
        std::function<void(application::NodeAssignmentIdentity)> disconnect;
    };
    explicit BrowserTerminals(Ports);
    ~BrowserTerminals();
    bool open(std::string owner,std::uint16_t,std::function<bool()> authorized);
    std::optional<BrowserTerminalView> view(std::string_view owner);
    bool input(std::string_view owner,std::string_view handle,std::uint64_t sequence,std::string_view bytes);
    bool close(std::string_view owner,std::string_view handle);
    void close_all();
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}
