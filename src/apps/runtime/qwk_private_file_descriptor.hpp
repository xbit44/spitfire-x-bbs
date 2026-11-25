#pragma once
#include "sfx/platform/win32/installation_security.hpp"
#include <sddl.h>
namespace sfx::apps::runtime::qwk_native_detail {
inline platform::win32::installation::Descriptor private_file_descriptor(const std::wstring& owner){
    // Exact accepted FILE rights; no directory execute/delete-child or OICI.
    const auto text=L"O:"+owner+L"D:P(A;;0x13019f;;;"+owner+L")(A;;FA;;;SY)(A;;FA;;;BA)";
    PSECURITY_DESCRIPTOR raw{};platform::win32::installation::require(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),SDDL_REVISION_1,&raw,nullptr),"qwk_private_file_descriptor");return platform::win32::installation::Descriptor(raw);
}
} // namespace sfx::apps::runtime::qwk_native_detail
