#pragma once
// Independently authored SFX parser for the exact reviewed profile-4 contract.
// Parsing is only one prerequisite: the host must still verify the owned job,
// exact file set, completed output/input accounting, authority and deadlines.
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace sfx::apps::runtime::message_receive_detail {
enum class PolicyRefusal : unsigned { none, extension, too_large, empty_batch };
struct PolicyCompletion final {
    std::uint64_t consumed{};
    PolicyRefusal refusal{PolicyRefusal::none};
};
inline std::optional<PolicyCompletion> policy_completion(
    std::string_view receipt, std::string_view dsz, bool payload_present,
    std::size_t payload_bytes) {
    if (payload_present ? (!payload_bytes || payload_bytes > 262144) : payload_bytes != 0)
        return {};
    const auto expected_dsz = std::string(payload_present ? "Z " : "S ") +
        std::to_string(payload_bytes) + " 0 0 0 0 0 payload.bin\n";
    if (dsz != expected_dsz) return {};
    for (const auto reason : {PolicyRefusal::none, PolicyRefusal::extension, PolicyRefusal::too_large, PolicyRefusal::empty_batch}) {
        if ((reason == PolicyRefusal::none) != payload_present) continue;
        const auto expected = std::string("SFX-MESSAGE-RECEIVE 4\nattempted ") +
            (reason == PolicyRefusal::empty_batch ? "0" : "1") + "\ncompleted " +
            (payload_present ? "1" : "0") + "\nbytes " + std::to_string(payload_bytes) +
            "\nzeof " + (payload_present ? "1" : "0") + "\nbatch 1\nfailure 0\nrefusal " +
            std::to_string(static_cast<unsigned>(reason)) + "\ninput ";
        if (receipt.size() <= expected.size() + 1 || receipt.substr(0, expected.size()) != expected ||
            receipt.back() != '\n') continue;
        const auto digits = receipt.substr(expected.size(), receipt.size() - expected.size() - 1);
        if (digits.empty() || digits.size() > 20 || digits.front() < '1' || digits.front() > '9') return {};
        std::uint64_t count{};
        const auto parsed = std::from_chars(digits.data(), digits.data() + digits.size(), count);
        if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() || !count) return {};
        return PolicyCompletion{count, reason};
    }
    return {};
}
} // namespace sfx::apps::runtime::message_receive_detail
