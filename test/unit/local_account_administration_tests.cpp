#include "sfx/application/local_account_administration.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/authentication_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include <iostream>
#include <stdexcept>

int main() {
    namespace app = sfx::application;
    namespace test = sfx::test;
    using Action = app::LocalAccountAction;
    using Code = app::LocalAccountResultCode;
    unsigned checks{};
    const auto require = [&](bool ok, const char* message) { ++checks; if (!ok) throw std::runtime_error(message); };
    try {
        test::FakeAccountStorage accounts;
        test::FakeAuthenticationStorage credentials;
        test::DeterministicPasswordKdf kdf;
        test::FakeSessionRevocationSink revocations;
        test::DeterministicIdentifierSource ids;
        test::FakeClock clock;
        app::OperationalConfiguration configuration;
        app::AuthenticationSecurityService authentication(accounts, credentials, kdf, nullptr, revocations);
        app::LocalAccountAdministration owner(accounts, credentials, authentication, revocations, ids, clock,
            configuration, app::LocalAccountAuthority::installation_owner);
        app::LocalAccountAdministration admin(accounts, credentials, authentication, revocations, ids, clock,
            configuration, app::LocalAccountAuthority::elevated_administrator);
        app::LocalAccountAdministration denied(accounts, credentials, authentication, revocations, ids, clock,
            configuration, app::LocalAccountAuthority::none);
        app::CancellationSource cancellation;
        app::LocalAccountRequest request;
        request.action = Action::issue_temporary_password;
        request.account_id = 1; request.expected_revision = 3;
        request.password = request.confirmation = "Manually-entered-123";
        require(denied.execute(request, cancellation.token()).code == Code::unauthorized &&
            accounts.calls.empty() && credentials.calls.empty() && kdf.create_calls == 0,
            "untrusted context never reaches storage or KDF");
        require(request.password.empty() && request.confirmation.empty(), "denied request secrets erased");
        require(admin.execute(request, cancellation.token()).code == Code::unauthorized,
            "different elevated administrator cannot issue account resets");
        require(denied.snapshot({}).code == Code::unauthorized, "unauthorized read is refused");
        app::AccountRecord account;
        account.account_id = 1; account.display_name = "Test Caller"; account.revision = 3;
        account.state = app::AccountState::active; account.security_level = std::uint16_t{10};
        account.profile.email = "private@example.invalid";
        accounts.account_result = {{}, account};
        accounts.policy_result = {{}, app::SecurityPolicyRecord{1,10,999,std::string(64,'a'),1,std::string(64,'a')}};
        accounts.mutation_result = {{}, 1, 4, false};
        credentials.system_result = {{}, app::SystemCredentialRecord{3,{},clock.wall_seconds}};
        credentials.mutation_result = {{},1,4,false};
        credentials.temporary_result.status.code = app::StorageErrorCode::not_found;
        accounts.account_list_result = {{}, std::vector<app::AccountRecord>(33,account)};
        const auto page = owner.snapshot({});
        require(page.code == Code::ok && page.accounts.size() == 32 && page.has_more &&
            !page.accounts.front().profile.email, "bounded list minimizes profile disclosure");
        const auto detail = owner.snapshot({1,{},0});
        bool remote_live=true;
        app::LocalAccountAdministration remote(accounts,credentials,authentication,revocations,ids,clock,
            configuration,app::RuntimeSysopAccountAuthority{[&]()->std::optional<app::AccountId>{return remote_live?std::optional<app::AccountId>{99}:std::nullopt;},ids.generate()});
        const auto remote_detail=remote.snapshot({1,{},0});
        require(remote_detail.code==Code::ok&&remote_detail.accounts.size()==1&&remote_detail.system_revision==0,
            "authenticated Sysop reads selected account without local SY recovery authority");
        app::LocalAccountRequest forbidden;forbidden.action=Action::replace_system_password;
        forbidden.expected_revision=3;forbidden.password=forbidden.confirmation="RemoteForbidden123";
        require(remote.execute(forbidden,cancellation.token()).code==Code::unauthorized&&forbidden.password.empty(),
            "remote Sysop cannot invoke installation system-password recovery and secrets are erased");
        remote_live=false;
        require(remote.snapshot({1,{},0}).code==Code::unauthorized,"revoked remote read is refused");
        app::LocalAccountRequest stale;stale.action=Action::lock;stale.account_id=1;stale.expected_revision=3;
        require(remote.execute(stale,cancellation.token()).code==Code::unauthorized,"revoked remote mutation/replay refused");
        require(detail.code == Code::ok && detail.accounts.size() == 1 &&
            detail.accounts.front().profile.email && !detail.temporary && detail.system_revision == 3,
            "one selected detail exposes only non-credential metadata");
        require(admin.snapshot({}).code == Code::ok && admin.snapshot({}).accounts.empty() &&
            admin.snapshot({1,{},0}).code == Code::unauthorized,
            "administrator recovery snapshot excludes accounts");
        require(owner.snapshot({-1,{},0}).code == Code::invalid_request &&
            owner.snapshot({0,std::string(31,'x'),0}).code == Code::invalid_request &&
            owner.snapshot({0,{},1000001}).code == Code::invalid_request,
            "invalid identity/filter/offset bounds fail closed");
        request.password = "Manual-123"; request.confirmation = "Mismatch-123";
        require(owner.execute(request,cancellation.token()).code == Code::invalid_request &&
            kdf.create_calls == 0 && request.password.empty(), "confirmation mismatch rejected before KDF and erased");
        request.password = request.confirmation = "Manual-123";
        request.expected_revision = 2;
        require(owner.execute(request,cancellation.token()).code == Code::conflict && kdf.create_calls == 0,
            "stale selected account cannot trigger credential mutation");
        request.expected_revision = 3;
        request.password = request.confirmation = "Manual-123";
        const auto issued = owner.execute(request,cancellation.token());
        require(issued.code == Code::ok && issued.committed && issued.sessions_revoked &&
            request.password.empty() && credentials.issue_temporary_request &&
            credentials.issue_temporary_request->expires_utc_seconds == clock.wall_seconds + 72*60*60 &&
            credentials.issue_temporary_request->credential.password_change_required,
            "manual reset retains 72-hour expiry and forced replacement");
        require(revocations.commands.back().kind == app::SessionRevocationKind::account_all &&
            revocations.commands.back().account_id == 1, "reset revokes exact account sessions");
        request.password = request.confirmation = "Manual-123";
        revocations.result = false;
        const auto failed = owner.execute(request,cancellation.token());
        require(failed.code == Code::revocation_failed && failed.committed && !failed.sessions_revoked,
            "failed cleanup truthfully retains committed credential state");
        revocations.result = true;
        request = {}; request.action = Action::replace_system_password; request.expected_revision = 3;
        request.password = request.confirmation = "Permanent-new-SY-123";
        const auto system = admin.execute(request,cancellation.token());
        require(system.code == Code::ok && system.committed && system.sessions_revoked &&
            credentials.replace_system_request && !credentials.replace_system_request->credential.password_change_required &&
            revocations.commands.back().kind == app::SessionRevocationKind::sysop_level_all,
            "elevated admin may replace permanent SY without old value and revoke only Sysops");
        request = {}; request.account_id = 1; request.expected_revision = 3;
        request.action = Action::assign_security_level; request.expected_policy_revision = 1; request.security_level = 999;
        const auto assigned = owner.execute(request,cancellation.token());
        require(assigned.code == Code::ok && assigned.committed && !assigned.sessions_revoked &&
            revocations.access_refreshes.back()==1 && accounts.calls.back() == "assign_security_level", "typed level assignment refreshes online authority without an ordinary disconnect");
        request.action = Action::lock;
        accounts.mutation_result={{app::StorageErrorCode::conflict,"fixture last Sysop"},{},{},false,true};
        const auto revocations_before=revocations.commands.size();
        for(auto action:{Action::lock,Action::tombstone,Action::save_user,Action::assign_security_level}) {
            request.action=action;
            const auto refused=owner.execute(request,cancellation.token());
            require(refused.code==Code::last_sysop_protected && !refused.committed &&
                revocations.commands.size()==revocations_before,"last-Sysop reason reaches UI without session revocation");
        }
        accounts.mutation_result={{},1,4,false};request.action=Action::lock;
        accounts.mutation_result={{app::StorageErrorCode::system_sysop_protected,"fixed assignment guard"},{},{},false};
        require(owner.execute(request,cancellation.token()).code==Code::system_sysop_protected && revocations.commands.size()==revocations_before,"designated Sysop refusal is distinct and does not revoke sessions");
        accounts.mutation_result={{},1,4,false};
        require(owner.execute(request,cancellation.token()).code == Code::ok &&
            accounts.calls.back() == "change_account_state", "lock uses typed account storage and revocation");
        request.action = Action::tombstone;
        require(owner.execute(request,cancellation.token()).code == Code::ok &&
            accounts.calls.back()=="tombstone_account", "one Delete action disables, disconnects and tombstones");
        accounts.account_result.value->state = app::AccountState::locked;
        request.expected_revision=3;
        require(owner.execute(request,cancellation.token()).code == Code::ok &&
            accounts.calls.back() == "tombstone_account", "disabled account can be explicitly tombstoned");
        accounts.account_result.value->state = app::AccountState::tombstoned;
        request.action = Action::unlock;
        require(owner.execute(request,cancellation.token()).code == Code::conflict, "tombstone is not silently resurrected");
        accounts.account_result.value = account;
        request.action = Action::cancel_temporary_password; request.expected_reset_revision = 2;
        require(owner.execute(request,cancellation.token()).code == Code::ok &&
            credentials.transition_temporary_request &&
            credentials.transition_temporary_request->new_state == app::TemporaryPasswordState::cancelled,
            "unused temporary reset cancellation uses the exact reset revision");
        accounts.account_result.value.reset();
        request.action = Action::lock;
        require(owner.execute(request,cancellation.token()).code == Code::storage_failure,
            "missing backend value is never reported as success");
        request.password = request.confirmation = "Erase-on-cancel-123";
        cancellation.cancel();
        require(owner.execute(request,cancellation.token()).code == Code::unavailable &&
            request.password.empty() && request.confirmation.empty(), "cancellation erases both secret fields");
        app::LocalSecuritySnapshot catalog{Code::ok,{1,10,999,std::string(64,'a'),1,std::string(64,'a')},
            {{10,"Caller",{},app::SecurityLevelState::active},{999,"Sysop",{},app::SecurityLevelState::active}}};
        app::LocalSecurityChange change{1,app::LocalSecurityAction::upsert_level,500,0,10,999,"Trusted",{}};
        auto prepared = app::prepare_local_security_change(catalog,change);
        require(prepared && prepared->levels.size() == 3 && prepared->levels[1].level == 500,
            "catalog addition uses sorted valid non-magical level");
        change.expected_revision = 2;
        require(!app::prepare_local_security_change(catalog,change),"stale catalog selection refuses");
        change.expected_revision = 1; change.label.assign(app::kSecurityLabelMaximum+1,'a');
        require(!app::prepare_local_security_change(catalog,change),"overlength security label refuses");
        change.label = "Trusted"; change.level = 0;
        require(!app::prepare_local_security_change(catalog,change),"zero is not a configured level");
        change.level = 500; change.sysop_threshold = 500;
        prepared = app::prepare_local_security_change(catalog,change);
        require(prepared && prepared->sysop_threshold == 500,"new defined level can become Sysop threshold");
        catalog.levels = prepared->levels; catalog.policy.sysop_threshold = 500;
        change = {1,app::LocalSecurityAction::retire_level,10,500,10,500,{}, {}};
        require(!app::prepare_local_security_change(catalog,change),"retired new-user reference cannot remain active policy");
        change.new_user_level = 500;
        prepared = app::prepare_local_security_change(catalog,change);
        require(prepared && prepared->reassignments.size() == 1 &&
            prepared->reassignments[0].retired_level == 10 && prepared->reassignments[0].replacement_level == 500,
            "retirement includes explicit same-transaction account reassignment");
        catalog.levels = prepared->levels; catalog.policy.new_user_level = 500;
        change = {1,app::LocalSecurityAction::upsert_level,10,0,500,500,"Reused",{}};
        require(!app::prepare_local_security_change(catalog,change),"retired catalog number never resurrects");
        catalog.levels[1].description = "A|B=C%";
        require(app::security_policy_definition(catalog.levels,500,500).find("A%7CB%3DC%25") != std::string::npos,
            "human-readable catalog canonically escapes separators");
        require(app::security_policy_definition(catalog.levels,0,999).empty(),"invalid defaults cannot be published");
        std::cout << "PASS local account administration: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL local administration: " << error.what() << '\n'; return 1; }
}
