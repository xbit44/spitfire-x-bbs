#include "alpha_runtime_host.hpp"
#include "ansi_playback_file.hpp"
#include "runtime_control_dispatch.hpp"
#include "alpha_hostname_lookup.hpp"
#include "alpha_rlogin.hpp"
#ifdef SFX_ALPHA_TEST_AUTOMATION
#include "alpha_ssh_stream.hpp"
#include <fstream>
#endif

#include "sfx/application/local_control_protocol.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/service_secret_bridge.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/trusted_display_asset_reader.hpp"

#include <windows.h>
#include <shellapi.h>
#include <sddl.h>

#include <array>
#include <atomic>
#include <mutex>
#include <thread>
#include <cstdint>
#include <cwchar>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace app = sfx::application;
namespace runtime = sfx::apps::runtime;
namespace win32 = sfx::platform::win32;
std::atomic<bool> service_stop_requested{false};
bool service_dispatched{};
std::wstring service_first_start_operation;
SERVICE_STATUS_HANDLE service_status_handle{};
std::atomic<HANDLE> service_worker_thread{nullptr};
std::atomic<HANDLE> service_stop_wake{nullptr};
std::mutex service_control_io_mutex;
bool service_waiting_control_io{};
std::mutex service_status_mutex;
void report_service_state(DWORD state,DWORD exit_code=NO_ERROR,DWORD specific=0) {
    std::lock_guard<std::mutex> lock(service_status_mutex);
    SERVICE_STATUS status{};status.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    status.dwCurrentState=state;status.dwWin32ExitCode=exit_code;
    status.dwServiceSpecificExitCode=specific;
    status.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    status.dwWaitHint=state==SERVICE_STOP_PENDING||state==SERVICE_START_PENDING?30000:0;
    status.dwCheckPoint=state==SERVICE_STOP_PENDING||state==SERVICE_START_PENDING?1:0;
    if(service_status_handle)SetServiceStatus(service_status_handle,&status);
}
void report_service_error(const char* diagnostic) noexcept {
    try {
        std::wstring message;
        for(std::size_t i=0;diagnostic&&diagnostic[i]&&i<512;++i) {
            const auto ch=static_cast<unsigned char>(diagnostic[i]);message+=ch>=32&&ch<127?static_cast<wchar_t>(ch):L'?';
        }
        if(const auto log=RegisterEventSourceW(nullptr,L"Spitfire-X BBS")) {
            const wchar_t* strings[]={message.c_str()};
            (void)ReportEventW(log,EVENTLOG_ERROR_TYPE,0,1,nullptr,1,0,strings,nullptr);DeregisterEventSource(log);
        }
    }catch(...){/* SCM still receives the truthful failed status. */}
}
DWORD WINAPI service_control(DWORD control,DWORD,void*,void*) {
    if(control==SERVICE_CONTROL_STOP||control==SERVICE_CONTROL_SHUTDOWN) {
        report_service_state(SERVICE_STOP_PENDING);service_stop_requested.store(true);
        if(const auto wake=service_stop_wake.load())SetEvent(wake);
    }
    return NO_ERROR;
}
bool read_control_frame(win32::NamedPipeConnection& connection,app::LocalControlFrame& frame) {
    struct Wait {Wait(){std::lock_guard<std::mutex> lock(service_control_io_mutex);service_waiting_control_io=true;}
        ~Wait(){std::lock_guard<std::mutex> lock(service_control_io_mutex);service_waiting_control_io=false;}} wait;
    if(service_dispatched&&service_stop_requested.load())return false;
    return connection.read_frame(frame);
}
bool write_control_frame(win32::NamedPipeConnection& connection,const app::LocalControlFrame& frame) {
    struct Wait {Wait(){std::lock_guard<std::mutex> lock(service_control_io_mutex);service_waiting_control_io=true;}
        ~Wait(){std::lock_guard<std::mutex> lock(service_control_io_mutex);service_waiting_control_io=false;}} wait;
    if(service_dispatched&&service_stop_requested.load())return false;
    return connection.write_frame(frame);
}

