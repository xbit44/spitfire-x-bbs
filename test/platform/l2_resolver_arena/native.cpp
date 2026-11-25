// Test-only normal-user fixture. The result arena is in this EXE's writable
// static storage, has no destructor, and is NEVER freed/protected on shutdown.
#include <winsock2.h>
#include <windows.h>
#include <process.h>
#include "lifetime.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <exception>
#include <stdexcept>
#include <type_traits>

namespace {
constexpr UINT first_message = WM_APP + 32;
constexpr std::uint64_t canary = 0x5146495854555245ULL;
struct alignas(16) Buffer {
    std::uint64_t before = canary;
    std::uint64_t before_padding = canary;
    alignas(16) std::array<char, MAXGETHOSTSTRUCT> bytes{};
    std::uint64_t after = canary;
    std::uint64_t after_padding = canary;
};
static_assert(std::is_trivially_destructible<Buffer>::value, "no arena destructor");
// Process-owned, not coordinator-, Runtime-, listener- or request-owned.
std::array<Buffer, fixture::capacity> arena{};
const std::array<char, 4> loopback{{127, 0, 0, 1}};
std::atomic<unsigned> failures{};
std::atomic<bool> coordinator_claimed{};
bool claim_coordinator() noexcept { return !coordinator_claimed.exchange(true); }
HANDLE shutdown_ready{}, shutdown_requested{};
std::atomic<unsigned> checks{};
void check(bool condition, const char* text) {
    if (!condition) throw std::runtime_error(text);
    ++checks;
}
fixture::Identity caller(unsigned node, std::uint64_t generation = 1) {
    return fixture::Identity{{0xabc, 0xdef}, generation, 0x0100007f, node};
}
std::uint64_t now() noexcept { return GetTickCount64(); }
void arena_valid() {
    for (const auto& b : arena) {
        MEMORY_BASIC_INFORMATION info{};
        check(VirtualQuery(b.bytes.data(), &info, sizeof(info)) == sizeof(info), "arena mapped");
        check(info.State == MEM_COMMIT && info.Type == MEM_IMAGE,
            "arena remains process image storage");
        check((info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) == 0 &&
            ((info.Protect & 0xff) == PAGE_READWRITE || (info.Protect & 0xff) == PAGE_WRITECOPY),
            "every native buffer remains writable");
        check(b.before == canary && b.before_padding == canary &&
            b.after == canary && b.after_padding == canary, "bounded native result buffer canaries");
    }
}
struct Native {
    fixture::Ledger ledger;
    std::array<fixture::Identity, fixture::capacity> current{};
    HWND window{};
    unsigned submissions{}, received{}, ignored{}, published{}, message_reuses{};
    unsigned handle_reuses{}, cancel_success{}, cancel_already{};
    std::uintptr_t previous_handle{};
    std::uint64_t max_submit{}, max_cancel{}, max_stop{};
    fixture::Ticket submit(unsigned node, std::uint64_t generation, std::uint64_t clock) {
        current[node - 1] = caller(node, generation);
        auto t = ledger.begin(current[node - 1], clock);
        if (t.slot == fixture::capacity) return t; // no native operation on exhaustion
        const auto started = now();
        HANDLE task = WSAAsyncGetHostByAddr(window, first_message + static_cast<UINT>(t.slot),
            loopback.data(), static_cast<int>(loopback.size()), AF_INET,
            arena[t.slot].bytes.data(), MAXGETHOSTSTRUCT);
        const auto cost = now() - started;
        if (cost > max_submit) max_submit = cost;
        ++submissions;
        check(task != nullptr, "native loopback lookup submitted");
        auto h = reinterpret_cast<std::uintptr_t>(task);
        if (h == previous_handle) ++handle_reuses;
        previous_handle = h;
        check(ledger.bind(t, h), "native handle bound to immutable slot ticket");
        return t;
    }
    void message(UINT message, WPARAM wparam) {
        const auto index = static_cast<std::size_t>(message - first_message);
        if (index >= fixture::capacity) return;
        const auto& slot = ledger.slot(index);
        if (slot.state != fixture::State::pending || slot.handle != wparam) { ++ignored; return; }
        ++received;
        // Only the owning message slot and task can supply this ticket. An
        // uncertain slot is never rebound even if a native HANDLE is reused.
        if (ledger.completion(slot.ticket, wparam, current[slot.identity.node - 1])) ++published;
    }
    void drain() {
        MSG m{};
        unsigned drained = 0;
        while (PeekMessageW(&m, window, first_message,
                first_message + static_cast<UINT>(fixture::capacity) - 1, PM_REMOVE)) {
            check(++drained <= 64, "completion drain bounded");
            DispatchMessageW(&m);
        }
        for (std::size_t i = 0; i != fixture::capacity; ++i) {
            const auto copy = ledger.slot(i);
            if (copy.state == fixture::State::completed)
                check(ledger.retire_completed(copy.ticket, true), "matched completion and drained message slot");
        }
    }
    bool await_queue() {
        const auto start = now(); MSG m{};
        do {
            if (PeekMessageW(&m, window, first_message,
                first_message + static_cast<UINT>(fixture::capacity) - 1, PM_NOREMOVE)) return true;
            Sleep(1); // test harness waiting for an observation; not stop/caller logic
        } while (now() - start < 2000);
        return false;
    }
    int cancel(fixture::Ticket t) {
        ledger.unsubscribe(t); // revoke subscriber BEFORE optional native cancel
        const auto start = now();
        int rc = WSACancelAsyncRequest(reinterpret_cast<HANDLE>(ledger.slot(t.slot).handle));
        const int error = rc == 0 ? 0 : WSAGetLastError();
        const auto cost = now() - start;
        if (cost > max_cancel) max_cancel = cost;
        if (rc == 0) ++cancel_success;
        else if (error == WSAEALREADY || error == WSAEINVAL) ++cancel_already;
        // Neither result reclaims memory. Receipt+drain is the ONLY reusable path.
        drain();
        ledger.uncertain(t);
        return error;
    }
    void logical_stop() {
        const auto start = now();
        ledger.stop(); // FOUR bounded entries; no Winsock call or DNS wait
        const auto cost = now() - start;
        if (cost > max_stop) max_stop = cost;
    }
};
LRESULT CALLBACK proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* n = reinterpret_cast<Native*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (n && message >= first_message && message < first_message + fixture::capacity) {
        n->message(message, wparam); return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
unsigned __stdcall coordinator(void*) {
    // Exactly one owned coordinator, one HWND/class and one Winsock reference.
    // No per-lifetime thread/window/reference; all native buffers are elsewhere.
    try {
        check(claim_coordinator(), "process-wide coordinator cannot be recreated");
        Native n;
        WSADATA data{};
        check(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock initialization");
        WNDCLASSW wc{};
        wc.lpfnWndProc = proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"SFXResolverArenaFixture";
        check(RegisterClassW(&wc) != 0, "one private window class");
        n.window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, wc.hInstance, &n);
        check(n.window != nullptr, "one message-only coordinator window");
        check(n.ledger.start(1), "initial native lifetime");
        arena_valid();
        for (std::uint64_t generation = 1; generation <= 128; ++generation) {
            WSADATA listener_data{};
            check(WSAStartup(MAKEWORD(2, 2), &listener_data) == 0, "simulated listener acquires Winsock reference");
            const auto t = n.submit(1, generation, now());
            check(t.slot == 0, "completed native slot reused");
            check(n.await_queue(), "loopback completion observation within test deadline");
            n.drain();
            check(n.ledger.slot(0).state == fixture::State::unused, "completed/drained slot retired");
            ++n.message_reuses;
            n.logical_stop();
            check(WSACleanup() == 0, "simulated listener releases only its Winsock reference");
            check(n.ledger.start(generation + 1), "Runtime-style restart shares arena/coordinator");
        }
        std::printf("NATIVE PASS: completed lookups=128 message_slot_reuses=%u observed_handle_reuses=%u\n",
            n.message_reuses, n.handle_reuses);
        // Real completion is queued but deliberately not dispatched until after cancel.
        const auto queued = n.submit(1, 129, now());
        check(n.await_queue(), "real completion already queued");
        const auto before_publish = n.published;
        const int queued_error = n.cancel(queued);
        check(queued_error == WSAEALREADY || queued_error == WSAEINVAL, "native already-completed cancellation");
        check(n.published == before_publish, "queued cancelled subscriber not published");
        check(n.ledger.slot(queued.slot).state == fixture::State::unused,
            "receipt/drain not cancellation error proves queued retirement");
        std::puts("NATIVE PASS: already-queued completion; generic cancel return never retires buffer");
        // Try bounded immediate cancels. Successful cancel with no receipt consumes
        // a slot permanently. A fast already-completed result is drained and reusable.
        for (unsigned attempt = 0; attempt != 32 && n.cancel_success == 0; ++attempt) {
            const auto t = n.submit(1, 130 + attempt, now());
            check(t.slot != fixture::capacity, "cancellation attempt has bounded slot");
            n.cancel(t);
        }
        check(n.cancel_success > 0, "genuine successful native cancellation observed");
        std::printf("NATIVE PASS: successful cancellations=%u already_completed=%u\n", n.cancel_success, n.cancel_already);
        // The remaining native operations really run, but their uncertainty is
        // injected at the adapter boundary. No namespace provider/settings changes.
        unsigned injected = 0;
        for (unsigned node = 1; node <= fixture::capacity; ++node) {
            const auto t = n.submit(node, 200 + node, 0);
            if (t.slot == fixture::capacity) break;
            if (injected == 0) { n.ledger.uncertain(t); } // injected cancel failure: do not call cancel
            else if (injected == 1) { n.ledger.expire(2000); } // test clock timeout, do not dispatch
            // Final slot remains outstanding until logical stop below.
            ++injected;
        }
        const unsigned prior_submissions = n.submissions;
        const unsigned prior_publications = n.published;
        n.logical_stop();
        check(n.ledger.quarantined() == fixture::capacity, "four permanent native quarantines");
        DWORD handles_before{}, handles_after{};
        check(GetProcessHandleCount(GetCurrentProcess(), &handles_before) != 0, "initial handle count");
        for (std::uint64_t life = 1000; life < 11000; ++life) {
            WSADATA listener_data{};
            check(WSAStartup(MAKEWORD(2, 2), &listener_data) == 0, "repeated listener Winsock acquisition");
            check(n.ledger.start(life), "exhausted Runtime-style restart");
            check(n.submit(1, life, now()).slot == fixture::capacity, "exhaustion returns IP-only");
            n.drain(); // native late completions are discarded, never used to release quarantine
            n.logical_stop();
            check(WSACleanup() == 0, "repeated listener Winsock release with process owner retained");
        }
        check(GetProcessHandleCount(GetCurrentProcess(), &handles_after) != 0, "final handle count");
        check(n.submissions == prior_submissions, "no new native work after exhaustion");
        check(n.published == prior_publications, "no old result crossed restarted assignment");
        check(handles_after <= handles_before, "no handle accumulation across logical restarts");
        arena_valid();
        const auto native_late_ignored = n.ignored;
        // Force all quarantined message slots to receive their old native handles.
        // These are synthetic message injections, explicitly not new OS DNS replies.
        for (std::size_t i = 0; i != fixture::capacity; ++i)
            check(PostMessageW(n.window, first_message + static_cast<UINT>(i),
                n.ledger.slot(i).handle, 0) != 0, "inject old native handle/message-slot pair");
        n.drain();
        check(n.published == prior_publications, "injected late native messages rejected");
        check(n.ledger.quarantined() == fixture::capacity, "late messages never replenish arena");
        std::printf("NATIVE PASS: 10000 restart cycles; quarantine=4; submissions_after_exhaustion=0; handles=%lu/%lu\n",
            handles_before, handles_after);
        std::printf("NATIVE OBSERVATIONS: native_late_messages_ignored=%u synthetic_old_messages=4\n", native_late_ignored);
        std::printf("NATIVE OBSERVATIONS: received=%u ignored=%u max_submit_ms=%llu max_cancel_ms=%llu max_stop_ms=%llu arena_bytes=%zu\n",
            n.received, n.ignored, static_cast<unsigned long long>(n.max_submit),
            static_cast<unsigned long long>(n.max_cancel), static_cast<unsigned long long>(n.max_stop), sizeof(arena));
        n.ledger.close_process_coordinator();
        check(n.max_stop < 1000, "logical cleanup does not wait for DNS");
        check(SetEvent(shutdown_ready) != 0, "shutdown test ready");
        check(WaitForSingleObject(shutdown_requested, 5000) == WAIT_OBJECT_0, "bounded fixture shutdown command");
        // No cancel/retry/DNS wait in final coordinator close; buffered storage
        // survives window destruction, worker join, WSACleanup and CRT teardown.
        check(DestroyWindow(n.window) != 0, "close sole coordinator window");
        check(UnregisterClassW(wc.lpszClassName, wc.hInstance) != 0, "close sole coordinator class");
        const auto cleanup_start = now();
        check(WSACleanup() == 0, "one final Winsock cleanup");
        const auto cleanup_cost = now() - cleanup_start;
        arena_valid();
        std::printf("NATIVE PASS: coordinator closed; final_WSACleanup_ms=%llu; arena still writable/quarantined\n",
            static_cast<unsigned long long>(cleanup_cost));
        return 0;
    } catch (const std::exception& e) {
        ++failures; std::printf("FAIL NATIVE: %s (Win32 error %lu)\n", e.what(), GetLastError());
        SetEvent(shutdown_ready);
        return 1;
    }
}
}

int main() {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::puts("SFX RESOLVER LIFETIME FEASIBILITY FIXTURE v1 - TEST ONLY");
    std::puts("No installation, credentials, system-network changes, or non-loopback address input.");
    try {
        const auto model_count = fixture::model_tests();
        shutdown_ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        shutdown_requested = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        check(shutdown_ready && shutdown_requested, "two bounded fixture events");
        const auto thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, coordinator, nullptr, 0, nullptr));
        check(thread != nullptr, "one joinable coordinator");
        check(WaitForSingleObject(shutdown_ready, 30000) == WAIT_OBJECT_0, "bounded native fixture deadline");
        const auto stop_start = now();
        check(SetEvent(shutdown_requested) != 0, "request final coordinator close");
        check(WaitForSingleObject(thread, 5000) == WAIT_OBJECT_0, "bounded owned worker join");
        const auto joined_ms = now() - stop_start;
        DWORD exit_code{};
        check(GetExitCodeThread(thread, &exit_code) != 0 && exit_code == 0 && failures == 0,
            "native worker completed all checks");
        check(joined_ms < 1000, "final shutdown not waiting for DNS");
        check(CloseHandle(thread) && CloseHandle(shutdown_ready) && CloseHandle(shutdown_requested),
            "owned thread/event handles released");
        check(!claim_coordinator(), "second coordinator rejected before any new native resource");
        arena_valid();
        std::printf("NATIVE ASSERTIONS: %u passed; 0 failed; 0 skipped; owned_workers=0; join_ms=%llu\n",
            checks.load(), static_cast<unsigned long long>(joined_ms));
        std::printf("TOTAL ASSERTIONS: %u\n", model_count + checks.load());
        std::puts("RESULT: PASS - candidate lifetime fixture only; production L2/L3 remain paused");
        // Return normally. The EXE's trivial static arena is NOT destructed or
        // unmapped by a fixture destructor. Windows ends all remaining process
        // threads before reclaiming the process image. No per-Runtime allocation.
        return 0;
    } catch (const std::exception& e) {
        std::printf("RESULT: FAIL - %s\n", e.what());
        // Test watchdog failure ends this fixture; never detach a worker or free
        // the arena. This is not a production cleanup/recovery strategy.
        return 1;
    }
}
