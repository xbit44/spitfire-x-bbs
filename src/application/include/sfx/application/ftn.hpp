#pragma once

#include <cstdint>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

// FTN identity always includes the configured network. A claimed zone/AKA is
// never sufficient to select another network or grant authenticated rights.
struct FtnAddress {
    std::uint16_t zone{}, net{}, node{}, point{};
    std::string domain;
};
std::optional<FtnAddress> parse_ftn_address(std::string_view);
std::string format_ftn_address(const FtnAddress&);
bool valid_ftn_code(std::string_view) noexcept;
bool valid_ftn_echo_tag(std::string_view) noexcept;
bool valid_ftn_display_text(std::string_view,std::size_t maximum,bool blank=false) noexcept;
bool valid_ftn_echo_list_url(std::string_view) noexcept;

enum class FtnMailerMode : std::uint8_t { native, external_binkd, disabled };
enum class FtnConnectionUse : std::uint8_t { inbound_only, outbound_only, both };
enum class FtnMailFlavor : std::uint8_t { normal, hold, crash };
enum class FtnEchoListSource : std::uint8_t { url, local_file };

struct FtnSettings {
    FtnMailerMode mode{FtnMailerMode::native};
    bool listener_enabled{true};
    std::string bind_address{"0.0.0.0"};
    std::uint16_t port{24554};
    bool allow_direct_local_netmail{true};
    std::string external_executable;
    std::string tls_certificate, tls_private_key;
    bool msg_netmail_enabled{};
    std::string msg_netmail_directory;
    bool tls_listener_enabled{};
    std::uint16_t tls_port{24555}; // Editable SFX default, not a standards claim.
};
struct FtnNetwork {
    std::string code, name, address, origin_line, echo_list_url;
    std::vector<std::string> additional_addresses;
    bool enabled{};
    bool keep_processed{true};
    std::uint16_t retention_days{30}; // 0 = forever; ignored when keep is false.
    std::int64_t message_group{};
    std::string new_group_name;
    std::string description;
    FtnEchoListSource echo_list_source{FtnEchoListSource::url};
    std::string echo_list_file, nodelist_file, default_link;
    // Null retains the pre-existing shared-directory policy. New setup always
    // chooses explicitly Off or On; it never silently inherits another net.
    std::optional<bool> msg_netmail_enabled;
    std::string msg_netmail_directory;
};
// Only installation-owner protected envelopes cross the durable boundary.
// Portable validation does not decrypt, display or authenticate these bytes.
struct FtnProtectedCredentials {
    std::vector<std::uint8_t> session, packet, areafix, tic;
};
struct FtnLink {
    std::string code, network, name, address, host;
    std::vector<std::string> additional_addresses;
    FtnConnectionUse use{FtnConnectionUse::both};
    std::uint16_t port{24554}, poll_minutes{30};
    bool enabled{}, tls{}, allow_legacy_plain_auth{}, forward_missing_echoes{};
    std::optional<bool> areafix_enabled;
    FtnProtectedCredentials credentials;
    std::string tls_server_name, tls_ca_file, forward_via_link;
    bool contact_on_mail{}; // Legacy periodic policies are left unchanged.
};
bool ftn_areafix_enabled(const FtnLink&) noexcept;
bool ftn_outbound(const FtnLink&) noexcept;
bool ftn_inbound(const FtnLink&) noexcept;

struct FtnMessageEcho {
    std::string network, tag, description;
    std::int64_t local_echo{}; // 0 means no local conference / pass through.
    bool store_locally{true}, require_real_name{};
    std::uint16_t access_level{10}, read_level{10}, post_level{10};
    std::string local_name{};
    bool update_local_name{};
    bool pipe_colors{}, update_local_permissions{};
    std::optional<std::uint32_t> maximum_messages{500}, maximum_age_days{};
    std::uint8_t update_local_retention{}; // Count=1, Age=2; never implicit.
    std::optional<bool> ansi_colors{}; // SFX-authored outgoing posts only; null preserves legacy wire policy.
};
struct FtnFileEcho {
    std::string network, tag, description;
    std::int64_t file_area{};
};
struct FtnEchoPermission {
    std::string network, link, tag;
    bool file_echo{}, receive{}, send{}, manage{};
};
struct FtnRoute {
    std::string network, destination, via_link;
    FtnMailFlavor flavor{FtnMailFlavor::normal};
};
struct FtnSetupSubscriptions {std::string network,link;std::vector<std::string> tags;};
struct FtnConfiguration {
    std::int64_t revision{1};
    std::int64_t message_catalog_revision{}, file_catalog_revision{};
    FtnSettings settings;
    std::vector<FtnNetwork> networks;
    std::vector<FtnLink> links;
    std::vector<FtnMessageEcho> message_echoes;
    std::vector<FtnFileEcho> file_echoes;
    std::vector<FtnEchoPermission> permissions;
    std::vector<FtnRoute> routes;
    // Command-only: consumed atomically by the final configuration save and
    // omitted from the saved configuration returned to future editors.
    std::optional<FtnSetupSubscriptions> setup_subscriptions;
};
struct FtnValidationIssue {
    // Stable field/reason codes only: never interpolate a password or envelope.
    std::string field, reason;
};
std::vector<FtnValidationIssue> validate_ftn_configuration(const FtnConfiguration&);
// Leave space for writer accounting/mappings below the existing 8MiB queue.
inline constexpr std::size_t kFtnConfigurationMaximumBytes=4*1024*1024;
std::vector<std::uint8_t> encode_ftn_configuration(const FtnConfiguration&);
// Exact older encodings are used only to reconcile already-issued operation
// fingerprints. A request not representable in that version is refused.
std::vector<std::uint8_t> encode_ftn_configuration_legacy(const FtnConfiguration&,std::uint8_t version);
std::optional<FtnConfiguration> decode_ftn_configuration(const std::vector<std::uint8_t>&);

struct FtnEchoListEntry {
    std::string tag, description;
    std::size_t line{};
    bool selected{true};
};
struct FtnEchoListIssue { std::size_t line{}; std::string reason; };
struct FtnEchoList {
    std::vector<FtnEchoListEntry> entries;
    std::vector<FtnEchoListIssue> issues;
    bool complete{true};
};
FtnEchoList parse_ftn_echo_list(std::string_view);

} // namespace sfx::application
