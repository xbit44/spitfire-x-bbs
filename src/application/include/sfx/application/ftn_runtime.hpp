#pragma once
#include "sfx/application/ftn_work.hpp"
#include "sfx/application/ftn_configuration_transport.hpp"
namespace sfx::application {
enum class FtnRuntimeAction : std::uint8_t { status,poll,scan,queues,connections };
struct FtnRuntimeRequest {FtnRuntimeAction action{};std::string network,link;std::uint32_t offset{};bool include_hold{};};
struct FtnSessionStatus {
    std::uint64_t serial{};bool active{},outbound{},completed{},peer_verified{};
    std::string network,link,reason; // Presentation names; never routing keys.
    std::uint64_t received_bytes{},sent_bytes{};
    std::uint32_t received_files{},sent_files{};
    FtnFileProgress send,receive;
    std::string peer_address,endpoint; // Configured peer address and dial endpoint, never credentials.
};
struct FtnConnectionStatus {std::string network,link,name;bool enabled{},outbound{};};
// A bounded history survives fast sessions between Panel refreshes. Names in
// responses are presentation only; routing keys remain in connection choices.
struct FtnActivity {std::uint64_t utc_seconds{},session{};std::string network,link,message;};
struct FtnRuntimeResponse {
    FtnConfigurationCode code{FtnConfigurationCode::unavailable};FtnMailerMode mode{FtnMailerMode::disabled};
    std::int64_t saved_revision{},active_revision{};
    bool capable{},paused{},serial_recovery_required{},classic_listening{},tls_listening{},processing{},more_connections{};
    std::uint16_t classic_port{},tls_port{};std::string diagnostic,classic_error,tls_error,processing_error;
    std::uint64_t processed_receipts{},held_receipts{},imported_messages{},replayed_messages{},scanned_messages{};
    std::vector<FtnSessionStatus> sessions;std::vector<FtnConnectionStatus> connections;
    std::optional<FtnQueuePage> queues;
    std::vector<FtnActivity> activity;
};
std::vector<std::uint8_t> encode_ftn_runtime_request(const FtnRuntimeRequest&);
std::optional<FtnRuntimeRequest> decode_ftn_runtime_request(const std::vector<std::uint8_t>&);
std::vector<std::uint8_t> encode_ftn_runtime_response(const FtnRuntimeResponse&);
std::optional<FtnRuntimeResponse> decode_ftn_runtime_response(const std::vector<std::uint8_t>&);
}
