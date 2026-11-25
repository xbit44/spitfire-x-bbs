#pragma once
#include "sfx/application/ftn_wire.hpp"
#include "sfx/application/message.hpp"
namespace sfx::application {
struct MessageCanvas;
bool equal_ftn_secret(std::string_view,std::string_view)noexcept;
bool same_ftn_address(const FtnAddress&,const FtnAddress&,bool permit_domainless=false)noexcept;
struct FtnPeerMatch { std::string network,link; };
// A unique configured numeric/domain identity only. This does not authenticate
// it; transport must separately verify the chosen link's credential.
std::optional<FtnPeerMatch> resolve_ftn_peer(const FtnConfiguration&,const std::vector<FtnAddress>&,
    std::string_view expected_network={},std::string_view expected_link={});
bool ftn_echo_permission(const FtnConfiguration&,std::string_view network,std::string_view link,
    std::string_view tag,bool file_echo,bool receive,bool manage=false);
std::optional<std::string> ftn_route_link(const FtnConfiguration&,std::string_view network,const FtnAddress&);
bool ftn_local_address(const FtnNetwork&,const FtnAddress&);

struct FtnTicPath { FtnAddress address;std::optional<std::uint64_t> timestamp;std::string annotation; };
struct FtnTic {
    std::string area,file,long_file,replaces,password;
    FtnAddress origin,from;std::optional<FtnAddress> to;
    std::optional<std::uint64_t> size;std::optional<std::uint32_t> crc;
    std::vector<std::string> descriptions,long_descriptions,unknown;
    std::vector<FtnTicPath> path;std::vector<FtnAddress> seen_by;
};
struct FtnTicResult {std::optional<FtnTic> tic;std::string reason;};
FtnTicResult decode_ftn_tic(std::string_view,std::string_view network_domain={});
// Writer always emits size and CRC; caller supplies only the destination's
// separate password. Never log this serialization or include it in status IPC.
std::optional<std::string> encode_ftn_tic(const FtnTic&);
std::uint32_t ftn_crc32(std::string_view)noexcept;
class FtnCrc32 final {
public:
    void update(std::string_view)noexcept;
    std::uint32_t value()const noexcept{return state_^0xffffffffU;}
private:
    std::uint32_t state_{0xffffffffU};
};
enum class FtnTicAdmission : std::uint8_t { accepted,unauthorized,wrong_password,transport_proof_required,loop,wrong_destination };
FtnTicAdmission admit_ftn_tic(const FtnConfiguration&,const FtnPeerMatch&,const FtnTic&,
    std::string_view expected_password,bool authenticated_same_link,bool external_transport);

enum class FtnAreaFixKind : std::uint8_t { add,remove,list,linked,unlinked,help,status,rescan,pause,resume,unsupported };
struct FtnAreaFixCommand { FtnAreaFixKind kind{FtnAreaFixKind::unsupported};std::string tag;std::uint16_t rescan_count{100}; };
std::optional<std::vector<FtnAreaFixCommand>> parse_ftn_areafix(std::string_view);
struct FtnAreaFixForward {std::string link,tag;};
struct FtnAreaFixPlan {
    FtnConfiguration configuration;bool authorized{},changed{},paused{};
    std::string response;std::vector<FtnAreaFixCommand> rescans;
    std::vector<FtnAreaFixForward> upstream;
};
// The native host verifies the separate AreaFix credential against this exact
// configuration revision. Session authentication alone is never this proof.
FtnAreaFixPlan plan_ftn_areafix(const FtnConfiguration&,const FtnPeerMatch&,
    const std::vector<FtnAreaFixCommand>&,bool separate_password_verified,bool paused);
// Rendering view only. Original controls/charset/art stay in immutable wire
// metadata. Unsupported/unrepresentable codecs hold rather than guessed recode.
std::optional<MessageSource> ftn_message_display_source(const FtnPackedMessage&,const FtnMessageMetadata&);
std::optional<MessageCanvas> ftn_received_ansi_canvas(const FtnPackedMessage&,bool pipes,std::string_view domain={});
std::optional<std::string> ftn_message_field(std::string_view,const FtnMessageMetadata&,std::size_t maximum);
struct FtnMessageFingerprintMaterial {std::string identity,content;};
FtnMessageFingerprintMaterial ftn_message_fingerprint_material(const FtnPackedMessage&,const FtnMessageMetadata&);
}
