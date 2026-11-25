#pragma once

#include "sfx/application/account_storage.hpp"

namespace sfx::application {

// These identities are deliberately not interchangeable with caller-facing
// conference/message numbers or network MSGID/REPLY values.
struct MessageGroupId { std::int64_t value{}; };
struct LocalEchoId { std::int64_t value{}; };
struct MessageId { std::int64_t value{}; };
inline bool operator==(MessageGroupId a, MessageGroupId b) noexcept { return a.value == b.value; }
inline bool operator==(LocalEchoId a, LocalEchoId b) noexcept { return a.value == b.value; }
inline bool operator==(MessageId a, MessageId b) noexcept { return a.value == b.value; }

inline constexpr std::size_t kMessageSubjectCells = 70;
inline constexpr std::size_t kMessageCompositionColumns = 80;
inline constexpr std::size_t kMessageBodyBytes = 1024U * 1024U;
inline constexpr std::uint32_t kDefaultMessageLines = 200;
inline constexpr std::uint32_t kDefaultSysopMessageLines = 1000;
inline constexpr std::uint32_t kMaximumMessageLines = 65535;
inline constexpr std::size_t kMessagePageDefault = 50, kMessagePageMaximum = 100;

inline constexpr std::uint32_t kDefaultRetainedMessageCount = 1000;
inline constexpr std::uint32_t kMaximumRetainedMessageCount = 999999999;
inline constexpr std::uint32_t kMaximumMessageAgeDays = 65535;
inline constexpr std::uint32_t kDefaultUnreadMailAgeDays = 365;
struct MessageRetentionPolicy {
    // Null is explicit Unlimited. Zero is never a finite policy.
    std::optional<std::uint32_t> maximum_count{kDefaultRetainedMessageCount};
    std::optional<std::uint32_t> maximum_age_days;
};
struct ParsedMessageRetentionLimit {
    bool valid{};
    std::optional<std::uint32_t> value;
};
bool valid_message_retention_policy(const MessageRetentionPolicy&) noexcept;
ParsedMessageRetentionLimit parse_message_retention_limit(std::string_view text, std::uint32_t maximum) noexcept;
// Local arrival clock, not sender-authored time or local calendar midnight.
bool message_age_expired(std::int64_t arrival_utc, std::int64_t now_utc,
                        std::optional<std::uint32_t> age_days) noexcept;

enum class MessageValidation : std::uint8_t {
    valid, invalid_limits, invalid_control, subject_too_long,
    body_too_large, too_many_lines, blank_subject, blank_body, invalid_selection
};

struct MessageBody {
    MessageValidation validation{MessageValidation::valid};
    std::vector<std::string> rows;
    std::string canonical_lf;
    explicit operator bool() const noexcept { return validation == MessageValidation::valid; }
};

// A wrapped row retains every source byte. Only continuation rows may gain a
// copy of the quote prefix; offsets let editors and colored renderers retain
// the source cursor/attributes without mistaking that copy for authored text.
struct MessageWrappedRow {
    std::string text;
    std::size_t source_begin{}, source_size{}, inserted_prefix{};
};
std::vector<MessageWrappedRow> wrap_message_line(std::string_view source,
    std::size_t columns, bool preserve_quotes = true);

// Live editor commands are decoded by the UI, not by this data-only model.
// Both editor adapters use the same fixed-cell CP437 normalization/validation.
MessageBody normalize_message_body(std::string_view source, std::uint32_t maximum_lines);
// Map a source insertion offset through the full wrap (including its suffix),
// not a separately wrapped prefix that may choose a different word break.
std::optional<std::size_t> message_body_cursor_offset(std::string_view source,std::uint32_t maximum_lines,std::size_t source_offset);
MessageValidation validate_message_subject(std::string_view subject, bool saving,std::size_t maximum=kMessageSubjectCells);
MessageValidation validate_message_save(std::string_view subject, const MessageBody& body);
std::optional<std::vector<std::size_t>> message_quote_selection(
    std::string_view selection, std::size_t source_lines);

class MessageDraft final {
public:
    explicit MessageDraft(std::uint32_t maximum_lines);
    MessageValidation set_subject(std::string_view subject);
    MessageValidation replace_body(std::string_view source);
    // Cursor offset is in canonical LF CP437 bytes. On failure nothing changes.
    MessageValidation insert_quote(std::size_t cursor_offset,
        const std::vector<std::string>& source, const std::vector<std::size_t>& selection);
    MessageValidation insert_text(std::size_t cursor_offset, std::string_view data);
    MessageValidation replace_line(std::size_t one_based, std::string_view data);
    MessageValidation insert_line(std::size_t one_based, std::string_view data);
    MessageValidation delete_line(std::size_t one_based);
    MessageValidation erase_text(std::size_t cursor_offset, std::size_t count);
    MessageValidation change_limit(std::uint32_t maximum_lines);
    MessageValidation save_validation() const;
    const std::string& subject() const noexcept { return subject_; }
    const MessageBody& body() const noexcept { return body_; }
    std::uint32_t maximum_lines() const noexcept { return maximum_lines_; }
private:
    std::uint32_t maximum_lines_;
    std::string subject_;
    MessageBody body_;
};

// Host-created authority only. Never deserialize caller-supplied security level
// or privilege bits into an actor; storage rechecks the live session/account.
struct MessageActor {
    Identifier128 session;
    AccountId account{};
    std::int64_t account_revision{}, policy_revision{};
};
struct MessagePermissions { std::uint16_t access{1}, reading{1}, posting{1}; };
struct MessageGroup {
    MessageGroupId id;
    std::string name;
    std::int64_t order{}, revision{};
    std::uint16_t access{1};
    bool enabled{true};
};
struct LocalEcho {
    LocalEchoId id;
    MessageGroupId group;
    std::uint16_t conference_number{};
    std::string name;
    std::int64_t order{}, revision{};
    MessagePermissions permissions;
    bool enabled{true};
    bool renegade_pipe_colors{};
    MessageRetentionPolicy retention{};
};
enum class MessageKind : std::uint8_t { public_echo, private_local, private_ftn };
struct MessageRemoteTarget {
    std::string network,source_aka,address,name;
    friend bool operator==(const MessageRemoteTarget& a,const MessageRemoteTarget& b)noexcept{
        return a.network==b.network&&a.source_aka==b.source_aka&&a.address==b.address&&a.name==b.name;
    }
    friend bool operator!=(const MessageRemoteTarget& a,const MessageRemoteTarget& b)noexcept{return !(a==b);}
};
struct MessageTarget {
    MessageKind kind{MessageKind::public_echo};
    LocalEchoId echo;
    std::optional<AccountId> recipient; // null means public All Callers only
    std::optional<std::int64_t> sysop_assignment_revision;
    // A remote To name/address never becomes an SFX account or arrival recipient.
    std::optional<MessageRemoteTarget> remote{};
};
inline constexpr char kEchoRealNameRequired[] = "message_echo_real_name_required";
struct ComposeContext {
    MessageTarget target;
    std::int64_t group_revision{}, echo_revision{}, recipient_revision{}, limits_revision{};
    std::uint32_t maximum_lines{};
    std::optional<MessageId> parent;
    std::int64_t presentation_revision{};
    bool pipe_colors{};
    std::int64_t ftn_configuration_revision{};
};
enum class MessageSourceFormat : std::uint8_t { plain=1, ansi_canvas=2 };
struct MessageSource {
    MessageSourceFormat format{MessageSourceFormat::plain};
    std::uint8_t profile{1};
    std::string bytes;
};
struct CommitMessageRequest {
    MessageActor actor;
    Identifier128 operation;
    ComposeContext context;
    std::string subject, body;
    // Internal source variant. ESC never passes through the plain body model.
    // Absent preserves the accepted v1 commit fingerprint byte-for-byte.
    std::optional<MessageSource> original_source{};
};
struct CommitSourceMessageRequest {
    MessageActor actor;
    Identifier128 operation;
    ComposeContext context;
    std::string subject;
    MessageSource source;
};
struct CommittedMessage {
    MessageId id;
    std::int64_t area_number{}, arrival_ordinal{}, utc_seconds{};
};
struct MessageScanPreference {
    LocalEchoId echo;
    std::int64_t highest_opened{}, revision{};
    bool unread{true}, to_you{true};
};
enum class MessageScanColumn : std::uint8_t { unread, to_you };

} // namespace sfx::application
