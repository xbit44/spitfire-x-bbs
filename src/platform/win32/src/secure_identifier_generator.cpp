#include "sfx/platform/win32/secure_identifier_generator.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <stdexcept>

namespace sfx::platform::win32 {

application::Identifier128 WindowsSecureIdentifierGenerator::generate() {
    application::Identifier128 identifier;
    do {
        const NTSTATUS status = BCryptGenRandom(
            nullptr,
            identifier.bytes.data(),
            static_cast<ULONG>(identifier.bytes.size()),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            throw std::runtime_error("Windows system RNG failed to generate a storage identifier");
        }
    } while (!identifier.valid());
    return identifier;
}

}  // namespace sfx::platform::win32
