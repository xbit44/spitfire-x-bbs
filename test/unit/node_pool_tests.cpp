#include "sfx/application/node_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <vector>

// Test-executable-only fault injection. The production allocator is unchanged.
// Thread-local arming cannot affect concurrent worker tests on other threads.
namespace allocation_fixture { thread_local int fail_after = -1; }
void* operator new(std::size_t size) {
    if (allocation_fixture::fail_after >= 0 && allocation_fixture::fail_after-- == 0) {
        allocation_fixture::fail_after = -1;
        throw std::bad_alloc();
    }
    if (void* value = std::malloc(size ? size : 1)) return value;
    throw std::bad_alloc();
}
void* operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void* value) noexcept { std::free(value); }
void operator delete[](void* value) noexcept { std::free(value); }
void operator delete(void* value, std::size_t) noexcept { std::free(value); }
void operator delete[](void* value, std::size_t) noexcept { std::free(value); }

namespace {
namespace app = sfx::application;
using namespace std::chrono_literals;
unsigned assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
app::Identifier128 epoch(std::uint8_t value) {
    app::Identifier128 id;
    id.bytes[0] = value;
    return id;
}
// C++17 barrier: bounded waits, no sleeps or scheduler-order assumptions.
class Barrier {
public:
    explicit Barrier(unsigned count) : remaining_(count) {}
    void arrive() {
        std::unique_lock<std::mutex> lock(mutex_);
        if (--remaining_ == 0) condition_.notify_all();
        else if (!condition_.wait_for(lock, 10s, [this] { return remaining_ == 0; }))
            throw std::runtime_error("test barrier deadline");
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    unsigned remaining_;
};
template<class T> T collect(std::future<T>& future) {
    require(future.wait_for(10s) == std::future_status::ready, "worker completes within deadline");
    return future.get();
}
app::NodeLease take(app::FourNodePool& pool, std::string_view source = "127.0.0.1") {
    auto result = pool.allocate(source);
    require(result.code == app::NodeAllocationCode::allocated && result.lease.has_value(), "allocate lease");
    return std::move(*result.lease);
}

void sequential() {
    static_assert(!std::is_copy_constructible_v<app::NodeLease>);
    static_assert(!std::is_copy_assignable_v<app::NodeLease>);
    static_assert(std::is_nothrow_move_constructible_v<app::NodeLease>);
    app::FourNodePool pool(epoch(1));
    require(pool.allocate("bad-source").code == app::NodeAllocationCode::invalid_source, "invalid source consumes no slot");
    std::vector<app::NodeLease> leases;
    for (std::uint16_t node = 1; node <= 4; ++node) {
        leases.push_back(take(pool));
        require(leases.back().identity() == app::NodeAssignmentIdentity{epoch(1), node, 1}, "ascending lowest-free identities");
    }
    require(pool.snapshot().active == 4, "four simultaneous reservations");
    require(pool.allocate("127.0.0.2").code == app::NodeAllocationCode::full, "fifth source cannot get a fifth slot");
    require(pool.allocate("127.0.0.1").code == app::NodeAllocationCode::source_limit, "pre-login source count is atomic with reservation");
    const auto old_id = leases[1].identity();
    const auto old_gate = leases[1].callbacks();
    const auto old_token = *leases[1].cancellation();
    unsigned updates = 0;
    require(old_gate.invoke([&] { ++updates; }), "current-generation callback accepted");
    require(leases[3].release_after_cleanup(), "release node four");
    require(leases[1].release_after_cleanup(), "release node two");
    require(!leases[1].release_after_cleanup(), "duplicate completion rejected");
    require(old_token.is_cancelled(), "release cancels retained token");
    auto reused = take(pool);
    require(reused.identity() == app::NodeAssignmentIdentity{epoch(1), 2, 2}, "immediate reuse chooses two before four and increments generation");
    require(!pool.cancel(old_id), "stale cancellation rejected after reuse");
    require(!old_gate.invoke([&] { ++updates; }), "stale publication rejected after reuse");
    require(!reused.cancellation()->is_cancelled() && updates == 1, "old work cannot affect new occupant");
    auto moved = std::move(reused);
    require(!reused.release_after_cleanup() && !reused.cancellation(), "moved-from lease has no authority");
    require(moved.release_after_cleanup(), "moved owner releases once");
    require(leases[0].release_after_cleanup() && leases[2].release_after_cleanup(), "release remaining nodes");
    const auto empty = pool.snapshot();
    require(empty.active == 0, "all source occupancy returned");
    for (const auto& slot : empty.slots) require(!slot.occupied && slot.source.empty(), "free slot has no stale source");
    std::cout << "PASS lowest-free/full/release/reuse/move/stale-generation\n";
}

void exact_node_reservation() {
    app::FourNodePool pool(epoch(13), 4, 4);
    require(pool.allocate_exact(0, "127.0.0.1").code == app::NodeAllocationCode::invalid_node &&
        pool.allocate_exact(5, "127.0.0.1").code == app::NodeAllocationCode::invalid_node,
        "exact reservation rejects out-of-range node without touching pool state");
    require(pool.allocate_exact(2, "bad-source").code == app::NodeAllocationCode::invalid_source,
        "exact reservation preserves canonical source validation");
    auto third = pool.allocate_exact(3, "127.0.0.1");
    require(third.lease && third.lease->identity() == app::NodeAssignmentIdentity{epoch(13), 3, 1},
        "exact reservation owns only requested node");
    require(pool.allocate_exact(3, "127.0.0.2").code == app::NodeAllocationCode::full,
        "occupied exact node refuses without spilling into another slot");
    auto first = pool.allocate("127.0.0.2");
    require(first.lease && first.lease->identity().node == 1,
        "public lowest-free allocation remains independent of exact reservation");
    require(pool.set_enabled(2, false) &&
        pool.allocate_exact(2, "127.0.0.3").code == app::NodeAllocationCode::full,
        "disabled exact node refuses without fallback");
    const auto old = third.lease->identity();
    require(third.lease->release_after_cleanup(), "exact reservation releases through ordinary cleanup gate");
    auto reused = pool.allocate_exact(3, "127.0.0.4");
    require(reused.lease && reused.lease->identity().generation == 2 && !pool.cancel(old),
        "exact-node reuse advances generation and rejects stale cancellation");
    require(first.lease->release_after_cleanup() && reused.lease->release_after_cleanup(),
        "all exact-node fixture leases clean up");

    app::FourNodePool limited(epoch(14), 4, 1);
    auto local = limited.allocate_exact(4, "127.0.0.1");
    require(local.lease &&
        limited.allocate_exact(2, "127.0.0.1").code == app::NodeAllocationCode::source_limit,
        "exact reservation shares the atomic per-source admission ledger");
    require(local.lease->release_after_cleanup(), "source-limited exact reservation cleans up");
    std::cout << "PASS exact-node reservation/rules/reuse/source ledger\n";
}

void cancellation_and_shutdown() {
    app::FourNodePool pool(epoch(2));
    std::vector<app::NodeLease> leases;
    for (unsigned i = 0; i < 4; ++i) leases.push_back(take(pool));
    auto bad = leases[0].identity();
    bad.runtime_epoch = epoch(3);
    require(!pool.cancel(bad), "foreign Runtime epoch rejected");
    bad = leases[0].identity(); bad.node = 0;
    require(!pool.cancel(bad), "zero node rejected");
    bad.node = 5;
    require(!pool.cancel(bad), "out of range node rejected");
    require(pool.cancel(leases[1].identity()), "target assignment cancelled");
    for (unsigned i = 0; i < 4; ++i)
        require(leases[i].cancellation()->is_cancelled() == (i == 1), "cancellation is per assignment");
    require(pool.snapshot().active == 4, "cancellation does not release resources prematurely");
    require(!leases[1].callbacks().invoke([] {}), "cancelled callback suppressed");
    require(pool.set_enabled(3, false), "disable one node");
    require(leases[2].cancellation()->is_cancelled(), "disable signals its occupied assignment");
    require(pool.set_enabled(3, true) && leases[2].cancellation()->is_cancelled(), "enable never resets old cancellation");
    pool.stop(); pool.stop();
    require(pool.allocate("127.0.0.2").code == app::NodeAllocationCode::stopped, "shutdown closes admission");
    require(!pool.set_enabled(1, true), "shutdown cannot be reopened");
    for (auto& lease : leases) {
        require(lease.cancellation()->is_cancelled(), "stop signals every assignment");
        require(!lease.callbacks().invoke([] {}), "stop rejects all publications");
        require(lease.release_after_cleanup(), "stopped assignment can finish verified cleanup");
    }
    require(pool.snapshot().active == 0, "stop plus cleanup leaves no reservations");
    std::cout << "PASS independent cancellation/disable/stop/cleanup\n";
}

void limits_and_abandonment() {
    for (unsigned invalid : {0U, 129U}) {
        bool rejected = false;
        try { app::FourNodePool pool(epoch(4), static_cast<std::uint16_t>(invalid)); }
        catch (const std::invalid_argument&) { rejected = true; }
        require(rejected, "capacity outside 1..128 rejected");
    }
    bool rejected = false;
    try { app::FourNodePool pool({}); } catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "empty epoch rejected");
    app::FourNodePool limited(epoch(4), 4, 1);
    auto first = take(limited, "2001:db8::1");
    require(limited.allocate("2001:0db8:0:0:0:0:0:1").code == app::NodeAllocationCode::source_limit,
        "equivalent source spellings share ledger");
    require(first.release_after_cleanup(), "source count released");
    auto second = take(limited, "2001:0db8:0:0:0:0:0:1");
    require(second.release_after_cleanup(), "source can reconnect immediately");
    for (std::uint16_t n = 1; n <= 4; ++n) require(limited.set_enabled(n, false), "disable each free slot");
    require(limited.allocate("127.0.0.1").code == app::NodeAllocationCode::full, "all disabled refuses");
    require(limited.set_enabled(4, true), "enable only fourth");
    auto fourth = take(limited);
    require(fourth.identity().node == 4 && fourth.release_after_cleanup(), "lowest enabled free node selected");

