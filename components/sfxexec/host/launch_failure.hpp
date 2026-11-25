#pragma once
#include <windows.h>
#include <stdexcept>
#include <string>
#include <string_view>

// Capture an API failure before RAII cleanup or another Win32 call can replace
// its thread-local error. Validation failures deliberately have no Win32 code.
struct SfxDosLaunchFailure : std::runtime_error {
    DWORD win32;
    SfxDosLaunchFailure(const char* reason,DWORD value):std::runtime_error(reason),win32(value){}
};
inline void sfx_dos_api(bool ok,const char* reason){
    if(!ok){const auto error=GetLastError();throw SfxDosLaunchFailure(reason,error);}
}
inline std::string sfx_dos_failure_code(std::string_view stage,std::string_view reason){
    std::string code="door_dos_";
    code+=stage;code+='_';
    // Only fixed program messages are passed here, never commands, paths,
    // environment values, session tokens or caller credentials.
    for(unsigned char c:reason){
        if(code.size()>=72)break;
        if(c>='A'&&c<='Z')c+='a'-'A';
        if((c>='a'&&c<='z')||(c>='0'&&c<='9'))code+=static_cast<char>(c);
        else if(code.back()!='_')code+='_';
    }
    while(code.back()=='_')code.pop_back();
    return code;
}
