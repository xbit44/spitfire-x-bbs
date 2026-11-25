#include "sfx/application/botgate.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace sfx::application {
std::string render_botgate_feed_progress(const BotGateFeedStatus& status) {
    if (!status.last_attempt) return {};
    std::string text="[INFO] BotGate Aggregated Blocklist from AbuseIPDB update started\r\n";
    // One mark per received 32 KiB (including a partial final block), 72 per
    // line. Clamp even malformed snapshots; no guessed percentage or animation.
    const auto bytes=std::min<std::uint64_t>(status.bytes_received,kBotGateFeedMaximumBytes);
    const auto marks=static_cast<std::size_t>((bytes+32767)/32768);
    for (std::size_t offset=0;offset<marks;offset+=72)
        text+=std::string(std::min<std::size_t>(72,marks-offset),'#')+"\r\n";
    text+="Received "+std::to_string(bytes)+" bytes\r\n";
    if (status.updating)
        text+=status.result=="feed_validating" ? "Validating and activating...\r\n" :
            bytes ? "Downloading...\r\n" : "Connecting / waiting for download...\r\n";
    else if (status.available && status.result=="feed_update_success")
        text+="[INFO] BotGate Aggregated Blocklist from AbuseIPDB update complete\r\n";
    else {
        text+="[ERROR] BotGate Aggregated Blocklist from AbuseIPDB update failed: ";
        // Diagnostics are identifiers, not arbitrary display/control input.
        for (const auto c:status.result.substr(0,128))
            text+=(c>='a' && c<='z') || (c>='0' && c<='9') || c=='_' ? c : '?';
        text+=status.available ? "; last good feed retained.\r\n" : "; no valid cached feed.\r\n";
    }
    return text;
}

