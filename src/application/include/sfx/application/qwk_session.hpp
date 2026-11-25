#pragma once
#include "sfx/application/qwk_storage.hpp"
#include "sfx/application/menu.hpp"
#include "sfx/application/terminal.hpp"
namespace sfx::application {
struct QwkSessionPorts {
    IQwkStorage& storage;ICallerTerminal& terminal;
    std::function<std::optional<MessageActor>()> actor;
    std::function<Identifier128()> operation;
    std::function<std::optional<MessageArea>()> selection;
    std::function<std::optional<std::int64_t>(std::string_view)> local_entry_utc;
    std::function<std::optional<std::string>()> download,upload;
};
class QwkCallerSession final {
public:
    explicit QwkCallerSession(QwkSessionPorts p):ports_(std::move(p)){}
    std::optional<std::string> action(MenuAction);
private:
    QwkSessionPorts ports_;
    bool say(std::string_view);
    std::optional<std::string> held(std::string_view),settings(),pointers();
};
} // namespace sfx::application
