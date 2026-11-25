#pragma once

#include "sfx/application/account_storage.hpp"

namespace sfx::platform::win32 {

class WindowsSecureIdentifierGenerator final : public application::IIdentifierGenerator {
public:
    application::Identifier128 generate() override;
};

}  // namespace sfx::platform::win32
