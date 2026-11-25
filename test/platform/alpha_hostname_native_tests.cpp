// Compile the exact adapter in a separate test-only target so fault hooks are
// absent from the shipping library/executables. All real queries use loopback.
#define SFX_HOSTNAME_TEST
#include "../../src/apps/runtime/alpha_hostname_lookup.cpp"
#include <cstdio>
#include <stdexcept>

namespace sfx::apps::runtime {
namespace {
unsigned assertions{};
void require(bool ok, const char* label) { ++assertions; if (!ok) throw std::runtime_error(label); }
app::NodeAssignmentIdentity identity(unsigned generation) {
    app::Identifier128 epoch; epoch.bytes[0] = 1;
    return {epoch, 1, generation};
}
template<class Predicate> void await(Predicate predicate, const char* label) {
    const auto start = GetTickCount64();
    while (!predicate()) { require(GetTickCount64() - start < 3000, label); Sleep(1); }
}
void writable_arena() {
    for (auto& buffer : arena) {
        MEMORY_BASIC_INFORMATION info{};
        require(VirtualQuery(buffer.bytes.data(), &info, sizeof(info)) == sizeof(info), "arena mapped");
        require(info.State == MEM_COMMIT && info.Type == MEM_IMAGE, "arena is process image");
        require(!(info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
            ((info.Protect & 255) == PAGE_READWRITE || (info.Protect & 255) == PAGE_WRITECOPY), "arena writable");
    }
}
void native_bounds() {
    NativeBuffer buffer{}; hostent header{};
    header.h_name = buffer.bytes.data() + sizeof(header); header.h_addrtype = AF_INET; header.h_length = 4;
    std::memcpy(buffer.bytes.data(), &header, sizeof(header));
    std::memcpy(header.h_name, "LOCALHOST", 10);
    const auto bytes = static_cast<WORD>(sizeof(header) + 10);
    require(native_hostname(buffer, WSAMAKEASYNCREPLY(bytes, 0)) == "LOCALHOST", "bounded native name view");
    for (WORD size = 0; size < bytes; ++size)
        require(native_hostname(buffer, WSAMAKEASYNCREPLY(size, 0)).empty(), "every short native result rejected");
    require(native_hostname(buffer, WSAMAKEASYNCREPLY(MAXGETHOSTSTRUCT + 1, 0)).empty(), "oversized native result rejected");
    require(native_hostname(buffer, WSAMAKEASYNCREPLY(bytes, WSAHOST_NOT_FOUND)).empty(), "native lookup error IP-only");
    for (auto pointer : {static_cast<char*>(nullptr), buffer.bytes.data(), buffer.bytes.data() + bytes,
            buffer.bytes.data() + MAXGETHOSTSTRUCT}) {
        header.h_name = pointer; std::memcpy(buffer.bytes.data(), &header, sizeof(header));
        require(native_hostname(buffer, WSAMAKEASYNCREPLY(bytes, 0)).empty(), "invalid native name pointer never read");
    }
    header.h_name = buffer.bytes.data() + sizeof(header); header.h_addrtype = AF_INET6;
    std::memcpy(buffer.bytes.data(), &header, sizeof(header));
    require(native_hostname(buffer, WSAMAKEASYNCREPLY(bytes, 0)).empty(), "unexpected native family rejected");
    header.h_addrtype = AF_INET; header.h_length = 16; std::memcpy(buffer.bytes.data(), &header, sizeof(header));
    require(native_hostname(buffer, WSAMAKEASYNCREPLY(bytes, 0)).empty(), "unexpected native address length rejected");
}
int native_test() {
    std::puts("SFX production hostname adapter - loopback-only engineering test");
    try {
        native_bounds();
        require(start_alpha_hostname_lookup(false) != 0, "Off lifetime created");
        require(workers == 0 && test_submissions == 0, "Off creates no worker or native lookup");
        DWORD before{}, after{}, cold{};
        std::uint64_t stop_max{};
        for (unsigned generation = 1; generation <= 256; ++generation) {
            WSADATA listener{}; require(WSAStartup(MAKEWORD(2, 2), &listener) == 0, "simulated listener reference");
            const auto life = start_alpha_hostname_lookup(true);
            const auto sub = subscribe_alpha_hostname(life, identity(generation), "127.0.0.1");
            require(sub.valid(), "exact subscription accepted");
            await([&] { return read_alpha_hostname(sub).size != 0; }, "loopback hostname available within deadline");
            require(app::normalize_hostname_hint(read_alpha_hostname(sub).view()).has_value(), "native buffer parsed to bounded hint");
            auto stale = sub; ++stale.assignment.generation;
            require(!read_alpha_hostname(stale).size, "generation mismatch cannot read mailbox");
            stale = sub; stale.assignment.runtime_epoch.bytes[0] = 2;
            require(!read_alpha_hostname(stale).size, "epoch mismatch cannot read mailbox");
            stale = sub; stale.address[3] = 2;
            require(!read_alpha_hostname(stale).size, "source mismatch cannot read mailbox");
            unsubscribe_alpha_hostname(sub);
            require(!read_alpha_hostname(sub).size, "unsubscribe immediate, no caller retained");
            const auto started = GetTickCount64(); stop_alpha_hostname_lookup(life);
            stop_max = std::max(stop_max, GetTickCount64() - started);
            require(WSACleanup() == 0, "listener cleanup while process resolver reference retained");
            if (generation == 1) require(GetProcessHandleCount(GetCurrentProcess(), &cold) != 0, "cold initial handles");
            if (generation == 128) require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "warmed completed-state handles");
            if (generation > 128) {
                require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "each measured completed-state handle count");
                require(after <= before, "no accumulated handle at any measured completed restart");
            }
        }
        require(test_submissions == 256 && test_completions == 256 && !alpha_hostname_exhausted(),
            "128 warmup plus 128 measured actual completed/drained slot reuses");
        require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "post-completion handle count");
        std::printf("OBSERVATION cold/warmed/128-measured-cycle handles=%lu/%lu/%lu\n", cold, before, after);
        require(after <= before, "no handle growth over completed logical restarts");
        // Native requests really execute. Unproved retirement is injected only
        // at the adapter boundary; no DNS/provider/network settings are changed.
        test_uncertain = true;
        const auto life = start_alpha_hostname_lookup(true);
        const auto pending = subscribe_alpha_hostname(life, identity(257), "127.0.0.1");
        await([] { return alpha_hostname_exhausted(); }, "four forced uncertain slots exhaust safely");
        require(!read_alpha_hostname(pending).size, "uncertain result never published");
        require(take_alpha_hostname_exhaustion_warning(), "exactly one exhaustion warning claim");
        require(GetProcessHandleCount(GetCurrentProcess(), &before) != 0, "quarantined-state initial handles");
        const auto submissions = test_submissions.load();
        for (unsigned generation = 130; generation < 10130; ++generation) {
            require(!take_alpha_hostname_exhaustion_warning(), "no repeated exhaustion warning");
            const auto current = start_alpha_hostname_lookup(true);
            require(!subscribe_alpha_hostname(current, identity(generation), "127.0.0.1").valid(), "exhaustion returns IP-only");
            stop_alpha_hostname_lookup(current);
        }
        require(test_submissions == submissions && submissions == 260, "no native submissions after four quarantines");
        writable_arena();
        require(GetProcessHandleCount(GetCurrentProcess(), &after) != 0, "quarantined-state final handles");
        std::printf("OBSERVATION exhausted-cycle handles=%lu/%lu\n", before, after);
        require(after <= before, "no handle growth over exhausted logical restarts");
        const auto closing = GetTickCount64(); close_alpha_hostname_process();
        require(alpha_hostname_worker_count() == 0, "one coordinator joined with outstanding/uncertain operations");
        require(GetTickCount64() - closing < 1000 && stop_max < 1000, "shutdown and logical stop do not wait DNS timeout");
        require(!start_alpha_hostname_lookup(true), "closed process coordinator cannot reopen");
        writable_arena();
        std::printf("PASS production adapter: assertions=%u; failed=0; skipped=0; completed=%u; submissions=%u; late_ignored=%u; handles=%lu/%lu; workers=0; stop_max_ms=%llu\n",
            assertions, test_completions.load(), test_submissions.load(), test_ignored.load(), before, after,
            static_cast<unsigned long long>(stop_max));
        return 0;
    } catch (const std::exception& e) {
        close_alpha_hostname_process(); std::printf("FAIL production adapter: %s\n", e.what()); return 1;
    }
}
}
}
int main() { return sfx::apps::runtime::native_test(); }
