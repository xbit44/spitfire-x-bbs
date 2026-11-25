#pragma once

#include "sfx/application/control_panel.hpp"

#include <filesystem>
#include <memory>

namespace sfx::platform::win32 {

class WindowsControlPanelMetricSampler final {
public:
    explicit WindowsControlPanelMetricSampler(std::filesystem::path installation_root);
    ~WindowsControlPanelMetricSampler();
    WindowsControlPanelMetricSampler(const WindowsControlPanelMetricSampler&) = delete;
    WindowsControlPanelMetricSampler& operator=(const WindowsControlPanelMetricSampler&) = delete;

    application::ControlPanelMetricSnapshot sample() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace sfx::platform::win32
