#pragma once
#include "sfx/application/file_management.hpp"
#include "sfx/platform/win32/desktop_theme.hpp"
#include <functional>
#include <memory>
namespace sfx::file_manager {
enum class ViewNavigationKind { page,member,archive_index,close };
struct ViewNavigation {ViewNavigationKind kind{ViewNavigationKind::page};application::Identifier128 cursor;std::uint16_t selection{};};
// Modeless INERT preview. Navigation is returned as a typed intent; this window
// has no storage, native-file path, SQL or execution/extraction capability.
class PreviewWindow final {
public:
    PreviewWindow(HINSTANCE,HWND,platform::win32::DesktopAppearance&,std::function<void(ViewNavigation)>);
    ~PreviewWindow();
    bool show(const application::FileManagementViewPage&);
    void busy(bool);
    void close();
    bool translate(MSG&);
    HWND window()const noexcept;
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
} // namespace sfx::file_manager