struct OwnerBootstrap {
    DWORD process_id{0};
    std::array<std::uint8_t, app::kLocalControlNonceBytes> nonce{};
};

bool read_exact(const HANDLE handle, void* destination, DWORD bytes) {
    auto* cursor = static_cast<std::uint8_t*>(destination);
    while (bytes != 0) {
        DWORD read = 0;
        if (!ReadFile(handle, cursor, bytes, &read, nullptr) || read == 0) return false;
        cursor += read;
        bytes -= read;
    }
    return true;
}

std::optional<HANDLE> owner_handle_from_command_line() {
    int count = 0;
    wchar_t** arguments = CommandLineToArgvW(GetCommandLineW(), &count);
    if (arguments == nullptr) return std::nullopt;
    HANDLE result = INVALID_HANDLE_VALUE;
    bool valid = count == 2;
    constexpr wchar_t prefix[] = L"--owner-handle=";
    if (valid && wcsncmp(arguments[1], prefix, _countof(prefix) - 1U) == 0) {
        wchar_t* end = nullptr;
        const unsigned long long raw = wcstoull(arguments[1] + _countof(prefix) - 1U, &end, 10);
        if (end == arguments[1] + _countof(prefix) - 1U || *end != L'\0' || raw == 0 ||
            raw > std::numeric_limits<ULONG_PTR>::max()) valid = false;
        else result = reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(raw));
    } else {
        valid = false;
    }
    LocalFree(arguments);
    if (!valid || result == INVALID_HANDLE_VALUE) return std::nullopt;
    return result;
}

bool constant_time_equal(
    const std::array<std::uint8_t, app::kLocalControlNonceBytes>& left,
    const std::array<std::uint8_t, app::kLocalControlNonceBytes>& right) noexcept {
    std::uint8_t difference = 0;
    for (std::size_t index = 0; index < left.size(); ++index)
        difference |= static_cast<std::uint8_t>(left[index] ^ right[index]);
    return difference == 0;
}

void erase_payload(std::vector<std::uint8_t>& payload) noexcept {
    if (!payload.empty()) SecureZeroMemory(payload.data(), payload.size());
    payload.clear();
}

app::LocalControlFrame response_frame(
    const app::LocalControlMessageType type,
    const std::uint32_t request_id,
    std::vector<std::uint8_t> payload) {
    app::LocalControlFrame response;
    response.type = type;
    response.request_id = request_id;
    response.payload = std::move(payload);
    return response;
}

}  // namespace

int sfx::apps::runtime::run_runtime_process(void* inherited_owner_handle) {
    const bool persistent=service_dispatched&&inherited_owner_handle==nullptr;
    const auto root=win32::alpha_installation_root();
    std::wstring installation_owner=win32::current_process_user_sid_string();
    const HANDLE owner_handle = static_cast<HANDLE>(inherited_owner_handle);
    OwnerBootstrap owner;
    HANDLE owner_process=nullptr;
    if(persistent) {
        const auto mode=win32::installation::verify_service_runtime_entry(root);
        installation_owner=mode.owner;
        win32::activate_service_file_owner(mode.owner);
        win32::configure_service_secret_scope(true,win32::load_service_secret_bridge(root,mode.owner));
    } else {
    if(win32::installation::read_service_mode(root))return 17;
    win32::configure_service_secret_scope(false);
    const bool owner_read = read_exact(owner_handle, &owner.process_id, sizeof(owner.process_id)) &&
        read_exact(owner_handle, owner.nonce.data(), static_cast<DWORD>(owner.nonce.size()));
    CloseHandle(owner_handle);
    if (!owner_read || owner.process_id == 0) return 10;
    owner_process = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, owner.process_id);
    if (owner_process == nullptr) return 11;
    }

