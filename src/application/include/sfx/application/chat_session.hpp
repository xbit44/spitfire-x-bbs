#pragma once
#include <array>
#include "sfx/application/chat.hpp"
#include "sfx/application/terminal_stream.hpp"
namespace sfx::application {
struct ChatSessionPorts {
    ICallerTerminal& terminal;
    std::function<StreamCallerTerminal::EditorByteResult()> read{};
    std::function<bool(std::string_view)> write{};
    std::function<std::uint64_t()> now{};
    std::function<bool()> refresh{};
    std::function<bool()> ansi{};
    std::function<unsigned()> rows{};
    std::function<unsigned()> columns{};
    std::function<std::uint16_t()> node_count{};
    std::function<void(bool)> accepted_key{};
    std::function<std::vector<ChatEmote>()> emotes{};
    std::function<std::optional<std::string>(bool)> private_art{};
    std::function<std::optional<MenuDoorCommands>()> door_commands{};
    std::function<std::optional<std::string>(const MenuDoorSelection&)> door_command{};
    std::function<std::uint8_t()> text_color{};
    std::function<void()> page_accepted{};
    std::function<bool()> hotkeys{};
    std::function<bool()> can_disconnect{};
    std::function<std::string(const ChatParticipant&)> disconnect{};
    std::function<bool()> take_idle_warning{};
};
// Accept only frame decoration; interpreted output may never occupy live cells.
bool valid_pchat_frame(std::string_view) noexcept;
std::string stock_pchat_frame();
class ChatCallerSession final {
public:
    ChatCallerSession(ChatHub&,ChatIdentity,ChatSessionPorts);
    std::optional<ChatExit> enter(ChatRoomId); // null means caller lifetime ended
    std::optional<ChatExit> menu_action(MenuAction);
    std::optional<ChatRoom> selected_room(); // per-call choice; recheck live access/name
    bool online();
    bool safe_events(const std::function<void(std::string)>& notice);
    std::optional<bool> page_sysop(); // true offers Comment to Sysop
    std::uint64_t presentations() const noexcept { return presentations_; }
private:
    bool present(std::string_view);
    std::optional<std::string> line(std::string_view,std::size_t=512);
    bool events(bool conversation,const std::function<void(std::string)>& notice,bool private_mode=false);
    bool acknowledge_end(std::string_view);
    std::string caller_row(const ChatParticipant&,std::uint16_t maximum_node=99) const;
    bool private_chat();
    bool run_conversation(bool private_mode,ChatExit& exit);
    bool compose(bool whisper=false,const ChatEvent* reply=nullptr);
    std::optional<ChatParticipant> choose(bool room_only=false);
    bool invite(const ChatParticipant&);
    std::optional<bool> consent(std::uint64_t request);
    bool override_dnd(const ChatParticipant&);
    bool help(bool private_mode);
    bool pause_panel(std::string_view prompt,bool prompt_visible=false);
    bool show_list(std::string_view title,const std::vector<std::string>& rows);
    bool redraw(bool private_mode,bool full=true);
    bool framed() const;
    void append(std::string_view,ChatLineStyle=ChatLineStyle::text,std::size_t prefix=0,bool yellow=false);
    void show_roster();
    struct HistoryLine { std::string text; ChatLineStyle style{}; std::size_t prefix{}; bool yellow{}; };
    ChatHub& hub_; ChatIdentity identity_; ChatSessionPorts ports_;
    std::deque<HistoryLine> history_;std::string draft_;std::uint64_t presentations_{};
    std::array<std::string,24> public_rows_{};
    std::uint64_t history_version_{},plain_seen_{};
    bool plain_ready_{},plain_prompt_pending_{};
    bool private_return_to_menu_{};
    ChatRoomId selected_room_{};
};
}
