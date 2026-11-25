#include "sfx/application/administration_reporting.hpp"
#include <algorithm>

namespace sfx::application {
bool valid_administration_summary(const AdministrationAuditSummary& value) noexcept {
    return value.event_id.valid() && valid_utc_seconds(value.occurred_utc_seconds) &&
        value.subject_account_id >= 0 && value.actor_account_id >= 0 &&
        value.kind <= (value.authentication
            ? static_cast<unsigned>(AuthenticationAuditEventKind::throttling_cleared)
            : static_cast<unsigned>(AuditEventKind::session_completed));
}
bool administration_summary_before(const AdministrationAuditSummary& left,
    const AdministrationAuditSummary& right) noexcept {
    if (left.occurred_utc_seconds != right.occurred_utc_seconds)
        return left.occurred_utc_seconds > right.occurred_utc_seconds;
    if (left.authentication != right.authentication) return left.authentication < right.authentication;
    return left.event_id.bytes < right.event_id.bytes;
}
std::string_view administration_summary_label(const AdministrationAuditSummary& value) noexcept {
    if (!valid_administration_summary(value)) return {};
    return value.authentication ? storage_code(static_cast<AuthenticationAuditEventKind>(value.kind)) :
        storage_code(static_cast<AuditEventKind>(value.kind));
}
bool valid_administration_reporting(const AdministrationReportingSnapshot& value) noexcept {
    if (value.recent.size() > kAdministrationHistoryLimit || (!value.available && !value.recent.empty())) return false;
    for (std::size_t index = 0; index < value.recent.size(); ++index) {
        if (!valid_administration_summary(value.recent[index]) || (index &&
            !administration_summary_before(value.recent[index-1],value.recent[index]))) return false;
    }
    return true;
}
const AdministrationReportingSnapshot& AdministrationReportingCache::refresh(
    const std::uint64_t committed_operations, const Loader& load) {
    if (observed_commits_ == committed_operations) return snapshot_;
    observed_commits_ = committed_operations;
    snapshot_ = {};
    try {
        auto rows = load();
        if (!rows || rows->size() > 2 * kAdministrationHistoryLimit ||
            !std::all_of(rows->begin(),rows->end(),valid_administration_summary)) return snapshot_;
        std::sort(rows->begin(),rows->end(),administration_summary_before);
        if (rows->size() > kAdministrationHistoryLimit) rows->resize(kAdministrationHistoryLimit);
        AdministrationReportingSnapshot candidate{true,std::move(*rows)};
        if (valid_administration_reporting(candidate)) snapshot_ = std::move(candidate);
    } catch (...) { snapshot_ = {}; }
    return snapshot_;
}
} // namespace sfx::application
