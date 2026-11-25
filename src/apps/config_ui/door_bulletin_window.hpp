#pragma once
#include "sfx/application/local_door_administration.hpp"
#include "sfx/application/door_bulletin_preview.hpp"
#include "sfx/platform/win32/desktop_theme.hpp"
#include <functional>
#include <memory>
#include <utility>
namespace sfx::config_ui {
struct DoorBulletinWindowPorts {
    std::function<std::optional<application::DoorBulletinCatalog>()> load;
    std::function<application::LocalDoorResult(const application::DoorBulletinChange&)> save;
    std::function<std::optional<application::DoorBulletinPreview>(application::DoorBulletinId,bool,std::int64_t)> preview;
    std::function<std::vector<std::pair<application::DoorId,std::string>>()> doors;
    std::function<application::Identifier128()> operation;
};
class DoorBulletinWindow final {
public:
    DoorBulletinWindow(HINSTANCE,HWND,platform::win32::DesktopAppearance&,DoorBulletinWindowPorts);
    ~DoorBulletinWindow();
    bool show();
    bool translate(MSG&);
    bool confirm_close();
    void close();
private:
    struct Impl;std::unique_ptr<Impl> impl_;
};
}
