#pragma once
#include "sfx/application/caller_assignment.hpp"
#include <filesystem>
#include <memory>
namespace sfx::apps::runtime {
// Four bounded resolver workers own subprocess lifetimes. Caller/status paths
// only enqueue a weak assignment; they never wait for resolver I/O.
class LinuxHostnameLookup final {
public:
    explicit LinuxHostnameLookup(std::filesystem::path,bool enabled);
    ~LinuxHostnameLookup();
    void subscribe(const std::shared_ptr<application::CallerAssignment>&);
    void stop() noexcept;
private:
    class Impl;std::unique_ptr<Impl> impl_;
};
}
