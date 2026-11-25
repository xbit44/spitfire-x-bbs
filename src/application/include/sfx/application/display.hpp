#pragma once

#include "sfx/application/presentation.hpp"
#include "sfx/application/terminal.hpp"
#include "sfx/core/security.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

inline constexpr std::uint16_t kMaximumDisplayFieldWidth = 200;
inline constexpr std::size_t kMaximumDisplayIncludeDepth = 8;
inline constexpr std::size_t kMaximumRenderedDisplayBytes = 1024 * 1024;
inline constexpr std::size_t kMaximumTrustedDisplaySourceBytes = 256 * 1024;
inline constexpr std::size_t kMaximumTrustedDisplayAggregateSourceBytes =
    1024 * 1024;
inline constexpr std::size_t kMaximumTrustedDisplayIncludeInstances = 64;

enum class DisplayTrust : std::uint8_t { untrusted, trusted_sysop_asset };
enum class DisplayTokenKind : std::uint8_t { value, control, include };
enum class FieldAlignment : std::uint8_t { none, left, right, center, zero_pad };

struct TokenFormat {
    FieldAlignment alignment{FieldAlignment::none};
    std::uint16_t width{0};
    std::string named_format;
};

struct ParsedDisplayToken {
    DisplayTokenKind kind{DisplayTokenKind::value};
    std::string name;
    std::string include_filename;
    TokenFormat format;
};

enum class DisplaySegmentKind : std::uint8_t { literal, token };

struct DisplaySegment {
    DisplaySegmentKind kind{DisplaySegmentKind::literal};
    std::string literal;
    std::optional<ParsedDisplayToken> token;
};

struct DisplayParseResult {
    std::vector<DisplaySegment> segments;
    std::vector<std::string> errors;
    bool valid() const noexcept { return errors.empty(); }
};

struct DisplayLookupPlan {
    std::string exact_security_candidate;
    std::string fallback_candidate;
};

class DisplayTokenRegistry final {
public:
    bool is_known_value(std::string_view name) const noexcept;
    bool is_known_control(std::string_view name) const noexcept;
    std::optional<ParsedDisplayToken> parse_expression(std::string_view expression) const;
};

enum class TrustedDisplayIdentity : std::uint8_t {
    sfprelog,
    welcome1,
    newreg,
    newuser,
    nonodes,
    menu_main, menu_messages, menu_files,
    // Retired artwork identities 8/9 stay unused.
    menu_doors=10, menu_bulletins, menu_door_bulletins, menu_chat, menu_settings,
    menu_file_groups, menu_file_areas,
    all, chatdone, eventime, goodbye, lockout, sfasleep, sfnewsletter, bulletin_page,
    menu_batch_transfer, menu_qwk, door_submenu, door_prelaunch, menu_sysop,
    door_bulletin_page, door_bulletin_generated
};

enum class TrustedDisplaySelectionCode : std::uint8_t {
    available,
    not_found,
    invalid,
    unavailable
};

struct TrustedDisplayRenderContext {
    std::optional<std::string> bbs_name;
    std::optional<std::string> sysop;
    std::optional<std::string> version;
    std::optional<std::string> date;
    std::optional<std::string> time;
    std::optional<std::string> datetime;
    std::optional<std::string> node;
    std::optional<std::string> nodes;
    std::optional<std::string> transport;
    std::optional<std::string> columns;
    std::optional<std::string> rows;

    std::optional<std::string> first_name;
    std::optional<std::string> city_state;
    std::optional<std::string> phone_number;
    std::optional<std::string> name;
    std::optional<std::string> alias;
    std::optional<std::string> email;
    std::optional<std::string> security_level;
    std::optional<std::string> logons;
    std::optional<std::string> last_call;
    std::optional<std::string> time_on;
    std::optional<std::string> protocol;
    std::optional<std::string> ip_address;
    std::optional<std::string> password_age;

    // Authenticated menu values only. Absence means unavailable, not a guessed
    // zero. Values are inert CP437 text, never reparsed as tokens or ANSI.
    std::optional<std::string> original_logon, logon_minutes, time_left;
    std::optional<std::string> subscription_days, quiet, hotkey;
    std::optional<std::string> uploads, downloads, uploaded_kb, downloaded_kb;
    std::optional<std::string> posts, hostname, mail_total, mail_unread;
    std::optional<std::string> online, uptime;
    std::optional<std::string> message_conference, message_conference_number;
    std::optional<std::string> message_count, new_messages, message_number;
    std::optional<std::string> message_to, message_from, message_subject, message_date;
    std::optional<std::string> file_area, file_area_number, file_count, new_files;
    std::optional<std::string> filename, file_description, file_size, file_uploader;
    std::optional<std::string> file_date, file_downloads;
    std::optional<std::string> door_name, door_number, door_menu;
    std::optional<std::string> chat_room, chat_count, ftn_network, ftn_address;
};

