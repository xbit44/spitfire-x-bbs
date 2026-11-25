#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace sfx::application {

enum class CredentialResult : std::uint8_t { verified, rejected, throttled, unavailable };

class ICredentialVerifier {
public:
    virtual ~ICredentialVerifier() = default;
    virtual CredentialResult verify(
        std::string_view caller_identity,
        std::string_view candidate_secret) = 0;
};

class ICredentialChanger {
public:
    virtual ~ICredentialChanger() = default;
    virtual bool replace_secret(
        std::string_view caller_identity,
        std::string_view new_secret) = 0;
};

struct AuthenticationAttemptSignal {
    std::string caller_identity;
    std::string untrusted_peer_address;
    std::uint32_t attempt_number{0};
    bool accepted{false};
};

}  // namespace sfx::application

