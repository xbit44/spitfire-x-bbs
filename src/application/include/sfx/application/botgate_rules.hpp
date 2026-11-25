#pragma once
#include "sfx/application/botgate.hpp"
#include <array>
#include <map>

namespace sfx::application {
enum class BotGateRuleFile : unsigned { exempt, ip, silent, host, temporary, feed_exempt, count };
inline constexpr std::array<const char*,6> kBotGateRuleNames{
    "ipfilter_exempt.cfg","ip.can","ip-silent.can","host.can","temp_ip.can","feed-exempt.can"};
struct BotGatePattern {
    bool negated{};
    std::optional<Ipv4Network> network;
    std::string text;
    std::int64_t expires{};
};
using BotGatePatternList=std::vector<BotGatePattern>;
// One rule per line. Strict candidate validation; no partial list activation.
std::optional<BotGatePatternList> parse_botgate_patterns(std::string_view, bool host=false, bool temporary=false);
bool botgate_patterns_match(const BotGatePatternList&,std::string_view,std::int64_t now) noexcept;
struct BotGateGeoList {
    std::string name;
    std::vector<std::pair<std::uint32_t,std::uint32_t>> ranges;
    bool contains(std::uint32_t) const noexcept;
};
std::optional<BotGateGeoList> parse_botgate_geo(std::string_view name,std::string_view bytes);
struct BotGateRuleSnapshot {
    std::array<BotGatePatternList,6> lists;
    std::vector<BotGateGeoList> geo;
    std::string status{"rules_not_loaded"};
    bool ready{};
    bool exempt(std::string_view ip,std::int64_t now) const noexcept;
    // Empty means no file-based refusal. Exemptions are checked first.
    std::string refusal(std::string_view ip,std::string_view verified_host,std::int64_t now) const;
};
// Twenty arrivals in a rolling ten-second interval; the next creates a
// 90-minute temporary refusal. Caller supplies monotonic seconds. Bounded map;
// saturation refuses new sources rather than evicting active bans.
class BotGateArrivalLimits {
public:
    bool admit(std::uint32_t ip,std::uint64_t now);
    std::uint64_t banned_until(std::uint32_t ip) const;
private:
    struct Entry {std::vector<std::uint64_t> arrivals;std::uint64_t until{};};
    std::map<std::uint32_t,Entry> entries_;
};
}
