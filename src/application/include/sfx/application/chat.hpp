#pragma once
#include "sfx/application/door_storage.hpp"
#include "sfx/application/menu.hpp"
#include <deque>
#include <functional>
#include <map>
#include <mutex>

namespace sfx::application {
using ChatRoomId=std::uint16_t;
// Local caller flow only; never serialized into configuration, records or IPC.
enum class ChatExit { main_menu, goodbye, quick_goodbye, chat_menu };
struct ChatRoom {
    ChatRoomId id{1}; std::string name{"Lobby"}; bool enabled{true};
    DoorRequirements access{DoorSecurityMode::minimum,10,false};
};
struct ChatDoorCommand {
    MenuId menu{MenuId::main}; char key{'K'}; DoorId door{};
    std::string name; bool enabled{true}; DoorRequirements access;
};
struct ChatConfiguration {
    std::int64_t revision{1}; ChatRoomId entry{1}; bool exclude_sysop_statistics{true};
    std::vector<ChatRoom> rooms{{}};
    std::vector<ChatDoorCommand> commands;
};
struct ChatPreferences {
    std::int64_t revision{}; bool dnd{},ghost{},notices{true};
};
struct ChatIdentity {
    Identifier128 session; AccountId account{}; std::uint16_t node{};
    bool operator==(const ChatIdentity& other) const noexcept;
    bool operator!=(const ChatIdentity& other) const noexcept {return !(*this==other);}
};
struct ChatParticipant {
    ChatIdentity identity; std::string name,activity;
    std::uint16_t level{},sysop_threshold{999}; bool sysop{};
    ChatPreferences preferences; ChatRoomId room{}; bool safe{};
    std::optional<std::uint32_t> private_chat_minutes;
    std::optional<std::int64_t> private_seconds_remaining_today;
};
enum class ChatEventKind : std::uint8_t { public_text,private_text,presence,notice,invite,started,ended };
struct ChatEvent {
    ChatEventKind kind{}; ChatIdentity sender; std::string name,text;
    std::uint64_t request{}; ChatRoomId room{}; bool override_dnd{};
};
enum class ChatResult : std::uint8_t { ok,unavailable,denied,dnd,busy,full,conflict,invalid };
struct ChatConfigurationResult {ChatResult code{ChatResult::unavailable};std::optional<ChatConfiguration> configuration{};};
std::vector<std::uint8_t> encode_chat_result(const ChatConfigurationResult&);
std::optional<ChatConfigurationResult> decode_chat_result(const std::vector<std::uint8_t>&);
struct ChatRequestResult {ChatResult code{ChatResult::unavailable};std::uint64_t request{};};
struct ChatPage {std::uint64_t request{};ChatIdentity caller;std::string name;};
struct ChatOperatorLine {std::uint64_t sequence{};std::string text;};
struct ChatOperatorSnapshot {
    ChatResult code{ChatResult::unavailable}; bool available{},pending{},active{},paused{};
    std::uint64_t token{};std::string name;std::vector<ChatPage> pages;
    std::vector<ChatOperatorLine> lines;
};
std::string_view chat_result_text(ChatResult) noexcept;
bool valid_chat_configuration(const ChatConfiguration&) noexcept;
bool chat_room_access(const ChatRoom&,const ChatParticipant&) noexcept;
std::string chat_safe_text(std::string_view,std::size_t maximum=512);
std::string chat_node(std::uint16_t node,std::uint16_t configured_nodes=99);
std::vector<std::uint8_t> encode_chat_configuration(const ChatConfiguration&);
std::optional<ChatConfiguration> decode_chat_configuration(const std::vector<std::uint8_t>&);

// Runtime owns one hub. No conversation persistence and no callback while the
// caller holds an unrelated Runtime lock. Exact session identities survive node
// reuse. Public history is bounded to pending delivery, never a join backlog.
class ChatHub final {
public:
    bool configure(const ChatConfiguration&);
    ChatConfiguration configuration() const;
    ChatResult change_configuration(const ChatConfiguration&,const std::function<bool()>& commit);
    bool attach(ChatParticipant);
    bool refresh(const ChatParticipant&);
    void detach(const ChatIdentity&,std::uint64_t now=0);
    void safe(const ChatIdentity&,bool);
    std::optional<ChatParticipant> participant(const ChatIdentity&) const;
    std::vector<ChatParticipant> visible(const ChatIdentity&,bool room_only=false) const;
    std::vector<ChatRoom> rooms(const ChatIdentity&) const;
    ChatResult join(const ChatIdentity&,ChatRoomId);
    void leave(const ChatIdentity&);
    ChatResult say(const ChatIdentity&,std::string_view,bool action=false);
    ChatResult text(const ChatIdentity&,const ChatIdentity&,std::string_view,bool whisper=false,bool override_dnd=false);
    ChatRequestResult invite(const ChatIdentity&,const ChatIdentity&,std::uint64_t now,bool override_dnd=false);
    ChatResult answer(const ChatIdentity&,std::uint64_t,bool accept,std::uint64_t now);
    bool invite_pending(const ChatIdentity&,std::uint64_t request) const;
    void cancel(const ChatIdentity&,std::uint64_t);
    void tick(std::uint64_t now);
    void end(const ChatIdentity&,std::uint64_t now=0);
    std::optional<std::uint64_t> private_usage_millis(const ChatIdentity&,std::uint64_t now) const;
    std::optional<ChatIdentity> peer(const ChatIdentity&) const;
    std::vector<ChatEvent> poll(const ChatIdentity&,bool safe_boundary);
    std::vector<ChatEvent> poll_request(const ChatIdentity&,std::uint64_t request);
    ChatResult notice(std::optional<ChatIdentity> target,ChatRoomId room,std::string_view,bool override_dnd);
    ChatOperatorSnapshot operator_status() const;
    void operator_available(bool);
    ChatRequestResult page(const ChatIdentity&,std::uint64_t now);
    bool page_pending(const ChatIdentity&,std::uint64_t request) const;
    void cancel_page(const ChatIdentity&,std::uint64_t request);
    ChatOperatorSnapshot operator_start(const ChatIdentity&,Identifier128 view,
        std::uint64_t page,bool override_dnd,std::uint64_t now);
    ChatOperatorSnapshot operator_poll(std::uint64_t token,Identifier128 view,
        std::uint64_t acknowledged,std::uint64_t now);
    ChatResult operator_send(std::uint64_t token,Identifier128 view,
        std::uint64_t sequence,std::string_view,std::uint64_t now);
    void operator_end(std::uint64_t token,Identifier128 view,std::uint64_t now);
    bool claim_operator(const ChatIdentity&,std::uint64_t now);
    bool operator_active(const ChatIdentity&) const;
    std::optional<std::uint64_t> operator_paused_millis(const ChatIdentity&,std::uint64_t now) const;
    void end_operator(const ChatIdentity&,std::uint64_t now);
private:
    struct Session {ChatParticipant caller;std::deque<ChatEvent> events;std::optional<ChatIdentity> peer;ChatRoomId suspended_room{};
        std::uint64_t page{},page_expires{},operator_token{},paused_millis{},private_started{},private_millis{};};
    struct Invite {std::uint64_t id{},expires{};ChatIdentity from,to;bool override_dnd{};};
    struct Operator {
        std::uint64_t token{};Identifier128 view;ChatIdentity caller;
        bool pending{},active{},pause{},override_dnd{};
        std::uint64_t lease{},claim_by{},started{},next_sequence{1},last_send{};
        std::string name,last_send_text;std::deque<ChatOperatorLine> lines;
    };
    Session* find(const ChatIdentity&);
    const Session* find(const ChatIdentity&) const;
    bool visible_to(const Session&,const Session&,bool room_only=false) const;
    bool push(Session&,ChatEvent);
    void room_notice(const ChatParticipant&,std::string_view);
    void leave_locked(Session&);
    void end_locked(Session&,std::string_view,std::uint64_t now,bool departure=false);
    void restore_room(Session&);
    void finish_invite(std::map<std::uint64_t,Invite>::iterator,std::string_view);
    bool operator_line(Operator&,std::string);
    void end_operator_locked(Operator&,std::uint64_t now,std::string_view);
    ChatOperatorSnapshot operator_snapshot(const Operator&) const;
    mutable std::mutex mutex_;
    ChatConfiguration config_;
    std::map<std::uint16_t,Session> sessions_;
    std::map<std::uint64_t,Invite> invites_;
    std::uint64_t next_request_{1};
    bool operator_available_{};
    std::map<std::uint64_t,Operator> operators_;
};
struct ChatEmote {std::string command;bool targeted{};std::string text;};
std::vector<ChatEmote> parse_chat_emotes(std::string_view);
std::optional<std::string> render_chat_emote(const ChatEmote&,std::string_view caller,std::string_view target);
} // namespace sfx::application
