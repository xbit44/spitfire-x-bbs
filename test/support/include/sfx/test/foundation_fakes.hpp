#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "sfx/application/accounting.hpp"
#include "sfx/application/configuration.hpp"
#include "sfx/application/lifecycle.hpp"
#include "sfx/application/logging.hpp"
#include "sfx/application/presentation.hpp"
#include "sfx/application/time.hpp"

namespace sfx::test {

class FakeClock final : public application::IClock {
public:
    std::int64_t wall_seconds{1'700'000'000};
    std::uint64_t monotonic{1'000};

    std::int64_t unix_seconds() const noexcept override;
    std::uint64_t monotonic_millis() const noexcept override;
    void advance(std::uint64_t milliseconds) noexcept;
};

class FakeLogSink final : public application::ILogSink {
public:
    std::vector<application::LogRecord> records;
    void write(const application::LogRecord& record) override;
};

class FakeOutcomeSink final : public application::IOutcomeSink {
public:
    bool accept{true};
    std::vector<application::OutcomeRecord> records;
    bool commit(const application::OutcomeRecord& record) override;
};

class FakeConfigurationSource final : public application::IConfigurationSource {
public:
    std::optional<application::FoundationConfiguration> candidate;
    std::optional<application::FoundationConfiguration> load() override;
};

struct PresentationOperation {
    std::string name;
    std::string value;
};

class FakePresentation final : public application::ISessionPresentation {
public:
    std::vector<PresentationOperation> operations;
    std::string next_input;
    bool named_display_result{true};

    void write_text(std::string_view text) override;
    void clear_screen() override;
    void move_cursor(std::uint16_t row, std::uint16_t column) override;
    void set_paging(bool enabled) override;
    void set_abort_enabled(bool enabled) override;
    void beep() override;
    void pause_for_enter() override;
    std::string prompt(const application::PromptRequest& request) override;
    void show_generated_menu(const application::GeneratedMenu& menu) override;
    bool render_named_display(const application::DisplayRequest& request) override;
};

class FakeOwnership final : public application::IRuntimeOwnership {
public:
    std::optional<std::string> owner;
    std::optional<application::RuntimeOwnershipMode> mode;

    application::OwnershipResult acquire(
        application::RuntimeOwnershipMode requested_mode,
        std::string owner_identity) override;
    void release(std::string owner_identity) noexcept override;
};

class FakeLocalControl final : public application::ILocalControl {
public:
    application::AttachResult next_attach_result{application::AttachResult::attached};
    application::RuntimeStatus status;
    std::vector<application::ShutdownRequest> shutdown_requests;
    std::uint32_t cancel_requests{0};
    bool attached{false};

    application::AttachResult attach(
        const application::ControlProtocolVersion version,
        const application::ControlAuthority authority) override {
        observed_version = version;
        observed_authority = authority;
        attached = next_attach_result == application::AttachResult::attached;
        return next_attach_result;
    }

    void detach() noexcept override { attached = false; }
    application::RuntimeStatus observe_status() const override { return status; }

    bool submit_shutdown(application::ShutdownRequest request) override {
        if (!attached) return false;
        shutdown_requests.push_back(request);
        return true;
    }

    bool cancel_shutdown() override {
        if (!attached) return false;
        ++cancel_requests;
        return true;
    }

    application::ControlProtocolVersion observed_version;
    application::ControlAuthority observed_authority{application::ControlAuthority::observe};
};

}  // namespace sfx::test