    app::FourNodePool exhaustion(epoch(5), 1, 4, 2);
    for (unsigned i = 0; i < 2; ++i) { auto lease = take(exhaustion); require(lease.release_after_cleanup(), "bounded generation release"); }
    require(exhaustion.allocate("127.0.0.1").code == app::NodeAllocationCode::full, "generation exhaustion never wraps");
    require(exhaustion.snapshot().slots[0].quarantined, "exhausted slot retired");

    app::FourNodePool abandoned(epoch(6), 1);
    { auto lost = take(abandoned); }
    const auto lost = abandoned.snapshot();
    require(lost.active == 1 && lost.slots[0].cancelled && lost.slots[0].quarantined, "unverified cleanup quarantines occupied slot");
    require(abandoned.allocate("127.0.0.2").code == app::NodeAllocationCode::full, "abandoned slot never reused");
    require(!abandoned.set_enabled(1, true), "enable cannot hide failed cleanup");

    auto owner = std::make_unique<app::FourNodePool>(epoch(7));
    auto retained = take(*owner);
    auto gate = retained.callbacks();
    const auto token = *retained.cancellation();
    owner.reset();
    require(token.is_cancelled() && !gate.invoke([] {}), "pool destruction stops retained capabilities");
    require(retained.release_after_cleanup(), "lease can safely outlive stopped pool owner");
    require(!gate.invoke([] {}), "expired weak callback is safe");
    std::cout << "PASS capacity/source limits/exhaustion/abandonment/owner lifetime\n";
}

void simultaneous_allocation() {
    for (unsigned round = 0; round < 64; ++round) {
        app::FourNodePool pool(epoch(8));
        Barrier start(9);
        std::vector<std::future<app::NodeAllocation>> workers;
        for (unsigned i = 0; i < 8; ++i) workers.push_back(std::async(std::launch::async, [&, i] {
            start.arrive();
            return pool.allocate("127.0.0." + std::to_string(i + 1));
        }));
        start.arrive();
        std::vector<app::NodeLease> winners;
        unsigned refused = 0;
        for (auto& worker : workers) {
            auto result = collect(worker);
            if (result.lease) winners.push_back(std::move(*result.lease));
            else { require(result.code == app::NodeAllocationCode::full, "concurrent loser sees full"); ++refused; }
        }
        require(winners.size() == 4 && refused == 4 && pool.snapshot().active == 4, "eight contenders yield exactly four held winners");
        std::vector<unsigned> numbers;
        for (auto& winner : winners) numbers.push_back(winner.identity().node);
        std::sort(numbers.begin(), numbers.end());
        require(numbers == std::vector<unsigned>{1, 2, 3, 4}, "simultaneous assignments are unique nodes 1..4");
        for (auto& winner : winners) require(winner.release_after_cleanup(), "concurrent assignment cleans up");
        require(pool.snapshot().active == 0, "no occupancy leak after concurrent round");
    }
    std::cout << "PASS simultaneous allocation: 64 rounds x 8 contenders, exactly 4 winners per round\n";
}

void publication_vs_reuse() {
    app::FourNodePool pool(epoch(9), 1);
    auto lease = take(pool);
    const auto old = lease.callbacks();
    std::promise<void> entered, finish, release_started;
    const auto permit = finish.get_future().share();
    std::atomic<unsigned> order{0};
    auto publisher = std::async(std::launch::async, [&] {
        return old.invoke([&] {
            entered.set_value();
            if (permit.wait_for(10s) != std::future_status::ready) throw std::runtime_error("publication deadline");
            order.store(1);
        });
    });
    auto arrived = entered.get_future(); collect(arrived);
    auto releaser = std::async(std::launch::async, [&] {
        release_started.set_value();
        const bool released = lease.release_after_cleanup();
        const bool serialized = order.load() == 1;
        order.store(2);
        return released && serialized;
    });
    auto releasing = release_started.get_future(); collect(releasing);
    finish.set_value();
    require(collect(publisher) && collect(releaser), "release waits for in-flight publication to finish");
    auto next = take(pool);
    require(next.identity().generation == 2, "post-publication reuse increments generation");
    require(!old.invoke([&] { order.store(3); }) && order.load() == 2, "delayed old publication cannot overwrite new slot");
    bool threw = false;
    try { (void)next.callbacks().invoke([] { throw std::runtime_error("fixture callback"); }); }
    catch (const std::runtime_error&) { threw = true; }
    require(threw && next.callbacks().invoke([] {}), "throwing publication releases its lock without losing lease");
    require(next.release_after_cleanup(), "new owner cleans up");
    std::cout << "PASS publication/release serialization and delayed stale-callback rejection\n";
}

void concurrent_reuse_and_source_limit() {
    app::FourNodePool pool(epoch(11));
    std::vector<app::NodeLease> leases;
    std::vector<app::NodeCallbackGate> stale;
    std::vector<app::NodeAssignmentIdentity> identities;
    for (unsigned i = 0; i < 4; ++i) {
        leases.push_back(take(pool));
        stale.push_back(leases.back().callbacks());
        identities.push_back(leases.back().identity());
    }
    Barrier release(5);
    std::vector<std::future<bool>> releasers;
    for (auto& lease : leases) releasers.push_back(std::async(std::launch::async, [&release, &lease] {
        release.arrive(); return lease.release_after_cleanup();
    }));
    release.arrive();
    for (auto& worker : releasers) require(collect(worker), "simultaneous release succeeds once per owner");
    require(pool.snapshot().active == 0, "simultaneous release returns all occupancy");
    leases.clear();
    Barrier reuse(5);
    std::vector<std::future<app::NodeAllocation>> reusers;
    for (unsigned i = 0; i < 4; ++i) reusers.push_back(std::async(std::launch::async, [&] {
        reuse.arrive(); return pool.allocate("127.0.0.1");
    }));
    reuse.arrive();
    for (auto& worker : reusers) {
        auto assigned = collect(worker);
        require(assigned.lease && assigned.lease->identity().generation == 2, "every simultaneous reuse receives a fresh generation");
        leases.push_back(std::move(*assigned.lease));
    }
    for (unsigned i = 0; i < 4; ++i) {
        require(!pool.cancel(identities[i]) && !stale[i].invoke([] {}), "all four old cancel/publication capabilities rejected");
    }
    for (auto& lease : leases) {
        require(!lease.cancellation()->is_cancelled(), "all reused assignments have independent fresh cancellation");
        require(lease.release_after_cleanup(), "reused assignment cleanup");
    }
    app::FourNodePool limited(epoch(12), 4, 1);
    Barrier sources(9);
    std::vector<std::future<app::NodeAllocation>> contenders;
    for (unsigned i = 0; i < 8; ++i) contenders.push_back(std::async(std::launch::async, [&] {
        sources.arrive(); return limited.allocate("127.0.0.1");
    }));
    sources.arrive();
    std::optional<app::NodeLease> winner;
    unsigned denied = 0;
    for (auto& worker : contenders) {
        auto assigned = collect(worker);
        if (assigned.lease) {
            require(!winner && assigned.lease->identity().node == 1, "one atomic source-limit winner on lowest slot");
            winner = std::move(assigned.lease);
        } else {
            require(assigned.code == app::NodeAllocationCode::source_limit, "concurrent excess source consumes no node");
            ++denied;
        }
    }
    require(denied == 7 && limited.snapshot().active == 1, "eight same-source contenders obey limit one");
    require(winner && winner->release_after_cleanup(), "source winner cleanup");
    std::cout << "PASS four simultaneous releases/reuses with stale rejection; eight-way source-limit race\n";
}

void stop_vs_accept() {
    for (unsigned round = 0; round < 64; ++round) {
        app::FourNodePool pool(epoch(10));
        Barrier start(6);
        std::vector<std::future<app::NodeAllocation>> workers;
        for (unsigned i = 0; i < 4; ++i) workers.push_back(std::async(std::launch::async, [&] {
            start.arrive(); return pool.allocate("127.0.0.1");
        }));
        auto stopper = std::async(std::launch::async, [&] { start.arrive(); pool.stop(); });
        start.arrive(); collect(stopper);
        for (auto& worker : workers) {
            auto result = collect(worker);
            if (result.lease) {
                require(result.lease->cancellation()->is_cancelled(), "assignment winning stop race is cancelled");
                require(result.lease->release_after_cleanup(), "race winner releases after cleanup");
            } else {
                require(result.code == app::NodeAllocationCode::stopped, "assignment losing stop race is refused");
                require(!result.lease, "refused assignment holds no lease");
            }
        }
        require(pool.snapshot().stopped && pool.snapshot().active == 0, "shutdown race leaves no orphan lease");
    }
    std::cout << "PASS stop/accept race: 64 rounds, winners cancelled, losers refused, no orphan leases\n";
}
void same_epoch_replacement() {
    app::FourNodePool original(epoch(11));
    std::vector<app::NodeLease> leases;
    std::vector<app::NodeCallbackGate> callbacks;
    for (unsigned i = 0; i < 4; ++i) {
        auto result = original.allocate("127.0.0.1");
        require(result.lease.has_value(), "initial replacement-test assignment allocated");
        callbacks.push_back(result.lease->callbacks());
        leases.push_back(std::move(*result.lease));
    }
    for (auto& lease : leases) require(lease.release_after_cleanup(), "old service releases before replacement");
    original.stop();
    std::array<std::uint64_t, app::kAlphaNodePoolCapacity> seeds{};
    const auto stopped = original.snapshot();
    for (unsigned i = 0; i < 4; ++i) seeds[i] = stopped.slots[i].identity.generation;
    app::FourNodePool replacement(epoch(11), 4, 4, UINT64_MAX, seeds);
    for (std::uint16_t node = 1; node <= 4; ++node) {
        auto next = replacement.allocate("127.0.0.1");
        require(next.lease && next.lease->identity().node == node && next.lease->identity().generation == 2,
            "each node independently carries its previous generation");
        require(!callbacks[node - 1].invoke([] {}), "every old node callback remains stale");
        require(next.lease->release_after_cleanup(), "each replacement node releases");
        (void)replacement.set_enabled(node, false);
    }
    app::FourNodePool exhausted(epoch(11), 4, 4, 2, {{2, 2, 2, 2}});
    require(!exhausted.allocate("127.0.0.1").lease, "replacement never wraps exhausted generations");
    bool rejected = false;
    try { app::FourNodePool invalid(epoch(11), 4, 4, 2, {{3, 0, 0, 0}}); }
    catch (const std::invalid_argument&) { rejected = true; }
    require(rejected, "out-of-range generation seed rejects");
    std::cout << "PASS same-epoch service replacement: all four generation high-water marks retained; old callbacks and overflow rejected\n";
}
void allocation_failures() {
    unsigned failures = 0;
    for (const char* source : {"127.0.0.1", "2001:db8:1234:5678:90ab:cdef:1234:5678"}) {
        bool completed = false;
        for (int position = 0; position < 128 && !completed; ++position) {
            app::FourNodePool pool(epoch(12));
            bool failed = false;
            app::NodeAllocation allocation;
            allocation_fixture::fail_after = position;
            try { allocation = pool.allocate(source); }
            catch (const std::bad_alloc&) { failed = true; }
            allocation_fixture::fail_after = -1;
            if (failed) {
                ++failures;
                const auto snapshot = pool.snapshot();
                require(snapshot.active == 0 && !snapshot.stopped, "allocation failure leaves no reservation");
                for (const auto& slot : snapshot.slots)
                    require(!slot.occupied && !slot.quarantined && !slot.identity.generation && slot.source.empty(),
                        "allocation failure preserves source ledger and every generation");
                auto retry = pool.allocate(source);
                require(retry.lease && retry.lease->identity().node == 1 && retry.lease->identity().generation == 1,
                    "allocation retry uses lowest free node without identity loss");
                require(retry.lease->release_after_cleanup(), "allocation retry releases normally");
            } else {
                require(allocation.lease.has_value(), "allocation fault sweep reaches actual success");
                require(allocation.lease->release_after_cleanup(), "successful allocation sweep releases");
                completed = true;
            }
        }
        require(completed, "bounded sweep covers every allocation point");
    }
    require(failures >= 2, "both IPv4 and IPv6 actually injected allocation failure");
    std::cout << "PASS allocation-failure sweep: " << failures << " bad_alloc points, no orphan source/node/generation state\n";
}
void full_capacity() {
    app::FourNodePool pool(epoch(15), 128, 4);
    std::vector<app::NodeLease> leases;
    for (std::uint16_t node = 1; node <= 128; ++node) {
        leases.push_back(take(pool, "127.0.0." + std::to_string(1 + (node - 1) / 4)));
        require(leases.back().identity().node == node && leases.back().identity().generation == 1,
            "128 actual pool reservations have unique ascending assignment identity");
    }
    require(pool.snapshot().active == 128 &&
        pool.allocate("127.0.0.250").code == app::NodeAllocationCode::full &&
        pool.allocate("127.0.0.1").code == app::NodeAllocationCode::source_limit,
        "node 129 and fifth source connection refuse independently at full capacity");
    const auto old = leases.back().identity();
    const auto callback = leases.back().callbacks();
    require(pool.cancel(old) && leases.back().cancellation()->is_cancelled() &&
        !leases.front().cancellation()->is_cancelled(), "node 128 cancellation is isolated");
    require(leases.back().release_after_cleanup(), "node 128 releases after cleanup");
    auto reused = pool.allocate_exact(128, "127.0.0.250");
    require(reused.lease && reused.lease->identity().generation == 2 && !pool.cancel(old) &&
        !callback.invoke([] {}), "node 128 reuse rejects stale cancellation/publication");
    require(reused.lease->release_after_cleanup(), "reused node 128 releases");
    for (std::size_t index = 0; index < 127; ++index)
        require(leases[index].release_after_cleanup(), "all lower nodes release independently");
    require(pool.snapshot().active == 0, "128-node run leaves no live lease/source reservation");
    pool.stop();
    require(!pool.allocate_exact(128, "127.0.0.250").lease, "stopped high node cannot reopen");
    std::cout << "PASS 128-node allocation/source bounds/cancellation/reuse/cleanup\n";
}
} // namespace

