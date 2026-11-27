#include "sfx/adapters/authentication/phase3a_authentication_adapters.hpp"
#include "sfx/application/phase3a_private_adapters.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/authentication_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include "sfx/test/phase3a_fakes.hpp"
#include "sfx/test/phase3a_storage_fakes.hpp"
#include "sfx/test/terminal_stream_fake.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <locale>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <cstdlib>
#include <new>

// Test-executable-only allocator sweep; thread-local arming leaves concurrent
// tests and the production allocator unchanged.
namespace admission_allocation_fixture { thread_local int fail_after = -1; }
void* operator new(std::size_t size) {
    if (admission_allocation_fixture::fail_after >= 0 && admission_allocation_fixture::fail_after-- == 0) {
        admission_allocation_fixture::fail_after = -1;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
// GCC 11 and newer can warn about the inlined free in this malloc-backed allocator.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ >= 11
#pragma GCC diagnostic pop
#endif

namespace {
namespace app = sfx::application;
namespace auth = sfx::adapters::authentication;
namespace core = sfx::core;
namespace test = sfx::test;

std::size_t assertions = 0;

class HostileNumericPunctuation final : public std::numpunct<char> {
private:
    char do_thousands_sep() const override { return '\''; }
    std::string do_grouping() const override { return "\1"; }
};

class GlobalLocaleGuard final {
public:
    explicit GlobalLocaleGuard(const std::locale& replacement)
        : previous_(std::locale::global(replacement)) {}
    ~GlobalLocaleGuard() {
        try {
            std::locale::global(previous_);
        } catch (...) {
        }
    }

    GlobalLocaleGuard(const GlobalLocaleGuard&) = delete;
    GlobalLocaleGuard& operator=(const GlobalLocaleGuard&) = delete;

private:
    std::locale previous_;
};

void require(const bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}

app::Identifier128 identifier(const std::uint32_t value) {
    test::DeterministicIdentifierSource source(value);
    return source.next();
}

app::AuthenticationAuditContext authentication_audit(const std::uint32_t value) {
    return {identifier(value), identifier(value + 1000U), std::nullopt,
        1'700'000'000LL + value};
}

class FixedRules final : public app::IBbsTimeZoneRuleProvider {
public:
    std::optional<app::BbsTimeZoneYearRules> value;
    std::optional<std::int32_t> alternate_year;
    std::optional<app::BbsTimeZoneYearRules> alternate_value;
    bool reject{false};
    mutable std::vector<std::int32_t> years;

    std::optional<app::BbsTimeZoneYearRules> rules_for_year(
        const std::string_view key, const std::int32_t year) const override {
        years.push_back(year);
        if (reject || key != "Pacific Standard Time") return std::nullopt;
        if (alternate_year == year) return alternate_value;
        return value;
    }
};

constexpr std::int64_t days_from_civil(
    std::int32_t year, const unsigned month, const unsigned day) noexcept {
    year -= month <= 2U;
    const std::int32_t era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned mp = month > 2U ? month - 3U : month + 9U;
    const unsigned doy = (153U * mp + 2U) / 5U + day - 1U;
    const unsigned doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    return static_cast<std::int64_t>(era) * 146097LL + doe - 719468LL;
}

constexpr std::int64_t utc(
    const std::int32_t year, const unsigned month, const unsigned day,
    const unsigned hour, const unsigned minute, const unsigned second) noexcept {
    return days_from_civil(year, month, day) * 86400LL + hour * 3600LL +
        minute * 60LL + second;
}

app::BbsTimeZoneYearRules pacific_rules() {
    app::BbsTimeZoneYearRules rules;
    rules.bias_minutes = 480;
    rules.standard_bias_minutes = 0;
    rules.daylight_bias_minutes = -60;
    rules.daylight_transition = {3, 2, 0, 2, 0, 0, 0};
    rules.standard_transition = {11, 1, 0, 2, 0, 0, 0};
    return rules;
}

void admission_lifecycle_and_concurrency() {
    app::HostIndependentAccountSessionAdmissionCoordinator coordinator(16);
    app::CancellationSource cancellation;
    const app::AccountSessionAdmissionRequest first{
        identifier(1), 42, identifier(101), 2, 3};
    const auto acquired = coordinator.acquire(first, cancellation.token());
    require(acquired.acquired() && coordinator.active_leases() == 1,
        "valid account-session lease acquires");
    const auto duplicate = coordinator.acquire({identifier(1001), 42, identifier(1101), 1, 4}, cancellation.token());
    require(duplicate.code == app::AccountSessionAdmissionCode::account_limit && duplicate.existing_node == 3,
        "duplicate attention receives exact original node captured under admission lock");
    require(coordinator.acquire(first, cancellation.token()).code ==
            app::AccountSessionAdmissionCode::unavailable,
        "duplicate lease and intended-session identities fail closed");
    require(!coordinator.release({first.lease_id, 43, first.intended_session_id}),
        "mismatched lease ownership cannot release");
    require(coordinator.release(*acquired.lease) &&
            !coordinator.release(*acquired.lease) && coordinator.active_leases() == 0,
        "lease release is exact and exactly once");

    app::CancellationSource stopped;
    stopped.cancel();
    require(coordinator.acquire({identifier(2), 42, identifier(102), 1},
                stopped.token()).code == app::AccountSessionAdmissionCode::cancelled,
        "admission observes cancellation before mutation");
    require(coordinator.acquire({}, cancellation.token()).code ==
            app::AccountSessionAdmissionCode::unavailable,
        "invalid admission request fails closed");

    std::vector<std::future<app::AccountSessionAdmissionResult>> futures;
    for (std::uint32_t index = 0; index < 8; ++index) {
        futures.push_back(std::async(std::launch::async, [&, index] {
            return coordinator.acquire({identifier(20 + index), 77,
                identifier(120 + index), 2}, cancellation.token());
        }));
    }
    std::vector<app::AccountSessionAdmissionLease> leases;
    std::size_t limited = 0;
    for (auto& future : futures) {
        auto result = future.get();
        if (result.acquired()) leases.push_back(*result.lease);
        else if (result.code == app::AccountSessionAdmissionCode::account_limit) ++limited;
    }
    require(leases.size() == 2 && limited == 6 && coordinator.active_leases() == 2,
        "concurrent admission enforces the exact account limit");
    for (const auto& lease : leases) require(coordinator.release(lease),
        "concurrent lease releases cleanly");

    for (const std::uint16_t limit : {std::uint16_t{1}, std::uint16_t{4}}) {
        std::promise<void> start;
        const auto barrier = start.get_future().share();
        futures.clear(); leases.clear(); limited = 0;
        for (std::uint32_t index = 0; index < 8; ++index)
            futures.push_back(std::async(std::launch::async, [&, index, barrier] {
                barrier.wait();
                return coordinator.acquire({identifier(300 + index), 88,
                    identifier(400 + index), limit}, cancellation.token());
            }));
        start.set_value();
        for (auto& future : futures) {
            const auto result = future.get();
            if (result.acquired()) leases.push_back(*result.lease);
            else if (result.code == app::AccountSessionAdmissionCode::account_limit) ++limited;
        }
        require(leases.size() == limit && limited == 8U - limit && coordinator.active_leases() == limit,
            "barrier-released same-account contenders produce exact ordinary/Sysop-admitted count atomically");
        for (const auto& lease : leases) require(coordinator.release(lease),
            "independent same-account leases release without cancelling neighbors");
        require(coordinator.active_leases() == 0, "all concurrent account leases released");
    }
}

void admission_allocation_failures() {
    unsigned failures = 0;
    for (const bool same_account : {false, true}) {
        bool completed = false;
        for (int point = 0; point < 16; ++point) {
            app::HostIndependentAccountSessionAdmissionCoordinator coordinator(4);
            app::CancellationSource cancellation;
            const auto first = coordinator.acquire({identifier(500), 42, identifier(600), 4}, cancellation.token());
            const app::AccountSessionAdmissionRequest request{identifier(501), same_account ? 42 : 43, identifier(601), 4};
            admission_allocation_fixture::fail_after = point;
            const auto acquired = coordinator.acquire(request, cancellation.token());
            admission_allocation_fixture::fail_after = -1;
            if (!acquired.acquired()) {
                ++failures;
                require(acquired.code == app::AccountSessionAdmissionCode::unavailable && coordinator.active_leases() == 1,
                    "allocation failure leaves existing account lease and no orphan candidate");
                const auto retried = coordinator.acquire(request, cancellation.token());
                require(retried.acquired(), "same exact lease/session identity retries after failed publication");
                require(coordinator.release(*retried.lease), "retried lease releases independently");
            } else {
                require(coordinator.release(*acquired.lease), "fault sweep reaches successful acquisition");
                completed = true;
            }
            require(coordinator.release(*first.lease) && coordinator.active_leases() == 0,
                "original lease survives each allocation fault and independently releases");
            if (completed) break;
        }
        require(completed, "bounded allocation sweep covers every admission index");
    }
    require(failures >= 5, "new/existing account sweeps actually injected every map/set allocation failure");
    std::cout << "PASS C3 account admission: " << failures << " allocation failures; exact retry and original lease preservation\n";
}

void bbs_local_time_policy() {
    FixedRules provider;
    provider.value = pacific_rules();
    test::FakeClock clock;
    clock.wall_seconds = utc(2024, 2, 29, 8, 15, 0);
    app::BbsLocalTimeAdapters local({"Pacific Standard Time"}, provider, clock);
    require(local.format_utc_seconds(utc(2024, 2, 29, 8, 15, 0)) ==
            "2024-02-29 00:15:00" &&
            local.today().year == 2024 && local.today().month == 2 &&
            local.today().day == 29,
        "normal and leap-day BBS-local conversion use one clock and zone");
    {
        const std::locale hostile(std::locale::classic(),
            new HostileNumericPunctuation);
        GlobalLocaleGuard locale_guard(hostile);
        require(local.format_utc_seconds(utc(2024, 2, 29, 8, 15, 0)) ==
                "2024-02-29 00:15:00",
            "BBS-local timestamp formatting is independent of process-global locale");
    }
    require(local.format_utc_seconds(utc(2024, 3, 10, 9, 59, 59)) ==
            "2024-03-10 01:59:59" &&
            local.format_utc_seconds(utc(2024, 3, 10, 10, 0, 0)) ==
            "2024-03-10 03:00:00",
        "spring DST transition applies the represented year's installed rule");
    require(local.format_utc_seconds(utc(2024, 11, 3, 8, 30, 0)) ==
            "2024-11-03 01:30:00" &&
            local.format_utc_seconds(utc(2024, 11, 3, 9, 30, 0)) ==
            "2024-11-03 01:30:00",
        "fall-back overlap retains two UTC instants with one local label");
    provider.alternate_year = 2024;
    provider.alternate_value = pacific_rules();
    provider.alternate_value->bias_minutes = 540;
    require(local.format_utc_seconds(utc(2025, 1, 1, 7, 30, 0)) ==
            "2024-12-31 22:30:00" &&
            provider.years.size() >= 2 &&
            provider.years[provider.years.size() - 2] == 2025 &&
            provider.years.back() == 2024,
        "UTC year boundary selects installed dynamic rules for the represented local year");
    require(!local.format_utc_seconds(app::kMinimumUtcSeconds).has_value(),
        "unrepresentable offset conversion fails closed");
    provider.reject = true;
    require(!local.format_utc_seconds(utc(2024, 1, 1, 0, 0, 0)).has_value(),
        "missing or unavailable zone rules fail closed");
    clock.wall_seconds = utc(2024, 1, 1, 0, 0, 0);
    require(!app::valid_civil_date(local.today()),
        "date provider returns an invalid sentinel rather than a fallback zone");
    require(!app::BbsTimeZoneConfiguration{""}.valid() &&
            !app::BbsTimeZoneConfiguration{std::string(129, 'X')}.valid() &&
            !app::BbsTimeZoneConfiguration{std::string("\xc0\xaf", 2)}.valid(),
        "empty, overlong, and malformed UTF-8 typed time-zone inputs are rejected");
}

void credential_bridge() {
    test::FakeAccountStorage accounts;
    test::FakeAuthenticationStorage authentication_storage;
    test::DeterministicPasswordKdf kdf;
    test::FakePasswordBlocklist blocklist;
    test::FakeSessionRevocationSink revocations;
    app::AuthenticationSecurityService service(accounts, authentication_storage,
        kdf, &blocklist, revocations);
    app::CancellationSource cancellation;
    auto bridge = auth::Phase3aCredentialOperationsBridge::create(service, kdf,
        1'700'000'000LL, cancellation.token());
    require(bridge != nullptr, "credential bridge creates one bounded dummy envelope");
    const auto dummy = bridge->perform_dummy_verification(
        "SFX fixed public dummy verification credential v1",
        1'700'000'001LL, cancellation.token());
    require(dummy.code == app::AuthenticationOperationCode::rejected &&
            !dummy.verified,
        "dummy verification is never an authentication success");

    const app::PasswordPolicyContext policy{
        app::PasswordMode::standard, "Caller Name", "Spitfire-X BBS"};
    auto created = bridge->create_registration_credential("valid password", policy,
        1'700'000'002LL, cancellation.token());
    require(created.succeeded() && created.credential.has_value() &&
            !created.credential->password_change_required,
        "registration creation reuses accepted policy and KDF self-check");
    blocklist.entries.insert("blocked-password");
    auto strong = policy;
    strong.mode = app::PasswordMode::strong;
    const auto rejected = bridge->create_registration_credential(
        "blocked-password", strong, 1'700'000'003LL, cancellation.token());
    require(rejected.code == app::AuthenticationOperationCode::policy_rejected &&
            !rejected.credential.has_value(),
        "registration bridge preserves accepted policy rejection");

    app::AccountRecord account;
    account.account_id = 7;
    account.display_name = "Caller Name";
    account.name_key = "caller name";
    account.security_level = static_cast<std::uint16_t>(10);
    account.state = app::AccountState::active;
    account.revision = 1;
    accounts.account_result = {app::StorageStatus::success(), account};
    accounts.credential_result = {app::StorageStatus::success(), *created.credential};
    authentication_storage.temporary_result = {
        {app::StorageErrorCode::not_found, "no temporary password"}, std::nullopt};
    const auto verified = bridge->verify_existing_account(7, "valid password",
        1'700'000'004LL, authentication_audit(40), cancellation.token());
    require(verified.authenticated() && !verified.password_change_required,
        "existing-account bridge delegates to AuthenticationSecurityService");

    const auto temporary_credential = kdf.create(
        "temporary password", 1'700'000'005LL, true, cancellation.token());
    accounts.credential_result = {
        app::StorageStatus::success(), *temporary_credential.credential};
    app::TemporaryPasswordRecord temporary;
    temporary.account_id = 7;
    temporary.revision = 2;
    temporary.state = app::TemporaryPasswordState::active;
    temporary.issued_utc_seconds = 1'700'000'005LL;
    temporary.expires_utc_seconds =
        temporary.issued_utc_seconds + app::kTemporaryPasswordLifetimeSeconds;
    authentication_storage.temporary_result = {
        app::StorageStatus::success(), temporary};
    const auto temporary_verified = bridge->verify_existing_account(
        7, "temporary password", 1'700'000'006LL,
        authentication_audit(41), cancellation.token());
    require(temporary_verified.authenticated() &&
            temporary_verified.password_change_required,
        "credential bridge preserves the accepted temporary-password lifecycle");

    const auto system_credential = kdf.create(
        "system password", 1'700'000'007LL, false, cancellation.token());
    app::SystemCredentialRecord system;
    system.revision = 3;
    system.credential = *system_credential.credential;
    system.modified_utc_seconds = 1'700'000'007LL;
    authentication_storage.system_result = {app::StorageStatus::success(), system};
    const auto system_verified = bridge->verify_system_password(
        "system password", 1'700'000'008LL,
        authentication_audit(42), cancellation.token());
    require(system_verified.authenticated() &&
            !system_verified.password_change_required,
        "credential bridge preserves separate accepted SY password verification");

    app::CancellationSource stopped;
    stopped.cancel();
    require(auth::Phase3aCredentialOperationsBridge::create(service, kdf,
                1'700'000'005LL, stopped.token()) == nullptr,
        "dummy-envelope initialization honors cancellation");
}

void candidate_a_rate_coordination() {
    {
        test::FakeAuthenticationStorage sound_storage;
        sound_storage.rate_result={app::StorageStatus::success(),std::vector<app::RateLimitSnapshot>{}};
        unsigned sounds{};
        auth::Phase3aRateControlCoordinator audible(sound_storage,1'700'000'000LL,16,[&]{++sounds;});
        const app::Phase3aRateScopeSet account_only{42,"",false,false,false};
        for(unsigned i=0;i<3;++i)require(audible.record_failure(account_only,1'700'000'000LL).succeeded()&&sounds==0,"ordinary password failures within configured burst silent");
        require(audible.record_failure(account_only,1'700'000'000LL).succeeded()&&sounds==1,"new threshold activates one Security sound");
        require(audible.record_failure(account_only,1'700'000'000LL).succeeded()&&sounds==1,"already blocked retry does not repeat Security sound");
        require(audible.record_failure(account_only,1'700'000'020LL).succeeded()&&sounds==2,"later new protection episode sounds once");
        test::FakeAuthenticationStorage broken_audio;
        broken_audio.rate_result={app::StorageStatus::success(),std::vector<app::RateLimitSnapshot>{}};
        auth::Phase3aRateControlCoordinator optional(broken_audio,1'700'000'000LL,16,[]{throw std::runtime_error("optional sink");});
        for(unsigned i=0;i<4;++i)require(optional.record_failure(account_only,1'700'000'000LL).succeeded(),"audio exception cannot change auth enforcement");
    }
    test::FakeAuthenticationStorage storage;
    storage.rate_result = {app::StorageStatus::success(),
        std::vector<app::RateLimitSnapshot>{}};
    auth::Phase3aRateControlCoordinator rates(storage, 1'700'000'000LL, 16);
    require(rates.ready(), "empty durable Candidate A state restores cleanly");
    const app::Phase3aRateScopeSet all{42, "192.0.2.7", true, true, true};
    for (int index = 0; index < 4; ++index) {
        require(rates.record_failure(all, 1'700'000'000LL + index).succeeded(),
            "Candidate A multi-scope failure persists");
    }
    const auto account = std::find_if(storage.stored_rate_snapshots.begin(),
        storage.stored_rate_snapshots.end(), [](const auto& value) {
            return value.key.scope == app::RateLimitScope::account;
        });
    const auto source = std::find_if(storage.stored_rate_snapshots.begin(),
        storage.stored_rate_snapshots.end(), [](const auto& value) {
            return value.key.scope == app::RateLimitScope::source;
        });
    const auto global = std::find_if(storage.stored_rate_snapshots.begin(),
        storage.stored_rate_snapshots.end(), [](const auto& value) {
            return value.key.scope == app::RateLimitScope::global;
        });
    const auto system = std::find_if(storage.stored_rate_snapshots.begin(),
        storage.stored_rate_snapshots.end(), [](const auto& value) {
            return value.key.scope == app::RateLimitScope::system_password;
        });
    require(account != storage.stored_rate_snapshots.end() &&
            account->failure_count == 4 && account->blocked_until_utc_seconds ==
                1'700'000'004LL,
        "account Candidate A profile is exactly 3/1/8/60/900");
    require(source != storage.stored_rate_snapshots.end() &&
            source->failure_count == 4 &&
            source->blocked_until_utc_seconds == 1'700'000'003LL,
        "source Candidate A profile is exactly 8/1/8/60/900");
    require(global != storage.stored_rate_snapshots.end() &&
            global->failure_count == 4 &&
            global->expires_utc_seconds == 1'700'000'303LL,
        "global Candidate A profile is exactly 64/1/4/30/300");
    require(system != storage.stored_rate_snapshots.end() &&
            system->failure_count == 4 &&
            system->blocked_until_utc_seconds == 1'700'000'005LL &&
            system->expires_utc_seconds == 1'700'001'803LL,
        "SY Candidate A profile is exactly 3/2/16/120/1800");

    const auto durable_snapshots = storage.stored_rate_snapshots;
    test::FakeAuthenticationStorage restored_storage;
    restored_storage.rate_result = {
        app::StorageStatus::success(), durable_snapshots};
    auth::Phase3aRateControlCoordinator restored(
        restored_storage, 1'700'000'003LL, 16);
    require(restored.ready(),
        "all accepted Candidate A scopes restore from durable snapshots");

    auto decayed_snapshot = *account;
    decayed_snapshot.failure_count = 1;
    decayed_snapshot.window_started_utc_seconds = 1'700'000'000LL;
    decayed_snapshot.last_failure_utc_seconds = 1'700'000'000LL;
    decayed_snapshot.blocked_until_utc_seconds = 1'700'000'000LL;
    decayed_snapshot.expires_utc_seconds = 1'700'000'900LL;
    test::FakeAuthenticationStorage decay_storage;
    decay_storage.rate_result = {
        app::StorageStatus::success(),
        std::vector<app::RateLimitSnapshot>{decayed_snapshot}};
    auth::Phase3aRateControlCoordinator decayed(
        decay_storage, 1'700'000'061LL, 16);
    require(decayed.ready() &&
            decayed.await_admission({42, {}, false, false, false},
                1'700'000'061LL, app::CancellationSource{}.token()).code ==
                app::Phase3aRateGateCode::admitted &&
            !decay_storage.removed_rate_keys.empty(),
        "restored account state decays and removes its durable snapshot exactly");

    auto expired_snapshot = decayed_snapshot;
    expired_snapshot.expires_utc_seconds = 1'700'000'061LL;
    test::FakeAuthenticationStorage expired_storage;
    expired_storage.rate_result = {
        app::StorageStatus::success(),
        std::vector<app::RateLimitSnapshot>{expired_snapshot}};
    auth::Phase3aRateControlCoordinator expired(
        expired_storage, 1'700'000'061LL, 16);
    require(!expired.ready(),
        "an impossible expired row from the active-snapshot query fails restore closed");

    app::CancellationSource delayed_cancel;
    auto delayed = std::async(std::launch::async, [&] {
        return rates.await_admission(all, 1'700'000'003LL,
            delayed_cancel.token());
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    delayed_cancel.cancel();
    require(delayed.get().code == app::Phase3aRateGateCode::cancelled,
        "bounded delay remains cancellation-aware without unbounded sleeping");
    require(rates.record_success(all, 1'700'000'006LL).succeeded() &&
            storage.removed_rate_keys.size() >= 4,
        "successful authentication clears every selected durable scope");

    test::FakeAuthenticationStorage limited_storage;
    limited_storage.rate_result = {app::StorageStatus::success(),
        std::vector<app::RateLimitSnapshot>{}};
    auth::Phase3aRateControlCoordinator limited(limited_storage,
        1'700'000'000LL, 1);
    require(limited.record_failure({1, {}, false, false, false},
                1'700'000'000LL).succeeded() &&
            limited.record_failure({2, {}, false, false, false},
                1'700'000'000LL).code == app::StorageErrorCode::backpressure,
        "bounded rate capacity fails closed instead of losing state");

    test::FakeAuthenticationStorage failed_storage;
    failed_storage.rate_result = {app::StorageStatus::success(),
        std::vector<app::RateLimitSnapshot>{}};
    auth::Phase3aRateControlCoordinator failed(failed_storage,
        1'700'000'000LL, 4);
    failed_storage.status = {app::StorageErrorCode::io, "injected rate persistence"};
    require(failed.record_failure({1, {}, false, false, false},
                1'700'000'000LL).code == app::StorageErrorCode::io &&
            !failed.ready(),
        "rate persistence failure permanently fails the coordinator closed");
}

void non_network_composition_boundary() {
    test::FakeCallerNameClassificationPort names;
    test::FakePhase3aCredentialOperations credentials;
    test::FakeAccountStorage accounts;
    test::FakeAuthenticationStorage authentication_storage;
    test::FakePhase3aStorage phase3a_storage;
    test::FakeRegistrationCommitPort registration;
    test::FakeAccountSessionAdmissionPort admission;
    test::FakeAuthenticatedSessionCommitPort session_commit;
    test::FakeTrustedDisplaySelectionPort displays;
    test::FakePhase3aRateControlPort rates;
    test::FixedBbsLocalTimestampFormatter local_time;
    test::FakeCallerWelcomeStatisticsPort welcome_statistics;
    test::FixedBbsLocalDateProvider local_date;
    test::FakeClock clock;
    test::DeterministicIdentifierSource identifiers;
    app::Phase3aNonNetworkComposition composition({names, credentials, accounts,
        authentication_storage, phase3a_storage, registration, admission,
        session_commit, displays, rates, local_time, welcome_statistics,
        local_date, clock, identifiers});
    test::DeterministicInteractiveByteStreamFake stream;
    app::Phase3aConfigurationValues values;
    values.foundation.logical_nodes = 128;
    values.foundation.security = core::SecurityPolicy::fresh_install();
    values.bootstrap_complete = true;
    values.security_policy = {1, std::string(64, 'a'), 10, 999};
    values.connection_limits.maximum_logical_nodes = 128;
    values.prelogin_identity = {"Test BBS", "Test Sysop", "0.1-test"};
    auto configured = app::Phase3aConfigurationSnapshot::create(std::move(values));
    require(configured.valid(), "composition test configuration is valid");
    core::Session session("window6c-non-network", *core::NodeNumber::create(1),
        {core::TransportKind::telnet, "192.0.2.7", {}, "ansi"});
    app::CancellationSource cancellation;
    app::Phase3aExistingCallerRequest request;
    request.source_ip = "invalid-source";
    request.correlation_id = identifier(500);
    require(composition.run_existing(stream, *configured.snapshot, session, request,
                cancellation.token()).code == app::Phase3aWorkflowCode::invalid_context &&
            stream.read_observations().empty() && stream.write_observations().empty(),
        "composition accepts only an already-admitted byte stream and preserves fail-closed ordering");

    app::AccountRecord account;
    account.account_id = 42;
    account.display_name = "Caller Name";
    account.name_key = "caller name";
    account.security_level = static_cast<std::uint16_t>(10);
    account.state = app::AccountState::active;
    account.revision = 7;
    account.created_utc_seconds = 1'700'000'000LL;
    account.modified_utc_seconds = 1'700'000'000LL;
    account.statistics.successful_login_count = 12;
    accounts.account_result = {app::StorageStatus::success(), account};
    accounts.policy_result = {app::StorageStatus::success(),
        app::SecurityPolicyRecord{1, static_cast<std::uint16_t>(10),
            static_cast<std::uint16_t>(999), std::string(64, 'a'),
            1, std::string(64, 'a')}};
    authentication_storage.temporary_result = {
        {app::StorageErrorCode::not_found, "no temporary password"}, {}};
    phase3a_storage.status_result = app::StorageStatus::success();
    phase3a_storage.dynamic_claim_result = true;
    session_commit.result.status = app::StorageStatus::success();
    rates.status = app::StorageStatus::success();
    names.results.push_back({app::StorageStatus::success(),
        app::CallerNameResolution{app::CallerNameClassification::existing,
            "Caller Name", 42, "Caller Name", static_cast<std::uint16_t>(10)}});
    credentials.existing_results.push_back(
        {app::AuthenticationOperationCode::completed, true, false, false, false});
    displays.results.push_back({app::TrustedDisplaySelectionCode::not_found, {}});
    displays.results.push_back({app::TrustedDisplaySelectionCode::invalid, {}});
    test::DeterministicInteractiveByteStreamFake admitted_stream;
    admitted_stream.script_non_secret_text("\rCaller\rName\r");
    admitted_stream.script_generated_secret_input(12, 3);
    admitted_stream.script_non_secret_text("\r");
    core::Session admitted_session("window6c-admitted", *core::NodeNumber::create(1),
        {core::TransportKind::telnet, "192.0.2.7", {}, "ansi"});
    app::Phase3aExistingCallerRequest admitted_request;
    admitted_request.source_ip = "192.0.2.7";
    admitted_request.correlation_id = identifier(501);
    auto completed = composition.run_existing(admitted_stream, *configured.snapshot,
        admitted_session, admitted_request, cancellation.token());
    require(completed.caller_ready() && completed.ready.has_value() &&
            session_commit.requests.size() == 1 &&
            admitted_stream.pending_reads() == 0 &&
            admitted_stream.retained_secret_bytes() == 0,
        "non-network composition reaches one ready handoff over an already admitted deterministic byte stream");
    auto delivered = completed.ready->take();
    require(delivered.has_value() && !completed.ready->take().has_value() &&
            delivered->admission_lease.has_value() &&
            delivered->admission_lease->release() &&
            !delivered->admission_lease->release(),
        "composed ready handoff and move-only admission lease each transfer exactly once");
}

}  // namespace

int main() {
    try {
        admission_lifecycle_and_concurrency();
        admission_allocation_failures();
        bbs_local_time_policy();
        credential_bridge();
        candidate_a_rate_coordination();
        non_network_composition_boundary();
        std::cout << "all SFX Phase 3A Window 6C portable adapter tests passed assertions="
                  << assertions << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "SFX Phase 3A Window 6C portable adapter test failure: "
                  << error.what() << '\n';
        return 1;
    }
}
