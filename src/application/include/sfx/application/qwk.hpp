#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {

inline constexpr std::uint64_t kQwkCompressedBytes = 16U * 1024U * 1024U;
inline constexpr std::uint64_t kQwkExpandedBytes = 64U * 1024U * 1024U;
inline constexpr std::uint32_t kQwkDefaultMessageCount = 10000;
inline constexpr std::uint32_t kQwkMaximumMessageCount = 65535;
inline constexpr std::uint32_t kQwkMaximumReference = 9999999;
inline constexpr std::string_view kQwkControlName = "SFXQWK";
inline constexpr std::string_view kQwkEmptyNotice = "No messages available for your current packet settings.";
inline constexpr std::string_view kQwkMoreNotice = u8"More messages remain—download again to continue.";
inline constexpr std::string_view kQwkReplayNotice = "This reply packet has already been imported.";

enum class QwkPrivateMode : std::uint8_t { unread, all_received, none };
enum class QwkEncoding : std::uint8_t { cp437, utf8 };
struct QwkPreferences {
    QwkPrivateMode private_mail{QwkPrivateMode::unread};
    bool own_public_posts{};
    QwkEncoding encoding{QwkEncoding::cp437};
    bool preserve_colors{true};
    // No provisional product default: the owner must choose before seeding.
    std::optional<bool> word_wrap;
    bool indexes{true};
    bool full_headers{true};
};
bool valid_qwk_preferences(const QwkPreferences&, bool require_wrap_choice = true) noexcept;
bool valid_qwk_message_count(std::uint32_t) noexcept;
bool valid_qwk_board_id(std::string_view) noexcept;
std::string canonical_qwk_board_id(std::string_view);
// Permanent reference namespace, independent of online friendly numbering.
// Invalid board/reference produces no identifier, never a guessed parent.
std::string qwk_message_identifier(std::string_view board_id,std::uint32_t reference);

// Strict, lossless conversion. No best-fit replacement of unsupported input.
std::string qwk_cp437_to_utf8(std::string_view);
std::optional<std::string> qwk_utf8_to_cp437(std::string_view);

struct QwkConference { std::uint16_t number{}; std::string name; };
struct QwkPacketMetadata {
    std::string board_id, board_name, city, phone, sysop, caller_name;
    std::string created; // MM-DD-YYYY,HH:MM:SS in BBS local time
    std::vector<QwkConference> conferences; // mail=0, stable public mappings
    std::optional<std::uint16_t> netmail_conference{}; // QWKE AREA PN, separate from local mail
};
struct QwkRecord {
    std::uint16_t conference{};
    std::uint32_t number{}, reference{};
    bool private_mail{}, read{};
    // In-memory text is CP437, body uses LF. Storage authorizes/validates it.
    std::string from, to, subject, body;
    std::string date, time; // MM-DD-YY and HH:MM; not trusted import timestamps
    std::string message_id, in_reply_to, when_written;
    bool artwork{}; // preserve deliberate layout regardless of wrap selection
};
struct QwkFile { std::string name, bytes; };
enum class QwkCodecError : std::uint8_t {
    none, invalid_preferences, invalid_metadata, invalid_record, invalid_headers,
    wrong_board, unsupported_content, unsupported_command, limit_exceeded
};
struct QwkCodecResult {
    QwkCodecError error{QwkCodecError::none};
    std::string detail;
    std::vector<QwkFile> files;
    explicit operator bool() const noexcept { return error == QwkCodecError::none; }
};
// Complete-record encoding only; this function never truncates a message to fit.
QwkCodecResult encode_qwk(const QwkPacketMetadata&, const QwkPreferences&,
    const std::vector<QwkRecord>&, std::uint32_t maximum_messages = kQwkDefaultMessageCount);

enum class QwkScanCommand : std::uint8_t { add, drop };
struct QwkReplyRecord {
    QwkRecord message;
    std::optional<QwkScanCommand> command;
};
struct QwkReplyPacket {
    QwkCodecError error{QwkCodecError::none};
    std::string detail, board_id;
    std::vector<QwkReplyRecord> records;
    // Canonical logical records, independent of archive order/compression/date
    // and HEADERS field order/spelling. Caller-scoped receipt hashing is storage.
    std::string logical_identity;
    explicit operator bool() const noexcept { return error == QwkCodecError::none; }
};
// Files must be verified ZIP entries with actual aggregate limits enforced by
// the archive adapter. The codec also bounds bytes and permits no extra files.
QwkReplyPacket decode_qwk_reply(std::string_view expected_board_id,
    const std::vector<QwkFile>&);

} // namespace sfx::application
