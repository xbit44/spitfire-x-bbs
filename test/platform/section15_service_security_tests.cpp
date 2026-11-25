#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <iostream>
#include <fstream>
namespace p=sfx::platform::win32;namespace ins=p::installation;
namespace {unsigned checks{};void check(bool v,const char* why){++checks;ins::require(v,why);}
void attempt(const ins::Path& image,const std::wstring& argument,DWORD expected){
    auto command=L"\""+image.native()+L"\" "+argument;STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    check(CreateProcessW(image.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,image.parent_path().c_str(),&startup,&process),"only exact installed duplicate attempt launched");
    ins::Handle child(process.hProcess),thread(process.hThread);DWORD code{};
    check(WaitForSingleObject(child.get(),5000)==WAIT_OBJECT_0&&GetExitCodeProcess(child.get(),&code)&&code==expected,"duplicate launch refuses before creating a second Runtime model");
}
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==3&&!ins::elevated(),"ordinary closed service security test");const auto root=ins::canonical_root(argv[1]);
    check(root==ins::canonical_root(L"C:/Program Files/SFX-Service-Fresh-Test-20261105-r2"),"only named fresh service fixture");
    const auto out=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(out),"new security evidence");std::filesystem::create_directories(out);
    const auto mode=ins::read_service_mode(root);check(mode.has_value(),"protected Service Mode selected");
    const auto before=ins::query_owned_service(*mode);check(before.state==SERVICE_RUNNING&&before.process_id,"actual owned service already Running");
    ins::verify_owned_service_peer(*mode,before.process_id);check(true,"ordinary owner attests System service via pinned receipt SCM and exact image");
    DWORD runtime_session{},client_session{};check(ProcessIdToSessionId(before.process_id,&runtime_session)&&ProcessIdToSessionId(GetCurrentProcessId(),&client_session)&&runtime_session==0&&client_session!=0,"actual Session0 service versus interactive owner session");
    p::RuntimeInstanceGuard duplicate;const auto guarded=duplicate.acquire(root,mode->owner);
    check(!guarded&&guarded.operation.find("duplicate")!=std::string::npos,"Global installation guard rejects another interactive-session Runtime");
    attempt(root/L"Bin"/L"SFXRuntime.exe",L"--owner-handle=1",17);
    attempt(root/L"Bin"/L"SFXRuntime.exe",L"--windows-service",16);
    bool wrong=false;try{ins::verify_owned_service_peer(*mode,GetCurrentProcessId());}catch(...){wrong=true;}
    check(wrong,"wrong PID cannot be substituted as service peer");
    const auto names=p::installation_object_names(root);
    {
        auto connection=p::connect_local_control_server(names.pipe_name,before.process_id,5000);
        check(connection.valid(),"same-owner non-ControlPanel probe reaches only bound service endpoint");
        const auto deadline=GetTickCount64()+5000;bool rejected=false,challenge=false;
        while(GetTickCount64()<deadline){DWORD bytes{};
            if(!PeekNamedPipe(connection.native_handle(),nullptr,0,nullptr,&bytes,nullptr)){rejected=true;break;}
            if(bytes){challenge=true;break;}Sleep(10);
        }
        check(rejected&&!challenge,"same-owner arbitrary executable is rejected before receiving authentication challenge");
    }
    check(WaitNamedPipeW(names.pipe_name.c_str(),5000),"service accepts a fresh endpoint after rejecting the arbitrary client");
    ins::Handle process_token,restricted,impersonation;HANDLE raw{};
    check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&raw),"own test token opened");process_token.reset(raw);
    BYTE sid[SECURITY_MAX_SID_SIZE];DWORD size=sizeof(sid);check(CreateWellKnownSid(WinWorldSid,nullptr,sid,&size),"test restricted SID created without a Windows account");SID_AND_ATTRIBUTES restriction{sid,0};
    check(CreateRestrictedToken(process_token.get(),DISABLE_MAX_PRIVILEGE,0,nullptr,0,nullptr,1,&restriction,&raw),"own restricted test token created");restricted.reset(raw);
    check(DuplicateTokenEx(restricted.get(),TOKEN_QUERY|TOKEN_IMPERSONATE,nullptr,SecurityImpersonation,TokenImpersonation,&raw),"restricted impersonation token duplicated");impersonation.reset(raw);
    check(SetThreadToken(nullptr,impersonation.get()),"only current test thread restricted");
    const auto probe=CreateFileW(names.pipe_name.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);const auto error=GetLastError();
    const bool restored=RevertToSelf()!=FALSE;if(probe!=INVALID_HANDLE_VALUE)CloseHandle(probe);
    check(restored&&probe==INVALID_HANDLE_VALUE&&error==ERROR_ACCESS_DENIED,"restricted principal denied by real service pipe ACL; thread restored");
    const auto after=ins::query_owned_service(*mode);check(after.state==SERVICE_RUNNING&&after.process_id==before.process_id,"denied and duplicate attempts leave authoritative service unchanged");
    std::ofstream report(out/L"results.txt");report<<"PASS "<<checks<<" actual installed service authority checks\nSession0 Runtime PID "<<before.process_id<<"\nInteractive session "<<client_session<<'\n';
    std::cout<<"PASS "<<checks<<" actual installed service authority checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}
