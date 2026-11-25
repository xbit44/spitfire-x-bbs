// The generated include is the actual launcher wait function with only its
// 16-bit SS/offset mapping adapted to this ordinary-process NTVDM-import fixture.
#include <windows.h>
#include <string>
#include <iostream>
#include <stdexcept>
#include <cstring>
#include <ctime>
#include "vdd_func.h"
#include "sfx_gate.h"
extern "C" __declspec(dllimport) void __stdcall TestRegisters(USHORT,BYTE,BYTE,USHORT,USHORT);
extern "C" __declspec(dllimport) BYTE* __stdcall TestBuffer(void);
extern "C" __declspec(dllimport) USHORT __stdcall TestResult(void);
static unsigned checks,round_id;static BYTE node_num=1;static unsigned elapsed;static bool fast_clock;
static void(__cdecl* dispatch)();
static void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static int vdd_op(BYTE op){TestRegisters(1,node_num,op,0,0);dispatch();return TestResult();}
static int vdd_buf(BYTE op,int count,WORD,const void* pointer){if(op==VDD_WRITE)std::memcpy(TestBuffer()+4096,pointer,count);
    TestRegisters(1,node_num,op,USHORT(count),4096);dispatch();auto result=TestResult();
    if(op==VDD_READ&&result<=count)std::memcpy(const_cast<void*>(pointer),TestBuffer()+4096,result);return result;}
static time_t gate_time(time_t*){return fast_clock?100+elapsed++:std::time(nullptr);}
#define time gate_time
#include "launcher_gate.inc"
#undef time
struct H{HANDLE h;~H(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}};
static std::string slot(const std::string& s,const char* kind){return "\\\\.\\mailslot\\sfxexec\\v1\\"+s+"\\n1\\"+kind;}
static std::string ev(const std::string& s,const char* kind){return "Local\\SFXEXEC.v1."+s+".n1."+kind;}
static std::string receive(HANDLE file,HANDLE credit){char data[4000];DWORD count{};check(ReadFile(file,data,sizeof(data),&count,nullptr),"fixture output read");check(ReleaseSemaphore(credit,1,nullptr),"output credit returned");return {data,count};}
static void run(const std::string& token,const std::string& input,bool success,bool cancel=false,bool timeout=false,bool exhaust=false){
    std::memcpy(TestBuffer(),token.data(),32);TestRegisters(1,1,20,32,0);dispatch();check(TestResult()==0x6424,"actual core session bind");
    H output{CreateMailslotA(slot(token,"from_dos").c_str(),4000,1000,nullptr)},hungup{CreateEventA(nullptr,TRUE,FALSE,ev(token,"hungup").c_str())},hangup{CreateEventA(nullptr,TRUE,FALSE,ev(token,"hangup").c_str())},carrier{CreateEventA(nullptr,FALSE,FALSE,ev(token,"carrier").c_str())};
    H from{CreateSemaphoreA(nullptr,64,64,ev(token,"from_credit").c_str())},to{CreateSemaphoreA(nullptr,64,64,ev(token,"to_credit").c_str())};
    check(output.h!=INVALID_HANDLE_VALUE&&hungup.h&&hangup.h&&carrier.h&&from.h&&to.h,"fresh private session endpoints");
    if(round_id++%2)check(vdd_op(19)==0,"unchanged UART setup (IRQ mocked)");check(vdd_op(0)==0,"actual core open");
    check(receive(output.h,from.h).find("SFXPAIR1|SFXEXEC-20261024-01|"+token+"|1|")==0,"core identity report first");
    H writer{CreateFileA(slot(token,"to_dos").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr)};check(writer.h!=INVALID_HANDLE_VALUE,"private input channel");
    if(!input.empty()){DWORD sent{};check(WaitForSingleObject(to.h,0)==WAIT_OBJECT_0&&WriteFile(writer.h,input.data(),DWORD(input.size()),&sent,nullptr)&&sent==input.size(),"approval uses ordinary bounded input credit");
        auto until=GetTickCount64()+2000;while(vdd_op(VDD_INBUF_FULL)==0&&GetTickCount64()<until)Sleep(1);}
    if(cancel)SetEvent(hungup.h);if(exhaust)for(int i=0;i<64;++i)check(WaitForSingleObject(from.h,0)==WAIT_OBJECT_0,"exhaust ack output credits");
    fast_clock=timeout;elapsed=0;check((sfx_wait_start(token.c_str())!=0)==success,"actual launcher gate result controls game permission");
    if(success){check(receive(output.h,from.h)=="SFXSTARTED2|"+token+"|1","launcher ack is session/node bound");check(vdd_op(VDD_INBUF_FULL)==0,"startup token consumed; no credential/protocol bytes reach game");}
    else {DWORD count{};check(GetMailslotInfo(output.h,nullptr,nullptr,&count,nullptr)&&count==0,"refusal sends no startup acknowledgment");}
    check(vdd_op(VDD_CLOSE)==0,"actual core joined cleanup after success or refusal");
}
int main(int argc,char** argv){try{check(argc==3,"exact core path and unique session family");auto module=LoadLibraryExA(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(module!=nullptr,"actual core loaded with private VDM shim");dispatch=reinterpret_cast<void(__cdecl*)()>(GetProcAddress(module,"VDDDispatch"));check(dispatch!=nullptr,"actual dispatcher");
    const std::string base=argv[2];for(int i=0;i<9;++i){std::string token=base;token.back()="0123456789abcdef"[i];std::string input="SFXSTART2|"+token+"|1";
        if(i==1)input="SFXSTART2|"+std::string(32,'f')+"|1";if(i==2)input.back()='2';if(i==3)input+='!';if(i==4)input=std::string(65,'x');if(i==5)input.clear();if(i==6)input="SFXSTART2|";
        run(token,input,i==0||i==8,i==7,i==5||i==6,false);}
    std::string token=base;token.back()='a';run(token,"SFXSTART2|"+token+"|1",false,false,false,true);
    check(FreeLibrary(module),"exact fixture core released");std::cout<<"PASS "<<checks<<" actual launcher gate/core IPC checks; native DOS/IRQ/coexistence PENDING\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" error="<<GetLastError()<<"\n";return 1;}}
