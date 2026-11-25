#pragma once
#include "sfx/application/ftn.hpp"
#include <array>
#include <functional>

namespace sfx::application {
inline constexpr std::uint64_t kFtnTransferMaximumBytes=1024ULL*1024*1024;
inline constexpr std::size_t kBinkpFrameMaximumBytes=32767;
inline constexpr std::size_t kBinkpCommandMaximumBytes=4096;
enum class BinkpCommand : std::uint8_t { nul,adr,pwd,file,ok,eob,got,err,bsy,get,skip };
struct BinkpFrame { bool command{};std::uint8_t id{};std::string bytes; };
// Consumes arbitrarily split TCP reads without allocating a peer-sized buffer.
// A callback refuses semantic state transitions; a failure poisons the decoder.
class BinkpDecoder final {
public:
    BinkpDecoder()=default;
    ~BinkpDecoder();
    BinkpDecoder(const BinkpDecoder&)=delete;
    BinkpDecoder& operator=(const BinkpDecoder&)=delete;
    bool feed(std::string_view,const std::function<bool(BinkpFrame)>&);
    bool pending()const noexcept{return used_!=0||header_used_!=0;}
    bool failed()const noexcept{return failed_;}
private:
    std::array<std::uint8_t,2> header_{};std::size_t header_used_{},expected_{},used_{};
    bool command_{},failed_{};std::string payload_;
};
std::string binkp_command(BinkpCommand,std::string_view argument={});
std::string binkp_data(std::string_view);
// Safe ordinary leaf name only; never a path, device or canonicalized alias.
bool valid_ftn_leaf_name(std::string_view)noexcept;
bool ftn_message_wire_name(std::string_view)noexcept;
std::optional<std::string> decode_binkp_name(std::string_view);
std::string encode_binkp_name(std::string_view);
struct BinkpFile {
    std::string name;
    std::uint64_t size{},timestamp{},offset{};
    bool request_offset{};
};
std::optional<BinkpFile> parse_binkp_file(std::string_view,bool with_offset,bool version11);
std::string format_binkp_file(const BinkpFile&,bool with_offset);
bool same_binkp_file(const BinkpFile&,const BinkpFile&)noexcept;
std::optional<std::vector<FtnAddress>> parse_binkp_addresses(std::string_view);
struct BinkpOptions {
    bool version11{},request_nr{};
    std::string challenge_hex;
};
bool observe_binkp_information(BinkpOptions&,std::string_view,bool handshake);

enum class FtnPacketProfile : std::uint8_t { type2,type2plus };
struct FtnPacketHeader {
    FtnPacketProfile profile{FtnPacketProfile::type2plus};
    FtnAddress origin,destination;
    std::uint16_t year{},month{},day{},hour{},minute{},second{};
    std::array<std::uint8_t,8> password{};
    std::uint16_t product{0x00fe}; // FTSC registry: no product ID allocated.
    std::uint8_t major{1},minor{};
};
struct FtnPackedMessage {
    std::uint16_t origin_net{},origin_node{},destination_net{},destination_node{},attributes{},cost{};
    std::string date,to,from,subject,text;
};
struct FtnPacket { FtnPacketHeader header;std::vector<FtnPackedMessage> messages; };
struct FtnPacketResult { std::optional<FtnPacket> packet;std::string reason; };
inline constexpr std::size_t kFtnPacketMaximumBytes=64*1024*1024;
inline constexpr std::size_t kFtnMessageMaximumBytes=262144;
// Native receiving may retain safely framed, semantically rejected messages so
// an unrelated message is not lost. Structural framing remains strict.
FtnPacketResult decode_ftn_packet(std::string_view,std::string_view network_domain={},bool retain_rejected=false);
std::optional<std::string> encode_ftn_packet(const FtnPacket&);
// Preserve source text and opaque controls exactly. Address controls and AREA
// are parsed separately; neither grants ingress or recipient authority.
struct FtnMessageMetadata {
    std::string area,msgid,reply,charset;
    FtnAddress origin,destination;
    std::vector<std::string> controls;
};
std::optional<FtnMessageMetadata> inspect_ftn_message(const FtnPacketHeader&,const FtnPackedMessage&);
// For a held-item observation only. Never grants import/forwarding authority.
std::optional<FtnMessageMetadata> inspect_ftn_held_message(const FtnPacketHeader&,const FtnPackedMessage&);
}
