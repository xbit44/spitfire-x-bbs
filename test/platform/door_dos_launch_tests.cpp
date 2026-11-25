// Launch the real normal-host source with an independently pinned synthetic
// Runtime image. The x64 platform guard MUST stop every valid request before
// any DOS/System32 work. This qualifies error transport, not native NTVDM.
#include "../../src/apps/runtime/alpha_local_door.cpp"
#include "sfx/application/operational_log.hpp"
#include <fstream>
#include <iostream>
namespace rt=sfx::apps::runtime;
namespace app=sfx::application;
namespace wi=sfx::platform::win32::installation;
namespace dp=sfx::core::dos_host;
namespace {
unsigned checks{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
std::uint64_t number(HANDLE h){return reinterpret_cast<std::uintptr_t>(h);}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh report directory argument");BOOL wow{};
    check(IsWow64Process(GetCurrentProcess(),&wow)&&wow,"must run as x86 on x64; no actual DOS/System32 execution");
    std::array<wchar_t,32768> image{};GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size()));
    const auto root=std::filesystem::path(image.data()).parent_path().parent_path();
    const auto reports=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(reports),"preserve earlier reports");std::filesystem::create_directory(reports);
    const auto owner=wi::operator_sid();app::DoorChannel channel;channel.stop=[]()->std::optional<app::DoorEnd>{return {};};
    for(const auto kind:{"bad_request","bad_parent","redirected","valid"}){
        rt::Pipe request,input,output,error,control,status;
        request.create(owner,false);input.create(owner,false);output.create(owner,true);error.create(owner,true);control.create(owner,false);status.create(owner,true);
        HANDLE copy{};check(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&copy,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,TRUE,0),"parent duplicate");wi::Handle parent(copy);
        FILETIME born{},exit{},kernel{},user{};check(GetProcessTimes(GetCurrentProcess(),&born,&exit,&kernel,&user),"parent identity");
        dp::Request r;r.parent_pid=GetCurrentProcessId();r.parent_birth=(std::uint64_t(born.dwHighDateTime)<<32)|born.dwLowDateTime;
        r.parent_process=number(parent.get());r.account=1;r.node=1;r.generation=1;r.sequence=1;r.mode=4;
        r.input=number(input.child.get());r.output=number(output.child.get());r.error=number(error.child.get());r.control=number(control.child.get());r.status=number(status.child.get());
        r.root=root.u8string();r.startup=r.root;r.node_directory=(root/"Node"/"1").u8string();r.drop_directory=r.startup;r.private_directory=(root/"Node"/"1"/"D1234567").u8string();r.command="SFGUESS.EXE";r.session=std::string(32,'a');
        if(std::string(kind)=="bad_parent")++r.parent_birth;
        auto bytes=dp::encode(r);check(bytes.has_value(),"valid bounded request encoding");if(std::string(kind)=="bad_request")(*bytes)[0]='X';
        const auto text="\""+(root/"Bin"/"SFXDOSHost.exe").u8string()+"\" --runtime "+std::to_string(number(request.child.get()))+" --status "+std::to_string(number(status.child.get()));
        auto command=rt::command(text,root,false);rt::Child child;rt::DosHostDevices devices;const bool redirected=std::string(kind)=="redirected";
        rt::start(child,command,root,rt::environment(root),redirected?input.child.get():devices.input.get(),devices.output.get(),devices.error.get(),{request.child.get(),input.child.get(),output.child.get(),error.child.get(),control.child.get(),status.child.get(),parent.get(),devices.input.get(),devices.output.get(),devices.error.get()});
        request.child.reset();input.child.reset();output.child.reset();error.child.reset();control.child.reset();status.child.reset();parent.reset();
        rt::private_write(request,*bytes,channel);request.close();rt::HostStatus received;
        const auto until=GetTickCount64()+5000;while(!received.finished&&GetTickCount64()<until){received.poll(status);Sleep(2);}
        check(received.finished&&!received.started&&!received.error.empty(),"persistent failure status before host disappears");
        const std::string expected=std::string(kind)=="bad_request"?"door_dos_request_invalid_private_launch_request":std::string(kind)=="bad_parent"?"door_dos_parent_identity_parent_identity_mismatch":redirected?"door_dos_local_devices_dos_host_standard_devices_must_be_local":"door_dos_platform_dos_runtime_unavailable_on_x64_optional_runtime_is_dis";
        check(received.error==expected,"exact failing stage/reason");
        check(WaitForSingleObject(child.process.get(),1000)==WAIT_OBJECT_0,"host exits promptly");DWORD code{};check(GetExitCodeProcess(child.process.get(),&code)&&code==1,"failure process exit recorded");
        check(child.stop(),"owned host reaped");
        app::OperationalEvent event;event.kind=app::OperationalKind::stage_changed;event.context.epoch.bytes[0]=1;event.context.origin_sequence=1;event.context.node=1;event.context.generation=1;event.context.attempt=1;event.context.transport=app::OperationalTransport::telnet;event.context.source_ip="127.0.0.1";event.context.resource_id=1;event.context.resource_name="SF Guess";event.context.diagnostic_code=received.error;
        const auto entry=app::make_operational_entry(1,1792998000,event);const auto encoded=app::encode_operational_record(entry);check(!encoded.empty(),"failure accepted by persistent operational log format");
        const auto decoded=app::decode_operational_record(encoded,1);check(decoded&&decoded->event&&decoded->event->context.diagnostic_code==received.error,"exact diagnostic survives durable log roundtrip");
        std::ofstream(reports/(std::string(kind)+".txt"))<<encoded<<"\nhost_exit="<<code<<'\n';
        std::cout<<"PASS "<<kind<<" "<<received.error<<'\n';
    }
    std::cout<<"PASS normal DOS launch diagnostics: "<<checks<<" checks; native DOS remains untested\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
