#pragma once
#include "sfx/application/chat.hpp"
#include "sfx/application/node_pool.hpp"
namespace sfx::application {
struct ChatNoticeRequest {
    // Null target and zero room means All Callers.
    std::optional<NodeAssignmentIdentity> target;
    ChatRoomId room{};
    std::string text;
    bool override_dnd{};
};
std::vector<std::uint8_t> encode_chat_notice(const ChatNoticeRequest&);
std::optional<ChatNoticeRequest> decode_chat_notice(const std::vector<std::uint8_t>&);
enum class ChatOperatorAction : std::uint8_t {status,availability,start,answer,poll,send,end};
struct ChatOperatorRequest {
    ChatOperatorAction action{ChatOperatorAction::status};
    std::optional<NodeAssignmentIdentity> target;
    Identifier128 view;
    std::uint64_t token{},page{},sequence{};
    bool available{},override_dnd{};
    std::string text;
};
std::vector<std::uint8_t> encode_chat_operator_request(const ChatOperatorRequest&);
std::optional<ChatOperatorRequest> decode_chat_operator_request(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_chat_operator_snapshot(const ChatOperatorSnapshot&);
std::optional<ChatOperatorSnapshot> decode_chat_operator_snapshot(const std::vector<std::uint8_t>&);
}
