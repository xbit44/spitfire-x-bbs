/* Executes the candidate's real dispatcher/Win32 IPC against fake VDM registers.
 * This is not native DOS, ISR/UART timing, or real Synchronet coexistence proof. */
#include <windows.h>
#include <sddl.h>
#include <array>
#include <string>
#include <vector>
#include <iostream>
#include <stdexcept>
#include <cstring>
extern "C" __declspec(dllimport) void __stdcall TestRegisters(USHORT,BYTE,BYTE,USHORT,USHORT);
extern "C" __declspec(dllimport) BYTE* __stdcall TestBuffer(void);
extern "C" __declspec(dllimport) USHORT __stdcall TestResult(void);
static unsigned checks;
static void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
struct Handle {HANDLE h{};~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
static std::string slot(const std::string& session,unsigned node,const char* direction){return "\\\\.\\mailslot\\sfxexec\\v1\\"+session+"\\n"+std::to_string(node)+"\\"+direction;}
static std::string event(const std::string& session,unsigned node,const char* kind){return "Local\\SFXEXEC.v1."+session+".n"+std::to_string(node)+"."+kind;}
static std::string receive(HANDLE pipe,HANDLE credit){std::array<char,4096> bytes{};DWORD count{};
    check(ReadFile(pipe,bytes.data(),static_cast<DWORD>(bytes.size()),&count,nullptr)!=FALSE,"owned fixture mailslot read");
    check(ReleaseSemaphore(credit,1,nullptr)!=FALSE,"return consumed private output credit");return {bytes.data(),count};}
int main(int argc,char** argv){try{
    check(argc==3,"candidate DLL and fresh session token required");const std::string session=argv[2];
    const auto module=LoadLibraryExA(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);
    check(module!=nullptr,"load exact candidate against private fake NTVDM dependency");
    const auto dispatch=reinterpret_cast<void(__cdecl*)()>(GetProcAddress(module,"VDDDispatch"));
    check(dispatch!=nullptr,"undecorated cdecl VDDDispatch export");
    const auto op=[&](BYTE node,BYTE code,USHORT size=0,USHORT offset=0){TestRegisters(1,node,code,size,offset);dispatch();return TestResult();};
    check(op(0,21)!=0x6424&&op(129,21)!=0x6424,"invalid byte-sized node range rejected without wrapping");
    check(op(128,21)==0x6424,"highest supported node retains exact interface value");
    check(op(123,5,20)!=0,"status before session/open rejected");
    std::memset(TestBuffer(),'z',32);check(op(123,20,32)!=0x6424,"malformed session rejected");
    // Initialize redirected CRT/debug output before counting candidate resources.
    std::cerr<<"fixture startup\n";OutputDebugStringA("SFXEXEC private fixture startup\n");
    // Windows initializes its SDDL/SID conversion support on first use. Exercise
    // the same public APIs independently, releasing every returned allocation
    // and handle, before measuring the candidate's own session resources.
    {Handle token;check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h)!=FALSE,"fixture token warmup");DWORD size{};
        GetTokenInformation(token.h,TokenUser,nullptr,0,&size);std::vector<BYTE> data(size);check(GetTokenInformation(token.h,TokenUser,data.data(),size,&size)!=FALSE,"fixture SID warmup");
        char* sid{};check(ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(data.data())->User.Sid,&sid)!=FALSE,"fixture SID conversion warmup");
        std::string sddl="D:P(A;;GA;;;SY)(A;;GA;;;"+std::string(sid)+")";LocalFree(sid);PSECURITY_DESCRIPTOR descriptor{};
        check(ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"fixture descriptor warmup");LocalFree(descriptor);}
    const DWORD handles_before=[] {DWORD n=0;GetProcessHandleCount(GetCurrentProcess(),&n);return n;}();
    for(unsigned round=0;round<12;++round){
        DWORD observed{};GetProcessHandleCount(GetCurrentProcess(),&observed);
        std::cerr<<"resource round="<<round<<" expected="<<handles_before<<" observed="<<observed<<"\n";
        check(observed==handles_before,"prior session retained no Win32 handles");
        std::string token=session;token.back()="0123456789abcdef"[round];
        std::memcpy(TestBuffer(),token.data(),32);
        check(op(123,20,31)!=0x6424,"short session bind rejected");
        check(op(123,20,32)==0x6424,"matched pair binds exact session");
        check(op(124,0)!=0,"another node cannot use bound session");
        check(op(123,20,32)!=0x6424,"active session cannot be rebound");
        if(round==0){check(op(123,0)!=0,"missing host-side endpoints fail closed");continue;}
        Handle output{CreateMailslotA(slot(token,123,"from_dos").c_str(),4000,3000,nullptr)};
        check(output.h!=INVALID_HANDLE_VALUE,"create only private output mailslot");
        Handle hungup{CreateEventA(nullptr,TRUE,FALSE,event(token,123,"hungup").c_str())};check(hungup.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh hungup event");
        Handle hangup{CreateEventA(nullptr,TRUE,FALSE,event(token,123,"hangup").c_str())};check(hangup.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh hangup event");
        Handle carrier{CreateEventA(nullptr,FALSE,FALSE,event(token,123,"carrier").c_str())};check(carrier.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh carrier event");
        Handle from_credit{CreateSemaphoreA(nullptr,64,64,event(token,123,"from_credit").c_str())};check(from_credit.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh output credit semaphore");
        Handle to_credit{CreateSemaphoreA(nullptr,64,64,event(token,123,"to_credit").c_str())};check(to_credit.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh input credit semaphore");
        if(round%2)check(op(123,19)==0,"enable UART path before open");
        check(op(123,0)==0,"real driver opens only matching SFX endpoints");
        const auto hello=receive(output.h,from_credit.h);
        check(hello.find("SFXPAIR1|SFXEXEC-20261024-01|"+token+"|123|")==0,"first driver message binds pair/session/node and actual module path");
        check(op(123,99)==0xffff,"unknown operations are rejected");
        check(op(123,4,65535,4096)==0xffff,"invalid mapped DOS write is refused");
        Handle input{CreateFileA(slot(token,123,"to_dos").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)};
        check(input.h!=INVALID_HANDLE_VALUE,"open real driver private input mailslot");
        for(unsigned n=0;n<20;++n){
            const BYTE value=static_cast<BYTE>((session.front()=='a'?17:151)+n);DWORD sent{};
            check(WaitForSingleObject(to_credit.h,0)==WAIT_OBJECT_0,"acquire bounded caller input credit");
            check(WriteFile(input.h,&value,1,&sent,nullptr)&&sent==1,"write distinguishable private caller input");
            const auto until=GetTickCount64()+2000;while(op(123,7)==0&&GetTickCount64()<until)Sleep(1);
            check(op(123,3,1,4096)==1&&TestBuffer()[4096]==value,"peek retains exact session byte");
            check(op(123,2,1,4096)==1&&TestBuffer()[4096]==value,"read consumes exact session byte");
            TestBuffer()[4096]=value;check(op(123,4,1,4096)==1,"write exact driver output byte");
            check(receive(output.h,from_credit.h)==std::string(1,static_cast<char>(value)),"output belongs only to this private session");
        }
        if(round==1){
            // Fill the actual kernel output queue without consuming it. Credit
            // exhaustion must stop new writes, not allocate unbounded memory.
            std::memset(TestBuffer()+4096,session.front(),4001);
            check(op(123,4,4001,4096)==4000,"large FOSSIL write reports bounded partial count");
            for(unsigned n=1;n<64;++n)check(op(123,4,1,4096)==1,"consume one message credit");
            auto began=GetTickCount64();check(op(123,4,1,4096)==0,"exhausted output credits refuse another message");
            check(GetTickCount64()-began<2500,"output stall wait is finite");
            for(unsigned n=0;n<64;++n)check(receive(output.h,from_credit.h).size()==(n?1u:4000u),"drain exact bounded output queue");
            check(op(123,4,1,4096)==1,"output resumes after credit return");receive(output.h,from_credit.h);
            // More bytes than the 10K ring fit across kernel/ring pressure;
            // no truncation or duplication is allowed at the 256-byte edge.
            std::vector<BYTE> expected(256*140),actual;std::size_t at=0;auto until=GetTickCount64()+8000;
            for(std::size_t n=0;n<expected.size();++n)expected[n]=BYTE(n*31+session.front());
            while((at<expected.size()||actual.size()<expected.size())&&GetTickCount64()<until){
                if(at<expected.size()&&WaitForSingleObject(to_credit.h,0)==WAIT_OBJECT_0){DWORD sent{};
                    check(WriteFile(input.h,expected.data()+at,256,&sent,nullptr)&&sent==256,"burst input exact message");at+=256;}
                if(op(123,7)>0){auto n=op(123,2,127,4096);check(n<=127,"bounded burst read");actual.insert(actual.end(),TestBuffer()+4096,TestBuffer()+4096+n);}else Sleep(1);
            }
            check(actual==expected,"lossless bidirectional pressure across ring boundary");
        }
        check(op(123,12)==1,"private carrier initially present");
        SetEvent(hungup.h);SetEvent(carrier.h);check(op(123,12)==0,"private hungup changes only this session carrier");
        check(op(123,18)==0&&WaitForSingleObject(hangup.h,0)==WAIT_OBJECT_0,"private DTR hangup reaches owned host event");
        check(op(123,1)==0,"bounded joined thread cleanup succeeds");
        Handle stale{CreateFileA(slot(token,123,"to_dos").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)};
        check(stale.h==INVALID_HANDLE_VALUE,"closed driver endpoint cannot receive late bytes");
        check(op(123,2,1,4096)!=1,"closed session rejects late dispatch");
    }
    DWORD handles_after{};GetProcessHandleCount(GetCurrentProcess(),&handles_after);
    check(handles_after==handles_before,"repeated driver sessions retain no Win32 handles");
    FreeLibrary(module);
    std::cout<<"PASS "<<checks<<" actual VDD dispatcher/Win32 IPC fixture checks; native DOS/coexistence PENDING\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" error="<<GetLastError()<<"\n";return 1;}}
