#include "sfx/application/authentication_service.hpp"
#include "sfx/application/authentication_security.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/authentication_fakes.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

namespace app = sfx::application;
namespace test = sfx::test;

constexpr std::int64_t now = 1'788'000'000;

void require(const bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

app::Identifier128 identifier(const std::uint32_t value) {
    test::DeterministicIdentifierSource source(value);
    return source.next();
}

app::AuthenticationAuditContext audit(const std::uint32_t value, const std::int64_t instant = now) {
    return {identifier(value), identifier(100'000U + value), std::nullopt, instant};
}

app::RateLimitKey key(const app::RateLimitScope scope, const std::uint8_t marker) {
    app::RateLimitKey value;
    value.scope = scope;
    value.subject_hash.fill(marker);
    return value;
}

void password_policy_tests() {
    test::FakePasswordBlocklist blocklist;
    blocklist.entries.insert("correct horse battery staple");
    app::PasswordPolicyContext standard{app::PasswordMode::standard, "Caller", "Test BBS"};
    require(app::evaluate_password_policy("1234", standard, nullptr) == app::PasswordPolicyCode::accepted,
        "standard mode accepts four printable ASCII bytes");
    require(app::evaluate_password_policy("  a ", standard, nullptr) == app::PasswordPolicyCode::accepted,
        "spaces are exact standard-mode password bytes");
    require(app::evaluate_password_policy("123", standard, nullptr) == app::PasswordPolicyCode::too_short,
        "standard minimum is four");
    require(app::evaluate_password_policy(std::string(128, 'x'), standard, nullptr) ==
            app::PasswordPolicyCode::accepted,
        "password maximum accepts 128");
    require(app::evaluate_password_policy(std::string(129, 'x'), standard, nullptr) ==
            app::PasswordPolicyCode::too_long,
        "password maximum rejects 129");
    require(app::evaluate_password_policy(std::string("abcd\x1f", 5), standard, nullptr) ==
            app::PasswordPolicyCode::non_printable_ascii,
        "control bytes are rejected");

    app::PasswordPolicyContext strong{app::PasswordMode::strong, "LongCallerName", "Test BBS"};
    require(app::evaluate_password_policy(std::string(14, 'x'), strong, &blocklist) ==
            app::PasswordPolicyCode::too_short,
        "strong minimum rejects 14");
    require(app::evaluate_password_policy("CORRECT HORSE BATTERY STAPLE", strong, &blocklist) ==
            app::PasswordPolicyCode::blocklisted,
        "blocklist compares the complete candidate case-insensitively");
    require(app::evaluate_password_policy("prefix correct horse battery staple suffix", strong, &blocklist) ==
            app::PasswordPolicyCode::accepted,
        "blocklist does not reject listed substrings");
    require(app::evaluate_password_policy("prefix LongCallerName suffix", strong, &blocklist) ==
            app::PasswordPolicyCode::accepted,
        "context checks do not reject arbitrary substrings");
    require(app::evaluate_password_policy("LongCallerName1", strong, &blocklist) ==
            app::PasswordPolicyCode::context_derived,
        "simple account-derived candidate is rejected");
    require(app::evaluate_password_policy("SpitfireSpitfire", strong, &blocklist) ==
            app::PasswordPolicyCode::context_derived,
        "product-derived candidate is rejected");
    blocklist.available = false;
    require(app::evaluate_password_policy("An otherwise valid passphrase", strong, &blocklist) ==
            app::PasswordPolicyCode::blocklist_unavailable,
        "strong creation fails closed when blocklist is unavailable");
    require(app::evaluate_password_policy("1234", standard, &blocklist) == app::PasswordPolicyCode::accepted,
        "standard mode does not depend on the strong blocklist");
}

void credential_envelope_tests() {
    const std::vector<std::uint8_t> salt(app::kArgon2idSaltBytes, 0x11);
    const std::vector<std::uint8_t> verifier(app::kArgon2idVerifierBytes, 0x22);
    const auto valid = app::make_argon2id_credential_envelope({}, salt, verifier, now, false);
    require(app::decode_argon2id_credential_envelope(valid).valid(), "canonical envelope decodes");
    require(!app::credential_needs_rehash(valid, {}), "current envelope does not need rehash");

    auto malformed = valid;
    malformed.canonical_parameters.pop_back();
    require(app::decode_argon2id_credential_envelope(malformed).code ==
            app::CredentialEnvelopeCode::malformed,
        "truncated parameters fail closed");
    malformed = valid;
    malformed.canonical_parameters[0] = 0;
    require(app::decode_argon2id_credential_envelope(malformed).code ==
            app::CredentialEnvelopeCode::malformed,
        "corrupt canonical magic fails closed");
    malformed = valid;
    malformed.pepper_key_identifier = "forbidden";
    require(app::decode_argon2id_credential_envelope(malformed).code ==
            app::CredentialEnvelopeCode::malformed,
        "v1 pepper identifiers fail closed");
    malformed = valid;
    malformed.envelope_format_version = 2;
    require(app::decode_argon2id_credential_envelope(malformed).code ==
            app::CredentialEnvelopeCode::unsupported_version,
        "future envelope version is rejected");
    malformed = valid;
    malformed.scheme_identifier = "future-scheme";
    require(app::decode_argon2id_credential_envelope(malformed).code ==
            app::CredentialEnvelopeCode::unsupported_scheme,
        "unknown scheme is rejected");
    malformed = valid;
    malformed.verifier.resize(app::kCredentialEnvelopeMaximum + 1U);
    require(app::decode_argon2id_credential_envelope(malformed).code ==
            app::CredentialEnvelopeCode::oversized,
        "oversized envelope is rejected before parameter use");
    malformed = valid;
    malformed.canonical_parameters[4] = 0;
    malformed.canonical_parameters[5] = 0;
    malformed.canonical_parameters[6] = 0;
    malformed.canonical_parameters[7] = 0;
    require(app::decode_argon2id_credential_envelope(malformed).code ==
            app::CredentialEnvelopeCode::unsupported_parameters,
        "unsupported memory profile is rejected");
}

void authorization_contract_tests() {
    require(app::next_sysop_sequence_action(true, 999, 999) ==
            app::SysopSequenceAction::request_system_password,
        "eligible account requests SY only after account success");
    require(app::next_sysop_sequence_action(false, 999, 999) == app::SysopSequenceAction::reject,
        "failed account password never requests SY");
    require(app::next_sysop_sequence_action(true, 998, 999) == app::SysopSequenceAction::reject,
        "below-threshold account never requests SY");
    require(app::sysop_authority_granted(app::SysopSequenceAction::request_system_password, true),
        "both sequential secrets grant Sysop authority");
    require(!app::sysop_authority_granted(app::SysopSequenceAction::reject, true),
        "SY alone cannot grant authority");
    require(app::valid_connection_limit_policy({4, 1, 128}), "locked connection defaults validate");
    require(!app::valid_connection_limit_policy({0, 1, 128}), "zero source limit is invalid");
    require(!app::valid_connection_limit_policy({4, 1, 129}), "logical-node ceiling remains 128");

    app::ConnectionAttemptState attempts;
    require(app::record_failed_account_attempt(attempts) == app::ConnectionAttemptAction::continue_login,
        "first failed account attempt remains on connection");
    require(app::record_failed_account_attempt(attempts) == app::ConnectionAttemptAction::continue_login,
        "second failed account attempt remains on connection");
    require(app::record_failed_account_attempt(attempts) == app::ConnectionAttemptAction::disconnect &&
            attempts.failed_account_attempts == app::kFailedAttemptsPerConnection,
        "third failed account attempt requires disconnect");
    require(app::record_failed_account_attempt(attempts) == app::ConnectionAttemptAction::disconnect &&
            attempts.failed_account_attempts == app::kFailedAttemptsPerConnection,
        "failed-attempt counter saturates at locked connection limit");

    const app::ConnectionLimitPolicy limits{4, 1, 128};
    require(app::evaluate_prelogin_admission(3, 127, limits) == app::ConnectionAdmissionCode::admitted,
        "pre-login connection within source and node limits is admitted");
    require(app::evaluate_prelogin_admission(4, 0, limits) ==
            app::ConnectionAdmissionCode::source_connection_limit,
        "excess pre-login source is rejected before consuming a logical node");
    require(app::evaluate_prelogin_admission(0, 128, limits) ==
            app::ConnectionAdmissionCode::logical_node_limit,
        "logical-node ceiling is enforced independently");
    require(app::evaluate_account_session_admission(1, limits) ==
            app::ConnectionAdmissionCode::account_session_limit,
        "default one-session-per-account limit is enforced");
}

void rate_control_tests() {
    app::RateControlCoordinator rate(app::provisional_rate_profile_a(), 4);
    const app::RateLimitKey account = key(app::RateLimitScope::account, 1);
    require(rate.record_failure(account, now).delay_seconds == 0, "account burst failure one admitted");
    require(rate.record_failure(account, now).delay_seconds == 0, "account burst failure two admitted");
    require(rate.record_failure(account, now).delay_seconds == 0, "account burst failure three admitted");
    require(rate.record_failure(account, now).delay_seconds == 1, "account failure four delays one second");
    require(!rate.assess(account, now).admitted, "active account delay blocks assessment");
    require(rate.assess(account, now + 1).admitted, "bounded delay expires");
    require(rate.record_failure(account, now + 1).delay_seconds == 2, "account progression doubles");

    const app::RateLimitKey source = key(app::RateLimitScope::source, 2);
    require(rate.assess(source, now).failure_count == 0, "source state is independent from account state");
    const auto persisted = rate.snapshots(now + 1);
    require(persisted.size() == 1, "active account state can be persisted across restart");
    app::RateControlCoordinator restored(app::provisional_rate_profile_a(), 4);
    require(restored.restore(persisted.front(), now + 1), "short-lived state restores before expiry");
    const auto restored_delay = restored.assess(account, now + 2);
    require(!restored_delay.admitted && restored_delay.delay_seconds == 1,
        "restored delay preserves only its bounded remainder");
    require(restored.assess(account, now + 122).failure_count < persisted.front().failure_count,
        "restored failure state decays");
    require(!restored.restore(persisted.front(), persisted.front().expires_utc_seconds),
        "expired restart state is discarded");
    restored.record_success(account);
    require(restored.assess(account, now + 123).failure_count == 0, "success clears account failures");

    rate.record_failure(key(app::RateLimitScope::source, 3), now);
    rate.record_failure(key(app::RateLimitScope::global, 4), now);
    rate.record_failure(key(app::RateLimitScope::system_password, 5), now);
    const auto capacity = rate.record_failure(key(app::RateLimitScope::account, 6), now);
    require(!capacity.admitted && capacity.delay_seconds == 8,
        "bounded subject capacity fails closed at the scope maximum delay");
}

void service_tests() {
    test::FakeAccountStorage accounts;
    test::FakeAuthenticationStorage authentication_storage;
    test::DeterministicPasswordKdf kdf;
    test::FakePasswordBlocklist blocklist;
    test::FakeSessionRevocationSink revocations;
    app::AuthenticationSecurityService service(
        accounts, authentication_storage, kdf, &blocklist, revocations);

    app::CancellationSource cancellation;
    const auto created = kdf.create("good-password", now - 1, false, cancellation.token());
    require(created.succeeded(), "fake setup credential creates");
    app::AccountRecord account;
    account.account_id = 42;
    account.revision = 7;
    account.state = app::AccountState::active;
    account.security_level = static_cast<std::uint16_t>(999);
    accounts.account_result = {app::StorageStatus::success(), account};
    accounts.credential_result = {app::StorageStatus::success(), *created.credential};
    authentication_storage.temporary_result.status.code = app::StorageErrorCode::not_found;
    authentication_storage.mutation_result.status = app::StorageStatus::success();
    authentication_storage.mutation_result.account_id = 42;
    authentication_storage.mutation_result.revision = 8;
    authentication_storage.status = app::StorageStatus::success();

    const auto correct = service.verify_account_password(
        42, "good-password", now, audit(1), cancellation.token());
    require(correct.authenticated() && !correct.password_change_required,
        "correct account password verifies");
    require(correct.verified_credential&&correct.verified_credential->verifier==created.credential->verifier,
        "session handoff binds the exact successfully verified account credential");
    const auto incorrect = service.verify_account_password(
        42, "bad-password", now, audit(2), cancellation.token());
    require(!incorrect.authenticated() && incorrect.code == app::AuthenticationOperationCode::rejected,
        "incorrect account password rejects generically");
    require(!incorrect.verified_credential,"failed password never supplies a session credential proof");

    app::VoluntaryPasswordChangeRequest change;
    change.account_id = 42;
    change.expected_account_revision = 7;
    change.current_password = "good-password";
    change.new_password = "new-good-password";
    change.policy = {app::PasswordMode::standard, "Caller", "Test BBS"};
    change.current_session_id = identifier(40);
    change.audit = audit(3);
    const auto changed = service.change_account_password(change, cancellation.token());
    require(changed.succeeded() && changed.credential_committed && changed.revocation_completed,
        "voluntary change verifies, commits, and revokes");
    require(revocations.commands.back().kind == app::SessionRevocationKind::account_except_current &&
            revocations.commands.back().current_session_id == change.current_session_id,
        "voluntary change keeps only the current account session");
    require(authentication_storage.replace_account_request.has_value() &&
            authentication_storage.replace_account_request->credential.pepper_key_identifier == std::nullopt,
        "account replacement stores no pepper identifier");
    require(changed.committed_credential&&changed.committed_credential->verifier==authentication_storage.replace_account_request->credential.verifier,
        "password-change handoff binds the exact newly committed verifier");

    app::TemporaryPasswordIssueOperation issue;
    issue.account_id = 42;
    issue.expected_account_revision = 8;
    issue.temporary_password = "manual-temporary-password";
    issue.policy = change.policy;
    issue.audit = audit(4);
    const auto issued = service.issue_temporary_password(issue, cancellation.token());
    require(issued.credential_committed &&
            authentication_storage.issue_temporary_request->credential.password_change_required,
        "Sysop-entered temporary password is stored as forced-change");
    require(revocations.commands.back().kind == app::SessionRevocationKind::account_all,
        "reset issuance revokes every account session");

    const auto temporary_credential = kdf.create(
        issue.temporary_password, now, true, cancellation.token());
    accounts.credential_result = {app::StorageStatus::success(), *temporary_credential.credential};
    app::TemporaryPasswordRecord reset;
    reset.account_id = 42;
    reset.revision = 2;
    reset.state = app::TemporaryPasswordState::active;
    reset.issued_utc_seconds = now;
    reset.expires_utc_seconds = now + app::kTemporaryPasswordLifetimeSeconds;
    authentication_storage.temporary_result = {app::StorageStatus::success(), reset};
    app::TemporaryPasswordReplacementOperation replace;
    replace.account_id = 42;
    replace.expected_account_revision = 8;
    replace.expected_reset_revision = 2;
    replace.temporary_password = issue.temporary_password;
    replace.permanent_password = "replacement-password";
    replace.policy = change.policy;
    replace.current_session_id = identifier(41);
    replace.consumed_audit = audit(5, now + 1);
    replace.changed_audit = audit(6, now + 1);
    const auto completed = service.replace_temporary_password(replace, cancellation.token());
    require(completed.succeeded() && authentication_storage.complete_temporary_request.has_value(),
        "temporary use and permanent replacement use one typed atomic operation");
    require(!authentication_storage.complete_temporary_request->credential.password_change_required,
        "completed reset stores a permanent credential");

    reset.expires_utc_seconds = now;
    authentication_storage.temporary_result = {app::StorageStatus::success(), reset};
    const auto expired = service.verify_account_password(
        42, issue.temporary_password, now, audit(7), cancellation.token());
    require(!expired.authenticated() && authentication_storage.transition_temporary_request.has_value() &&
            authentication_storage.transition_temporary_request->new_state ==
                app::TemporaryPasswordState::expired,
        "expired temporary password is atomically marked expired before rejection");

    authentication_storage.temporary_result.status.code = app::StorageErrorCode::not_found;
    accounts.credential_result = {app::StorageStatus::success(), *created.credential};
    kdf.force_rehash = true;
    authentication_storage.mutation_result.status.code = app::StorageErrorCode::conflict;
    const auto rehash = service.verify_account_password(
        42, "good-password", now + 2, audit(8, now + 2), cancellation.token());
    require(rehash.authenticated() && rehash.rehash_attempted && !rehash.rehash_completed,
        "failed atomic rehash retains a valid authenticated result and old credential");
    require(rehash.verified_credential&&rehash.verified_credential->verifier==created.credential->verifier,
        "failed rehash cannot bind a concurrently replaced credential to an old password");
    kdf.force_rehash = false;
    authentication_storage.mutation_result.status = app::StorageStatus::success();

    const auto system_created = kdf.create("system-password", now, false, cancellation.token());
    app::SystemCredentialRecord system;
    system.revision = 2;
    system.credential = *system_created.credential;
    system.modified_utc_seconds = now;
    authentication_storage.system_result = {app::StorageStatus::success(), system};
    const auto system_correct = service.verify_system_password(
        "system-password", now + 1, audit(9, now + 1), cancellation.token());
    const auto system_wrong = service.verify_system_password(
        "wrong-system", now + 1, audit(10, now + 1), cancellation.token());
    require(system_correct.authenticated() && !system_wrong.authenticated(),
        "global SY verifies correct and rejects incorrect independently");

    app::SystemPasswordReplacementOperation system_replace;
    system_replace.expected_system_revision = 2;
    system_replace.new_password = "new-system-password";
    system_replace.policy = change.policy;
    system_replace.audit = audit(11);
    const auto system_changed = service.replace_system_password(system_replace, cancellation.token());
    require(system_changed.succeeded() &&
            revocations.commands.back().kind == app::SessionRevocationKind::sysop_level_all,
        "global SY replacement revokes Sysop-level sessions only");
}

}  // namespace

int main() {
    try {
        password_policy_tests();
        credential_envelope_tests();
        authorization_contract_tests();
        rate_control_tests();
        service_tests();
        std::cout << "all SFX Stage 5C authentication security unit tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Stage 5C authentication unit test failure: " << error.what() << '\n';
        return 1;
    }
}
