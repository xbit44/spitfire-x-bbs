#pragma once

#include "sfx/application/authentication_storage.hpp"
#include <functional>

namespace sfx::application {
inline constexpr std::size_t kAdministrationHistoryLimit = 32;

// Non-secret, read-only projections of committed SQLite audit records. Never
// include credential envelopes, profile values, source hashes or caller input.
struct AdministrationAuditSummary {
    bool authentication{false};
    std::uint8_t kind{0};
    AccountId subject_account_id{0}, actor_account_id{0}; // zero means absent
    std::int64_t occurred_utc_seconds{0};
    Identifier128 event_id;
};
struct AdministrationReportingSnapshot {
    bool available{false};
    std::vector<AdministrationAuditSummary> recent; // newest first
};
bool valid_administration_summary(const AdministrationAuditSummary& value) noexcept;
bool administration_summary_before(const AdministrationAuditSummary& left,
    const AdministrationAuditSummary& right) noexcept;
std::string_view administration_summary_label(const AdministrationAuditSummary& value) noexcept;
bool valid_administration_reporting(const AdministrationReportingSnapshot& value) noexcept;

// A change notification from the existing writer health counter, not a timer
// issuing SQLite queries. Failed refreshes are unavailable until another commit.
class AdministrationReportingCache final {
public:
    using Loader = std::function<std::optional<std::vector<AdministrationAuditSummary>>() >;
    const AdministrationReportingSnapshot& refresh(std::uint64_t committed_operations,
        const Loader& load);
private:
    std::optional<std::uint64_t> observed_commits_;
    AdministrationReportingSnapshot snapshot_;
};
} // namespace sfx::application
