#pragma once
#include "sfx/application/durable_journal.hpp"
#include "sfx/application/new_caller_names.hpp"
#include <filesystem>
#include <memory>

namespace sfx::platform::linux_host {
// Implements the existing journal contract; rendering/publication remains in
// application::DurableJournalPublisher. One serialized Runtime worker owns it.
class LinuxDurableJournalFiles final : public application::IDurableJournalFiles {
public:
    explicit LinuxDurableJournalFiles(std::filesystem::path root, std::wstring owner = {});
    ~LinuxDurableJournalFiles();
    std::optional<application::JournalFileState> inspect(application::DurableJournalKind) override;
    std::optional<std::string> read(application::DurableJournalKind, std::uint64_t, std::size_t) override;
    bool append(application::DurableJournalKind, const application::JournalFileState&, std::string_view) override;
    bool archive_and_reopen(application::DurableJournalKind) override;
    bool configure(const application::LoggingConfiguration&) noexcept;
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
application::NewCallerNames load_new_caller_names(const std::filesystem::path&) noexcept;
} // namespace sfx::platform::linux_host