int main() {
    try {
        sequential(); exact_node_reservation(); cancellation_and_shutdown(); limits_and_abandonment();
        simultaneous_allocation(); publication_vs_reuse(); concurrent_reuse_and_source_limit(); stop_vs_accept();
        same_epoch_replacement();
        allocation_failures();
        full_capacity();
        {
            app::FourNodePool pool(epoch(90),8,2,UINT64_MAX,{},{{"127.0.0.1",3},{"192.0.2.1",1}});
            std::vector<app::NodeLease> leases;
            for (int i=0;i<3;++i) leases.push_back(take(pool,"127.0.0.1"));
            require(pool.allocate_exact(8,"127.0.0.1").code == app::NodeAllocationCode::source_limit,
                "exact-node allocation obeys explicit address limit too");
            leases.push_back(take(pool,"192.0.2.1"));
            require(pool.allocate("192.0.2.1").code == app::NodeAllocationCode::source_limit,
                "lower explicit address limit is enforced before another node lease");
            leases.push_back(take(pool,"192.0.2.2")); leases.push_back(take(pool,"192.0.2.2"));
            require(pool.allocate("192.0.2.2").code == app::NodeAllocationCode::source_limit,
                "unlisted address retains configured global source limit");
            for (auto& lease : leases) require(lease.release_after_cleanup(),"overridden connections release normal leases");
            require(pool.snapshot().active == 0,"source overrides do not bypass resource cleanup");
        }
        std::cout << "PASS node pool: " << assertions << " assertions\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL node pool: " << error.what() << '\n';
        return 1;
    }
}
