#include "sfx/application/new_caller_names.hpp"
#include "sfx/application/account_storage.hpp"

namespace sfx::application {
namespace {
bool valid_text(const std::string_view text) {
    std::size_t offset{};
    while (offset < text.size()) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        std::size_t count{}; std::uint32_t point{};
        if (lead < 128) { if ((lead < 32 && lead != '\t') || lead == 127) return false; point = lead; }
        else if (lead >= 0xc2 && lead <= 0xdf) { count = 1; point = lead & 31; }
        else if (lead >= 0xe0 && lead <= 0xef) { count = 2; point = lead & 15; }
        else if (lead >= 0xf0 && lead <= 0xf4) { count = 3; point = lead & 7; }
        else return false;
        if (offset + count >= text.size()) return false;
        for (std::size_t i = 1; i <= count; ++i) {
            const auto byte = static_cast<unsigned char>(text[offset + i]);
            if ((byte & 0xc0) != 0x80) return false;
            point = (point << 6) | (byte & 63);
        }
        if ((count == 1 && point < 128) || (count == 2 && point < 2048) ||
            (count == 3 && point < 65536) || point > 0x10ffff ||
            (point >= 0xd800 && point <= 0xdfff) || (point >= 0x80 && point <= 0x9f)) return false;
        offset += count + 1;
    }
    return true;
}
std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
    return text;
}
}
NewCallerNames NewCallerNames::unavailable() noexcept {
    NewCallerNames value; value.valid_ = false; return value;
}
NewCallerNames NewCallerNames::parse(const std::string_view bytes) {
    if (bytes.size() > maximum_bytes || bytes.substr(0, 3) == "\xef\xbb\xbf") return unavailable();
    NewCallerNames result;
    std::size_t offset = 0, lines = 0;
    while (offset < bytes.size()) {
        if (++lines > maximum_lines) return unavailable();
        const auto end = bytes.find('\n', offset);
        auto line = bytes.substr(offset, end == std::string_view::npos ? bytes.size() - offset : end - offset);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.size() > maximum_line_bytes) return unavailable();
        // Comments may be valid UTF-8; actual names still pass the exact existing
        // printable-ASCII account policy. Reject controls/malformed encodings.
        if (!valid_text(line)) return unavailable();
        line = trim(line);
        if (!line.empty() && line.front() != ';') {
            const auto normalized = normalize_account_name(line);
            if (!normalized) return unavailable();
            const auto key = account_name_comparison_key(*normalized);
            if (!key) return unavailable();
            result.keys_.insert(*key);
        }
        if (end == std::string_view::npos) break;
        offset = end + 1;
    }
    return result;
}
NewCallerNameDecision NewCallerNames::evaluate(const std::string_view name) const {
    if (!valid_) return NewCallerNameDecision::temporarily_unavailable;
    const auto normalized = normalize_account_name(name);
    const auto key = normalized ? account_name_comparison_key(*normalized) : std::nullopt;
    if (!key || keys_.count(*key)) return NewCallerNameDecision::unavailable;
    return NewCallerNameDecision::allowed;
}
} // namespace sfx::application
