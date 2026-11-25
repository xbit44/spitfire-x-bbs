#pragma once
#include "sfx/application/ftn_work.hpp"

namespace sfx::application {
enum class FtnExternalPathStyle { canonical, linux_lowercase_v1 };
// Translate only the closed SFX external-mailer path namespace. Wire payload
// basenames and serialized receive/publication records keep their spelling.
std::string ftn_external_physical_path(std::string_view,FtnExternalPathStyle = FtnExternalPathStyle::canonical);
std::string ftn_external_canonical_path(std::string_view,FtnExternalPathStyle = FtnExternalPathStyle::canonical);
// Local receive-hook record, never accepted from caller/browser IPC. The host
// verifies the owned file and hash before this context may become custody.
struct FtnExternalReceiveRecord {
    Identifier128 id;
    std::string network, relative_file;
    std::vector<FtnAddress> remote;
    bool protected_session{};
    std::uint64_t size{}, timestamp{};
    FtnDigest digest{};
    std::string run;
    FtnDigest run_digest{};
    std::vector<FtnAddress> authenticated_remote;
};
bool valid_ftn_external_path(std::string_view) noexcept;
std::string ftn_external_root(std::string_view network,FtnExternalPathStyle = FtnExternalPathStyle::canonical);
std::optional<std::string> encode_ftn_external_receive(const FtnExternalReceiveRecord&);
std::optional<FtnExternalReceiveRecord> decode_ftn_external_receive(std::string_view);
// A profile scopes address resolution to one network; it does not resolve
// ambiguous links within that network or authenticate a claimed address.
std::optional<FtnPeerMatch> match_ftn_external_receive(
    const FtnConfiguration&,const FtnExternalReceiveRecord&,std::string_view network={});
// Only genuinely unknown, unprotected callers may enter the existing direct
// local NetMail path. Ambiguous or disabled configured peers stay held.
bool ftn_external_nonsecure_receive(const FtnConfiguration&,const FtnExternalReceiveRecord&);

struct FtnExternalProfileNetwork {std::string code,domain;std::uint16_t zone{};};
struct FtnExternalProfile {
    // Contains stock configuration secrets: keep only in private profile files.
    std::string configuration;
    std::vector<FtnExternalProfileNetwork> networks;
    std::vector<FtnAddress> local,remote,protected_remote;
};
std::optional<FtnExternalProfile> parse_ftn_external_profile(std::string_view text,FtnExternalPathStyle = FtnExternalPathStyle::canonical);

struct FtnExternalOutboundPaths {
    std::string busy, flow, copy;
};
// All results are relative to the board-root working directory. The disposable
// copy preserves its wire basename in a unique job directory. No canonical
// SFX original is named in the delete-after-send FLO line.
std::optional<FtnExternalOutboundPaths> ftn_external_outbound_paths(
    const FtnNetwork&,const FtnLink&,const FtnOutboundJob&,FtnExternalPathStyle = FtnExternalPathStyle::canonical);
std::string ftn_external_flow_line(const FtnExternalOutboundPaths&,FtnExternalPathStyle = FtnExternalPathStyle::canonical);
}
