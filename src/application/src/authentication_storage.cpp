#include "sfx/application/authentication_storage.hpp"

#include <algorithm>

namespace sfx::application {

std::string_view storage_code(const TemporaryPasswordState value) noexcept {
    switch (value) {
        case TemporaryPasswordState::active: return "active";
        case TemporaryPasswordState::consumed: return "consumed";
        case TemporaryPasswordState::cancelled: return "cancelled";
        case TemporaryPasswordState::expired: return "expired";
    }
    return {};
}

std::string_view storage_code(const AuthenticationProtocol value) noexcept {
    switch (value) {
        case AuthenticationProtocol::telnet: return "telnet";
        case AuthenticationProtocol::ssh: return "ssh";
    }
    return {};
}

std::string_view storage_code(const AuthenticationAuditEventKind value) noexcept {
    switch (value) {
        case AuthenticationAuditEventKind::account_password_changed: return "account.password_changed";
        case AuthenticationAuditEventKind::account_credential_rehashed: return "account.credential_rehashed";
        case AuthenticationAuditEventKind::temporary_password_issued: return "account.temp_password_issued";
        case AuthenticationAuditEventKind::temporary_password_consumed: return "account.temp_password_consumed";
        case AuthenticationAuditEventKind::temporary_password_cancelled: return "account.temp_password_cancelled";
        case AuthenticationAuditEventKind::temporary_password_expired: return "account.temp_password_expired";
        case AuthenticationAuditEventKind::system_password_replaced: return "system.password_replaced";
        case AuthenticationAuditEventKind::system_credential_rehashed: return "system.credential_rehashed";
        case AuthenticationAuditEventKind::throttling_cleared: return "authentication.throttling_cleared";
    }
    return {};
}

std::string_view storage_code(const RateLimitScope value) noexcept {
    switch (value) {
        case RateLimitScope::account: return "account";
        case RateLimitScope::source: return "source";
        case RateLimitScope::system_password: return "system_password";
        case RateLimitScope::global: return "global";
    }
    return {};
}

bool valid_authentication_audit_context(const AuthenticationAuditContext& audit) noexcept {
    return audit.event_id.valid() && audit.correlation_id.valid() &&
        (!audit.actor_account_id.has_value() || *audit.actor_account_id > 0) &&
        valid_utc_seconds(audit.occurred_utc_seconds);
}

bool valid_source_ip_text(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 45) return false;
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return (character >= static_cast<unsigned char>('0') && character <= static_cast<unsigned char>('9')) ||
            (character >= static_cast<unsigned char>('a') && character <= static_cast<unsigned char>('f')) ||
            (character >= static_cast<unsigned char>('A') && character <= static_cast<unsigned char>('F')) ||
            character == static_cast<unsigned char>(':') || character == static_cast<unsigned char>('.');
    });
}

bool valid_rate_limit_snapshot(const RateLimitSnapshot& snapshot) noexcept {
    return snapshot.failure_count > 0 && valid_utc_seconds(snapshot.window_started_utc_seconds) &&
        valid_utc_seconds(snapshot.last_failure_utc_seconds) &&
        valid_utc_seconds(snapshot.blocked_until_utc_seconds) &&
        valid_utc_seconds(snapshot.expires_utc_seconds) &&
        snapshot.window_started_utc_seconds <= snapshot.last_failure_utc_seconds &&
        snapshot.last_failure_utc_seconds <= snapshot.expires_utc_seconds &&
        snapshot.blocked_until_utc_seconds <= snapshot.expires_utc_seconds;
}

}  // namespace sfx::application
