#pragma once
#include "sfx/platform/portable_credential_codec.hpp"
#include <filesystem>

namespace sfx::platform::linux_host {
enum class PortableKeyStatus {ready,missing,invalid,unavailable};
struct PortableBoardKey {portable::PortableBoardIdentity board;portable::PortableCredentialKey key;};
struct PortableKeyResult {
    PortableKeyStatus status{PortableKeyStatus::unavailable};
    std::optional<PortableBoardKey> value;
    const char* diagnostic{"portable_key_unavailable"};
    explicit operator bool() const noexcept{return status==PortableKeyStatus::ready&&value.has_value();}
};
// Reads only; never repairs or regenerates missing/damaged identity.
PortableKeyResult load_portable_board_key(const std::filesystem::path& root);
// Trusted bootstrap owner only: hold the installation lock and prove incomplete
// first-run state. The existing private first-run intent is additionally required.
// An established marker forbids creation of a missing key. An interrupted fresh
// key publication can finish its marker without changing the original key.
PortableKeyResult initialize_fresh_portable_board_key(const std::filesystem::path& root);
} // namespace sfx::platform::linux_host
