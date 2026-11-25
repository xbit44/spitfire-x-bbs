// Bounded DOS output drain shared by the production host and Windows probe.
// UART produces one mailslot message per byte; FOSSIL produces larger writes.
#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <stdexcept>
inline std::string sfx_dos_output_batch(HANDLE output,HANDLE credit){
    std::string pending;
    for(unsigned batch=0;batch<64;++batch){DWORD next{},count{};
        if(!GetMailslotInfo(output,nullptr,&next,&count,nullptr)||count>64||
            (next!=MAILSLOT_NO_MESSAGE&&next>4000))throw std::runtime_error("DOS output bound exceeded");
        if(!count||pending.size()+next>32768)break;
        std::array<char,4000> bytes{};DWORD read{};
        if(!ReadFile(output,bytes.data(),static_cast<DWORD>(bytes.size()),&read,nullptr)||read!=next)
            throw std::runtime_error("DOS output message failed");
        pending.append(bytes.data(),read);
        if(!ReleaseSemaphore(credit,1,nullptr))throw std::runtime_error("DOS output credit failed");
    }
    return pending;
}
