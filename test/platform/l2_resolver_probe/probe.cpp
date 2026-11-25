// Test-only feasibility probe. Not linked to Runtime or shipped to callers.
// Uses only loopback lookups: it does NOT simulate an unresponsive DNS provider.
// PAGE_NOACCESS turns any observed post-retirement buffer write into a test crash.
#include <winsock2.h>
#include <windows.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
constexpr UINT first_message = WM_APP + 16;
constexpr std::size_t slot_count = 4;
struct Slot {
    char* buffer{};
    HANDLE task{};
    bool settled{};
    bool protected_page{};
};
std::array<Slot, slot_count> slots{};
unsigned completions{}, cancelled{}, already{}, unsettled{}, unexpected_messages{};
std::atomic<unsigned> current_round{};
std::atomic<const char*> current_phase{"initialization"};
std::array<std::atomic<ULONG_PTR>, slot_count> buffer_addresses{};
double max_submit_ms{}, max_cancel_ms{}, max_cleanup_ms{};
using Clock = std::chrono::steady_clock;
LONG WINAPI report_exception(EXCEPTION_POINTERS* exception) {
    const auto* record = exception->ExceptionRecord;
    int buffer_index = -1;
    if (record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record->NumberParameters >= 2) {
        const auto address = record->ExceptionInformation[1];
        for (std::size_t i = 0; i != slots.size(); ++i) {
            const auto base = buffer_addresses[i].load();
            if (base && address >= base && address - base < MAXGETHOSTSTRUCT)
                buffer_index = static_cast<int>(i);
        }
    }
    std::printf("NATIVE_EXCEPTION code=%08lx round=%u phase=%s buffer_slot=%d operation=%llu\n",
        record->ExceptionCode, current_round.load(), current_phase.load(), buffer_index,
        static_cast<unsigned long long>(record->ExceptionInformation[0]));
    return EXCEPTION_EXECUTE_HANDLER;
}
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
void require(bool value, const char* label) {
    if (value) return;
    std::printf("FAIL %s error=%lu\n", label, GetLastError());
    std::fflush(stdout);
    // A test failure exits the isolated probe, never frees an unsettled buffer.
    std::exit(1);
}
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message >= first_message && message < first_message + slot_count) {
        auto& slot = slots[message - first_message];
        if (slot.task != reinterpret_cast<HANDLE>(wparam) || slot.protected_page) {
            ++unexpected_messages;
        } else {
            // Do not inspect provider-controlled pointers in this lifetime-only probe.
            slot.settled = true;
            ++completions;
        }
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
void drain(HWND window) {
    MSG message{};
    while (PeekMessageW(&message, window, first_message,
            first_message + static_cast<UINT>(slot_count) - 1, PM_REMOVE))
        DispatchMessageW(&message);
}
void protect(Slot& slot) {
    DWORD previous{};
    require(VirtualProtect(slot.buffer, MAXGETHOSTSTRUCT, PAGE_NOACCESS, &previous) != 0,
        "protect retired native buffer");
    slot.protected_page = true;
}
void cancel(Slot& slot) {
    const auto start = Clock::now();
    const int result = WSACancelAsyncRequest(slot.task);
    const int error = result == 0 ? 0 : WSAGetLastError();
    const double ms = elapsed(start);
    if (ms > max_cancel_ms) max_cancel_ms = ms;
    if (result == 0) { ++cancelled; slot.settled = true; }
    else if (error == WSAEALREADY || error == WSAEINVAL) {
        ++already; slot.settled = true;
    } else { ++unsettled; }
}
void submit(HWND window) {
    // Byte representation of 127.0.0.1; no hostname or user credential input.
    static const std::array<char, 4> address{{127, 0, 0, 1}};
    for (std::size_t i = 0; i != slots.size(); ++i) {
        auto& slot = slots[i];
        slot = Slot{};
        slot.buffer = static_cast<char*>(VirtualAlloc(nullptr, MAXGETHOSTSTRUCT,
            MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        require(slot.buffer != nullptr, "allocate fixed native buffer");
        buffer_addresses[i].store(reinterpret_cast<ULONG_PTR>(slot.buffer));
        const auto start = Clock::now();
        slot.task = WSAAsyncGetHostByAddr(window, first_message + static_cast<UINT>(i),
            address.data(), static_cast<int>(address.size()), AF_INET,
            slot.buffer, MAXGETHOSTSTRUCT);
        const double ms = elapsed(start);
        if (ms > max_submit_ms) max_submit_ms = ms;
        require(slot.task != nullptr, "native loopback submission");
    }
}
void cleanup() {
    const auto start = Clock::now();
    require(WSACleanup() == 0, "final Winsock cleanup");
    const double ms = elapsed(start);
    if (ms > max_cleanup_ms) max_cleanup_ms = ms;
}
void release_buffers(HWND window) {
    // Caller must have completed final WSACleanup before reaching this point.
    drain(window);
    for (auto& slot : slots) if (!slot.protected_page) protect(slot);
    Sleep(5);
    drain(window);
    for (auto& slot : slots) {
        require(VirtualFree(slot.buffer, 0, MEM_RELEASE) != 0, "free retired buffer");
        slot = Slot{};
    }
}
}

int main(int argc, char** argv) {
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetUnhandledExceptionFilter(report_exception);
    std::puts("L2 native feasibility probe: loopback only; not a Windows 7 or stalled-provider qualification");
    const bool cancel_only = argc == 2 && std::strcmp(argv[1], "--cancel-only") == 0;
    const bool observe_cleanup = argc == 2 && std::strcmp(argv[1], "--observe-cleanup") == 0;
    const bool cleanup_only = argc == 2 && std::strcmp(argv[1], "--cleanup-only") == 0;
    require(argc == 1 || cancel_only || observe_cleanup || cleanup_only, "recognized probe mode");
    WNDCLASSW wc{};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SFXL2ResolverLifetimeProbe";
    require(RegisterClassW(&wc) != 0, "register private message-only window");
    const HWND window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
    require(window != nullptr, "create private message-only window");
    const unsigned rounds = observe_cleanup || cleanup_only ? 1 : 64;
    for (unsigned round = 0; round != rounds; ++round) {
        current_round = round;
        current_phase = "startup";
        WSADATA data{};
        require(WSAStartup(MAKEWORD(2, 2), &data) == 0, "Winsock startup");
        current_phase = "submit";
        submit(window);
        if (round % 3 == 1) {
            // Leave a genuine completion queued, then cancel before dispatch.
            MSG message{};
            const auto start = Clock::now();
            while (!PeekMessageW(&message, window, first_message,
                    first_message + static_cast<UINT>(slot_count) - 1, PM_NOREMOVE)
                    && elapsed(start) < 2000.0) Sleep(1);
            require(elapsed(start) < 2000.0, "queued loopback completion deadline");
        }
        if (!observe_cleanup && !cleanup_only && (cancel_only || round % 3 != 2)) {
            current_phase = "cancel";
            for (auto& slot : slots) cancel(slot);
            drain(window); // queued-before-cancel completion may still exist
            current_phase = "guard-after-cancel";
            for (auto& slot : slots) if (slot.settled) protect(slot);
            Sleep(5);
            drain(window);
        }
        // The third branch exercises final cleanup with requests outstanding.
        current_phase = "final-cleanup";
        cleanup();
        if (observe_cleanup) {
            const unsigned before = completions;
            const auto start = Clock::now();
            do {
                drain(window);
                if (completions == slot_count) break;
                Sleep(1);
            } while (elapsed(start) < 2000.0);
            std::printf("after_final_cleanup_completions=%u observation_ms=%.3f\n",
                completions - before, elapsed(start));
            // If completion never arrives the probe cannot prove retirement.
            // Exit without destructing provider-owned memory (test process only).
            require(completions == slot_count, "all outstanding buffers completed after cleanup");
        }
        current_phase = "guard-after-final-cleanup";
        release_buffers(window);
    }
    require(DestroyWindow(window) != 0, "destroy pump window");
    require(UnregisterClassW(wc.lpszClassName, wc.hInstance) != 0, "unregister pump class");
    std::printf("rounds=%u requests=%u cancelled=%u already=%u completions=%u unsettled=%u unexpected=%u\n",
        rounds, rounds * static_cast<unsigned>(slot_count), cancelled, already,
        completions, unsettled, unexpected_messages);
    std::printf("max_submit_ms=%.3f max_cancel_ms=%.3f max_final_cleanup_ms=%.3f\n",
        max_submit_ms, max_cancel_ms, max_cleanup_ms);
    require(unexpected_messages == 0, "no completion after proved retirement or wrong native handle");
    require(unsettled == 0, "loopback cancellation outcomes settled");
    std::puts("PASS loopback observations only; stalled-provider and genuine Windows 7 gates NOT RUN");
    return 0;
}
