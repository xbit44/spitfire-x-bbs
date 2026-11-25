// Deliberate Windows 7 asynchronous API; synchronous getnameinfo has no bounded
// cancellation boundary. This suppression applies only to this private adapter.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include "alpha_hostname_lookup.hpp"
#include <array>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>

namespace sfx::apps::runtime {
namespace {
namespace app = application;
constexpr UINT first_message = WM_APP + 32;
constexpr wchar_t window_class[] = L"SFXHostnameCoordinator";
struct alignas(16) NativeBuffer {
    std::array<std::uint8_t, 4> address{};
    std::array<std::uint8_t, 12> alignment_padding{};
    alignas(16) std::array<char, MAXGETHOSTSTRUCT> bytes{};
};
static_assert(std::is_trivially_destructible<NativeBuffer>::value, "arena has no owning destructor");
// EXE process image, never freed/protected/reset on timeout, cancel, close or
// WSACleanup. Even input bytes remain stable. Uncertain slots are NEVER reused.
std::array<NativeBuffer, 4> arena{};
std::atomic<bool> claimed{}, exhausted{}, warned{};
std::atomic<unsigned> workers{};
#if defined(SFX_HOSTNAME_TEST) || defined(SFX_ALPHA_TEST_AUTOMATION)
// Private engineering tests only; excluded from the owner/shipping build.
std::atomic<bool> test_uncertain{};
std::atomic<unsigned> test_submissions{}, test_completions{}, test_ignored{};
#endif

std::string_view native_hostname(const NativeBuffer& buffer, LPARAM packed) noexcept {
    const auto bytes = static_cast<std::size_t>(WSAGETASYNCBUFLEN(packed));
    if (WSAGETASYNCERROR(packed) != 0 || bytes < sizeof(hostent) || bytes > MAXGETHOSTSTRUCT) return {};
    hostent header{};
    std::memcpy(&header, buffer.bytes.data(), sizeof(header));
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.bytes.data());
    const auto start = reinterpret_cast<std::uintptr_t>(header.h_name);
    if (header.h_addrtype != AF_INET || header.h_length != 4 ||
        start < base + sizeof(header) || start >= base + bytes) return {};
    const auto limit = base + bytes - start;
    const auto* end = static_cast<const char*>(std::memchr(header.h_name, 0, limit));
    return end ? std::string_view(header.h_name, static_cast<std::size_t>(end - header.h_name)) : std::string_view{};
}

class Process final {
public:
    ~Process() { close(); }
    std::uint64_t activate(bool enabled) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            if (closed_.load() || lifetime_ == UINT64_MAX) return 0;
            ++lifetime_; enabled_ = enabled; desired_ = {}; published_ = {};
            if (enabled && !claimed.exchange(true)) {
                stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
                if (!stop_event_) { enabled_ = false; return lifetime_; }
                try { thread_ = std::thread([this] { run(); }); }
                catch (...) { CloseHandle(stop_event_); stop_event_ = nullptr; enabled_ = false; }
            }
            return lifetime_;
        } catch (...) { return 0; }
    }
    void stop(std::uint64_t lifetime) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            if (lifetime && lifetime == lifetime_) {
                enabled_ = false; desired_ = {}; published_ = {};
                if (lifetime_ != UINT64_MAX) ++lifetime_;
            }
        } catch (...) {}
    }
    app::HostnameSubscription subscribe(std::uint64_t life, app::NodeAssignmentIdentity id,
        std::string_view ip) noexcept {
        const auto address = app::hostname_source_address(ip);
        if (!address || !life || !id.runtime_epoch.valid() || !id.generation ||
            id.node < 1 || id.node > app::kAlphaNodePoolCapacity) return {};
        try {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            if (closed_.load() || !enabled_ || life != lifetime_ || exhausted.load()) return {};
            const auto now = GetTickCount64();
            if (now > UINT64_MAX - 2000) return {};
            app::HostnameSubscription sub{id, *address, life, now + 2000};
            desired_[id.node - 1] = sub; published_[id.node - 1] = {};
            return sub;
        } catch (...) { return {}; }
    }
    void unsubscribe(const app::HostnameSubscription& sub) noexcept {
        if (!sub.valid()) return;
        try {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            const auto i = sub.assignment.node - 1;
            if (desired_[i] == sub) { desired_[i] = {}; published_[i] = {}; }
        } catch (...) {}
    }
    app::HostnameHint read(const app::HostnameSubscription& sub) noexcept {
        if (!sub.valid()) return {};
        try {
            std::lock_guard<std::mutex> lock(mailbox_mutex_);
            const auto& p = published_[sub.assignment.node - 1];
            if (!closed_.load() && enabled_ && sub.lifetime == lifetime_ && p.subscription == sub &&
                desired_[sub.assignment.node - 1] == sub) return p.hint;
        } catch (...) {}
        return {};
    }
    void close() noexcept {
        if (closed_.exchange(true)) return;
        if (stop_event_) SetEvent(stop_event_);
        if (thread_.joinable()) thread_.join();
        if (stop_event_) { CloseHandle(stop_event_); stop_event_ = nullptr; }
        // Trivial process-image arena deliberately outlives this destructor.
    }
