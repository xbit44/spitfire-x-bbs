// Adapted from SFX own Windows caller orchestration; original source preserved.
#pragma once
#include "sfx/application/file_session.hpp"
#include "sfx/platform/linux/file_payload_store.hpp"
namespace sfx::apps::runtime {
struct LinuxCallerFileViewerPorts {
    application::IFileStorage& storage;
    std::filesystem::path installation;
    std::shared_ptr<platform::linux_host::IFilePayloadBindings> bindings;
    std::function<std::optional<application::FileActor>()> actor;
    application::FileListPorts list;
    std::function<application::TerminalOutcome(const application::TerminalDisplayProgram&,
        const std::function<bool()>&)> display;
    std::function<application::FileAuthority()> authority;
};
// Native held-source reads only: no extraction, execution, catalog edits or
// download accounting. Current actor/catalog/payload checked through output.
class LinuxFileViewer final {
public:
    explicit LinuxFileViewer(LinuxCallerFileViewerPorts);
    bool view(application::FileId,bool archive);
private:
    LinuxCallerFileViewerPorts p_;
    bool notice(std::string_view);
};
} // namespace sfx::apps::runtime
