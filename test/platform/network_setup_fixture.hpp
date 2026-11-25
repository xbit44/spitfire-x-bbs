#pragma once
#include <windows.h>
#include <filesystem>
#include <stdexcept>
#include <string_view>
// Test-only closed alternatives. Failed r1 is deliberately excluded/retained.
inline std::filesystem::path network_setup_qualification_root(){
#ifdef SFX_RC19_QUALIFICATION
    return L"C:/Program Files/SFX-R19-Test-20261104-r1";
#elif defined(SFX_RC18_QUALIFICATION)
    return L"C:/Program Files/SFX-ColorFix-Test-20261103-r1";
#elif defined(SFX_RC17_QUALIFICATION)
    return L"C:/Program Files/SFX-ReaderFix-Test-20261103-r1";
#elif defined(SFX_RC16_QUALIFICATION)
    return L"C:/Program Files/SFX-AnsiFix-Test-20261103-r1";
#elif defined(SFX_RC15_QUALIFICATION)
    return L"C:/Program Files/SFX-TossFix-Test-20261102-r1";
#elif defined(SFX_RC14_QUALIFICATION)
    return L"C:/Program Files/SFX-ImportFix-Test-20261102-r1";
#else
    wchar_t choice[16]{};const auto count=GetEnvironmentVariableW(L"SFX_RC13_TEST_FIXTURE",choice,16);
    if(!count||std::wstring_view(choice)==L"r2")return L"C:/Program Files/SFX-NetworkSetup-Test-20261102-r2";
    if(count<16&&std::wstring_view(choice)==L"r3")return L"C:/Program Files/SFX-NetworkSetup-Test-20261102-r3";
    throw std::runtime_error("closed RC13 test fixture selection required");
#endif
}