#ifdef SFX_ALPHA_TEST_AUTOMATION
    // One launch only, in the compile-time disposable root. No shipping control,
    // installed configuration field, network change or production fault switch.
    alpha_hostname_test_uncertain(std::filesystem::exists(
        win32::alpha_installation_root() / L"hostname-uncertain.test"));
#endif
    app::AnsiPlaybackSettings playback_settings(
        [&] { return read_ansi_playback_file(root, installation_owner); },
        win32::ansi_playback_path_rules());
    AlphaRuntimeHost host(root,{},persistent?installation_owner:std::wstring{});
    app::AlphaControlResponse start = host.start();
    if(persistent&&start.code!=app::LocalControlResponseCode::ok&&start.code!=app::LocalControlResponseCode::setup_required)
        report_service_error(start.diagnostic_code.c_str());
    start.runtime_epoch = host.runtime_epoch();
    if (start.diagnostic_code.find("duplicate") != std::string::npos) {
        CloseHandle(owner_process);
        return 12;
    }
    if(persistent&&!service_first_start_operation.empty()&&
       (start.code==app::LocalControlResponseCode::ok||start.code==app::LocalControlResponseCode::setup_required)){
        try{win32::installation::acknowledge_prepared_first_start(root,installation_owner,service_first_start_operation);}
        catch(...){report_service_error("first_start_acknowledgment_failed_board_stopped");(void)host.shutdown();return 14;}
    }
    if(persistent)report_service_state(SERVICE_RUNNING);
    const auto names = win32::installation_object_names(win32::alpha_installation_root());
    app::AlphaControlResponse shutdown_result;
    bool finished=false;
    do {
    if(persistent&&service_stop_requested.load()) {
        shutdown_result=host.shutdown();
        if(shutdown_result.code==app::LocalControlResponseCode::ok)break;
        // Keep ownership and recovery state alive after an incomplete stop.
        service_stop_requested.store(false);report_service_state(SERVICE_RUNNING);
    }
    auto connection = win32::accept_local_control_client(names.pipe_name,
        installation_owner,owner.process_id,persistent?200:15000);
    if (!connection.valid()) {
        if(persistent)continue;
        (void)host.shutdown();
        CloseHandle(owner_process);
        SecureZeroMemory(owner.nonce.data(), owner.nonce.size());
        return 13;
    }
    if(persistent) {
        try {
            ULONG client{};
            win32::installation::require(GetNamedPipeClientProcessId(connection.native_handle(),&client),"service_client_pid_unavailable");
            const auto receipt=win32::installation::read_receipt(root,installation_owner);
            win32::installation::verify_installed_program_process(receipt,client,win32::installation::control_panel_program_index);
            if(!win32::fill_secure_random(owner.nonce.data(),owner.nonce.size()))continue;
            app::LocalControlFrame challenge;challenge.type=app::LocalControlMessageType::hello;
            challenge.request_id=1;challenge.payload=app::encode_hello_payload(owner.nonce);
            if(!write_control_frame(connection,challenge))continue;
        } catch(...) {continue;}
    }

    app::LocalControlFrame request;
    bool authenticated = false;
    bool stop = false;
    std::uint32_t last_request_id = 0;
    app::LocalControlMessageType last_request_type = app::LocalControlMessageType::hello;
    std::optional<app::LocalControlFrame> last_response;
    app::AlphaStatusSnapshotCache status_cache;
    while (!stop && (!persistent||!service_stop_requested.load())&&
        (persistent||WaitForSingleObject(owner_process, 0) == WAIT_TIMEOUT) &&
        read_control_frame(connection,request)) {
        struct EraseRequestPayload {
            std::vector<std::uint8_t>& value;
            ~EraseRequestPayload() { erase_payload(value); }
        } erase_request{request.payload};
        if (request.request_id == last_request_id) {
            if (last_response && request.type == last_request_type) {
                if (!write_control_frame(connection,*last_response)) break;
                continue;
            }
            break;
        }
        app::LocalControlFrame response;
        if (!authenticated) {
            std::array<std::uint8_t, app::kLocalControlNonceBytes> received{};
            const bool valid = request.type == app::LocalControlMessageType::hello &&
                app::decode_hello_payload(request.payload, received) &&
                constant_time_equal(received, owner.nonce);
            SecureZeroMemory(received.data(), received.size());
            erase_payload(request.payload);
            if (!valid) break;
            authenticated = true;
            response = response_frame(app::LocalControlMessageType::hello_response,
                request.request_id, app::encode_control_response(start));
        } else if (request.type == app::LocalControlMessageType::shutdown_request &&
            request.payload.empty()) {
            const app::AlphaControlResponse result = host.shutdown();
            shutdown_result = result;
            response = response_frame(app::LocalControlMessageType::shutdown_response,
                request.request_id, app::encode_control_response(result));
            stop = persistent?result.code==app::LocalControlResponseCode::ok:
                result.code != app::LocalControlResponseCode::shutting_down;
        } else {
            auto dispatched=runtime::dispatch_runtime_control(host,request,status_cache);
            if(!dispatched)break;
            response=std::move(*dispatched);
        }
        if (response.payload.empty() || !write_control_frame(connection,response)) break;
        last_request_id = request.request_id;
        last_request_type = request.type;
        last_response = response;
    }
    connection.disconnect();SecureZeroMemory(owner.nonce.data(),owner.nonce.size());
    if(!persistent) {if(!stop)shutdown_result=host.shutdown();finished=true;}
    else if(stop)finished=true;
    // A closed/crashed UI only ends this connection. Callers and jobs stay owned.
    } while(!finished);
    close_alpha_hostname_process(); // callers joined first; process arena remains writable
    close_alpha_rlogin_process(); // same final ownership boundary, including DNS coordinator
