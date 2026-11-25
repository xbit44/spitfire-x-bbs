#pragma once
#include <string>
namespace sfx::platform::win32 {
// Called only by Runtime after exact SCM/receipt/image/System attestation.
// Never changes the process token or tokens inherited by Door children.
void activate_service_file_owner(const std::wstring& installation_owner);
// Only the consented preparation worker, after current image/ordinary-parent/
// nonce attestation. No Door/program is launched by this worker.
void activate_preparation_file_owner(const std::wstring& current_owner);
bool service_file_owner_enabled(const std::wstring& owner) noexcept;
class ServiceFileOwnerScope final {
public:
    explicit ServiceFileOwnerScope(const std::wstring& owner) noexcept;
    ServiceFileOwnerScope(const std::wstring& owner, bool read_metadata) noexcept;
    ~ServiceFileOwnerScope();
    ServiceFileOwnerScope(const ServiceFileOwnerScope&)=delete;
    ServiceFileOwnerScope& operator=(const ServiceFileOwnerScope&)=delete;
    bool valid() const noexcept{return valid_;}
    bool active() const noexcept{return changed_;}
private:
    void* previous_{};
    bool changed_{},valid_{};
};
}
