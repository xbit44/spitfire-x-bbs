#pragma once

#include "sfx/application/authentication_service.hpp"
#include "sfx/application/operational_configuration.hpp"
#include "sfx/application/time.hpp"
#include <functional>

namespace sfx::application {

// Established by the protected local host, NEVER decoded from a client request.
// A different elevated administrator has the locked SY recovery authority only.
enum class LocalAccountAuthority : std::uint8_t { none, installation_owner, elevated_administrator };
// Runtime-only capability. The host binds the authenticated session and its SY
// challenge, and rechecks live entitlement on every call. Never serialized or
// constructed from caller input; it conveys no installation-owner authority.
struct RuntimeSysopAccountAuthority {
    std::function<std::optional<AccountId>()> revalidate;
    Identifier128 session;
};
enum class LocalAccountAction : std::uint8_t {
    rename, update_profile, lock, unlock, tombstone, set_purge_allowed,
    assign_security_level, issue_temporary_password, cancel_temporary_password,
    replace_system_password, create_user, save_user
};
enum class LocalAccountResultCode : std::uint8_t {
    ok, unauthorized, invalid_request, conflict, unavailable, policy_rejected,
    storage_failure, revocation_failed, last_sysop_protected, system_sysop_protected
};

struct LocalAccountRequest {
    LocalAccountAction action{LocalAccountAction::rename};
    AccountId account_id{};
    std::int64_t expected_revision{}, expected_policy_revision{}, expected_reset_revision{};
    std::string name;
    AccountProfile profile;
    std::uint16_t security_level{};
    bool purge_allowed{};
    std::string password, confirmation;
};
void erase_local_account_secrets(LocalAccountRequest& request) noexcept;

struct LocalAccountResult {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    bool committed{}, sessions_revoked{};
    std::int64_t revision{};
    PasswordPolicyCode password_policy{PasswordPolicyCode::accepted};
};

struct LocalAccountQuery {
    AccountId account_id{}; // zero = bounded name-filtered list, otherwise one detail
    std::string name_prefix{};
    std::uint32_t offset{};
};
inline constexpr std::size_t kLocalAccountPageSize = 32;
struct LocalAccountSnapshot {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    std::vector<AccountRecord> accounts{}; // no credential envelope is in AccountRecord
    bool has_more{};
    std::optional<TemporaryPasswordRecord> temporary{};
    std::int64_t system_revision{};
    SecurityPolicyRecord policy{};
    bool online{}; // Runtime-owned selected-account session status; false offline
};

// Catalog publication is a Runtime-owned or exclusive offline transaction. The
// database commits policy/reassignments/audit together; its matching public
// snapshot may be regenerated after an interrupted publication.
struct LocalSecuritySnapshot {
    LocalAccountResultCode code{LocalAccountResultCode::unavailable};
    SecurityPolicyRecord policy{};
    std::vector<SecurityLevelRecord> levels;
    bool has_more{}; // bounded IPC page only; offline/service snapshots are complete
};
enum class LocalSecurityAction : std::uint8_t { upsert_level, retire_level, set_defaults };
struct LocalSecurityChange {
    std::int64_t expected_revision{};
    LocalSecurityAction action{LocalSecurityAction::upsert_level};
    std::uint16_t level{}, replacement{}, new_user_level{}, sysop_threshold{};
    std::string label;
    std::optional<std::string> description;
    std::string affected_accounts_hash{};
    std::optional<std::uint32_t> minutes_per_call{}, minutes_per_day{};
    std::optional<std::uint32_t> private_chat_minutes{}, private_chat_minutes_per_day{};
};
std::string security_reassignment_definition(std::vector<std::pair<AccountId,std::int64_t>> accounts);
std::optional<PublishSecurityPolicyRequest> prepare_local_security_change(
    const LocalSecuritySnapshot& current, const LocalSecurityChange& change);
std::string security_policy_definition(const std::vector<SecurityLevelRecord>& levels,
    std::uint16_t new_user_level, std::uint16_t sysop_threshold);

// Trusted host adapter: hash the canonical non-secret definition and publish a
// protected public snapshot. A failed publication never rolls back DB truth or
// permits the GUI to repeat an already committed mutation.
class ISecurityPolicyPublication {
public:
    virtual ~ISecurityPolicyPublication() = default;
    virtual std::string fingerprint(std::string_view definition) = 0;
    virtual StorageStatus publish(const LocalSecuritySnapshot& truth) = 0;
};
class LocalSecurityAdministration final {
public:
    // The host must retain Runtime writer ownership or exclusive offline ownership for this entire
    // service lifetime. Different elevated administrators have SY recovery only.
    LocalSecurityAdministration(IAccountStorage& accounts, ISecurityPolicyPublication& publication,
        IIdentifierGenerator& identifiers, IClock& clock, LocalAccountAuthority authority) noexcept;
    LocalSecuritySnapshot snapshot();
    LocalAccountResult execute(const LocalSecurityChange& change);
private:
    IAccountStorage& accounts_;
    ISecurityPolicyPublication& publication_;
    IIdentifierGenerator& identifiers_;
    IClock& clock_;
    LocalAccountAuthority authority_;
};

// Shared typed application boundary for Runtime-owned online operations and
// explicitly guarded offline operations. Callers own serialization and the
// lifetime/exclusivity guard; GUI code never issues SQL or credential writes.
class LocalAccountAdministration final {
public:
    LocalAccountAdministration(IAccountStorage& accounts, IAuthenticationStorage& credentials,
        AuthenticationSecurityService& authentication, ISessionRevocationSink& revocations,
        IIdentifierGenerator& identifiers, IClock& clock,
        const OperationalConfiguration& configuration, LocalAccountAuthority authority) noexcept;
    LocalAccountAdministration(IAccountStorage& accounts, IAuthenticationStorage& credentials,
        AuthenticationSecurityService& authentication, ISessionRevocationSink& revocations,
        IIdentifierGenerator& identifiers, IClock& clock,
        const OperationalConfiguration& configuration, RuntimeSysopAccountAuthority authority) noexcept;
    LocalAccountSnapshot snapshot(const LocalAccountQuery& query);
    LocalAccountResult execute(LocalAccountRequest& request, const CancellationToken& cancellation);
private:
    IAccountStorage& accounts_;
    IAuthenticationStorage& credentials_;
    AuthenticationSecurityService& authentication_;
    ISessionRevocationSink& revocations_;
    IIdentifierGenerator& identifiers_;
    IClock& clock_;
    const OperationalConfiguration& configuration_;
    LocalAccountAuthority authority_;
    RuntimeSysopAccountAuthority sysop_;
};

} // namespace sfx::application