#ifdef SFX_ALPHA_TEST_AUTOMATION
    // Test-build-only post-join evidence. No control capability, secrets or
    // installation override are added to a shipping executable.
    const auto resources = alpha_caller_resource_counts();
    const auto ssh = AlphaSshBackend::statistics();
    std::ofstream audit(win32::alpha_installation_root() / L"Logs" /
        (L"SFXRuntime-qa-cleanup-" + std::to_wstring(GetCurrentProcessId()) + L".txt"));
    audit << "shutdown=" << static_cast<unsigned>(shutdown_result.code)
        << "\nworkers=" << resources.workers << "\ncontexts=" << resources.contexts
        << "\nsockets=" << resources.sockets << "\nssh_sessions=" << ssh.live_sessions
        << "\nkey_loads=" << ssh.key_load_attempts
        << "\nresolver_workers=" << alpha_hostname_worker_count() << "\n";
    const auto dns = alpha_hostname_test_statistics();
    std::ofstream resolver_audit(win32::alpha_installation_root() / L"Logs" /
        (L"SFXRuntime-qa-hostname-" + std::to_wstring(GetCurrentProcessId()) + L".txt"));
    resolver_audit << "submissions=" << dns.submissions << "\ncompletions=" << dns.completions
        << "\nignored=" << dns.ignored << "\nexhausted=" << alpha_hostname_exhausted() << "\n";
#endif
    if(owner_process)CloseHandle(owner_process);
    SecureZeroMemory(owner.nonce.data(), owner.nonce.size());
    return shutdown_result.code == app::LocalControlResponseCode::ok ? 0 : 14;
}

