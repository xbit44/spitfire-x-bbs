#include "sfx/application/phase3a_storage.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <sstream>
#include <vector>

namespace sfx::application {
namespace {

bool parse_ipv4(const std::string_view value, std::array<std::uint8_t, 4>* const output) {
    std::size_t begin = 0;
    for (std::size_t part = 0; part < 4; ++part) {
        const std::size_t end = value.find('.', begin);
        const std::size_t stop = end == std::string_view::npos ? value.size() : end;
        if (stop == begin || stop - begin > 3 || (stop - begin > 1 && value[begin] == '0')) {
            return false;
        }
        unsigned number = 0;
        for (std::size_t index = begin; index < stop; ++index) {
            const unsigned char character = static_cast<unsigned char>(value[index]);
            if (character < '0' || character > '9') return false;
            number = number * 10U + static_cast<unsigned>(character - '0');
        }
        if (number > 255U) return false;
        (*output)[part] = static_cast<std::uint8_t>(number);
        if (part < 3) {
            if (end == std::string_view::npos) return false;
            begin = end + 1;
        } else if (end != std::string_view::npos || stop != value.size()) {
            return false;
        }
    }
    return true;
}

bool parse_hex_group(const std::string_view value, std::uint16_t* const output) {
    if (value.empty() || value.size() > 4) return false;
    unsigned number = 0;
    for (const unsigned char character : value) {
        unsigned digit = 0;
        if (character >= '0' && character <= '9') digit = character - '0';
        else if (character >= 'a' && character <= 'f') digit = character - 'a' + 10U;
        else if (character >= 'A' && character <= 'F') digit = character - 'A' + 10U;
        else return false;
        number = number * 16U + digit;
    }
    *output = static_cast<std::uint16_t>(number);
    return true;
}

bool parse_ipv6_side(const std::string_view side, std::vector<std::uint16_t>* const groups) {
    if (side.empty()) return true;
    std::size_t begin = 0;
    while (begin <= side.size()) {
        const std::size_t end = side.find(':', begin);
        const std::size_t stop = end == std::string_view::npos ? side.size() : end;
        const std::string_view token = side.substr(begin, stop - begin);
        if (token.empty()) return false;
        if (token.find('.') != std::string_view::npos) {
            if (end != std::string_view::npos) return false;
            std::array<std::uint8_t, 4> ipv4{};
            if (!parse_ipv4(token, &ipv4)) return false;
            groups->push_back(static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(ipv4[0]) << 8U | ipv4[1]));
            groups->push_back(static_cast<std::uint16_t>(
                static_cast<std::uint16_t>(ipv4[2]) << 8U | ipv4[3]));
        } else {
            std::uint16_t group = 0;
            if (!parse_hex_group(token, &group)) return false;
            groups->push_back(group);
        }
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    return true;
}

bool parse_ipv6(const std::string_view value, std::array<std::uint8_t, 16>* const output) {
    const std::size_t compression = value.find("::");
    if (compression != std::string_view::npos && value.find("::", compression + 2) != std::string_view::npos) {
        return false;
    }
    std::vector<std::uint16_t> left;
    std::vector<std::uint16_t> right;
    if (compression == std::string_view::npos) {
        if (!parse_ipv6_side(value, &left) || left.size() != 8) return false;
    } else {
        if (!parse_ipv6_side(value.substr(0, compression), &left) ||
            !parse_ipv6_side(value.substr(compression + 2), &right) ||
            left.size() + right.size() >= 8) {
            return false;
        }
    }
    std::array<std::uint16_t, 8> groups{};
    std::copy(left.begin(), left.end(), groups.begin());
    std::copy(right.rbegin(), right.rend(), groups.rbegin());
    for (std::size_t index = 0; index < groups.size(); ++index) {
        (*output)[index * 2] = static_cast<std::uint8_t>((groups[index] >> 8U) & 0xffU);
        (*output)[index * 2 + 1] = static_cast<std::uint8_t>(groups[index] & 0xffU);
    }
    return true;
}

std::string format_ipv4(const std::uint8_t* const bytes) {
    return std::to_string(bytes[0]) + "." + std::to_string(bytes[1]) + "." +
        std::to_string(bytes[2]) + "." + std::to_string(bytes[3]);
}

std::string format_ipv6(const std::uint8_t* const bytes) {
    std::array<std::uint16_t, 8> groups{};
    for (std::size_t index = 0; index < groups.size(); ++index) {
        groups[index] = static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[index * 2]) << 8U | bytes[index * 2 + 1]);
    }
    std::size_t best_begin = groups.size();
    std::size_t best_length = 0;
    for (std::size_t begin = 0; begin < groups.size();) {
        if (groups[begin] != 0) {
            ++begin;
            continue;
        }
        std::size_t end = begin;
        while (end < groups.size() && groups[end] == 0) ++end;
        if (end - begin > best_length && end - begin >= 2) {
            best_begin = begin;
            best_length = end - begin;
        }
        begin = end;
    }
    std::ostringstream output;
    output << std::hex << std::nouppercase;
    for (std::size_t index = 0; index < groups.size();) {
        if (index == best_begin) {
            output << "::";
            index += best_length;
            continue;
        }
        const std::string current = output.str();
        if (!current.empty() && current.back() != ':') output << ':';
        output << groups[index++];
    }
    return output.str();
}

}  // namespace

std::string_view storage_code(const TransferProtocolPreference value) noexcept {
    switch (value) {
        case TransferProtocolPreference::xmodem: return "xmodem";
        case TransferProtocolPreference::ymodem: return "ymodem";
        case TransferProtocolPreference::ymodem_g: return "ymodem_g";
        case TransferProtocolPreference::zmodem: return "zmodem";
        case TransferProtocolPreference::select_at_transfer: return "select_at_transfer";
    }
    return {};
}

std::optional<TransferProtocolPreference> transfer_preference_from_storage_code(
    const std::string_view value) noexcept {
    for (const TransferProtocolPreference preference : {
             TransferProtocolPreference::xmodem,
             TransferProtocolPreference::ymodem,
             TransferProtocolPreference::ymodem_g,
             TransferProtocolPreference::zmodem,
             TransferProtocolPreference::select_at_transfer}) {
        if (storage_code(preference) == value) return preference;
    }
    return std::nullopt;
}

std::optional<CanonicalSourceAddress> canonicalize_source_ip(const std::string_view value) {
    CanonicalSourceAddress result;
    if (value.find(':') == std::string_view::npos) {
        std::array<std::uint8_t, 4> ipv4{};
        if (!parse_ipv4(value, &ipv4)) return std::nullopt;
        result.family = 4;
        result.size = 4;
        std::copy(ipv4.begin(), ipv4.end(), result.bytes.begin());
        result.text = format_ipv4(result.bytes.data());
        return result;
    }
    if (!parse_ipv6(value, &result.bytes)) return std::nullopt;
    result.family = 6;
    result.size = 16;
    result.text = format_ipv6(result.bytes.data());
    return result;
}

std::optional<std::string> canonical_source_ip(
    const std::uint8_t family,
    const std::vector<std::uint8_t>& bytes) {
    if (family == 4 && bytes.size() == 4) return format_ipv4(bytes.data());
    if (family == 6 && bytes.size() == 16) return format_ipv6(bytes.data());
    return std::nullopt;
}

}  // namespace sfx::application
