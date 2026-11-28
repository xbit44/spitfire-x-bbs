#include "sfx/platform/network.hpp"
#ifdef _WIN32
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#else
#include "linux_ssh_host_key.hpp"
#endif

#include "alpha_ssh_stream.hpp"
#include "sfx/application/botgate.hpp"
#include <deucessh.h>
#include <deucessh-auth.h>
#include <deucessh-conn.h>
#include <deucessh-algorithms.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>

namespace sfx::apps::runtime {
namespace net = sfx::platform::network;
namespace {
using Outcome = application::InteractiveStreamOutcome;
std::mutex initialization_mutex;
bool algorithms_attempted{false};
bool algorithms_ready{false};
std::weak_ptr<const AlphaSshBackend> shared_backend;
AlphaSshBackendStatistics backend_statistics; // guarded by initialization_mutex
std::atomic<unsigned> live_sessions{0};

#ifdef _WIN32
class FileGuard final {
public:
    explicit FileGuard(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~FileGuard() { if (valid()) CloseHandle(value_); }
    FileGuard(const FileGuard&) = delete;
    FileGuard& operator=(const FileGuard&) = delete;
    bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }
    HANDLE get() const noexcept { return value_; }
private:
    HANDLE value_;
};

std::wstring final_path(HANDLE file) {
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetFinalPathNameByHandleW(file, path.data(),
        static_cast<DWORD>(path.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    return length && length < path.size() ? std::wstring(path.data(), length) : std::wstring{};
}

bool regular_object(HANDLE file, bool directory) {
    BY_HANDLE_FILE_INFORMATION info{};
    return GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        ((info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory &&
        (directory || (info.nNumberOfLinks == 1 && info.nFileSizeHigh == 0 &&
            info.nFileSizeLow > 0 && info.nFileSizeLow <= 16384));
}

std::optional<std::string> exact_ansi_path(const std::filesystem::path& path) {
    const auto& wide = path.native();
    if (wide.empty() || wide.size() > 32767) return std::nullopt;
    BOOL substituted = FALSE;
    const int length = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS,
        wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, &substituted);
    if (!length || substituted) return std::nullopt;
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, wide.data(),
            static_cast<int>(wide.size()), result.data(), length, nullptr, &substituted)
            != length || substituted) return std::nullopt;
    return result;
}

int no_key_password(char*, int, int, void*) { return 0; }

bool load_host_key(const std::filesystem::path& root, const std::wstring& owner,
    std::string& diagnostic) {
    using namespace platform::win32;
    const auto fail = [&](const char* reason) { diagnostic = reason; return false; };
    if (owner.empty()) return fail("ssh_host_key_owner_missing");
    const auto config = root / L"Config";
    const auto key = config / L"ssh_host_ed25519.pem";
    const WindowsStorageDaclPolicy policy(owner);
    const FileGuard root_guard(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const FileGuard config_guard(CreateFileW(config.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const auto root_final = root_guard.valid() ? final_path(root_guard.get()) : L"";
    const auto config_final = config_guard.valid() ? final_path(config_guard.get()) : L"";
    const auto config_security = policy.assess(config, StorageDaclObjectKind::directory);
    if (root_final.empty() || config_final.empty() ||
        !regular_object(root_guard.get(), true) || !regular_object(config_guard.get(), true) ||
        _wcsicmp(config_final.c_str(), (root_final + L"\\Config").c_str()) != 0 ||
        !config_security.status || !config_security.private_access || !config_security.effective_access) {
        return fail("ssh_host_key_config_root_insecure");
    }
    const auto encoded_key = exact_ansi_path(key);
    if (!encoded_key) return fail("ssh_host_key_path_encoding_unsupported");
    const DWORD attributes = GetFileAttributesW(key.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) return fail("ssh_host_key_unreadable");
        // Private staging file, exclusive creation, then no-overwrite publication.
        // An interrupted first-run staging file is preserved for local recovery.
        const auto pending = config / L"ssh_host_ed25519.pending";
        const auto encoded_pending = exact_ansi_path(pending);
        if (!encoded_pending) return fail("ssh_host_key_path_encoding_unsupported");
        {
            const FileGuard created(CreateFileW(pending.c_str(), GENERIC_WRITE, 0,
                nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!created.valid()) return fail("ssh_host_key_pending_exists_or_unwritable");
        }
        if (!policy.apply_for_publication(pending, StorageDaclObjectKind::file, key)) {
            return fail("ssh_host_key_generation_or_protection_failed");
        }
        ++backend_statistics.key_generation_attempts;
        if (dssh_ed25519_generate_key() < 0 ||
            dssh_ed25519_save_key_file(encoded_pending->c_str(), nullptr, nullptr) < 0 ||
            !policy.apply_for_publication(pending, StorageDaclObjectKind::file, key)) {
            return fail("ssh_host_key_generation_or_protection_failed");
        }
        const auto secured = policy.assess(pending, StorageDaclObjectKind::file);
        if (!secured.status || !secured.private_access || !secured.effective_access) {
            return fail("ssh_host_key_generated_permissions_failed");
        }
        {
            const FileGuard flush(CreateFileW(pending.c_str(), GENERIC_WRITE, 0, nullptr,
                OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            if (!flush.valid() || !regular_object(flush.get(), false) ||
                !FlushFileBuffers(flush.get())) return fail("ssh_host_key_flush_failed");
        }
        if (!MoveFileExW(pending.c_str(), key.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return fail("ssh_host_key_publication_failed");
        }
    }
    // No write/delete sharing during verification and the library's read. This
    // excludes key replacement/truncation between ACL assessment and PEM load.
    const FileGuard key_guard(CreateFileW(key.c_str(), GENERIC_READ | READ_CONTROL,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    const auto key_security = policy.assess(key, StorageDaclObjectKind::file);
    if (!key_guard.valid() || !regular_object(key_guard.get(), false) ||
        _wcsicmp(final_path(key_guard.get()).c_str(),
            (config_final + L"\\ssh_host_ed25519.pem").c_str()) != 0 ||
        !key_security.status || !key_security.private_access || !key_security.effective_access) {
        return fail("ssh_host_key_insecure_or_unreadable");
    }
    ++backend_statistics.key_load_attempts;
    if (dssh_ed25519_load_key_file(encoded_key->c_str(), no_key_password, nullptr) < 0) {
        return fail("ssh_host_key_invalid");
    }
    diagnostic = "ssh_host_key_ready";
    return true;
}
#else
bool load_host_key(const std::filesystem::path& root, const std::wstring& owner,
    std::string& diagnostic) {
    return load_linux_ssh_host_key(root, owner, diagnostic,
        backend_statistics.key_generation_attempts, backend_statistics.key_load_attempts);
}
#endif

} // namespace

struct AlphaSshStream::Impl {
    // Declared first: destroyed last, after session cleanup and callback state.
    const std::shared_ptr<const AlphaSshBackend> backend;
    net::Socket socket;
    const std::atomic<bool>& stopping;
    std::function<void(std::string_view)> output;
    std::function<void(SshTerminalDimensions)> resized;
    std::function<void(bool)> activity;
    std::function<void()> encrypted;
    dssh_session session{nullptr};
    dssh_channel channel{nullptr};
    application::Phase3aTransportPasswordLogin* login{nullptr};
    std::atomic<std::uint64_t> setup_deadline{net::monotonic_millis() + 120000U};
    std::atomic<std::uint64_t> binary_deadline{0};
    std::size_t denial_io_remaining{0}; // single-threaded, transport-only path
    bool denial_only{false};
    std::atomic<bool> ansi_terminal{false};

    Impl(std::shared_ptr<const AlphaSshBackend> backend_value,
        std::uintptr_t socket_value, const std::atomic<bool>& stop,
        std::function<void(std::string_view)> observer,
        std::function<void(SshTerminalDimensions)> resize,
        std::function<void(bool)> traffic, std::function<void()> secured)
        : backend(std::move(backend_value)), socket(static_cast<net::Socket>(socket_value)), stopping(stop),
          output(std::move(observer)), resized(std::move(resize)),
          activity(std::move(traffic)), encrypted(std::move(secured)) {
        if (!backend) throw std::invalid_argument("SSH backend required before session creation");
    }
    ~Impl() {
        if (channel) { (void)dssh_chan_close(channel, -1); channel = nullptr; }
        if (session) {
            dssh_session_cleanup(session); // joins demux before callback data dies
            --live_sessions;
        }
    }
    static int io(uint8_t* bytes, size_t count, dssh_session session_value,
        void* data, bool sending) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (self.denial_only) {
            if (count > self.denial_io_remaining) return DSSH_ERROR_TOOLONG;
            self.denial_io_remaining -= count;
        }
        std::size_t offset = 0;
        while (offset < count) {
            const auto deadline = self.setup_deadline.load();
            const auto binary=self.binary_deadline.load();
            if (self.stopping.load() || dssh_session_is_terminated(session_value) ||
                (binary && net::monotonic_millis()>=binary) ||
                (deadline && net::monotonic_millis() >= deadline)) return DSSH_ERROR_TERMINATED;
            const int offered = static_cast<int>(std::min<std::size_t>(
                count - offset, 16384U));
            const int transferred = sending
                ? net::send_bytes(self.socket, reinterpret_cast<const char*>(bytes + offset), offered)
                : net::receive_bytes(self.socket, reinterpret_cast<char*>(bytes + offset), offered);
            if (transferred > 0) { offset += static_cast<std::size_t>(transferred); continue; }
            if (transferred == net::socket_error) {
                if (!sending && net::retry_receive_error()) continue;
            }
            return DSSH_ERROR_TERMINATED;
        }
        return 0;
    }
    static int tx(uint8_t* bytes, size_t count, dssh_session session_value, void* data) noexcept {
        return io(bytes, count, session_value, data, true);
    }
    static int rx(uint8_t* bytes, size_t count, dssh_session session_value, void* data) noexcept {
        return io(bytes, count, session_value, data, false);
    }
    static void terminated(dssh_session, void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        // Refusal output must survive a client sending SERVICE_REQUEST just
        // after NEWKEYS. Half-close, then the bounded denial worker drains the
        // peer before final close; aborting reads here can turn that into RST.
        shutdown(self.socket, self.denial_only ? net::shutdown_send : net::shutdown_both);
    }
    static int password(const uint8_t* user, size_t user_size, const uint8_t* secret,
        size_t secret_size, uint8_t**, size_t*, void* data) noexcept {
        auto& self = *static_cast<Impl*>(data);
        if (!self.login || !user || !secret || self.stopping.load()) return DSSH_AUTH_DISCONNECT;
        const auto result = self.login->authenticate(
            {reinterpret_cast<const char*>(user), user_size},
            {reinterpret_cast<const char*>(secret), secret_size});
        if (const auto notice = self.login->registration_refusal_notice(); !notice.empty()) {
            // This is a fixed neutral registration refusal, never the submitted
            // username/password. Reject before shell/provisional registration.
            // Auth DISCONNECT does not flush DeuceSSH's pending banner; send the
            // documented bounded disconnect explanation instead. No auth success.
            dssh_transport_disconnect(self.session, DSSH_DISCONNECT_BY_APPLICATION, notice.data());
            return DSSH_AUTH_DISCONNECT;
        }
        switch (result) {
            case application::TransportPasswordResult::accepted: return DSSH_AUTH_SUCCESS;
            case application::TransportPasswordResult::rejected: return DSSH_AUTH_FAILURE;
            case application::TransportPasswordResult::disconnect: return DSSH_AUTH_DISCONNECT;
        }
        return DSSH_AUTH_DISCONNECT;
    }
    void dimensions(std::uint32_t cols, std::uint32_t rows) {
        // Metadata only. Never resize/reflow ANSI artwork or alter pager policy.
        if (cols && rows && cols <= 4096U && rows <= 4096U && resized) {
            resized({static_cast<std::uint16_t>(cols), static_cast<std::uint16_t>(rows)});
        }
    }
    static bool receive_allowed(void* data) noexcept {
        const auto& self = *static_cast<Impl*>(data);
        const auto now = net::monotonic_millis();
        const auto setup = self.setup_deadline.load();
        const auto binary = self.binary_deadline.load();
        return !self.stopping.load() && (!setup || now < setup) &&
            (!binary || now < binary);
    }
    static int shell(dssh_channel, const dssh_chan_params* params,
        dssh_chan_accept_result* result, void* data) noexcept {
        try {
            auto& self = *static_cast<Impl*>(data);
            if (self.stopping.load()) return DSSH_ERROR_REJECTED;
            result->zc_cb = nullptr;
            result->zc_cbdata = nullptr;
            // Credit is separate from storage: the library keeps two fixed
            // 64 KiB rings and bounds the retained packet wait. This is a
            // connection policy; advertised credit cannot be revoked when
            // a transfer exits. Existing binary NODELAY restoration stays.
            result->max_window = 16777216;
            result->bounded_receive = true;
            result->receive_allowed = Impl::receive_allowed;
            result->receive_allowed_data = &self;
            if (params && (params->flags & DSSH_PARAM_HAS_PTY)) {
                self.dimensions(params->cols, params->rows);
                if (params->term) {
                    const auto length=strnlen(params->term,65);
                    self.ansi_terminal.store(application::botgate_ansi_terminal({params->term,length}));
                }
            }
            return 0;
        } catch (...) { return DSSH_ERROR_REJECTED; }
    }
    int poll(int requested, int wait_milliseconds = 100) {
        const int ready = dssh_chan_poll(channel, requested | DSSH_POLL_EVENT, wait_milliseconds);
        if (ready > 0 && (ready & DSSH_POLL_EVENT)) {
            dssh_chan_event event{};
            // poll freezes a bounded event batch; do not retain event pointers.
            while (dssh_chan_read_event(channel, &event) == 0) {
                if (event.type == DSSH_EVENT_WINDOW_CHANGE) {
                    dimensions(event.window_change.cols, event.window_change.rows);
                }
            }
        }
        return ready;
    }
};

AlphaSshBackend::AlphaSshBackend(std::filesystem::path root, std::wstring owner)
    : root_(std::move(root)), owner_(std::move(owner)) {}

std::shared_ptr<const AlphaSshBackend> AlphaSshBackend::acquire(const std::filesystem::path& root,
    const std::wstring& owner, std::string& diagnostic) noexcept {
    try {
        const auto absolute_root = std::filesystem::absolute(root).lexically_normal();
        std::lock_guard<std::mutex> lock(initialization_mutex);
        if (const auto existing = shared_backend.lock()) {
#ifdef _WIN32
            const bool same_root=_wcsicmp(existing->root_.c_str(),absolute_root.c_str())==0;
#else
            const bool same_root=existing->root_==absolute_root;
#endif
            if (!same_root || existing->owner_ != owner) {
                diagnostic = "ssh_backend_identity_in_use";
                return {};
            }
            diagnostic = "ssh_host_key_ready";
            return existing;
        }
        // Allocate ownership before changing global crypto state. A failed
        // initialization publishes no capability, so no session can use it.
        auto backend = std::shared_ptr<const AlphaSshBackend>(new AlphaSshBackend(absolute_root, owner));
        if (!algorithms_attempted) {
            algorithms_attempted = true;
            ++backend_statistics.registration_attempts;
            // RFC 4253 excludes hyphens from the software token in the SSH identification line.
            algorithms_ready = dssh_transport_set_version("SpitfireX", nullptr) == 0 &&
                dssh_register_mlkem768x25519_sha256() == 0 &&
                dssh_register_curve25519_sha256() == 0 && dssh_register_ssh_ed25519() == 0 &&
                dssh_register_aes256_ctr() == 0 && dssh_register_hmac_sha2_256() == 0 &&
                dssh_register_none_comp() == 0 &&
                dssh_transport_set_callbacks(AlphaSshStream::Impl::tx, AlphaSshStream::Impl::rx,
                    nullptr, nullptr) == 0;
        }
        if (!algorithms_ready) { diagnostic = "ssh_algorithm_initialization_failed"; return {}; }
        if (!load_host_key(absolute_root, owner, diagnostic)) return {};
        shared_backend = backend;
        return backend;
    } catch (...) { diagnostic = "ssh_initialization_failed"; return {}; }
}

AlphaSshBackendStatistics AlphaSshBackend::statistics() {
    std::lock_guard<std::mutex> lock(initialization_mutex);
    auto result = backend_statistics;
    result.live_sessions = live_sessions.load();
    return result;
}
bool AlphaSshStream::ansi_terminal() const noexcept { return impl_->ansi_terminal.load(); }

AlphaSshStream::AlphaSshStream(std::shared_ptr<const AlphaSshBackend> backend,
    std::uintptr_t socket, const std::atomic<bool>& stopping,
    std::function<void(std::string_view)> output,
    std::function<void(SshTerminalDimensions)> resized,
    std::function<void(bool)> activity, std::function<void()> encrypted)
    : impl_(std::make_unique<Impl>(std::move(backend), socket, stopping, std::move(output),
          std::move(resized), std::move(activity), std::move(encrypted))) {}
AlphaSshStream::~AlphaSshStream() = default;

void AlphaSshStream::finish_shell(const bool normal_logoff) noexcept {
    if (impl_->channel) {
        (void)dssh_chan_close(impl_->channel, normal_logoff ? 0 : 1);
        impl_->channel = nullptr;
    }
}

bool AlphaSshStream::establish(application::Phase3aTransportPasswordLogin& login,
    std::string& diagnostic, const std::string& authentication_banner) noexcept {
    try {
        auto& self = *impl_;
        if (self.session) return false;
        struct ClearLogin {
            Impl& state;
            ~ClearLogin() { state.login = nullptr; }
        } clear_login{self};
        self.session = dssh_session_init(false, 0);
        if (!self.session) { diagnostic = "ssh_session_init_failed"; return false; }
        ++live_sessions;
        self.login = &login;
        if (dssh_session_set_cbdata(self.session, &self, &self, &self, &self) < 0 ||
            dssh_session_set_terminate_cb(self.session, Impl::terminated, &self) < 0 ||
            dssh_transport_handshake(self.session) < 0) {
            diagnostic = "ssh_handshake_failed"; return false;
        }
        // Handshake/NEWKEYS completed: encryption exists before authentication.
        if (self.encrypted) self.encrypted();
        dssh_auth_server_cbs auth{};
        auth.methods_str = "password";
        auth.password_cb = Impl::password;
        auth.cbdata = &self;
        if (!authentication_banner.empty() &&
            dssh_auth_set_banner(self.session, authentication_banner.c_str(), nullptr) < 0) {
            diagnostic = "ssh_banner_failed"; return false;
        }
        if (dssh_auth_server(self.session, &auth, nullptr, nullptr) < 0) {
            diagnostic = "ssh_authentication_failed"; return false;
        }
        self.login = nullptr;
        if (dssh_session_start(self.session) < 0) { diagnostic = "ssh_demux_failed"; return false; }
        dssh_chan_accept_cbs accept{};
        accept.shell = Impl::shell; // NULL exec/subsystem/env: deliberately rejected
        accept.cbdata = &self;
        self.channel = dssh_chan_accept(self.session, &accept, 30000);
        if (!self.channel) { diagnostic = "ssh_shell_required"; return false; }
        self.setup_deadline.store(0);
        diagnostic = "ssh_shell_ready";
        return true;
    } catch (...) { diagnostic = "ssh_session_failed"; return false; }
}

bool AlphaSshStream::deny_capacity(const std::string_view notice) noexcept {
    try {
        auto& self = *impl_;
        if (self.session || notice.empty() || notice.size() > 200) return false;
        const std::string message(notice);
        self.denial_only = true;
        self.denial_io_remaining = 65536;
        self.setup_deadline.store(net::monotonic_millis() + 2000);
        self.session = dssh_session_init(false, 0);
        if (!self.session) return false;
        ++live_sessions;
        if (dssh_session_set_cbdata(self.session, &self, &self, &self, &self) < 0 ||
            dssh_session_set_terminate_cb(self.session, Impl::terminated, &self) < 0 ||
            dssh_transport_handshake(self.session) < 0) return false;
        // tx completes bounded sends before terminate/cleanup. This SSH disconnect
        // is useful to clients without requesting credentials or granting a shell.
        return dssh_transport_disconnect(self.session, DSSH_DISCONNECT_BY_APPLICATION,
            message.c_str()) == 0;
    } catch (...) { return false; }
}

void AlphaSshStream::enable_menu_polling() noexcept { menu_polling_ = true; }

bool AlphaSshStream::prepare_listener_socket(std::uintptr_t socket) noexcept {
    const int maximum=65536;
    int actual{};
#ifdef _WIN32
    const int requested=maximum;
    int length=sizeof(actual);
#else
    // Linux returns twice the requested size, including kernel bookkeeping.
    const int requested=maximum/2;
    socklen_t length=sizeof(actual);
#endif
    const auto native=static_cast<net::Socket>(socket);
    return setsockopt(native,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<const char*>(&requested),sizeof(requested))==0 &&
        getsockopt(native,SOL_SOCKET,SO_RCVBUF,reinterpret_cast<char*>(&actual),&length)==0 &&
        length==sizeof(actual) && actual>0 && actual<=maximum;
}

application::InteractiveStreamReadResult AlphaSshStream::read_some(std::size_t maximum) {
    return read_polled(maximum,100);
}
application::InteractiveStreamReadResult AlphaSshStream::read_ready(std::size_t maximum) {
    return read_polled(maximum,0);
}
application::InteractiveStreamReadResult AlphaSshStream::read_polled(std::size_t maximum,int wait_milliseconds) {
    if (!impl_->channel || !maximum || maximum > application::kInteractiveStreamMaximumReadBytes) {
        return {Outcome::failed, {}};
    }
    std::vector<std::uint8_t> bytes(maximum);
    while (!impl_->stopping.load()) {
        const int ready = impl_->poll(DSSH_POLL_READ,wait_milliseconds);
        if (ready < 0) return {Outcome::failed, {}};
        if (!(ready & DSSH_POLL_READ)) {
            if (menu_polling_ || !wait_milliseconds) return {Outcome::timed_out, {}};
            continue;
        }
        const auto read = dssh_chan_read(impl_->channel, 0, bytes.data(), bytes.size());
        if (read == DSSH_ERROR_NOMORE) {if(message_binary_mode_||!wait_milliseconds)return {Outcome::timed_out,{}};continue;}
        if (read <= 0) return {Outcome::disconnected, {}};
        bytes.resize(static_cast<std::size_t>(read));
        if (impl_->activity) impl_->activity(true); // Channel payload only; no keepalives.
        return {Outcome::completed, std::move(bytes)};
    }
    return {Outcome::shutdown_requested, {}};
}

application::InteractiveStreamOutcome AlphaSshStream::poll_output() {
    if (impl_->stopping.load()) return Outcome::shutdown_requested;
    return impl_->channel ? Outcome::completed : Outcome::disconnected;
}

application::InteractiveStreamWriteResult AlphaSshStream::write_some(std::string_view bytes) {
    return write_polled(bytes,false);
}
application::InteractiveStreamWriteResult AlphaSshStream::write_ready(std::string_view bytes) {
    return write_polled(bytes,true);
}
application::InteractiveStreamWriteResult AlphaSshStream::write_polled(std::string_view bytes,bool ready_only) {
    if (!impl_->channel || bytes.empty()) return {Outcome::failed, 0};
    while (!impl_->stopping.load()) {
        const int ready = impl_->poll(DSSH_POLL_WRITE,ready_only?0:100);
        if (ready < 0) return {Outcome::failed, 0};
        if (!(ready & DSSH_POLL_WRITE)) {if(message_binary_mode_||ready_only)return {Outcome::timed_out,0};continue;}
        const auto written = (ready_only?dssh_chan_write_ready:dssh_chan_write)(impl_->channel, 0,
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size());
        if (written == DSSH_ERROR_NOMORE) {if(message_binary_mode_||ready_only)return {Outcome::timed_out,0};continue;}
        if (written <= 0) return {Outcome::disconnected, 0};
        const auto count = static_cast<std::size_t>(written);
        if (count > bytes.size()) return {Outcome::failed, 0};
        if (impl_->activity) impl_->activity(false);
        if (impl_->output) impl_->output(bytes.substr(0, count));
        return {Outcome::completed, count};
    }
    return {Outcome::shutdown_requested, 0};
}

bool AlphaSshStream::message_binary_mode(bool enabled) noexcept {
    const std::uint32_t receive=enabled?100:250,send_timeout=enabled?100:5000;
    // SSH receive-credit and transfer replies are separate small writes. During
    // a binary transfer, do not hold one behind the peer's delayed TCP ACK.
    // Restore normal coalescing on exit; channel credit and all I/O bounds stay.
    const int immediate=enabled?1:0;
    if(setsockopt(impl_->socket,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<const char*>(&immediate),sizeof(immediate))!=0 ||
        !net::set_timeout(impl_->socket,SO_RCVTIMEO,receive) ||
        !net::set_timeout(impl_->socket,SO_SNDTIMEO,send_timeout))return false;
    message_binary_mode_=enabled;impl_->binary_deadline.store(enabled?net::monotonic_millis()+60000:0);return true;
}
void AlphaSshStream::message_binary_deadline(std::uint64_t deadline) noexcept {if(message_binary_mode_)impl_->binary_deadline.store(deadline);}
} // namespace sfx::apps::runtime
