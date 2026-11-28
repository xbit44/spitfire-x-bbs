#include "sfx/application/terminal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

namespace sfx::application {
namespace {

bool valid_identity(const std::string_view value) noexcept {
    if (value.empty() || value.size() > 128U) return false;
    for (const unsigned char character : value) {
        const bool letter = (character >= static_cast<unsigned char>('a') &&
            character <= static_cast<unsigned char>('z')) ||
            (character >= static_cast<unsigned char>('A') &&
                character <= static_cast<unsigned char>('Z'));
        const bool digit = character >= static_cast<unsigned char>('0') &&
            character <= static_cast<unsigned char>('9');
        if (!letter && !digit && character != static_cast<unsigned char>('.') &&
            character != static_cast<unsigned char>('-') &&
            character != static_cast<unsigned char>('_')) {
            return false;
        }
    }
    return true;
}

bool ordinary_logical_text(const std::string_view value) noexcept {
    if (value.empty()) return false;
    for (const unsigned char character : value) {
        if (character == static_cast<unsigned char>('\r') ||
            character == static_cast<unsigned char>('\n')) {
            continue;
        }
        if (character < 0x20U || character > 0x7eU) return false;
    }
    return true;
}

bool valid_ansi_parameters(
    const std::string_view parameters,
    const std::size_t maximum_count,
    const std::uint16_t maximum_value,
    std::vector<std::uint16_t>* const parsed = nullptr) noexcept {
    if (parameters.empty()) {
        if (parsed != nullptr) parsed->clear();
        return true;
    }
    std::size_t count = 1;
    for (const char character : parameters) {
        if (character == ';') {
            if (++count > maximum_count) return false;
        } else if (character < '0' || character > '9') {
            return false;
        }
    }
    if (count > maximum_count) return false;

    std::vector<std::uint16_t> values;
    if (parsed != nullptr) {
        try {
            values.reserve(count);
        } catch (...) {
            return false;
        }
    }
    std::size_t start = 0;
    while (start <= parameters.size()) {
        const auto end = parameters.find(';', start);
        const auto field = parameters.substr(start,
            end == std::string_view::npos
                ? parameters.size() - start
                : end - start);
        std::uint32_t value = 0;
        for (const char character : field) {
            value = value * 10U +
                static_cast<std::uint32_t>(character - '0');
            if (value > maximum_value) return false;
        }
        if (parsed != nullptr) {
            values.push_back(static_cast<std::uint16_t>(value));
        }
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    if (parsed != nullptr) *parsed = std::move(values);
    return true;
}

bool valid_cp437_display_text(const std::string_view bytes) noexcept {
    if (bytes.empty()) return false;
    for (const unsigned char character : bytes) {
        if (character == 0x1bU || character < 0x20U) return false;
    }
    return true;
}

bool valid_key(const char value) noexcept {
    const auto character = static_cast<unsigned char>(value);
    return character >= 0x20U && character <= 0x7eU;
}

}  // namespace

bool valid_terminal_input_request(const TerminalInputRequest& request) noexcept {
    if(request.initial_value.size()>request.maximum_characters)return false;
    for(unsigned char c:request.initial_value)if(c<32 || c==127)return false;
    return valid_identity(request.identity) && !request.prompt.empty() &&
        ordinary_logical_text(request.prompt) && request.maximum_characters > 0 &&
        request.maximum_characters <= kTerminalMaximumInputCharacters &&
        (request.empty_policy == EmptyInputPolicy::reject ||
            request.empty_policy == EmptyInputPolicy::use_default);
}

bool valid_terminal_key_request(const TerminalKeyRequest& request) noexcept {
    if (!valid_identity(request.identity) || request.prompt.empty() ||
        !ordinary_logical_text(request.prompt) || request.allowed_keys.empty() ||
        request.allowed_keys.size() > 64U) {
        return false;
    }
    std::array<bool, 128> seen{};
    for (const char key : request.allowed_keys) {
        const auto value = static_cast<unsigned char>(key);
        if (!valid_key(key) || seen[value]) return false;
        seen[value] = true;
    }
    if (request.default_key.has_value()) {
        const auto value = static_cast<unsigned char>(*request.default_key);
        if (!valid_key(*request.default_key) || !seen[value]) return false;
    }
    return true;
}

bool valid_terminal_input_result(
    const TerminalInputRequest& request,
    const TerminalInputResult& result) noexcept {
    if (!valid_terminal_input_request(request)) return false;
    switch (result.outcome) {
        case TerminalOutcome::completed:
            return result.has_value() &&
                result.value.size() <= request.maximum_characters;
        case TerminalOutcome::empty:
            return request.empty_policy == EmptyInputPolicy::use_default &&
                result.value.empty();
        case TerminalOutcome::cancelled:
        case TerminalOutcome::timed_out:
        case TerminalOutcome::disconnected:
        case TerminalOutcome::shutdown_requested:
        case TerminalOutcome::invalid:
        case TerminalOutcome::overlength:
        case TerminalOutcome::terminal_failure:
        case TerminalOutcome::output_failure:
            return result.value.empty();
    }
    return false;
}

bool valid_terminal_key_result(
    const TerminalKeyRequest& request,
    const TerminalKeyResult& result) noexcept {
    if (!valid_terminal_key_request(request)) return false;
    if(result.navigation!=TerminalNavigationKey::none)return request.navigation_keys && result.outcome==TerminalOutcome::completed && !result.key &&
        result.navigation>=TerminalNavigationKey::up && result.navigation<=TerminalNavigationKey::right;
    switch (result.outcome) {
        case TerminalOutcome::completed:
            return result.has_key() &&
                std::find(request.allowed_keys.begin(), request.allowed_keys.end(),
                    *result.key) != request.allowed_keys.end();
        case TerminalOutcome::empty:
            return result.has_key() && request.default_key.has_value() &&
                result.key == request.default_key;
        case TerminalOutcome::cancelled:
        case TerminalOutcome::timed_out:
        case TerminalOutcome::disconnected:
        case TerminalOutcome::shutdown_requested:
        case TerminalOutcome::invalid:
        case TerminalOutcome::overlength:
        case TerminalOutcome::terminal_failure:
        case TerminalOutcome::output_failure:
            return !result.key.has_value();
    }
    return false;
}

bool valid_terminal_presentation_request(
    const TerminalPresentationRequest& request) noexcept {
    return valid_identity(request.identity) &&
        (request.role==TerminalPresentationRole::system || request.role==TerminalPresentationRole::caller_value) &&
        ordinary_logical_text(request.non_secret_text);
}

bool valid_terminal_ansi_sequence(
    const std::string_view sequence) noexcept {
    if (sequence.size() == 2U &&
        static_cast<unsigned char>(sequence[0]) == 0x1bU) {
        return sequence[1] == '7' || sequence[1] == '8' ||
            sequence[1] == 'D' || sequence[1] == 'E' ||
            sequence[1] == 'M';
    }
    if (sequence.size() < 3U || sequence.size() > 40U ||
        static_cast<unsigned char>(sequence[0]) != 0x1bU ||
        sequence[1] != '[') {
        return false;
    }

    const char final = sequence.back();
    const std::string_view parameters =
        sequence.substr(2, sequence.size() - 3U);
    if (final == 'h' || final == 'l') {
        return parameters == "?7" || parameters == "?25";
    }
    if (parameters.find('?') != std::string_view::npos) return false;

    std::vector<std::uint16_t> parsed;
    switch (final) {
        case 'm':
            return valid_ansi_parameters(parameters, 16U, 255U);
        case 't':
            // Generated CTerm RGB color only, never terminal window actions.
            return valid_ansi_parameters(parameters,4U,255U,&parsed)&&parsed.size()==4U&&parsed[0]<=1U;
        case 'A':
        case 'B':
        case 'C':
        case 'D':
        case 'E':
        case 'F':
        case 'G':
        case 'd':
            return valid_ansi_parameters(parameters, 1U, 999U);
        case 'H':
        case 'f':
            return valid_ansi_parameters(parameters, 2U, 999U);
        case 'J':
            if (!valid_ansi_parameters(parameters, 1U, 3U, &parsed)) {
                return false;
            }
            return parsed.empty() || parsed.size() == 1U;
        case 'K':
            if (!valid_ansi_parameters(parameters, 1U, 2U, &parsed)) {
                return false;
            }
            return parsed.empty() || parsed.size() == 1U;
        case 's':
        case 'u':
            return parameters.empty();
        default:
            return false;
    }
}

bool valid_terminal_display_program(
    const TerminalDisplayProgram& program) noexcept {
    if(program.paging_prompt_color && *program.paging_prompt_color>15) return false;
    if (program.operations.empty() ||
        program.operations.size() > kTerminalMaximumDisplayOperations ||
        program.output_byte_count > kTerminalMaximumDisplayOutputBytes) {
        return false;
    }
    std::size_t output_bytes = 0;
    for (const auto& operation : program.operations) {
        if (operation.playback_bps != 0 &&
            (operation.playback_bps < 300 || operation.playback_bps > 115200)) return false;
        std::size_t addition = 0;
        switch (operation.kind) {
            case TerminalDisplayOperationKind::cp437_text:
                if (!valid_cp437_display_text(operation.bytes)) return false;
                addition = operation.bytes.size();
                break;
            case TerminalDisplayOperationKind::logical_newline:
            case TerminalDisplayOperationKind::paging_newline:
                if (!operation.bytes.empty()) return false;
                addition = 2U;
                break;
            case TerminalDisplayOperationKind::ansi_sequence:
                if (!valid_terminal_ansi_sequence(operation.bytes)) return false;
                addition = operation.bytes.size();
                break;
            case TerminalDisplayOperationKind::clear_screen:
                if (!operation.bytes.empty()) return false;
                addition = 7U;
                break;
            case TerminalDisplayOperationKind::caller_bell:
                if (!operation.bytes.empty()) return false;
                addition = 1U;
                break;
            case TerminalDisplayOperationKind::pause:
                if (!operation.bytes.empty()) return false;
                addition = 2U;
                break;
            case TerminalDisplayOperationKind::paging_disabled:
            case TerminalDisplayOperationKind::paging_enabled:
            case TerminalDisplayOperationKind::abort_disabled:
            case TerminalDisplayOperationKind::abort_enabled:
                if (!operation.bytes.empty()) return false;
                break;
        }
        if (addition > kTerminalMaximumDisplayOutputBytes - output_bytes) {
            return false;
        }
        output_bytes += addition;
    }
    return output_bytes == program.output_byte_count;
}

bool valid_terminal_display_paging_policy(
    const TerminalDisplayPagingPolicy& policy) noexcept {
    if (!policy.page_length.has_value()) {
        return !policy.more_prompt_enabled;
    }
    return *policy.page_length >= kTerminalMinimumPageLength &&
        *policy.page_length <= kTerminalMaximumPageLength;
}

bool valid_terminal_display_presentation_request(
    const TerminalDisplayPresentationRequest& request) noexcept {
    return valid_identity(request.identity) && request.program != nullptr &&
        valid_terminal_display_program(*request.program) &&
        valid_terminal_display_paging_policy(request.paging);
}

bool valid_terminal_pause_request(const TerminalPauseRequest& request) noexcept {
    return valid_identity(request.identity);
}

bool terminal_outcome_has_no_input(const TerminalOutcome outcome) noexcept {
    switch (outcome) {
        case TerminalOutcome::cancelled:
        case TerminalOutcome::timed_out:
        case TerminalOutcome::disconnected:
        case TerminalOutcome::shutdown_requested:
        case TerminalOutcome::invalid:
        case TerminalOutcome::overlength:
        case TerminalOutcome::terminal_failure:
        case TerminalOutcome::output_failure:
            return true;
        case TerminalOutcome::completed:
        case TerminalOutcome::empty:
            return false;
    }
    return true;
}

namespace detail {

void explicitly_erase_bytes(void* const data, const std::size_t byte_count) noexcept {
    auto* const bytes = static_cast<volatile unsigned char*>(data);
    for (std::size_t index = 0; index < byte_count; ++index) {
        bytes[index] = 0U;
    }
    std::atomic_signal_fence(std::memory_order_seq_cst);
}

}  // namespace detail

void explicitly_erase_terminal_secret(std::string& value) noexcept {
    detail::explicitly_erase_bytes(value.data(), value.size());
    value.clear();
}

}  // namespace sfx::application
