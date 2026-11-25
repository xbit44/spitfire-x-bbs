#include "sfx/test/foundation_fakes.hpp"

#include <utility>

namespace sfx::test {

std::int64_t FakeClock::unix_seconds() const noexcept { return wall_seconds; }
std::uint64_t FakeClock::monotonic_millis() const noexcept { return monotonic; }

void FakeClock::advance(const std::uint64_t milliseconds) noexcept {
    monotonic += milliseconds;
    wall_seconds += static_cast<std::int64_t>(milliseconds / 1000U);
}

void FakeLogSink::write(const application::LogRecord& record) { records.push_back(record); }

bool FakeOutcomeSink::commit(const application::OutcomeRecord& record) {
    if (!accept) {
        return false;
    }
    records.push_back(record);
    return true;
}

std::optional<application::FoundationConfiguration> FakeConfigurationSource::load() { return candidate; }

void FakePresentation::write_text(const std::string_view text) { operations.push_back({"text", std::string(text)}); }
void FakePresentation::clear_screen() { operations.push_back({"clear", {}}); }

void FakePresentation::move_cursor(const std::uint16_t row, const std::uint16_t column) {
    operations.push_back({"cursor", std::to_string(row) + ":" + std::to_string(column)});
}

void FakePresentation::set_paging(const bool enabled) { operations.push_back({"paging", enabled ? "on" : "off"}); }
void FakePresentation::set_abort_enabled(const bool enabled) { operations.push_back({"abort", enabled ? "on" : "off"}); }
void FakePresentation::beep() { operations.push_back({"beep", {}}); }
void FakePresentation::pause_for_enter() { operations.push_back({"pause", {}}); }

std::string FakePresentation::prompt(const application::PromptRequest& request) {
    operations.push_back({"prompt", request.identity});
    return next_input;
}

void FakePresentation::show_generated_menu(const application::GeneratedMenu& menu) {
    operations.push_back({"menu", menu.identity});
}

bool FakePresentation::render_named_display(const application::DisplayRequest& request) {
    operations.push_back({"display", request.basename});
    return named_display_result;
}

application::OwnershipResult FakeOwnership::acquire(
    const application::RuntimeOwnershipMode requested_mode,
    std::string owner_identity) {
    if (owner.has_value()) {
        return application::OwnershipResult::already_owned;
    }
    owner = std::move(owner_identity);
    mode = requested_mode;
    return application::OwnershipResult::acquired;
}

void FakeOwnership::release(const std::string owner_identity) noexcept {
    if (owner == owner_identity) {
        owner.reset();
        mode.reset();
    }
}

}  // namespace sfx::test