struct TrustedDisplaySelectionRequest {
    TrustedDisplayIdentity identity{TrustedDisplayIdentity::sfprelog};
    GraphicsMode graphics{GraphicsMode::ascii};
    std::optional<std::uint16_t> authenticated_security_level;
    TrustedDisplayRenderContext context;
    // Pre-authentication welcome sequence number, never a security suffix.
    std::uint8_t welcome_number{1};
    std::uint8_t bulletin_number{1};
    std::uint16_t door_asset_number{}; // Typed DRnnn/DPnnn identity, never a path.
};

struct RenderedTrustedDisplayProgram {
    TrustedDisplayIdentity identity{TrustedDisplayIdentity::sfprelog};
    GraphicsMode graphics{GraphicsMode::ascii};
    std::string selected_logical_asset;
    bool exact_security_match{false};
    TerminalDisplayProgram terminal_program;
    std::size_t aggregate_source_bytes{0};
    std::size_t resolved_include_count{0};
    // UTC Unix epoch, 100 ns precision; same verified root-file read as content.
    // No timestamp means unknown, never a guessed new-file indication.
    std::optional<std::int64_t> modified_utc_100ns{};
    std::uint16_t door_asset_number{};
};

struct TrustedDisplaySelectionResult {
    TrustedDisplaySelectionCode code{TrustedDisplaySelectionCode::unavailable};
    std::optional<RenderedTrustedDisplayProgram> program;

    bool available() const noexcept;
};

class ITrustedDisplaySelectionPort {
public:
    virtual ~ITrustedDisplaySelectionPort() = default;
    virtual TrustedDisplaySelectionResult select(
        const TrustedDisplaySelectionRequest& request) = 0;
};

enum class TrustedDisplayAssetReadCode : std::uint8_t {
    available,
    not_found,
    invalid,
    unavailable
};

struct TrustedDisplayAssetReadResult {
    TrustedDisplayAssetReadCode code{TrustedDisplayAssetReadCode::unavailable};
    std::string logical_identity;
    std::string stable_file_identity;
    std::string bytes;
    std::optional<std::int64_t> modified_utc_100ns{};
};

class ITrustedDisplayAssetReader {
public:
    virtual ~ITrustedDisplayAssetReader() = default;
    virtual TrustedDisplayAssetReadResult read_asset(
        std::string_view logical_identity,
        std::size_t maximum_bytes) = 0;
};

class TrustedDisplaySelector final : public ITrustedDisplaySelectionPort {
public:
    explicit TrustedDisplaySelector(
        ITrustedDisplayAssetReader& reader) noexcept;

    TrustedDisplaySelectionResult select(
        const TrustedDisplaySelectionRequest& request) override;

private:
    ITrustedDisplayAssetReader& reader_;
    DisplayTokenRegistry registry_;
};

DisplayParseResult parse_display(
    std::string_view source,
    DisplayTrust trust,
    const DisplayTokenRegistry& registry);
bool valid_include_filename(std::string_view filename) noexcept;
// Shared legacy EOF/SAUCE framing; leaves the original captured bytes intact.
std::optional<std::string_view> display_asset_content(std::string_view bytes);
std::string_view trusted_menu_basename(TrustedDisplayIdentity identity) noexcept;
bool valid_trusted_display_selection_request(
    const TrustedDisplaySelectionRequest& request) noexcept;
bool valid_rendered_trusted_display_program(
    const RenderedTrustedDisplayProgram& program) noexcept;
// Bounded, acknowledged Sysop screen; authored colors remain untouched.
bool prepare_acknowledged_display(TerminalDisplayProgram& program, GraphicsMode graphics,
    std::optional<std::uint8_t> caller_color = std::nullopt,
    bool blank_line_before_prompt = false);
std::optional<DisplayLookupPlan> make_display_lookup_plan(
    std::string_view basename,
    GraphicsMode graphics,
    core::SecurityLevel security_level);

}  // namespace sfx::application
