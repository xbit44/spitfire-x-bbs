#pragma once
#include "sfx/platform/win32/installation_security.hpp"
#include <functional>
#include <optional>
#include <string_view>
namespace sfx::platform::win32 {
// Startup-only: release this process's implicit current-directory delete pin
// while preparing directory metadata. All board operations use absolute paths.
// Restore the ordinary launch directory after the bounded attempt.
class PreparationDirectoryScope {
public:
    PreparationDirectoryScope();
    ~PreparationDirectoryScope();
    PreparationDirectoryScope(const PreparationDirectoryScope&)=delete;
    PreparationDirectoryScope& operator=(const PreparationDirectoryScope&)=delete;
private:
    std::wstring previous_;
};
struct PortablePreparationResult { bool confirmed{},cancelled{};std::string detail; };
// Ordinary current CP/Config entry. Prompts once for this attempt; cancellation
// performs no worker launch. Root always comes from the actual current image.
PortablePreparationResult offer_portable_preparation(std::size_t role,HWND parent=nullptr);
std::optional<int> portable_preparation_worker(std::wstring_view) noexcept;
// Recognizes only this verified live installation's protected Prepare record.
std::optional<std::wstring> confirmed_preparation_predecessor(const installation::Receipt& live);
// Consented elevated worker only. Exposed separately for native qualification;
// no production CLI accepts a root, owner, executable or service name.
installation::Receipt prepare_portable_installation(const installation::Path&,
    const std::wstring& owner,const std::function<void()>& progress={},const std::function<void()>& attention={},
    const std::function<void()>& clamav_attention={});
}
