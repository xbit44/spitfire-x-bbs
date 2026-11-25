#pragma once
#include "sfx/platform/win32/portable_credential_codec.hpp"
#include <filesystem>

namespace sfx::platform::win32 {
enum class PortableKeyStatus { ready,missing,invalid,unavailable };
struct PortableBoardKey {PortableBoardIdentity board;PortableCredentialKey key;};
struct PortableKeyResult {
    PortableKeyStatus status{PortableKeyStatus::unavailable};
    std::optional<PortableBoardKey> value;
    const char* diagnostic{"portable_key_unavailable"};
    std::uint32_t native_error{};
    explicit operator bool()const noexcept{return status==PortableKeyStatus::ready&&value.has_value();}
};
// Reads only. A missing/corrupt/inaccessible key never causes generation.
PortableKeyResult load_portable_board_key(const std::filesystem::path& root);
// Explicit initialization only: caller must hold the installation exclusion
// and establish fresh bootstrap or verified legacy migration with no v3 data.
// An established marker or damaged existing key prevents replacement.
PortableKeyResult create_portable_board_key(const std::filesystem::path& root,const std::wstring& current_owner);
}
