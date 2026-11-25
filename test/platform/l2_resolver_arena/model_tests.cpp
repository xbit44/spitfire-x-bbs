#include "lifetime.hpp"
#include <cstdio>
#include <stdexcept>

namespace fixture {
namespace {
unsigned assertions{};
void check(bool result, const char* label) {
    if (!result) throw std::runtime_error(label);
    ++assertions;
}
Identity caller(unsigned node = 1, std::uint64_t gen = 1) {
    return Identity{{0x101, 0x202}, gen, 0x0100007f, node};
}
}
unsigned model_tests() {
    assertions = 0;
    Ledger reusable;
    check(reusable.start(1), "initial lifetime");
    Ticket previous{};
    for (std::uint64_t generation = 1; generation <= 4096; ++generation) {
        auto who = caller(1, generation);
        const auto t = reusable.begin(who, generation);
        check(t.slot == 0, "lowest reusable slot");
        check(reusable.bind(t, 7), "deliberately reused native handle");
        check(!reusable.completion(previous, 7, who), "old token on reused message slot/handle");
        check(!reusable.retire_completed(t, true), "pending cannot retire");
        check(reusable.completion(t, 7, who), "exact completion");
        check(!reusable.completion(t, 7, who), "duplicate completion drained before reuse");
        check(!reusable.retire_completed(t, false), "completion alone cannot recycle message slot");
        check(reusable.retire_completed(t, true), "receipt plus drain permits reuse");
        previous = t;
    }
    std::puts("MODEL PASS: 4096 completed reuses; reused handle/message slot; stale/duplicate token rejection");
    for (unsigned mismatch = 0; mismatch != 5; ++mismatch) {
        Ledger scoped;
        check(scoped.start(1), "scope start");
        const auto t = scoped.begin(caller(), 0);
        check(scoped.bind(t, 9), "scope bind");
        auto changed = caller();
        if (mismatch == 0) ++changed.epoch[0];
        if (mismatch == 1) ++changed.epoch[1];
        if (mismatch == 2) ++changed.generation;
        if (mismatch == 3) ++changed.address;
        if (mismatch == 4) ++changed.node;
        check(!scoped.completion(t, 9, changed), "epoch/node/generation/address gate");
        check(scoped.retire_completed(t, true), "nonpublished result can retire only after completion");
    }
    // Cancel outcomes are simulated independently: none proves retirement.
    for (int code : {0, 10037, 10022, 10050, 10036}) {
        Ledger cancel;
        check(cancel.start(1), "cancel start");
        const auto t = cancel.begin(caller(), 0);
        check(cancel.bind(t, 8), "cancel bind");
        cancel.unsubscribe(t);
        (void)code; // success, already/invalid, network down, in-progress
        cancel.uncertain(t);
        check(cancel.quarantined() == 1, "cancel outcome retains uncertain buffer");
        check(!cancel.retire_completed(t, true), "cancel is not completion proof");
        check(!cancel.completion(t, 8, caller()), "late completion does not unquarantine");
        check(cancel.quarantined() == 1, "permanent quarantine");
        const auto other = cancel.begin(caller(2), 0);
        check(other.slot == 1, "uncertain message slot never rebound");
        check(cancel.bind(other, 8), "same native handle on different message slot");
        check(!cancel.completion(t, 8, caller(2)), "old slot cannot publish into reused handle");
        check(cancel.completion(other, 8, caller(2)), "new slot exact ownership");
    }
    std::puts("MODEL PASS: cancellation success/already/invalid/failure/indeterminate; permanent quarantine");
    Ledger timed;
    check(timed.start(1), "timeout start");
    const auto timeout = timed.begin(caller(), 0);
    check(timed.bind(timeout, 1), "timeout bind");
    timed.expire(1999);
    check(timed.quarantined() == 0, "before deadline");
    timed.expire(2000);
    check(timed.quarantined() == 1, "timeout without completion quarantines");
    check(!timed.retire_completed(timeout, true), "deadline cannot retire writable buffer");
    check(!timed.completion(timeout, 1, caller()), "late timeout completion rejected");
    Ledger queued;
    check(queued.start(1), "queued start");
    const auto q = queued.begin(caller(), 0);
    check(queued.bind(q, 1), "queued bind");
    queued.unsubscribe(q);
    check(!queued.completion(q, 1, caller()), "cancelled subscriber never publishes queued completion");
    check(queued.retire_completed(q, true), "queued completion receipt allows retirement");
    Ledger stopped;
    check(stopped.start(1), "stop start");
    std::array<Ticket, capacity> outstanding{};
    for (unsigned i = 0; i != capacity; ++i) {
        outstanding[i] = stopped.begin(caller(i + 1), 0);
        check(outstanding[i].slot == i, "four fixed slots");
        check(stopped.bind(outstanding[i], 1), "same fake native handle distinct slots");
    }
    stopped.stop();
    check(stopped.quarantined() == capacity, "shutdown quarantines all pending");
    for (std::uint64_t life = 2; life <= 10001; ++life) {
        check(stopped.start(life), "logical Runtime restart");
        check(stopped.begin(caller(1, life), life).slot == capacity, "exhaustion IP-only no submission");
        for (const auto old : outstanding)
            check(!stopped.completion(old, 1, caller(1, life)), "old lifetime completion rejected");
        stopped.stop();
        check(stopped.quarantined() == capacity, "restart never replenishes fixed arena");
    }
    stopped.close_process_coordinator();
    check(!stopped.start(10002), "final coordinator cannot reopen/recreate resources");
    // Deterministic fake provider retains actual pointers and writes after stop.
    // No OS provider is involved and no native response is modified by this test.
    Ledger late;
    std::array<std::array<unsigned char, 16>, capacity> fake_arena{};
    std::array<unsigned char*, capacity> held{};
    std::array<Ticket, capacity> old{};
    check(late.start(1), "fake late-writer start");
    for (unsigned i = 0; i != capacity; ++i) {
        old[i] = late.begin(caller(i + 1), 0);
        check(late.bind(old[i], 42), "fake native handle reuse with fixed messages");
        held[i] = fake_arena[i].data();
    }
    late.stop();
    check(late.start(2), "fake Runtime reuse");
    for (unsigned i = 0; i != capacity; ++i) {
        held[i][0] = static_cast<unsigned char>(i + 1); // deliberately late write
        check(held[i] == fake_arena[i].data(), "late write still addresses original writable arena");
        check(!late.completion(old[i], 42, caller(i + 1, 2)), "late writer cannot publish into reused node");
        check(late.begin(caller(i + 1, 2), 0).slot == capacity, "late writer storage never rebound");
        check(fake_arena[i][0] == i + 1, "late provider write safely retained");
    }
    late.close_process_coordinator();
    for (unsigned i = 0; i != capacity; ++i) {
        held[i][1] = 99; // even after coordinator close, no retirement or reuse
        check(fake_arena[i][1] == 99, "late write after coordinator shutdown remains safe");
    }
    std::puts("MODEL PASS: 8 injected late writes remain in original bounded buffers; no publication/reuse");
    std::puts("MODEL PASS: timeout without completion; 10000 stop/start cycles; four-slot exhaustion IP-only");
    std::printf("MODEL ASSERTIONS: %u passed; 0 failed; 0 skipped\n", assertions);
    return assertions;
}
}
#ifdef SFX_MODEL_ONLY
int main() {
    try { fixture::model_tests(); return 0; }
    catch (const std::exception& e) { std::printf("FAIL: %s\n", e.what()); return 1; }
}
#endif
