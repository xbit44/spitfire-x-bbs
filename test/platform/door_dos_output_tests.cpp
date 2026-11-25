// Actual Windows IPC qualification of the production DOS output drain.
// Does not load a VDD, start NTVDM, touch System32, or emulate owner acceptance.
#include "../../components/sfxexec/host/output_batch.hpp"
#include <iostream>
struct Handle{HANDLE h;~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Queue{
    Handle read{INVALID_HANDLE_VALUE},write{INVALID_HANDLE_VALUE},credit{nullptr};
    Queue(){const auto path=L"\\\\.\\mailslot\\SFX.OutputProbe."+std::to_wstring(GetCurrentProcessId());
        read.h=CreateMailslotW(path.c_str(),4000,0,nullptr);check(read.h!=INVALID_HANDLE_VALUE,"private mailslot");
        write.h=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);check(write.h!=INVALID_HANDLE_VALUE,"private writer");
        credit.h=CreateSemaphoreW(nullptr,64,64,nullptr);check(credit.h!=nullptr,"credits");}
    void send(const std::string& text){DWORD sent{};check(WaitForSingleObject(credit.h,0)==WAIT_OBJECT_0,"producer credit");check(WriteFile(write.h,text.data(),static_cast<DWORD>(text.size()),&sent,nullptr)&&sent==text.size(),"producer write");}
    void full(){LONG old{};check(ReleaseSemaphore(credit.h,1,&old)==FALSE&&GetLastError()==ERROR_TOO_MANY_POSTS,"all credits restored exactly");}
};
int main(){try{
    Queue q;check(sfx_dos_output_batch(q.read.h,q.credit.h).empty(),"idle never blocks");
    std::string expected;
    for(unsigned i=0;i<64;++i){const std::string byte(1,static_cast<char>(i));expected+=byte;q.send(byte);}
    check(WaitForSingleObject(q.credit.h,0)==WAIT_TIMEOUT,"full UART queue applies backpressure");
    check(sfx_dos_output_batch(q.read.h,q.credit.h)==expected,"64 one-byte UART messages delivered in one exact ordered burst");q.full();
    for(unsigned i=0;i<64;++i)q.send(std::string(4000,static_cast<char>(i)));
    for(unsigned batch=0;batch<8;++batch){const auto text=sfx_dos_output_batch(q.read.h,q.credit.h);check(text.size()==32000,"FOSSIL burst respects 32KiB ceiling");
        for(unsigned i=0;i<8;++i)check(text.substr(i*4000,4000)==std::string(4000,static_cast<char>(batch*8+i)),"FOSSIL ordering and byte integrity");}
    q.full();check(sfx_dos_output_batch(q.read.h,q.credit.h).empty(),"queue drained");
    const auto now=GetTickCount64();std::size_t total{};
    for(unsigned frame=0;frame<128;++frame){for(unsigned i=0;i<64;++i)q.send(std::string(1,static_cast<char>(i)));const auto text=sfx_dos_output_batch(q.read.h,q.credit.h);check(text==expected,"repeated UART credit/reuse cycle");total+=text.size();Sleep(2);}
    const auto elapsed=GetTickCount64()-now;q.full();
    std::cout<<"PASS DOS output: "<<checks<<" checks; UART bytes="<<total<<" bursts=128 old_one_message_turns=8192 elapsed_ms="<<elapsed<<"; actual NTVDM/game speed pending owner\n";
    return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
