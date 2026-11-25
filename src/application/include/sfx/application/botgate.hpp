#pragma once

#include <cstdint>
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sfx::application {
struct BotGateFeedStatus {
    bool updating{}, available{};
    std::int64_t last_attempt{}, last_success{};
    std::uint64_t bytes_received{}, entries{};
    std::string result{"feed_cache_unavailable"};
    bool enabled{}, telnet{}, ssh{}, rules_ready{};
    // Runtime epoch: checked, passed, failed, timed out, blocked, exempt, active.
    std::array<std::uint64_t,7> protection_counts{};
    std::uint64_t protection_started{};
    std::array<std::uint64_t,6> rule_counts{};
    std::string rule_status{"rules_not_loaded"}, geo_summary;
};

// Display-only latest-attempt progress. Never sent to the operational journal.
std::string render_botgate_feed_progress(const BotGateFeedStatus& status);

struct BotGateConfiguration {
    bool enabled{true}, ssh_enabled{false}; // Compatible key: challenge NEW SSH registrations only.
    std::uint16_t timeout_seconds{20};
    // Semicolon-separated canonical IPv4/CIDR or domain names; '-' is empty.
    std::string manual_blocks{"-"}, trusted_sources{"-"}, feed_exceptions{"-"}, domains{"-"};
};
// SSH source filtering is global; its registration-challenge choice is independent.
inline bool botgate_filters_enabled(const BotGateConfiguration& config,
    bool telnet_participating, bool ssh) noexcept {
    return config.enabled && (ssh || telnet_participating);
}
struct LogRetention {
    std::uint16_t size_mib{10}, archives{10};
    bool operator==(const LogRetention& b) const noexcept {
        return size_mib == b.size_mib && archives == b.archives;
    }
};
struct LoggingConfiguration {
    LogRetention runtime, callers, attention;
    bool operator==(const LoggingConfiguration& b) const noexcept {
        return runtime == b.runtime && callers == b.callers && attention == b.attention;
    }
};
bool valid_logging_configuration(const LoggingConfiguration&) noexcept;
bool botgate_ansi_terminal(std::string_view terminal) noexcept;
// Presentation only: bounded literal artwork, never macro/include expansion.
// Unknown/non-ANSI terminals receive the plain fallback without a question.
std::string render_botgate_art(std::string_view source, unsigned remaining, bool ansi, unsigned width=2);
bool valid_botgate_configuration(const BotGateConfiguration&) noexcept;
bool equivalent_botgate_configuration(const BotGateConfiguration&, const BotGateConfiguration&) noexcept;

struct Ipv4Network { std::uint32_t address{}, mask{}; };
std::optional<std::uint32_t> parse_botgate_ipv4(std::string_view) noexcept;
std::optional<Ipv4Network> parse_botgate_network(std::string_view) noexcept;
bool botgate_network_matches(std::string_view rules, std::uint32_t address) noexcept;
// Only call after a separate verifier has forward-confirmed the same numeric peer.
bool botgate_domain_matches(std::string_view rules, std::string_view verified_name) noexcept;

inline constexpr std::size_t kBotGateFeedMaximumBytes = 16U * 1024U * 1024U;
inline constexpr std::size_t kBotGateFeedMaximumEntries = 500000;
// Whole candidate or nothing. Comments are metadata only; no raw feed text is logged.
std::optional<std::vector<std::uint32_t>> parse_botgate_feed(std::string_view);
enum class BotGateDisposition { off, manual_block, domain_block, feed_block, trusted, challenge };
BotGateDisposition evaluate_botgate(const BotGateConfiguration&, bool participating,
    std::uint32_t peer, std::string_view forward_confirmed_name,
    const std::vector<std::uint32_t>& sorted_feed) noexcept;

// Receives transport-decoded payload, never IAC negotiation. Small mistypes and
// terminal key sequences do not count. Bounded scanner bursts fail immediately.
// A short quiet boundary after the second hit prevents coalesced tail/typeahead
// from becoming login input. TCP chunk boundaries are not treated as key events.
class BotGateChallenge final {
public:
    enum class Result { waiting, passed, invalid, timed_out };
    BotGateChallenge(std::uint64_t now_ms, std::uint16_t seconds) noexcept;
    Result input(std::string_view payload, std::uint64_t now_ms) noexcept;
    Result poll(std::uint64_t now_ms) noexcept;
    unsigned remaining(std::uint64_t now_ms) const noexcept;
private:
    std::uint64_t deadline_{}, settled_{};
    unsigned hits_{};
    unsigned stray_bytes_{}, escape_bytes_{};
    std::uint64_t last_input_{};
    bool last_escape_{}, escape_sequence_{};
    Result result_{Result::waiting};
};

} // namespace sfx::application
