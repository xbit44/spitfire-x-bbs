#include <winsock2.h>

#include "alpha_ssh_stream.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "installed_qualification_fixture.hpp"

#include <array>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace {
namespace runtime = sfx::apps::runtime;
namespace win = sfx::platform::win32;
unsigned assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
struct WinsockScope {
    WinsockScope() { WSADATA data{}; require(WSAStartup(MAKEWORD(2,2), &data)==0, "Winsock test startup"); }
    ~WinsockScope() { WSACleanup(); }
};
struct TcpSocket {
    SOCKET value{socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)};
    TcpSocket() { require(value!=INVALID_SOCKET, "test TCP socket created"); }
    ~TcpSocket() { if(value!=INVALID_SOCKET) closesocket(value); }
    int option(int level, int name) const {
        int value_out=0, size=sizeof(value_out);
        require(getsockopt(value, level, name, reinterpret_cast<char*>(&value_out), &size)==0 &&
            (size==sizeof(value_out) || (level==IPPROTO_TCP && name==TCP_NODELAY && size==1)),
            "read back real socket option (Windows may return TCP_NODELAY as one byte)");
        return value_out;
    }
};
void binary_socket_checks(const std::shared_ptr<const runtime::AlphaSshBackend>& backend) {
    std::atomic<bool> stopping{false};
    TcpSocket transfer, unrelated;
    const auto unrelated_receive=unrelated.option(SOL_SOCKET,SO_RCVBUF);
    require(runtime::AlphaSshStream::prepare_listener_socket(static_cast<std::uintptr_t>(transfer.value)),
        "prepare SSH listener receive backpressure before TCP connection");
    require(transfer.option(SOL_SOCKET,SO_RCVBUF)==65536 && unrelated.option(SOL_SOCKET,SO_RCVBUF)==unrelated_receive,
        "SSH receive queue bound is socket local; unrelated socket unchanged");
    require(!runtime::AlphaSshStream::prepare_listener_socket(static_cast<std::uintptr_t>(INVALID_SOCKET)),
        "invalid listener cannot report successful receive policy");
    require(transfer.option(IPPROTO_TCP,TCP_NODELAY)==0 &&
        unrelated.option(IPPROTO_TCP,TCP_NODELAY)==0, "ordinary sockets retain default coalescing");
    runtime::AlphaSshStream stream(backend, static_cast<std::uintptr_t>(transfer.value), stopping, nullptr, nullptr);
    for(unsigned round=0;round<3;++round) {
        require(stream.message_binary_mode(true), "enter SSH binary mode");
        require(transfer.option(IPPROTO_TCP,TCP_NODELAY)==1, "binary transfer sends short replies immediately");
        require(transfer.option(SOL_SOCKET,SO_RCVTIMEO)==100 &&
            transfer.option(SOL_SOCKET,SO_SNDTIMEO)==100, "binary I/O timeout bounds unchanged");
        require(unrelated.option(IPPROTO_TCP,TCP_NODELAY)==0, "no global or unrelated-socket change");
        require(stream.message_binary_mode(false), "leave SSH binary mode");
        require(transfer.option(IPPROTO_TCP,TCP_NODELAY)==0, "normal coalescing restored after transfer");
        require(transfer.option(SOL_SOCKET,SO_RCVTIMEO)==250 &&
            transfer.option(SOL_SOCKET,SO_SNDTIMEO)==5000, "ordinary I/O timeout bounds restored");
    }
    runtime::AlphaSshStream invalid_socket(backend, static_cast<std::uintptr_t>(INVALID_SOCKET), stopping, nullptr, nullptr);
    require(!invalid_socket.message_binary_mode(true) && !invalid_socket.message_binary_mode(false),
        "invalid socket fails binary-mode transition without success");
}
}

