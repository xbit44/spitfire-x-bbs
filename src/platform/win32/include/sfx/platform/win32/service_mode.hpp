#pragma once
#include "sfx/platform/win32/installation_security.hpp"
#include <optional>
namespace sfx::platform::win32::installation {
struct ServiceMode {
    Path root;std::wstring owner,id,name;
    bool enabled{true},pending{};
    std::wstring revision{L"legacy-v1"};
    DWORD start_policy{SERVICE_AUTO_START};
};
struct ServiceState {DWORD state{SERVICE_STOPPED},process_id{},win32_exit_code{},start_type{};};
// Registration is distinct from selected hosting mode. Recovery reads only
// expose a protected unfinished choice; Runtime must use the strict reader.
std::optional<ServiceMode> read_service_registration(const Path&,bool recovery=false);
// Protected metadata/receipt validation for UI polling; actions use the full reader.
std::optional<ServiceMode> read_service_selection(const Path&,bool recovery=true);
std::optional<ServiceMode> read_service_mode(const Path&);
ServiceState query_owned_service(const ServiceMode&);
// Poll protected selection and SCM identity without rehashing all programs.
// Actions still use the full strict reader and installed-peer validation.
ServiceState query_selected_service(const ServiceMode&);
// Interactive owner attests a System peer through protected mode+SCM+image,
// without requesting access to the System process token.
void verify_owned_service_peer(const ServiceMode&,DWORD process_id);
bool start_owned_service(const ServiceMode&,std::string& diagnostic);
bool stop_owned_service(const ServiceMode&,DWORD expected_process_id,std::string& diagnostic);
// Elevated, receipt-bound callers only; stopped-installation exclusion is held
// across credential preparation and the durable SCM/metadata transaction.
void configure_owned_service(const Path&,const std::wstring& owner,bool enabled,const std::wstring& operation={},
    std::optional<DWORD> start_policy={});
void ensure_owned_service(const Path&,const std::wstring& owner);
void remove_owned_service(const Path&,const std::wstring& owner);
void remove_obsolete_owned_service(const Receipt&);
bool is_owned_service_runtime(const Path&,const std::wstring& owner) noexcept;
// Exact SCM PID, LocalSystem token and protected installed Runtime verification.
ServiceMode verify_service_runtime_entry(const Path&);
}
