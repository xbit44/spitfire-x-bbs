#pragma once
#include "sfx/application/file_session.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
namespace sfx::apps::runtime {
struct CallerFileViewerPorts {
    application::IFileStorage& storage;
    std::filesystem::path installation;
    std::wstring owner;
    std::function<std::optional<application::FileActor>()> actor;
    application::FileListPorts list;
    std::function<application::TerminalOutcome(const application::TerminalDisplayProgram&,
        const std::function<bool()>&)> display;
    std::function<application::FileAuthority()> authority;
};
// Native held-source reads only: no extraction, execution, catalog edits or
// download accounting. Current actor/catalog/payload checked through output.
class AlphaFileViewer final {
public:
    explicit AlphaFileViewer(CallerFileViewerPorts);
    bool view(application::FileId,bool archive);
private:
    CallerFileViewerPorts p_;
    bool notice(std::string_view);
};
} // namespace sfx::apps::runtime
