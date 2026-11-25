#pragma once
#include <windows.h>
#include <initializer_list>
#include <stdexcept>
#include "launch_failure.hpp"

// NTVDM must see a local console, never the Runtime caller pipes as its
// standard devices. A DOS console read must not compete with the host's
// reader of that same synchronous pipe. The private driver carries all
// caller input/output; the console remains available to the sysop.
inline void sfx_dos_console_session(std::initializer_list<HANDLE> private_handles){
    const HANDLE input=GetStdHandle(STD_INPUT_HANDLE);
    const HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    const HANDLE error=GetStdHandle(STD_ERROR_HANDLE);
    // The invisible host uses NUL standard devices. The DOS child creates its
    // own console, as in the earlier working component launcher. Never assume
    // that an inherited standard-handle table describes a usable console.
    if(GetFileType(input)!=FILE_TYPE_CHAR||GetFileType(output)!=FILE_TYPE_CHAR||
       GetFileType(error)!=FILE_TYPE_CHAR)
        throw std::runtime_error("DOS host standard devices must be local");
    for(const auto handle:private_handles){
        if(!handle||handle==INVALID_HANDLE_VALUE||handle==input||handle==output||handle==error)
            throw std::runtime_error("DOS private handle isolation failed");
        sfx_dos_api(SetHandleInformation(handle,HANDLE_FLAG_INHERIT,0)!=FALSE,"private_handle_inheritance");
    }
}

inline STARTUPINFOA sfx_dos_child_startup(){
    STARTUPINFOA startup{};startup.cb=sizeof(startup);
    startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_SHOWMINNOACTIVE;
    return startup;
}
// Restore the working component's separate DOS console/VDM launch. The host
// has no window; only the game console appears minimized on the taskbar.
// Neither redirected standard devices nor private IPC handles reach the game.
inline constexpr DWORD sfx_dos_child_flags=CREATE_NEW_CONSOLE|CREATE_SEPARATE_WOW_VDM|CREATE_SUSPENDED;
