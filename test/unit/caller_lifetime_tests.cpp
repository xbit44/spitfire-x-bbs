#include "sfx/application/caller_assignment.hpp"
#include "sfx/application/runtime_statistics.hpp"
#include "sfx/test/account_storage_fakes.hpp"
#include "sfx/test/foundation_fakes.hpp"
#include "sfx/test/phase3a_fakes.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace app = sfx::application;
namespace test = sfx::test;
using namespace std::chrono_literals;
unsigned assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value) throw std::runtime_error(message);
}
app::Identifier128 id(unsigned value) { return test::DeterministicIdentifierSource(value).next(); }
template<class T> T collect(std::future<T>& future) {
    require(future.wait_for(10s) == std::future_status::ready, "bounded completion");
    return future.get();
}
std::string screen(const app::AlphaNodeStatus& status) {
    std::string result;
    for (const auto& cell : status.screen.cells) result += static_cast<char>(cell.character);
    return result;
}
void durable_cleanup() {
    {
        test::FakeAccountStorage accounts;
        test::FakeClock clock;
        auto health=std::make_shared<app::CallerCleanupHealth>();
        unsigned snapshots=0;
        accounts.finalize_hook=[&](const auto&) -> app::MutationResult {
            if(accounts.finalize_requests.size()==1) return {{app::StorageErrorCode::busy,"fixture"},{},{},false};
            return {};
        };
        {
            app::DurableSessionCleanup guard(accounts,clock,id(5),id(6),clock.wall_seconds,health,[&] {
                ++snapshots;return app::SessionAccountingSnapshot{3,{{"2026-10-03",3}},app::SessionOutcomeCode::timed_out};
            });
            guard.confirm_started();
            require(guard.finalize(0,app::SessionOutcomeCode::disconnected),"post-authentication welcome cleanup records elapsed and deadline outcome");
        }
        require(snapshots==1 && accounts.finalize_requests.size()==2 && !health->failed.load(),"accounting snapshot frozen once before exact retries/destructor");
        for(const auto& request:accounts.finalize_requests) require(request.total_authenticated_seconds==3 &&
            request.daily_seconds==std::vector<std::pair<std::string,std::int64_t>>{{"2026-10-03",3}} && request.outcome==app::SessionOutcomeCode::timed_out,
            "daily accounting and terminal outcome immutable across retries");
    }
    for (const auto outcome : {app::SessionOutcomeCode::normal, app::SessionOutcomeCode::disconnected,
        app::SessionOutcomeCode::timed_out, app::SessionOutcomeCode::shutdown, app::SessionOutcomeCode::runtime_error}) {
        test::FakeAccountStorage accounts;
        test::FakeClock clock;
        auto health = std::make_shared<app::CallerCleanupHealth>();
        {
            app::DurableSessionCleanup guard(accounts, clock, id(1), id(2), clock.wall_seconds, health);
            guard.confirm_started();
            clock.advance(7000);
            require(guard.finalize(7, outcome), "explicit outcome finalizes");
            require(guard.finalize(99, app::SessionOutcomeCode::runtime_error), "second finalization is inert");
        }
        require(accounts.finalize_requests.size() == 1 && !health->failed.load(), "one finalization including destructor");
        const auto& request = accounts.finalize_requests.front();
        require(request.session_id == id(1) && request.audit_event_id == id(2) &&
            request.total_authenticated_seconds == 7 && request.outcome == outcome &&
            request.completed_utc_seconds == clock.wall_seconds, "exact session audit timing and outcome");
    }
    {
        test::FakeAccountStorage accounts;
        test::FakeClock clock;
        auto health = std::make_shared<app::CallerCleanupHealth>();
        unsigned attempts = 0;
        accounts.finalize_hook = [&](const auto&) -> app::MutationResult {
            clock.advance(1000);
            if (++attempts == 1) return {{app::StorageErrorCode::busy, "fixture"}, {}, {}, false};
            if (attempts == 2) throw std::runtime_error("synthetic secret must never be logged");
            return {};
        };
        {
            app::DurableSessionCleanup first(accounts, clock, id(3), id(4), clock.wall_seconds, health);
            first.confirm_started();
            app::DurableSessionCleanup moved(std::move(first));
            require(moved.finalize(0, app::SessionOutcomeCode::disconnected), "busy and ambiguous throw retry successfully");
        }
        require(attempts == 3 && !health->failed.load(), "bounded exactly three attempts; moved-from destructor inert");
        const auto& first = accounts.finalize_requests.front();
        for (const auto& request : accounts.finalize_requests) require(
            request.session_id == first.session_id && request.audit_event_id == first.audit_event_id &&
            request.completed_utc_seconds == first.completed_utc_seconds &&
            request.total_authenticated_seconds == first.total_authenticated_seconds && request.outcome == first.outcome,
            "retry preserves every request field despite advancing clock");
    }
    for (bool confirmed : {false, true}) {
        test::FakeAccountStorage accounts;
        test::FakeClock clock;
        auto health = std::make_shared<app::CallerCleanupHealth>();
        accounts.mutation_result.status = {app::StorageErrorCode::not_found, "fixture"};
        {
            app::DurableSessionCleanup guard(accounts, clock, id(1), id(2), clock.wall_seconds, health);
            if (confirmed) guard.confirm_started();
            require(guard.finalize(0, app::SessionOutcomeCode::runtime_error) == !confirmed,
                "absence is accepted only for uncertain/uncommitted session");
        }
        require(health->failed.load() == confirmed, "missing confirmed session is sticky cleanup failure");
    }
    {
        test::FakeAccountStorage accounts;
        test::FakeClock clock;
        auto health = std::make_shared<app::CallerCleanupHealth>();
        accounts.mutation_result.status = {app::StorageErrorCode::io, "fixture"};
        { app::DurableSessionCleanup guard(accounts, clock, id(1), id(2), clock.wall_seconds, health); guard.confirm_started(); }
        require(accounts.finalize_requests.size() == 3 && health->failed.load(), "permanent failure cannot look like clean release");
    }
    {
        test::FakeAccountStorage accounts;
        test::FakeClock clock;
        { app::DurableSessionCleanup guard(accounts, clock, id(1), id(2), clock.wall_seconds); guard.commit_failed(app::StorageErrorCode::full); }
        require(accounts.finalize_requests.empty(), "definite atomic rejection creates no cleanup write");
    }
    {
        test::FakeAccountStorage accounts;
        test::FakeAccountSessionAdmissionPort admission;
        test::FakeClock clock;
        bool durable_before_release = false;
        accounts.finalize_hook = [&](const auto&) -> app::MutationResult {
            durable_before_release = admission.releases.empty();
            return {};
        };
        {
            app::AuthenticatedCallerReady ready;
            ready.account_id = 42; ready.node_id = 1; ready.security_level = 10;
            ready.display_name = "Test Caller"; ready.durable_session_id = id(1); ready.correlation_id = id(2);
            ready.admission_lease = app::AccountSessionAdmissionLeaseOwner::create({id(3), 42, id(1)}, admission);
            ready.durable_cleanup.emplace(accounts, clock, id(1), id(4), clock.wall_seconds);
            ready.durable_cleanup->confirm_started();
            auto handoff = app::AuthenticatedCallerReadyHandoff::create(std::move(ready));
            require(handoff.has_value(), "ready handoff owns cleanup");
            // Deliberately abandon it, as on an ANSI reset/consumer exception.
        }
        require(accounts.finalize_requests.size() == 1 && admission.releases.size() == 1 && durable_before_release,
            "unconsumed handoff finalizes durable state before account release");
    }
    {
        test::FakeAccountStorage accounts;
        test::FakeAccountSessionAdmissionPort admission;
        test::FakeClock clock;
        auto health = std::make_shared<app::CallerCleanupHealth>();
        bool ordered = true;
        accounts.finalize_hook = [&](const auto&) -> app::MutationResult {
            ordered = ordered && accounts.finalize_requests.size() == admission.releases.size() + 1;
            return {};
        };
        const auto make_ready = [&](unsigned number) {
            app::AuthenticatedCallerReady ready;
            ready.admission_lease = app::AccountSessionAdmissionLeaseOwner::create(
                {id(number + 1), 42, id(number)}, admission, health);
            ready.durable_cleanup.emplace(accounts, clock, id(number), id(number + 2), clock.wall_seconds, health);
            ready.durable_cleanup->confirm_started();
            return ready;
        };
        {
            auto first = make_ready(10);
            auto second = make_ready(20);
            first = std::move(second);
            require(accounts.finalize_requests.size() == 1 && admission.releases.size() == 1 && ordered,
                "overwriting a live ready value finalizes old durable state before old lease release");
            admission.release_result = false;
        }
        require(accounts.finalize_requests.size() == 2 && admission.releases.size() == 2 && ordered,
            "move assignment and moved-from destruction perform exactly one cleanup per owner");
        require(health->failed.load(), "abandoned account lease release failure remains sticky after destruction");
    }
    std::cout << "PASS durable cleanup: five outcomes, moves, abandonment, exact retries, ambiguous and permanent failures\n";
}
std::shared_ptr<app::CallerAssignment> assign(app::FourNodePool& pool, test::FakeClock& clock, bool ssh = false) {
    auto allocated = pool.allocate("127.0.0.1");
    require(allocated.lease.has_value(), "assignment allocated");
    return std::make_shared<app::CallerAssignment>(std::move(*allocated.lease), pool, clock, ssh, "127.0.0.1");
}
void assignment_lifetime() {
    test::FakeClock clock;
    app::FourNodePool pool(id(20), 1);
    auto old = assign(pool, clock);
    old->output("Old caller"); old->resize(132, 43); old->authenticated("Old Caller",41);
    require(old->snapshot().account_id==41,"publication carries authenticated account identity");
    for(const auto& location:std::vector<std::pair<app::MenuId,const char*>>{{app::MenuId::files,"File Menu"},{app::MenuId::messages,"Message Menu"},
        {app::MenuId::batch_transfer,"Batch Transfer Menu"},{app::MenuId::qwk,"QWK / Offline Mail"},{app::MenuId::main,"Main Menu"}}){
        old->menu_entered(location.first);require(old->snapshot().activity==location.second,"typed navigation publishes current menu in both directions");
    }
    old->hostname(old->identity(), "127.0.0.1", "old.example");
    old->diagnose(app::CallerDiagnostic::session_failure);
    std::promise<void> release;
    auto permit = release.get_future().share();
    auto delayed = std::async(std::launch::async, [old, permit] {
        if (permit.wait_for(10s) != std::future_status::ready) return false;
        old->output("late output"); old->resize(96, 31); old->authenticated("Wrong Caller",42);
        old->stage(app::CallerStage::registration); old->diagnose(app::CallerDiagnostic::cleanup_failure);
        old->menu_entered(app::MenuId::files);
        old->hostname(old->identity(), "127.0.0.1", "late.example");
        old->payload_activity(true); old->payload_activity(false);
        old->encrypted_transport_established();
        return true;
    });
    require(old->retire(true), "clean retirement releases after reset");
    require(old->identity().generation == 1 && old->identity().runtime_epoch == id(20),
        "connection assignment identity stays immutable even after lease release");
    const auto reset = old->snapshot();
    require(reset.account_id==0 && reset.caller_handle.empty() && reset.source_ip.empty() && reset.hostname.empty() && reset.connection.empty() &&
        reset.connected_seconds == 0 && !reset.screen_active && reset.terminal_columns == 80 && reset.terminal_rows == 25,
        "retirement clears all caller-specific published fields");
    auto current = assign(pool, clock, true);
    require(current->snapshot().caller_connected && !current->snapshot().ssh_encrypted,
        "accepted SSH connection is not yet encrypted");
    current->encrypted_transport_established();
    current->payload_activity(true); current->payload_activity(false);
    require(current->snapshot().ssh_encrypted && current->snapshot().caller_handle.empty() &&
        current->snapshot().receive_activity == 1 && current->snapshot().send_activity == 1,
        "encryption and payload observations are independent of authentication");
    current->output("New caller"); current->authenticated("New Caller",43);
    current->menu_entered(app::MenuId::qwk);require(current->snapshot().activity=="QWK / Offline Mail","new assignment owns its menu activity");
    release.set_value(); require(collect(delayed), "delayed callback completes after reuse");
    require(current->snapshot().account_id==43 && old->snapshot().account_id==0,"account identity cannot cross node reuse");
    current->transport_closed();require(current->snapshot().account_id==0,"transport closure removes Edit User identity");
    current->menu_entered(app::MenuId::files);require(current->snapshot().activity=="QWK / Offline Mail","closed transport cannot publish later menu activity");
    require(current->identity().generation == 2 && current->snapshot().caller_handle == "New Caller" &&
        current->snapshot().terminal_columns == 80 && screen(current->snapshot()).find("late output") == std::string::npos &&
        current->snapshot().hostname.empty() && old->snapshot().hostname.empty() &&
        current->diagnostic() == app::CallerDiagnostic::none, "stale output identity resize stage hostname and diagnostic cannot cross reuse");
    require(old->diagnostic() == app::CallerDiagnostic::none && old->snapshot().activity == "Idle",
        "retired context also ignores delayed updates");
    require(old->cancellation.is_cancelled() && !current->cancellation.is_cancelled(), "fresh assignment cancellation independent");
    current->cancel(); require(current->cancellation.is_cancelled() && current->stopping.load(), "per-assignment cancel reaches stream and workflow");
    require(current->retire(true) && pool.snapshot().active == 0, "cancel cleanup releases capacity");
    const auto cancelled = current->snapshot();
    require(!cancelled.caller_connected && !cancelled.ssh_encrypted && !cancelled.receive_activity &&
        !cancelled.send_activity && !old->snapshot().ssh_encrypted && !old->snapshot().receive_activity,
        "retirement and stale callbacks cannot leak modem state across reuse");
    require(cancelled.caller_handle.empty() && cancelled.connection.empty() && !cancelled.screen_active &&
        screen(cancelled) == std::string(2000, ' '), "owner reset still runs after cancellation closes callback gate");
    auto failed = assign(pool, clock);
    old->cancel();
    require(!failed->cancellation.is_cancelled(), "late cancellation of a released context cannot cancel reused assignment");
    failed->cleanup_health->failed.store(true);
    require(!failed->retire(true) && pool.snapshot().active == 1 && !failed->snapshot().available,
        "unverified durable cleanup keeps slot unavailable");
    failed->output("forbidden after quarantine");
    require(screen(failed->snapshot()).find("forbidden") == std::string::npos,
        "quarantined context is sealed against delayed output");
    app::FourNodePool stopped_pool(id(21), 1);
    auto stopped = assign(stopped_pool, clock);
    stopped->output("must clear during Runtime stop");
    stopped_pool.stop();
    require(stopped->retire(true) && screen(stopped->snapshot()) == std::string(2000, ' ') &&
        !stopped->snapshot().screen_active, "owner reset also runs after global pool stop closes callbacks");
    std::cout << "PASS stale generation, diagnostic isolation, cancellation, full reset and fail-closed quarantine\n";
}
void independent_metadata() {
    test::FakeClock clock; app::FourNodePool pool(id(30));
    std::vector<std::shared_ptr<app::CallerAssignment>> states;
    for (unsigned index = 0; index < 4; ++index) states.push_back(assign(pool, clock, index % 2 == 0));
    std::vector<std::future<void>> work;
    for (unsigned index = 0; index < 4; ++index) work.push_back(std::async(std::launch::async, [&, index] {
        states[index]->resize(static_cast<std::uint16_t>(100 + index), static_cast<std::uint16_t>(40 + index));
        states[index]->output(std::string(1, static_cast<char>('A' + index)));
        states[index]->stage(app::CallerStage::system_password);
        states[index]->hostname(states[index]->identity(), "127.0.0.1", "node" + std::to_string(index + 1) + ".example");
    }));
    for (auto& future : work) collect(future);
    for (unsigned index = 0; index < 4; ++index) {
        const auto snapshot = states[index]->snapshot();
        require(snapshot.hostname == "node" + std::to_string(index + 1) + ".example", "four independent hostname hints");
        states[index]->hostname(states[index]->identity(), "127.0.0.2", "wrong-source.example");
        auto stale = states[index]->identity(); ++stale.generation;
        states[index]->hostname(stale, "127.0.0.1", "wrong-generation.example");
        stale = states[index]->identity(); stale.runtime_epoch = id(31);
        states[index]->hostname(stale, "127.0.0.1", "wrong-epoch.example");
        states[index]->hostname(states[index]->identity(), "127.0.0.1", "bad\x1b[31m");
        const auto after_hint = states[index]->snapshot();
        require(after_hint.hostname == snapshot.hostname && after_hint.screen.sequence == snapshot.screen.sequence &&
            after_hint.activity == snapshot.activity && after_hint.caller_handle.empty(),
            "invalid hints cannot change screen/activity/caller or gain authentication authority");
        require(snapshot.terminal_columns == 100 + index && snapshot.terminal_rows == 40 + index &&
            snapshot.screen.cells.size() == 2000 && screen(snapshot).front() == static_cast<char>('A' + index),
            "four synthetic assignments retain independent metadata and fixed 80x25 mirrors");
        states[index]->resize(0, 0); states[index]->resize(4097, 4097);
        require(states[index]->snapshot().terminal_columns == 100 + index, "invalid sizes leave metadata unchanged");
        require(states[index]->retire(true), "synthetic assignment retired");
    }
}
app::AlphaRuntimeStatistics statistics(std::uint64_t value) {
    return {{true, value, value}, {true, value * 2, value * 2}, {true, value, value}};
}
void coherent_publication() {
    app::RuntimeStatisticsPublisher publisher;
    std::atomic<std::uint64_t> committed{1};
    std::promise<void> captured, resume;
    auto permit = resume.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        return publisher.refresh([&] {
            auto value = committed.load(); captured.set_value();
            if (permit.wait_for(10s) != std::future_status::ready) throw std::runtime_error("fixture deadline");
            return app::StorageResult<app::AlphaRuntimeStatistics>{app::StorageStatus::success(), statistics(value)};
        });
    });
    auto captured_future = captured.get_future(); collect(captured_future);
    committed.store(2);
    auto second = std::async(std::launch::async, [&] {
        return publisher.refresh([&] { return app::StorageResult<app::AlphaRuntimeStatistics>{
            app::StorageStatus::success(), statistics(committed.load())}; });
    });
    resume.set_value();
    const auto first_result = collect(first), second_result = collect(second);
    require(first_result.value == 1 && second_result.value == 2 && publisher.snapshot().successful_logons.total == 2,
        "delayed old read cannot overwrite newer read or double count commit");
    std::atomic<unsigned> inside{0}, overlaps{0};
    std::vector<std::future<void>> workers;
    for (unsigned index = 0; index < 8; ++index) workers.push_back(std::async(std::launch::async, [&] {
        for (unsigned count = 0; count < 32; ++count) (void)publisher.refresh([&] {
            if (++inside != 1) ++overlaps;
            const auto value = ++committed;
            --inside;
            return app::StorageResult<app::AlphaRuntimeStatistics>{app::StorageStatus::success(), statistics(value)};
        });
    }));
    for (auto& future : workers) collect(future);
    const auto final = publisher.snapshot();
    require(overlaps == 0 && final.successful_logons.total == 258 && final.caller_seconds.total == 516 &&
        final.new_users.total == 258, "256 concurrent refreshes publish coherent committed totals without lost increments");
    const auto failure = publisher.refresh([]() -> app::StorageResult<app::AlphaRuntimeStatistics> {
        throw std::runtime_error("synthetic ordinary/System/throwaway secret");
    });
    require(!failure && failure.status.operation == "runtime statistics unavailable" &&
        !publisher.snapshot().successful_logons.available && !publisher.snapshot().new_users.available,
        "failure is fixed diagnostic and unavailable, not secret text or zero-success");
    std::cout << "PASS statistics old/new publication race and 256 concurrent refreshes; no secrets in diagnostic\n";
}
}
int main() {
    try {
        durable_cleanup(); assignment_lifetime(); independent_metadata(); coherent_publication();
        std::cout << "PASS caller lifetime: " << assertions << " assertions; no additional live callers enabled\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL caller lifetime: " << error.what() << '\n'; return 1;
    }
}
