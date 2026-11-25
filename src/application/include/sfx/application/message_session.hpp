#pragma once
#include "sfx/application/message_compose.hpp"
#include "sfx/application/message_reader.hpp"
#include "sfx/application/menu.hpp"
namespace sfx::application {
struct MessageSessionPorts {
    MessageComposePorts compose;
    std::function<std::string()> caller_name;
    std::function<std::string(std::int64_t)> date;
    std::function<bool()> safe_boundary;
    std::function<TerminalDisplayPagingPolicy()> paging;
    std::function<bool()> lightbars_enabled{};
    std::function<bool()> legacy_pipe_colors{};
};
struct MessageReadNavigation {
    bool skip{}, list_return{}, previous{}, next{}, start_at_end{};
    std::string list_summary{};
};
class MessageCallerSession final {
public:
    explicit MessageCallerSession(MessageSessionPorts ports):ports_(std::move(ports)){}
    bool initialize();
    bool refresh_selection();
    bool login_scan();
    std::optional<std::string> action(MenuAction);
    const std::optional<MessageArea>& selection() const noexcept{return selected_;}
private:
    MessageSessionPorts ports_;
    std::optional<MessageArea> selected_;
    std::optional<bool> preview_;
    bool reply(const OpenedMessage&);
    std::optional<bool> preview(unsigned footer_row);
    MessageReaderOutcome read(const MessageQueryOptions&,bool headers=false,std::optional<std::int64_t> number={},
        MessageReadNavigation={},std::optional<MessageQueryContext> context={});
    std::optional<std::vector<MessageArea>> conferences(char scope);
    std::optional<char> scan_scope();
    std::optional<std::string> browse_results(const std::vector<MessageArea>&,const MessageQueryOptions&,bool allow_skip);
    std::optional<std::string> enter(bool sysop);
    std::optional<std::string> read_menu();
    std::optional<std::string> personal_menu();
    std::optional<std::string> specific_caller();
    MessageReaderOutcome read_conference(LocalEchoId,bool allow_skip=false);
};
}