private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* p = reinterpret_cast<Process*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (p && message >= first_message && message < first_message + 4) {
            p->completion(static_cast<std::size_t>(message - first_message), wparam, lparam);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    void completion(std::size_t i, std::uintptr_t handle, LPARAM packed) noexcept {
        const auto& r = model_.slot(i);
        if (r.state != app::HostnameSlotState::pending || !handle || r.handle != handle) {
#if defined(SFX_HOSTNAME_TEST) || defined(SFX_ALPHA_TEST_AUTOMATION)
            ++test_ignored;
#endif
            return;
        }
#if defined(SFX_HOSTNAME_TEST) || defined(SFX_ALPHA_TEST_AUTOMATION)
        ++test_completions;
#endif
        (void)model_.complete(i, handle, native_hostname(arena[i], packed), GetTickCount64());
    }
    void drain(HWND window) noexcept {
        MSG message{};
        // Bound dispatch per pump. If saturated, no completion slot is retired
        // until a subsequent pump actually observes the private queue drained.
        unsigned count = 0;
        while (count < 64 && PeekMessageW(&message, window, first_message, first_message + 3, PM_REMOVE)) {
            DispatchMessageW(&message); ++count;
        }
        if (!PeekMessageW(&message, window, first_message, first_message + 3, PM_NOREMOVE)) model_.drained();
    }
    void run() noexcept {
        ++workers;
        WSADATA data{};
        const bool winsock = WSAStartup(MAKEWORD(2, 2), &data) == 0;
        WNDCLASSW wc{}; wc.lpfnWndProc = window_proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = window_class;
        const bool registered = winsock && RegisterClassW(&wc) != 0;
        const HWND window = registered ? CreateWindowExW(0, window_class, L"", 0, 0, 0, 0, 0,
            HWND_MESSAGE, nullptr, wc.hInstance, this) : nullptr;
        if (window) {
            std::uint64_t active_lifetime{};
            std::array<std::uint64_t, 4> cancelled{};
            try { while (!closed_.load()) {
                std::array<app::HostnameSubscription, app::kAlphaNodePoolCapacity> desired{};
                std::uint64_t lifetime{}; bool enabled{};
                {
                    std::lock_guard<std::mutex> lock(mailbox_mutex_);
                    desired = desired_; lifetime = lifetime_; enabled = enabled_;
                }
                if (lifetime != active_lifetime) {
                    (void)model_.activate(lifetime, enabled); active_lifetime = lifetime;
                }
                const auto now = GetTickCount64();
                model_.synchronize(desired, now);
                drain(window);
                for (std::size_t i = 0; i < 4; ++i) {
                    const auto& r = model_.slot(i);
                    if (r.state == app::HostnameSlotState::quarantined && r.handle && cancelled[i] != r.request) {
                        cancelled[i] = r.request;
                        (void)WSACancelAsyncRequest(reinterpret_cast<HANDLE>(r.handle));
                        // ALL return values leave the slot permanently quarantined.
                    }
                }
                for (unsigned count = 0; count < 4 && !closed_.load(); ++count) {
                    const auto index = model_.next(GetTickCount64());
                    if (!index) break;
                    {
                        std::lock_guard<std::mutex> lock(mailbox_mutex_);
                        if (closed_.load() || !enabled_ || lifetime_ != active_lifetime) {
                            model_.uncertain(*index); break;
                        }
                        // Reservation linearizes before stop; native submission
                        // below never holds the mailbox lock over Winsock.
                    }
                    const auto r = model_.slot(*index);
                    arena[*index].address = r.address;
                    const HANDLE task = WSAAsyncGetHostByAddr(window, first_message + static_cast<UINT>(*index),
                        reinterpret_cast<const char*>(arena[*index].address.data()), 4, AF_INET,
                        arena[*index].bytes.data(), MAXGETHOSTSTRUCT);
                    if (!task || !model_.bind(*index, r.request, reinterpret_cast<std::uintptr_t>(task))) model_.uncertain(*index);
#if defined(SFX_HOSTNAME_TEST) || defined(SFX_ALPHA_TEST_AUTOMATION)
                    ++test_submissions;
                    if (test_uncertain.load()) model_.uncertain(*index);
#endif
                }
                if (model_.exhausted()) exhausted.store(true);
                const auto publications = model_.publications();
                {
                    std::lock_guard<std::mutex> lock(mailbox_mutex_);
                    if (enabled_ && lifetime_ == active_lifetime) for (std::size_t i = 0; i < 4; ++i)
                        if (publications[i].subscription == desired_[i]) published_[i] = publications[i];
                }
                if (WaitForSingleObject(stop_event_, 20) != WAIT_TIMEOUT) break;
            } } catch (...) { /* optional resolver degrades; native buffers stay valid */ }
            model_.close(); // no cancellation/DNS wait on final close
            if (model_.exhausted()) exhausted.store(true);
            DestroyWindow(window);
        }
        if (registered) UnregisterClassW(window_class, wc.hInstance);
        if (winsock) WSACleanup();
        --workers;
    }
    std::mutex mailbox_mutex_;
    std::array<app::HostnameSubscription, app::kAlphaNodePoolCapacity> desired_{};
    std::array<app::HostnamePublication, app::kAlphaNodePoolCapacity> published_{};
    std::uint64_t lifetime_{};
    bool enabled_{};
    std::atomic<bool> closed_{};
    HANDLE stop_event_{};
    std::thread thread_;
    app::HostnameLookupModel model_;
};
Process& process() { static Process instance; return instance; }
}
std::uint64_t start_alpha_hostname_lookup(bool enabled) noexcept { return process().activate(enabled); }
void stop_alpha_hostname_lookup(std::uint64_t life) noexcept { process().stop(life); }
application::HostnameSubscription subscribe_alpha_hostname(std::uint64_t life,
    application::NodeAssignmentIdentity id, std::string_view source) noexcept { return process().subscribe(life, id, source); }
void unsubscribe_alpha_hostname(const application::HostnameSubscription& sub) noexcept { process().unsubscribe(sub); }
application::HostnameHint read_alpha_hostname(const application::HostnameSubscription& sub) noexcept { return process().read(sub); }
bool alpha_hostname_exhausted() noexcept { return exhausted.load(); }
bool take_alpha_hostname_exhaustion_warning() noexcept { return exhausted.load() && !warned.exchange(true); }
void close_alpha_hostname_process() noexcept { process().close(); }
unsigned alpha_hostname_worker_count() noexcept { return workers.load(); }
#ifdef SFX_ALPHA_TEST_AUTOMATION
void alpha_hostname_test_uncertain(bool enabled) noexcept { test_uncertain.store(enabled); }
HostnameTestStatistics alpha_hostname_test_statistics() noexcept {
    return {test_submissions.load(), test_completions.load(), test_ignored.load()};
}
#endif
} // namespace sfx::apps::runtime