bool botgate_ansi_terminal(std::string_view terminal) noexcept {
    if (terminal.empty() || terminal.size()>64) return false;
    char lowered[65]{};
    for (std::size_t i=0;i<terminal.size();++i) {
        const auto c=terminal[i];
        if (c<32 || c>126) return false;
        lowered[i]=c>='A' && c<='Z' ? static_cast<char>(c-'A'+'a') : c;
    }
    const std::string_view name(lowered,terminal.size());
    return name=="ansi" || name=="pcansi" || name=="syncterm" || name=="vt100" ||
        name=="vt220" || name=="linux" || name=="xterm" || name=="xterm-256color";
}
std::string render_botgate_art(std::string_view source,unsigned remaining,bool ansi,unsigned width) {
    bool valid=ansi && !source.empty() && source.size()<=16384;
    // Permit printable CP437 and bounded ordinary CSI display commands only.
    // Reject OSC/DCS, terminal queries, private modes and non-display controls.
    for (std::size_t i=0;valid && i<source.size();++i) {
        const auto c=static_cast<unsigned char>(source[i]);
        if (c==27) {
            if (++i>=source.size() || source[i]!='[') { valid=false; break; }
            unsigned count=0;
            while (++i<source.size() && ((source[i]>='0' && source[i]<='9') || source[i]==';'))
                if (++count>32) { valid=false; break; }
            if (!valid || i==source.size() || std::string_view("mHfABCDGJKsu").find(source[i])==std::string_view::npos)
                valid=false;
        } else if ((c<32 && c!='\r' && c!='\n' && c!='\t') || c==127 || c==255) valid=false;
    }
    if (!valid) source="Press ESC or * twice if human...\r\nYou have ## seconds.";
    while (!source.empty() && (source.back()=='\r' || source.back()=='\n')) source.remove_suffix(1);
    std::string result=ansi ? "\x1b[0m\x1b[2J\x1b[H" : "\r";
    for (std::size_t i=0;i<source.size();++i) {
        if (source[i]=='#' && i+1<source.size() && source[i+1]=='#') {
            const auto digits=std::to_string(remaining);
            const auto columns=std::max<std::size_t>(std::min(width,3U),digits.size());
            result.append(columns-digits.size(),' '); result+=digits; ++i;
        } else result+=source[i];
    }
    if (ansi) result+="\x1b[0m";
    return result;
}
namespace {
template<class F> bool each_rule(std::string_view text, F f) noexcept {
    if (text == "-") return true;
    if (text.empty() || text.size() > 512) return false;
    unsigned count = 0;
    while (!text.empty()) {
        const auto end = text.find(';');
        if (++count > 32 || !f(text.substr(0,end))) return false;
        if (end == text.npos) return true;
        text.remove_prefix(end+1);
        if (text.empty()) return false;
    }
    return false;
}
bool valid_domain(std::string_view name) noexcept {
    if (name.empty() || name.size() > 253 || name.front() == '.' || name.back() == '.') return false;
    unsigned label = 0;
    char previous = 0;
    for (char c : name) {
        if (c == '.') { if (!label || previous == '-') return false; label = 0; }
        else {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-') ||
                (!label && c == '-') || ++label > 63) return false;
        }
        previous = c;
    }
    return label && previous != '-';
}
std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == s.npos) return {};
    return s.substr(first,s.find_last_not_of(" \t\r")-first+1);
}
}
std::optional<std::uint32_t> parse_botgate_ipv4(std::string_view s) noexcept {
    std::uint32_t address = 0;
    for (unsigned i=0;i<4;++i) {
        const auto end = s.find('.');
        if ((i == 3) != (end == s.npos)) return {};
        const auto part = s.substr(0,end);
        unsigned n{};
        if (part.empty() || part.size() > 3 || (part.size()>1 && part.front()=='0')) return {};
        const auto parsed = std::from_chars(part.data(),part.data()+part.size(),n);
        if (parsed.ec != std::errc{} || parsed.ptr != part.data()+part.size() || n>255) return {};
        address = (address << 8) | n;
        if (i != 3) s.remove_prefix(end+1);
    }
    return address;
}
std::optional<Ipv4Network> parse_botgate_network(std::string_view s) noexcept {
    const auto slash=s.find('/');
    const auto ip=parse_botgate_ipv4(s.substr(0,slash));
    if (!ip) return {};
    unsigned bits=32;
    if (slash!=s.npos) {
        const auto part=s.substr(slash+1);
        if (part.empty() || (part.size()>1 && part.front()=='0')) return {};
        auto parsed=std::from_chars(part.data(),part.data()+part.size(),bits);
        if (parsed.ec!=std::errc{} || parsed.ptr!=part.data()+part.size() || bits>32) return {};
    }
    const std::uint32_t mask=bits ? UINT32_MAX << (32-bits) : 0;
    if ((*ip & mask)!=*ip) return {}; // never silently broaden a mistyped CIDR
    return Ipv4Network{*ip,mask};
}
bool botgate_network_matches(std::string_view rules,std::uint32_t peer) noexcept {
    bool match=false;
    const bool valid=each_rule(rules,[&](auto token) { const auto n=parse_botgate_network(token);
        if (!n) return false;
        match=match || ((peer & n->mask)==n->address);
        return true; });
    return valid && match;
}
bool botgate_domain_matches(std::string_view rules,std::string_view name) noexcept {
    if (!valid_domain(name)) return false;
    bool match=false;
    const bool valid=each_rule(rules,[&](auto token) {
        if (!valid_domain(token)) return false;
        match=match || name==token || (name.size()>token.size() &&
            name[name.size()-token.size()-1]=='.' && name.substr(name.size()-token.size())==token);
        return true;
    });
    return valid && match;
}
bool valid_botgate_configuration(const BotGateConfiguration& c) noexcept {
    const auto networks=[](auto token) { return parse_botgate_network(token).has_value(); };
    return c.timeout_seconds>=5 && c.timeout_seconds<=120 &&
        each_rule(c.manual_blocks,networks) && each_rule(c.trusted_sources,networks) &&
        each_rule(c.feed_exceptions,networks) && each_rule(c.domains,valid_domain);
}
bool equivalent_botgate_configuration(const BotGateConfiguration& a,const BotGateConfiguration& b) noexcept {
    return a.enabled==b.enabled && a.ssh_enabled==b.ssh_enabled && a.timeout_seconds==b.timeout_seconds &&
        a.manual_blocks==b.manual_blocks && a.trusted_sources==b.trusted_sources &&
        a.feed_exceptions==b.feed_exceptions && a.domains==b.domains;
}
bool valid_logging_configuration(const LoggingConfiguration& c) noexcept {
    for (const auto& value : {c.runtime,c.callers,c.attention})
        if (value.size_mib<1 || value.size_mib>100 || value.archives<1 || value.archives>100) return false;
    return true;
}
std::optional<std::vector<std::uint32_t>> parse_botgate_feed(std::string_view text) {
    if (text.empty() || text.size()>kBotGateFeedMaximumBytes || text.back()!='\n') return {};
    std::vector<std::uint32_t> result;
    while (!text.empty()) {
        const auto end=text.find('\n');
        if (end==text.npos || end>1024) return {};
        auto line=text.substr(0,end); text.remove_prefix(end+1);
        if (line.find('\0')!=line.npos) return {};
        line=trim(line.substr(0,line.find('#')));
        if (line.empty()) continue;
        const auto ip=parse_botgate_ipv4(line);
        if (!ip || result.size()==kBotGateFeedMaximumEntries) return {};
        result.push_back(*ip);
    }
    if (result.empty()) return {};
    std::sort(result.begin(),result.end());
    result.erase(std::unique(result.begin(),result.end()),result.end());
    return result;
}
BotGateDisposition evaluate_botgate(const BotGateConfiguration& c,bool participating,
    std::uint32_t peer,std::string_view verified_name,const std::vector<std::uint32_t>& feed) noexcept {
    if (!c.enabled || !participating) return BotGateDisposition::off;
    if (botgate_network_matches(c.manual_blocks,peer)) return BotGateDisposition::manual_block;
    if (botgate_domain_matches(c.domains,verified_name)) return BotGateDisposition::domain_block;
    if (!botgate_network_matches(c.feed_exceptions,peer) && std::binary_search(feed.begin(),feed.end(),peer))
        return BotGateDisposition::feed_block;
    if (botgate_network_matches(c.trusted_sources,peer)) return BotGateDisposition::trusted;
    return BotGateDisposition::challenge;
}
BotGateChallenge::BotGateChallenge(std::uint64_t now,std::uint16_t seconds) noexcept
    : deadline_(now+static_cast<std::uint64_t>(seconds)*1000) {
    if (seconds<5 || seconds>120 || deadline_<now) result_=Result::invalid;
}
BotGateChallenge::Result BotGateChallenge::input(std::string_view bytes,std::uint64_t now) noexcept {
    if (result_!=Result::waiting) return result_;
    if (now>=deadline_) return result_=Result::timed_out;
    if (now>=last_input_ && now-last_input_>=100) stray_bytes_=0;
    last_input_=now;
    for (const unsigned char byte : bytes) {
        if (escape_sequence_) {
            if (++escape_bytes_>16) return result_=Result::invalid;
            if (byte>=0x40 && byte<=0x7e) escape_sequence_=false;
            continue;
        }
        if (last_escape_ && (byte=='[' || byte=='O')) {
            if (hits_) --hits_;
            settled_=0; last_escape_=false; escape_sequence_=true; escape_bytes_=0;
            continue;
        }
        last_escape_=byte==27;
        if (byte==27 || byte=='*') { hits_=std::min(hits_+1,2U); }
        else {
            if (hits_>=2 || ++stray_bytes_>8) return result_=Result::invalid;
        }
    }
    // A mixed burst cannot pass on incidental asterisks, even split over reads.
    if (hits_==2 && stray_bytes_) return result_=Result::invalid;
    if (hits_==2) settled_=now+100;
    return result_;
}
BotGateChallenge::Result BotGateChallenge::poll(std::uint64_t now) noexcept {
    if (result_!=Result::waiting) return result_;
    if (settled_ && now>=settled_) return result_=Result::passed;
    if (now>=deadline_) return result_=Result::timed_out;
    return result_;
}
unsigned BotGateChallenge::remaining(std::uint64_t now) const noexcept {
    return now>=deadline_ ? 0U : static_cast<unsigned>((deadline_-now+999)/1000);
}
} // namespace sfx::application
