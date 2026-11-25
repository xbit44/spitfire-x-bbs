// Actual diagnostic DLL with fake NTVDM register/memory services. Not native DOS.
#include <windows.h>
#include <iostream>
#include <string>
#include <cstring>
#include "sfx_load.h"
extern "C" __declspec(dllimport) void __stdcall TestRegisters(USHORT,BYTE,BYTE,USHORT,USHORT);
extern "C" __declspec(dllimport) BYTE* __stdcall TestBuffer(void);
extern "C" __declspec(dllimport) USHORT __stdcall TestResult(void);
static unsigned count;
static void check(bool b,const char* s){++count;if(!b)throw std::runtime_error(s);}
int main(int argc,char** argv){try{
    check(argc==4,"probe, target, expected kind");auto module=LoadLibraryExA(argv[1],nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(module!=nullptr,"load exact diagnostic DLL");
    auto call=reinterpret_cast<void(__cdecl*)()>(GetProcAddress(module,"VDDDispatch"));check(call!=nullptr,"cdecl diagnostic dispatch");
    auto op=[&](BYTE code,USHORT size){TestRegisters(1,123,code,size,0);call();return TestResult();};
    check(op(21,0)==SFX_LOAD_PAIR,"diagnostic pair identity");check(op(99,0)==0xf002,"unknown diagnostic opcode refused");
    check(op(0,1)==0xf003,"short diagnostic buffer refused");auto request=reinterpret_cast<sfx_load_request*>(TestBuffer());
    std::memset(request,'a',sizeof(*request));check(op(0,sizeof(*request))==0xf004,"nonterminated path refused");
    std::memset(request,0,sizeof(*request));std::strcpy(request->path,"SFXEXEC.DLL");check(op(0,sizeof(*request))==0xf004,"bare search refused");
    std::strcpy(request->path,"C:\\outside\\SFXEXEC.DLL");check(op(0,sizeof(*request))==0xf005,"outside private probe directory refused");
    check(std::strlen(argv[2])<SFX_LOAD_PATH,"fixture target fits DOS path bound");std::strcpy(request->path,argv[2]);
    check(op(0,sizeof(*request))==0,"bounded diagnostic completed");std::string report=request->report;std::cout<<report;
    const std::string expected=argv[3];
    if(expected=="good")check(report.find("loaded=1 win32_error=0")!=report.npos&&report.find("VDDDispatch=1 VDDInitialize=1")!=report.npos,"real candidate mapped in ordinary fixture only");
    else if(expected=="missing")check(report.find("loaded=0 win32_error=126 file_attributes_error=2")!=report.npos,"missing target distinguished from BOP code");
    else if(expected=="bits")check(report.find("loaded=0 win32_error=193")!=report.npos,"wrong bitness records real Win32 error193");
    else if(expected=="api")check(report.find("loaded=0 win32_error=127")!=report.npos&&report.find("missing: VDDInstallIOHook")!=report.npos,"missing actual export reports error127 and name");
    else if(expected=="dependency")check(report.find("loaded=0 win32_error=126 file_attributes_error=0")!=report.npos,"present file with missing dependency distinguished");
    else throw std::runtime_error("unknown fixture kind");
    check(report.find("diagnostic only; failed BOP registration remains failed; game not started")!=report.npos,"no diagnostic success promoted to game/registration pass");
    check(FreeLibrary(module)!=FALSE,"diagnostic unload");std::cout<<"PASS "<<count<<" probe fixture checks; native DOS PENDING\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" win32="<<GetLastError()<<"\n";return 1;}}
