#pragma once
#include "sfx/platform/portable_credential_codec.hpp"
// Preserve the accepted Windows API while both hosts use the same envelope.
namespace sfx::platform::win32 {
using portable::kPortableCredentialMaximum;
using portable::PortableBoardIdentity;
using portable::PortableCredentialFamily;
using portable::PortableCredentialKey;
using portable::seal_portable_credential;
using portable::open_portable_credential;
}