namespace {
void WINAPI windows_service_main(DWORD argument_count,LPWSTR* arguments) {
    try {
        // Register before receipt/ACL validation so a failed headless startup
        // can always report Stopped instead of stranding SCM in Start Pending.
        if(!argument_count||!arguments||!arguments[0])return;
        service_status_handle=RegisterServiceCtrlHandlerExW(arguments[0],service_control,nullptr);
        if(!service_status_handle)return;
        report_service_state(SERVICE_START_PENDING);
        const auto mode=win32::installation::read_service_mode(win32::alpha_installation_root());
        win32::installation::require(mode.has_value(),"service_mode_missing");
        win32::installation::require(!_wcsicmp(mode->name.c_str(),arguments[0]),"service_dispatch_name_mismatch");
        win32::installation::require(win32::current_process_user_sid_string()==L"S-1-5-18","service_requires_localsystem");
        if(const auto first=win32::installation::read_prepared_first_start(mode->root,mode->owner)){
            const auto expected=L"--sfx-first-start="+first->operation;
            win32::installation::require(argument_count==2&&arguments[1]&&expected==arguments[1],"prepared_board_stopped_open_SFX_and_choose_Start_BBS");
            service_first_start_operation=first->operation;
        }
        // Owner clients can attest the service PID/image and observe its exit.
        // No VM, duplicate-handle, create-thread or termination right is granted.
        const auto sddl=L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;0x101000;;;"+mode->owner+L")";
        PSECURITY_DESCRIPTOR raw{};
        win32::installation::require(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(),SDDL_REVISION_1,&raw,nullptr),"service_process_security_invalid");
        win32::installation::Descriptor process_security(raw);
        win32::installation::require(SetKernelObjectSecurity(GetCurrentProcess(),DACL_SECURITY_INFORMATION,raw),"service_process_observer_security_failed");
        HANDLE worker{};
        win32::installation::require(DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&worker,0,FALSE,DUPLICATE_SAME_ACCESS),"service_worker_handle_failed");
        service_worker_thread.store(worker);
        const auto wake=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        win32::installation::require(wake!=nullptr,"service_stop_event_failed");
        service_stop_wake.store(wake);
        // SCM now has a live service endpoint/PID; BBS readiness is reported by
        // the same Runtime status model and may still be Setup/Recovery required.
        service_dispatched=true;
        std::atomic<bool> done{false};
        std::thread cancel_waiter([&] {
            for(;;) {
                WaitForSingleObject(wake,INFINITE);
                if(done.load())break;
                // Cancellation is not sticky. Repeat only while a control wait
                // is announced, covering stop-before-ReadFile without cancelling
                // database, caller or file cleanup performed on the same thread.
                while(service_stop_requested.load()&&!done.load()) {
                    {
                        // Teardown takes this same lock before allowing the
                        // Runtime thread to enter any persistence/cleanup I/O.
                        std::lock_guard<std::mutex> lock(service_control_io_mutex);
                        if(service_waiting_control_io)CancelSynchronousIo(worker);
                    }
                    Sleep(25);
                }
            }
        });
        int result=14;
        try{result=runtime::run_runtime_process(nullptr);}
        catch(const std::exception& e){report_service_error(e.what());result=14;}
        catch(...){report_service_error("service_runtime_unexpected_failure");result=14;}
        done.store(true);SetEvent(wake);cancel_waiter.join();
        service_stop_wake.store(nullptr);CloseHandle(wake);
        report_service_state(SERVICE_STOPPED,result==0?NO_ERROR:ERROR_SERVICE_SPECIFIC_ERROR,static_cast<DWORD>(result));
    }catch(const std::exception& e){report_service_error(e.what());report_service_state(SERVICE_STOPPED,ERROR_SERVICE_SPECIFIC_ERROR,14);}
    catch(...){report_service_error("service_unexpected_failure");report_service_state(SERVICE_STOPPED,ERROR_SERVICE_SPECIFIC_ERROR,14);}
    if(const auto worker=service_worker_thread.exchange(nullptr))CloseHandle(worker);
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR command_line, int) {
    if(command_line&&std::wstring_view(command_line)==L"--windows-service") {
        SERVICE_TABLE_ENTRYW table[]={{const_cast<LPWSTR>(L""),windows_service_main},{nullptr,nullptr}};
        return StartServiceCtrlDispatcherW(table)?0:16;
    }
    const auto owner_handle = owner_handle_from_command_line();
    if (!owner_handle) return 2;
    return sfx::apps::runtime::run_runtime_process(*owner_handle);
}
