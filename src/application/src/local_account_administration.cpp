#include "sfx/application/local_account_administration.hpp"
#include <algorithm>
#include <limits>

namespace sfx::application {
namespace {
bool valid_level(const SecurityLevelRecord& level) {
    const auto printable = [](std::string_view value, std::size_t limit, bool required) {
        return value.size() <= limit && (!required || !value.empty()) &&
            (value.empty() || (value.front() != ' ' && value.back() != ' ')) &&
            std::all_of(value.begin(), value.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
    };
    return (!level.minutes_per_call || *level.minutes_per_call>0) &&
        (!level.minutes_per_day || *level.minutes_per_day>0) &&
        (!level.private_chat_minutes || *level.private_chat_minutes>0) &&
        (!level.private_chat_minutes_per_day || *level.private_chat_minutes_per_day>0) &&
        (level.level!=999 || (!level.minutes_per_call && !level.minutes_per_day)) && level.level >= 1 && level.level <= 999 &&
        level.state <= SecurityLevelState::retired && printable(level.label, kSecurityLabelMaximum, true) &&
        (!level.description || printable(*level.description, kSecurityDescriptionMaximum, true));
}
LocalAccountResultCode map_storage(const StorageStatus& status) {
    switch (status.code) {
        case StorageErrorCode::none: return LocalAccountResultCode::ok;
        case StorageErrorCode::conflict: return LocalAccountResultCode::conflict;
        case StorageErrorCode::invalid_argument: return LocalAccountResultCode::invalid_request;
        case StorageErrorCode::permission_denied: return LocalAccountResultCode::unauthorized;
        case StorageErrorCode::system_sysop_protected: return LocalAccountResultCode::system_sysop_protected;
        case StorageErrorCode::busy: case StorageErrorCode::backpressure:
        case StorageErrorCode::shutting_down: return LocalAccountResultCode::unavailable;
        default: return LocalAccountResultCode::storage_failure;
    }
}
LocalAccountResult password_result(const PasswordMutationResult& value) {
    LocalAccountResult result;
    result.committed = value.credential_committed;
    result.sessions_revoked = value.revocation_completed;
    result.revision = value.account_revision.value_or(0);
    result.password_policy = value.policy_code;
    switch (value.code) {
        case AuthenticationOperationCode::completed: result.code = LocalAccountResultCode::ok; break;
        case AuthenticationOperationCode::conflict: result.code = LocalAccountResultCode::conflict; break;
        case AuthenticationOperationCode::policy_rejected: result.code = LocalAccountResultCode::policy_rejected; break;
        case AuthenticationOperationCode::rejected: result.code = LocalAccountResultCode::unauthorized; break;
        case AuthenticationOperationCode::revocation_failed: result.code = LocalAccountResultCode::revocation_failed; break;
        case AuthenticationOperationCode::cancelled: case AuthenticationOperationCode::backpressure:
        case AuthenticationOperationCode::unavailable: result.code = LocalAccountResultCode::unavailable; break;
        default: result.code = LocalAccountResultCode::storage_failure; break;
    }
    return result;
}
}

std::string security_policy_definition(const std::vector<SecurityLevelRecord>& levels,
    const std::uint16_t new_user_level, const std::uint16_t sysop_threshold) {
    if (levels.empty() || levels.size() > 999) return {};
    std::uint16_t previous = 0;
    bool new_user_found = false, sysop_found = false;
    std::string result = "new_user_level=" + std::to_string(new_user_level) +
        "\nsysop_threshold=" + std::to_string(sysop_threshold) + "\n";
    const auto escaped = [](std::string_view value) {
        std::string encoded;
        constexpr char hex[] = "0123456789ABCDEF";
        for (const unsigned char ch : value) {
            if (ch == '%' || ch == '=' || ch == '|') {
                encoded += '%'; encoded += hex[ch >> 4]; encoded += hex[ch & 15];
            } else encoded += static_cast<char>(ch);
        }
        return encoded;
    };
    for (const auto& level : levels) {
        if (!valid_level(level) || level.level <= previous) return {};
        previous = level.level;
        const bool active = level.state == SecurityLevelState::active;
        new_user_found |= active && level.level == new_user_level;
        sysop_found |= active && level.level == sysop_threshold;
        result += "level." + std::to_string(level.level) + "=" +
            (active ? "active|" : "retired|") + escaped(level.label) + "|" +
            escaped(level.description.value_or("")) + "\n";
    }
    return new_user_found && sysop_found ? result : std::string{};
}

std::optional<PublishSecurityPolicyRequest> prepare_local_security_change(
    const LocalSecuritySnapshot& current, const LocalSecurityChange& change) {
    if (current.code != LocalAccountResultCode::ok || current.policy.revision < 1 ||
        current.policy.revision == std::numeric_limits<std::int64_t>::max() ||
        change.expected_revision != current.policy.revision ||
        change.action > LocalSecurityAction::set_defaults ||
        security_policy_definition(current.levels, current.policy.new_user_level,
            current.policy.sysop_threshold).empty()) return std::nullopt;
    PublishSecurityPolicyRequest result;
    result.expected_revision = change.expected_revision;
    result.levels = current.levels;
    result.new_user_level = change.new_user_level;
    result.sysop_threshold = change.sysop_threshold;
    auto found = std::find_if(result.levels.begin(), result.levels.end(),
        [&](const auto& value) { return value.level == change.level; });
    if (change.action == LocalSecurityAction::upsert_level) {
        SecurityLevelRecord level{change.level,change.label,change.description,SecurityLevelState::active};
        level.minutes_per_call=change.minutes_per_call;level.minutes_per_day=change.minutes_per_day;
        level.private_chat_minutes=change.private_chat_minutes;level.private_chat_minutes_per_day=change.private_chat_minutes_per_day;
        if (change.replacement || !valid_level(level)) return std::nullopt;
        if (found == result.levels.end()) result.levels.push_back(std::move(level));
        else if (found->state == SecurityLevelState::active) *found = std::move(level);
        else return std::nullopt; // permanent retirement cannot be silently reversed
    } else if (change.action == LocalSecurityAction::retire_level) {
        if (!change.label.empty() || change.description || found == result.levels.end() ||
            found->state != SecurityLevelState::active || change.replacement == change.level)
            return std::nullopt;
        const auto replacement = std::find_if(result.levels.begin(), result.levels.end(),
            [&](const auto& value) { return value.level == change.replacement && value.state == SecurityLevelState::active; });
        if (replacement == result.levels.end()) return std::nullopt;
        found->state = SecurityLevelState::retired;
        result.reassignments.push_back({change.level,change.replacement});
    } else if (change.level || change.replacement || !change.label.empty() || change.description) {
        return std::nullopt;
    }
    std::sort(result.levels.begin(), result.levels.end(), [](const auto& a,const auto& b) { return a.level < b.level; });
    if (security_policy_definition(result.levels,result.new_user_level,result.sysop_threshold).empty()) return std::nullopt;
    return result;
}

LocalSecurityAdministration::LocalSecurityAdministration(IAccountStorage& accounts,
    ISecurityPolicyPublication& publication, IIdentifierGenerator& identifiers,
    IClock& clock, LocalAccountAuthority authority) noexcept
    : accounts_(accounts), publication_(publication), identifiers_(identifiers), clock_(clock), authority_(authority) {}

LocalSecuritySnapshot LocalSecurityAdministration::snapshot() {
    if (authority_ != LocalAccountAuthority::installation_owner)
        return {LocalAccountResultCode::unauthorized,{}, {}};
    const auto policy = accounts_.security_policy();
    const auto levels = accounts_.security_levels();
    if (!policy || !policy.value || !levels || !levels.value) return {};
    if (policy.value->revision < 1 || policy.value->published_revision != policy.value->revision ||
        policy.value->published_policy_hash != policy.value->policy_hash ||
        security_policy_definition(*levels.value,policy.value->new_user_level,policy.value->sysop_threshold).empty())
        return {LocalAccountResultCode::storage_failure,{}, {}};
    return {LocalAccountResultCode::ok,*policy.value,*levels.value};
}

std::string security_reassignment_definition(std::vector<std::pair<AccountId,std::int64_t>> accounts) {
    std::sort(accounts.begin(),accounts.end());
    std::string result="SFX-LEVEL-REMOVAL-1\n";
    for(const auto& account:accounts) result+=std::to_string(account.first)+":"+std::to_string(account.second)+"\n";
    return result;
}
LocalAccountResult LocalSecurityAdministration::execute(const LocalSecurityChange& change) {
    if (authority_ != LocalAccountAuthority::installation_owner) return {LocalAccountResultCode::unauthorized};
    const auto current = snapshot();
    if (current.code != LocalAccountResultCode::ok) return {current.code};
    if (change.expected_revision != current.policy.revision) return {LocalAccountResultCode::conflict};
    auto proposed = prepare_local_security_change(current,change);
    if (!proposed) return {LocalAccountResultCode::invalid_request};
    if(change.action==LocalSecurityAction::retire_level) {
        if(change.affected_accounts_hash.size()!=64) return {LocalAccountResultCode::invalid_request};
        std::vector<std::pair<AccountId,std::int64_t>> affected;
        bool complete=false;
        for(std::size_t offset=0;offset<=1000000;offset+=256) {
            AccountListQuery query;query.security_level=change.level;query.limit=256;query.offset=offset;
            const auto page=accounts_.list_accounts(query);
            if(!page || !page.value) return {LocalAccountResultCode::storage_failure};
            for(const auto& account:*page.value) if(account.state!=AccountState::tombstoned) affected.emplace_back(account.account_id,account.revision);
            if(page.value->size()<256) {complete=true;break;}
        }
        if(!complete || publication_.fingerprint(security_reassignment_definition(affected))!=change.affected_accounts_hash)
            return {LocalAccountResultCode::conflict};
        std::sort(affected.begin(),affected.end());proposed->expected_reassigned_accounts=std::move(affected);
    } else if(!change.affected_accounts_hash.empty()) return {LocalAccountResultCode::invalid_request};
    const auto definition = security_policy_definition(proposed->levels,proposed->new_user_level,proposed->sysop_threshold);
    auto hash = publication_.fingerprint(definition);
    if (hash.size() != 64 || !std::all_of(hash.begin(),hash.end(),[](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    })) return {LocalAccountResultCode::storage_failure};
    proposed->policy_hash = proposed->published_policy_hash = std::move(hash);
    proposed->audit = {identifiers_.generate(),identifiers_.generate(),{},clock_.unix_seconds(),AuditReasonCode::security_policy};
    const auto committed = accounts_.publish_security_policy(*proposed);
    if (!committed) return {map_storage(committed)};
    LocalAccountResult result{LocalAccountResultCode::storage_failure,true,true,change.expected_revision+1};
    // From here onward, preserve the committed result even if a host adapter or
    // subsequent read throws. Retrying the mutation is not a recovery strategy.
    try {
        const auto truth = snapshot();
        if (truth.code == LocalAccountResultCode::ok && truth.policy.revision == result.revision &&
            truth.policy.policy_hash == proposed->policy_hash && publication_.publish(truth))
            result.code = LocalAccountResultCode::ok;
    } catch (...) {}
    return result;
}

void erase_local_account_secrets(LocalAccountRequest& request) noexcept {
    for (auto* value : {&request.password, &request.confirmation}) {
        volatile char* bytes = value->empty() ? nullptr : value->data();
        for (std::size_t index = 0; index < value->size(); ++index) bytes[index] = 0;
        value->clear();
    }
}

LocalAccountAdministration::LocalAccountAdministration(IAccountStorage& accounts,
    IAuthenticationStorage& credentials, AuthenticationSecurityService& authentication,
    ISessionRevocationSink& revocations, IIdentifierGenerator& identifiers, IClock& clock,
    const OperationalConfiguration& configuration, LocalAccountAuthority authority) noexcept
    : accounts_(accounts), credentials_(credentials), authentication_(authentication),
      revocations_(revocations), identifiers_(identifiers), clock_(clock),
      configuration_(configuration), authority_(authority) {}

LocalAccountAdministration::LocalAccountAdministration(IAccountStorage& accounts,
    IAuthenticationStorage& credentials, AuthenticationSecurityService& authentication,
    ISessionRevocationSink& revocations, IIdentifierGenerator& identifiers, IClock& clock,
    const OperationalConfiguration& configuration, RuntimeSysopAccountAuthority authority) noexcept
    : accounts_(accounts), credentials_(credentials), authentication_(authentication),
      revocations_(revocations), identifiers_(identifiers), clock_(clock),
      configuration_(configuration), authority_(LocalAccountAuthority::none), sysop_(std::move(authority)) {}

LocalAccountSnapshot LocalAccountAdministration::snapshot(const LocalAccountQuery& query) {
    LocalAccountSnapshot result;
    const auto sysop = sysop_.revalidate ? sysop_.revalidate() : std::optional<AccountId>{};
    if (authority_ != LocalAccountAuthority::installation_owner &&
        authority_ != LocalAccountAuthority::elevated_administrator && (!sysop || *sysop<=0 || !sysop_.session.valid())) {
        result.code = LocalAccountResultCode::unauthorized; return result;
    }
    if (query.account_id < 0 || query.name_prefix.size() > 30 ||
        query.offset > 1000000 || (query.account_id && (query.offset || !query.name_prefix.empty()))) {
        result.code = LocalAccountResultCode::invalid_request; return result;
    }
    const auto policy = accounts_.security_policy();
    const auto system = credentials_.system_password();
    if (!policy || !policy.value || !system || !system.value) return result;
    result.policy = *policy.value;
    result.system_revision = sysop ? 0 : system.value->revision; // remote Sysops have no SY recovery authority
    if (authority_ == LocalAccountAuthority::elevated_administrator) {
        result.code = query.account_id || query.offset || !query.name_prefix.empty()
            ? LocalAccountResultCode::unauthorized : LocalAccountResultCode::ok;
        return result;
    }
    if (query.account_id) {
        const auto account = accounts_.find_account_by_id(query.account_id);
        if (!account || !account.value) {
            result.code = account ? LocalAccountResultCode::storage_failure : map_storage(account.status); return result;
        }
        result.accounts.push_back(*account.value);
        const auto temporary = credentials_.temporary_password(query.account_id);
        if (temporary && temporary.value) result.temporary = *temporary.value;
        else if (temporary.status.code != StorageErrorCode::not_found) return result;
    } else {
        AccountListQuery page;
        page.exclude_tombstoned = true;
        if (!query.name_prefix.empty()) page.name_prefix = query.name_prefix;
        page.limit = kLocalAccountPageSize + 1; page.offset = query.offset;
        auto accounts = accounts_.list_accounts(page);
        if (!accounts || !accounts.value) {
            result.code = accounts ? LocalAccountResultCode::storage_failure : map_storage(accounts.status); return result;
        }
        result.accounts = std::move(*accounts.value);
        result.has_more = result.accounts.size() > kLocalAccountPageSize;
        if (result.has_more) result.accounts.resize(kLocalAccountPageSize);
        // Minimize list disclosure/payload; individual selection reads detail.
        for (auto& account : result.accounts) { account.profile = {}; account.statistics = {}; }
    }
    if (sysop && (!sysop_.revalidate || sysop_.revalidate()!=sysop))
        return {LocalAccountResultCode::unauthorized};
    result.code = LocalAccountResultCode::ok;
    return result;
}

LocalAccountResult LocalAccountAdministration::execute(LocalAccountRequest& request,
    const CancellationToken& cancellation) {
    struct Erase { LocalAccountRequest& value; ~Erase() { erase_local_account_secrets(value); } } erase{request};
    const bool system = request.action == LocalAccountAction::replace_system_password;
    const auto sysop = sysop_.revalidate ? sysop_.revalidate() : std::optional<AccountId>{};
    if (authority_ != LocalAccountAuthority::installation_owner &&
        !(system && authority_ == LocalAccountAuthority::elevated_administrator) &&
        !(sysop && *sysop>0 && sysop_.session.valid() && !system))
        return {LocalAccountResultCode::unauthorized};
    if (cancellation.is_cancelled()) return {LocalAccountResultCode::unavailable};
    const bool creating = request.action == LocalAccountAction::create_user;
    const bool password = system || creating || request.action == LocalAccountAction::issue_temporary_password;
    if (request.action > LocalAccountAction::save_user ||
        (password && (request.password.empty() || request.password.size() > kPasswordMaximum ||
            request.password != request.confirmation)) ||
        (!password && (!request.password.empty() || !request.confirmation.empty())) ||
        request.expected_revision < 1 || request.expected_revision == std::numeric_limits<std::int64_t>::max())
        return {LocalAccountResultCode::invalid_request};
    const auto now = clock_.unix_seconds();
    AuthenticationAuditContext credential_audit{identifiers_.generate(), identifiers_.generate(), sysop, now};
    if(sysop)credential_audit.sysop_session=sysop_.session;
    if (creating) {
        if (request.account_id || !valid_profile(request.profile) ||
            normalize_account_name(request.name) != request.name) return {LocalAccountResultCode::invalid_request};
        CredentialEnvelope envelope;
        const auto prepared = authentication_.create_password_credential(request.password,
            {configuration_.password_mode,request.name,configuration_.bbs_name},now,false,cancellation,envelope);
        if (prepared.code != AuthenticationOperationCode::completed) return password_result(prepared);
        if (sysop && (!sysop_.revalidate || sysop_.revalidate()!=sysop)) return {LocalAccountResultCode::unauthorized};
        const auto created = accounts_.create_account({request.name,request.profile,std::move(envelope),
            {credential_audit.event_id,credential_audit.correlation_id,sysop,now,AuditReasonCode::sysop_action,credential_audit.sysop_session}});
        return {map_storage(created.status),created.succeeded(),false,created.revision.value_or(0)};
    }
    if (system) {
        if (request.account_id || request.expected_reset_revision || request.expected_policy_revision)
            return {LocalAccountResultCode::invalid_request};
        return password_result(authentication_.replace_system_password({request.expected_revision,
            request.password, {configuration_.password_mode, "", configuration_.bbs_name},
            credential_audit}, cancellation));
    }
    if (request.account_id <= 0) return {LocalAccountResultCode::invalid_request};
    const auto account = accounts_.find_account_by_id(request.account_id);
    if (!account || !account.value)
        return {account ? LocalAccountResultCode::storage_failure : map_storage(account.status)};
    if (account.value->revision != request.expected_revision ||
        account.value->state == AccountState::tombstoned || !account.value->display_name)
        return {LocalAccountResultCode::conflict};
    const auto still_authorized=[&]{return !sysop || (sysop_.revalidate && sysop_.revalidate()==sysop);};
    if(!still_authorized())return {LocalAccountResultCode::unauthorized};
    if (request.action == LocalAccountAction::issue_temporary_password)
        return password_result(authentication_.issue_temporary_password({request.account_id,
            request.expected_revision, request.password,
            {configuration_.password_mode, *account.value->display_name, configuration_.bbs_name},
            credential_audit}, cancellation,still_authorized));
    if (request.action == LocalAccountAction::cancel_temporary_password) {
        if (request.expected_reset_revision < 1) return {LocalAccountResultCode::invalid_request};
        return password_result(authentication_.cancel_temporary_password({request.account_id,
            request.expected_reset_revision, credential_audit}));
    }
    const AuditContext audit{credential_audit.event_id, credential_audit.correlation_id, sysop, now,
        AuditReasonCode::sysop_action,credential_audit.sysop_session};
    MutationResult mutation;
    bool revoke = false;
    bool refresh_access = false;
    bool already_revoked = false;
    switch (request.action) {
        case LocalAccountAction::rename:
            mutation = accounts_.rename_account({request.account_id,request.expected_revision,request.name,audit});
            revoke = true; break;
        case LocalAccountAction::update_profile:
            mutation = accounts_.update_profile({request.account_id,request.expected_revision,request.profile,audit}); break;
        case LocalAccountAction::set_purge_allowed:
            mutation = accounts_.set_purge_allowed({request.account_id,request.expected_revision,request.purge_allowed,audit}); break;
        case LocalAccountAction::assign_security_level:
            mutation = accounts_.assign_security_level({request.account_id,request.expected_revision,
                request.expected_policy_revision,request.security_level,audit}); refresh_access = true; break;
        case LocalAccountAction::save_user:
            mutation = accounts_.save_account({request.account_id,request.expected_revision,
                request.expected_policy_revision,request.name,request.profile,request.security_level,
                request.purge_allowed,audit});
            revoke = request.name != *account.value->display_name;
            refresh_access = request.security_level != account.value->security_level;
            break;
        case LocalAccountAction::lock: case LocalAccountAction::unlock:
            mutation = accounts_.change_account_state({request.account_id,request.expected_revision,
                request.action == LocalAccountAction::lock ? AccountState::locked : AccountState::active,{},audit});
            revoke = request.action == LocalAccountAction::lock; break;
        case LocalAccountAction::tombstone:
            // Close admission transactionally (including last-Sysop protection)
            // before ending sessions. A failed final deletion remains inactive;
            // report that committed state rather than claiming no change.
            if (account.value->state != AccountState::locked) {
                mutation = accounts_.change_account_state({request.account_id,request.expected_revision,
                    AccountState::locked,{},audit});
                if (!mutation) return {mutation.last_sysop_protected ? LocalAccountResultCode::last_sysop_protected : map_storage(mutation.status)};
                request.expected_revision = *mutation.revision;
            }
            if (!revocations_.revoke({SessionRevocationKind::account_all,request.account_id,{}}))
                return {LocalAccountResultCode::revocation_failed,account.value->state!=AccountState::locked,false,request.expected_revision};
            already_revoked = true;
            mutation = accounts_.tombstone_account({request.account_id,request.expected_revision,
                AuditReasonCode::sysop_action,{identifiers_.generate(),audit.correlation_id,sysop,now,AuditReasonCode::sysop_action,audit.sysop_session}});
            if (!mutation && account.value->state != AccountState::locked)
                return {map_storage(mutation.status),true,true,request.expected_revision};
            break;
        default: return {LocalAccountResultCode::invalid_request};
    }
    LocalAccountResult result{mutation.last_sysop_protected ? LocalAccountResultCode::last_sysop_protected : map_storage(mutation.status), mutation.succeeded(), already_revoked,
        mutation.revision.value_or(0)};
    if (mutation && revoke) {
        result.sessions_revoked = revocations_.revoke({SessionRevocationKind::account_all,request.account_id,{}});
        if (!result.sessions_revoked) result.code = LocalAccountResultCode::revocation_failed;
    }
    if (mutation && refresh_access && !revoke && !revocations_.refresh_account_access(request.account_id))
        result.code = LocalAccountResultCode::revocation_failed;
    return result;
}

} // namespace sfx::application