int main(int argc, char** argv) {
    try {
        WinsockScope winsock;
        if(argc==2 && std::string_view(argv[1])=="--binary-socket-mode") {
            // Existing task-only installation; do not require/recreate the old
            // September fixture or mutate its receipt for this focused test.
            const std::filesystem::path root{L"C:\\Program Files\\SFX-Section9-Test-20261017-r10"};
            const auto owner=win::current_process_user_sid_string();
            win::installation::verify_runtime_installation(root,owner);
            win::OfflineInstallationGuard guard;
            require(guard.acquire(root,owner).succeeded(), "socket qualification requires stopped fixture");
            const auto key=root/L"Config"/L"ssh_host_ed25519.pem";
            const auto bytes=win::installation::read_file(key,65536);
            const auto modified=std::filesystem::last_write_time(key);
            require(!bytes.empty(), "existing fixture key required; no generation");
            std::string diagnostic;
            const auto backend=runtime::AlphaSshBackend::acquire(root,owner,diagnostic);
            require(static_cast<bool>(backend), "existing protected fixture SSH backend");
            binary_socket_checks(backend);
            require(win::installation::read_file(key,65536)==bytes && std::filesystem::last_write_time(key)==modified,
                "fixture key bytes and timestamp unchanged");
            require(runtime::AlphaSshBackend::statistics().key_generation_attempts==0 &&
                runtime::AlphaSshBackend::statistics().live_sessions==0, "no new key or live SSH session");
            std::cout << "PASS SSH binary-only TCP_NODELAY, repeated restoration, unchanged timeouts and isolated failure\n"
                << "PASS SSH socket mode: " << assertions << " assertions; protected existing fixture key unchanged\n";
            return 0;
        }
        require(argc==1, "closed backend qualification arguments");
        const auto scratch = std::filesystem::absolute(SFX_SSH_BACKEND_TEST_SCRATCH).lexically_normal();
        const auto evidence = scratch / ("ssh-backend-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
        sfx::test::InstalledQualificationFixture fixture(evidence);
        const auto root = fixture.next("ssh-backend");
        const auto owner = win::current_process_user_sid_string();
        require(!owner.empty() && win::provision_alpha_installation_tree(root, owner).succeeded(), "protected test installation");
        const auto before = runtime::AlphaSshBackend::statistics();
        require(before.registration_attempts == 0 && before.key_load_attempts == 0, "no initialization before ownership acquisition");
        std::promise<void> start;
        const auto ready = start.get_future().share();
        std::vector<std::future<std::shared_ptr<const runtime::AlphaSshBackend>>> workers;
        for (unsigned i = 0; i < 8; ++i) workers.push_back(std::async(std::launch::async, [&] {
            ready.wait();
            std::string diagnostic;
            return runtime::AlphaSshBackend::acquire(root, owner, diagnostic);
        }));
        start.set_value();
        std::vector<std::shared_ptr<const runtime::AlphaSshBackend>> owners;
        for (auto& worker : workers) {
            require(worker.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "backend acquisition completes");
            owners.push_back(worker.get());
            require(owners.back() && owners.back() == owners.front(), "all eight acquisitions share one identity object");
        }
        auto stats = runtime::AlphaSshBackend::statistics();
        require(stats.registration_attempts == 1 && stats.key_generation_attempts == 1 && stats.key_load_attempts == 1,
            "exactly one registration/generation/load under concurrent acquisition");
        const auto key = root / "Config" / "ssh_host_ed25519.pem";
        win::WindowsStorageDaclPolicy policy(owner);
        require(policy.assess(key, win::StorageDaclObjectKind::file).status.succeeded(), "host key retains exact protected policy");
        const auto key_time = std::filesystem::last_write_time(key);
        const auto key_size = std::filesystem::file_size(key);
        std::atomic<bool> stopping{false};
        binary_socket_checks(owners.front());
        std::array<std::unique_ptr<runtime::AlphaSshStream>, 4> streams;
        for (auto& stream : streams) stream = std::make_unique<runtime::AlphaSshStream>(owners.front(),
            static_cast<std::uintptr_t>(-1), stopping, nullptr, nullptr);
        const std::weak_ptr<const runtime::AlphaSshBackend> lifetime = owners.front();
        owners.clear(); // Simulates outer-owner teardown before stream destruction.
        require(!lifetime.expired(), "four not-yet-established streams pin shared backend lifetime");
        std::string diagnostic;
        require(!runtime::AlphaSshBackend::acquire(root / "another-installation", owner, diagnostic) &&
            diagnostic == "ssh_backend_identity_in_use", "different installation rejected while streams own identity");
        require(!runtime::AlphaSshBackend::acquire(root, L"S-1-5-18", diagnostic) &&
            diagnostic == "ssh_backend_identity_in_use", "different designated owner rejected without loading");
        auto shared = runtime::AlphaSshBackend::acquire(root / ".", owner, diagnostic);
        require(shared && shared == lifetime.lock(), "same normalized installation shares after outer owner disappears");
        shared.reset();
        for (std::size_t i = 0; i < streams.size(); ++i) {
            streams[i].reset();
            require(lifetime.expired() == (i == streams.size() - 1U), "identity lives until last stream is destroyed");
        }
        require(runtime::AlphaSshBackend::statistics().key_load_attempts == 1, "stream construction/destruction never reloads key");
        bool refused = false;
        try { runtime::AlphaSshStream invalid({}, static_cast<std::uintptr_t>(-1), stopping, nullptr, nullptr); }
        catch (const std::invalid_argument&) { refused = true; }
        require(refused, "session adapter cannot exist without initialized backend");

        // A new Runtime ownership lifetime may reload ONLY after all old stream
        // capabilities are gone. The on-disk installation identity is unchanged.
        shared = runtime::AlphaSshBackend::acquire(root, owner, diagnostic);
        stats = runtime::AlphaSshBackend::statistics();
        require(shared && stats.registration_attempts == 1 && stats.key_load_attempts == 2 && stats.key_generation_attempts == 1,
            "new quiescent Runtime lifetime loads existing key once without registration or rotation");
        require(std::filesystem::last_write_time(key) == key_time && std::filesystem::file_size(key) == key_size,
            "existing key file not rewritten");
        shared.reset();

        const auto valid_key = win::installation::read_file(key, 65536);
        const auto invalid_root = fixture.next("invalid-key");
        require(win::provision_alpha_installation_tree(invalid_root, owner).succeeded(), "protected malformed-key fixture");
        const auto invalid_key = invalid_root / "Config" / "ssh_host_ed25519.pem";
        { std::ofstream file(invalid_key, std::ios::binary); file << "deliberately invalid test PEM"; require(file.good(), "write malformed fixture"); }
        require(policy.apply(invalid_key, win::StorageDaclObjectKind::file).succeeded(), "malformed fixture exact permissions");
        require(!runtime::AlphaSshBackend::acquire(invalid_root, owner, diagnostic) && diagnostic == "ssh_host_key_invalid",
            "failed PEM load publishes no usable backend");
        stats = runtime::AlphaSshBackend::statistics();
        require(stats.key_generation_attempts == 1 && stats.key_load_attempts == 3 && stats.live_sessions == 0,
            "malformed existing key never regenerated; no failed-start session");
        require(!std::filesystem::exists(invalid_root / "Config" / "ssh_host_ed25519.pending"), "failed existing key never staged replacement");
        require(std::filesystem::remove(invalid_key), "remove only malformed synthetic PEM");
        const auto descriptor = win::installation::mutable_descriptor(owner);
        win::installation::create_file(key, valid_key, descriptor.get());
        require(policy.apply(key, win::StorageDaclObjectKind::file).succeeded(), "restore exact file-specific key DACL");
        shared = runtime::AlphaSshBackend::acquire(root, owner, diagnostic);
        if (!shared) throw std::runtime_error("failed partial startup recovery: " + diagnostic);
        require(static_cast<bool>(shared), "failed partial startup does not poison subsequent quiescent valid ownership");
        shared.reset();
        fixture.finish();
        std::cout << "PASS eight concurrent owners: registration=1 generation=1 load=1\n"
            << "PASS SSH binary-only TCP_NODELAY, repeated restoration, unchanged timeouts and isolated failure\n"
            << "PASS four stream lifetime pins, root/owner exclusion, quiescent reload, failed startup\n"
            << "PASS SSH backend: " << assertions << " assertions; no live caller sessions enabled\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL SSH backend: " << error.what() << '\n';
        return 1;
    }
}
