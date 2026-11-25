// Source-only component work under the independently reverified direct owner GO.
// See evidence/section11-sfxexec-components-20261024/BRIDGE_SCOPE_REVIEW.txt.
// No automatic installation or owner test execution. This independent SFX-owned
// bridge links no GPL driver code and never opens the BBS database.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <bcrypt.h>
#include <sddl.h>
#include <filesystem>
#include <memory>
#include <set>
#include <array>
#include <ctime>
#include <iostream>
#include <atomic>
#include "bridge_model.hpp"
#include "pair_hashes.hpp"
#include "pe_exports.hpp"
namespace fs=std::filesystem;
using namespace sfxdos;
struct Handle{HANDLE h{INVALID_HANDLE_VALUE};Handle()=default;explicit Handle(HANDLE value):h(value){}
    ~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);}Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;};
struct Socket{SOCKET s{INVALID_SOCKET};~Socket(){if(s!=INVALID_SOCKET)closesocket(s);}};
static std::atomic_bool stopping{};
static BOOL WINAPI stop_console(DWORD value){if(value==CTRL_C_EVENT||value==CTRL_BREAK_EVENT||value==CTRL_CLOSE_EVENT){stopping=true;return TRUE;}return FALSE;}
static std::string read_file(const fs::path& path,std::size_t bound,bool tail=false){
    Handle f(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    require(f.h!=INVALID_HANDLE_VALUE,"required file missing/inaccessible");BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    require(GetFileInformationByHandle(f.h,&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&GetFileSizeEx(f.h,&size)&&size.QuadPart>=0,"redirected/invalid file refused");
    auto count=static_cast<std::uint64_t>(size.QuadPart);bool cut=tail&&count>bound;
    if(cut){LARGE_INTEGER offset{};offset.QuadPart=size.QuadPart-bound;require(SetFilePointerEx(f.h,offset,nullptr,FILE_BEGIN),"log tail seek failed");count=bound;}
    require(count<=bound,"file exceeds candidate bound");std::string data(static_cast<std::size_t>(count),'\0');DWORD done{};
    require(data.empty()||(ReadFile(f.h,data.data(),static_cast<DWORD>(data.size()),&done,nullptr)&&done==data.size()),"bounded file read failed");
    if(cut){auto newline=data.find('\n');require(newline!=data.npos,"no complete log tail records");data.erase(0,newline+1);}return data;}
static std::string digest(const fs::path& path){auto data=read_file(path,16*1024*1024);BCRYPT_ALG_HANDLE algorithm{};BCRYPT_HASH_HANDLE hash{};
    require(BCryptOpenAlgorithmProvider(&algorithm,BCRYPT_SHA256_ALGORITHM,nullptr,0)==0,"SHA256 unavailable");DWORD length{},got{};
    require(BCryptGetProperty(algorithm,BCRYPT_OBJECT_LENGTH,reinterpret_cast<PUCHAR>(&length),sizeof(length),&got,0)==0,"SHA256 state unavailable");
    std::vector<BYTE> object(length);std::array<BYTE,32> value{};
    bool ok=BCryptCreateHash(algorithm,&hash,object.data(),length,nullptr,0,0)==0&&BCryptHashData(hash,reinterpret_cast<PUCHAR>(data.data()),static_cast<ULONG>(data.size()),0)==0&&BCryptFinishHash(hash,value.data(),32,0)==0;
    if(hash)BCryptDestroyHash(hash);BCryptCloseAlgorithmProvider(algorithm,0);require(ok,"SHA256 failed");std::string out;
    for(auto b:value){out+="0123456789ABCDEF"[b>>4];out+="0123456789ABCDEF"[b&15];}return out;}
static fs::path safe_path(const fs::path& path){auto result=fs::absolute(path).lexically_normal();require(result.has_root_name()&&result.native().rfind(L"\\\\",0)!=0,"local absolute path required");
    fs::path at;for(auto& part:result){at/=part;auto flags=GetFileAttributesW(at.c_str());if(flags!=INVALID_FILE_ATTRIBUTES)require(!(flags&FILE_ATTRIBUTE_REPARSE_POINT),"redirected candidate path refused");}return result;}
#include "system_module.hpp"
struct PairPins{Handle driver,launcher;fs::path driver_file;
    PairPins(const fs::path& base,const fs::path& selected):driver_file(safe_path(selected)){
    const fs::path paths[]={driver_file,safe_path(base/L"BIN"/L"SFXDOOR.EXE")};const char* expected[]={SFX_DLL_SHA256,SFX_LAUNCHER_SHA256};Handle* pins[]={&driver,&launcher};
    for(unsigned n=0;n<2;++n){pins[n]->h=CreateFileW(paths[n].c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
        require(pins[n]->h!=INVALID_HANDLE_VALUE,"required System32 SFX driver or matched launcher missing/inaccessible");
        same_pinned_file(paths[n],pins[n]->h,expected[n]);}
    require(GetFileAttributesW((base/L"BIN"/L"SFXEXEC.DLL").c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"old adjacent BIN driver present/inaccessible; preserve old BIN and use the new two-file BIN");
    std::cout<<"SFXLOAD expected System32 driver="<<driver_file.string()<<" sha256="<<SFX_DLL_SHA256<<"; registration name=SFXEXEC.DLL; pre-game identity gate required\n"<<std::flush;
}};
static fs::path private_work(const fs::path& base,const std::string& selected){auto path=fs::path(selected);auto work=safe_path(path.is_absolute()?path:base/path),games=safe_path(base/L"GAMES");auto prefix=games.wstring()+L"\\";
    require(work.wstring().size()>prefix.size()&&_wcsnicmp(work.c_str(),prefix.c_str(),prefix.size())==0,"use independent game copy beneath candidate GAMES");return work;}
static std::string dospath(const fs::path& path){std::array<wchar_t,512> out{};auto n=GetShortPathNameW(path.c_str(),out.data(),static_cast<DWORD>(out.size()));require(n&&n<out.size(),"DOS short path unavailable");std::string result;
    for(wchar_t c:std::wstring(out.data(),n)){require(c>=32&&c<127&&c!=' ',"candidate needs ASCII DOS short paths");result+=static_cast<char>(c);}return result;}
struct Acl{PSECURITY_DESCRIPTOR descriptor{};SECURITY_ATTRIBUTES attributes{};std::string sid;
    Acl(){Handle token;require(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token.h),"operator token unavailable");DWORD size{};GetTokenInformation(token.h,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
        require(GetTokenInformation(token.h,TokenUser,bytes.data(),size,&size),"operator identity unavailable");char* value{};require(ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&value),"operator SID unavailable");sid=value;LocalFree(value);
        auto sddl="D:P(A;;GA;;;SY)(A;;GA;;;"+sid+")";require(ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr),"private ACL failed");attributes={sizeof(attributes),descriptor,FALSE};}
    ~Acl(){if(descriptor)LocalFree(descriptor);}};
struct OwnedFiles{Acl& acl;std::vector<fs::path> paths;explicit OwnedFiles(Acl& value):acl(value){}
    ~OwnedFiles(){for(auto& p:paths)DeleteFileW(p.c_str());}
    void create(const fs::path& p,const std::string& data){Handle f(CreateFileW(p.c_str(),GENERIC_WRITE,0,&acl.attributes,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));require(f.h!=INVALID_HANDLE_VALUE,"session file already exists; preserve/review before retry");paths.push_back(p);DWORD done{};
        require(WriteFile(f.h,data.data(),static_cast<DWORD>(data.size()),&done,nullptr)&&done==data.size()&&FlushFileBuffers(f.h),"session file write failed");}};
static std::string session_id(){std::array<BYTE,16> bytes{};require(BCryptGenRandom(nullptr,bytes.data(),16,BCRYPT_USE_SYSTEM_PREFERRED_RNG)==0,"session RNG failed");std::string out;
    for(auto b:bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;}
static std::vector<Context> contexts(const fs::path& root){auto data=read_file(root/L"Logs"/L"SFXRuntime.log",256*1024,true);std::vector<Context> out;
    for(auto& line:split(data,'\n')){Context c;if(parse_context(line,c))out.push_back(c);}return out;}
static bool same_owner(HANDLE process,const std::string& owner){Handle token;DWORD size{};if(!OpenProcessToken(process,TOKEN_QUERY,&token.h))return false;
    GetTokenInformation(token.h,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);if(!GetTokenInformation(token.h,TokenUser,bytes.data(),size,&size))return false;
    char* sid{};if(!ConvertSidToStringSidA(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&sid))return false;bool result=owner==sid;LocalFree(sid);return result;}
static DWORD runtime_peer(SOCKET socket,const fs::path& root,const std::string& owner){sockaddr_in peer{},local{};int size=sizeof(peer);
    require(getpeername(socket,reinterpret_cast<sockaddr*>(&peer),&size)==0&&peer.sin_addr.s_addr==htonl(INADDR_LOOPBACK),"non-loopback peer refused");size=sizeof(local);
    require(getsockname(socket,reinterpret_cast<sockaddr*>(&local),&size)==0,"socket identity unavailable");DWORD bytes{};GetExtendedTcpTable(nullptr,&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0);std::vector<BYTE> table(bytes);
    require(GetExtendedTcpTable(table.data(),&bytes,FALSE,AF_INET,TCP_TABLE_OWNER_PID_ALL,0)==NO_ERROR,"TCP owner mapping unavailable");DWORD pid{};auto rows=reinterpret_cast<MIB_TCPTABLE_OWNER_PID*>(table.data());
    for(DWORD i=0;i<rows->dwNumEntries;++i){const auto& r=rows->table[i];if(r.dwLocalAddr==peer.sin_addr.s_addr&&r.dwRemoteAddr==local.sin_addr.s_addr&&r.dwLocalPort==peer.sin_port&&r.dwRemotePort==local.sin_port)pid=r.dwOwningPid;}
    require(pid,"Runtime peer process not found");Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,pid));require(process.h&&process.h!=INVALID_HANDLE_VALUE&&same_owner(process.h,owner),"Runtime peer owner mismatch");
    std::array<wchar_t,512> image{};DWORD count=static_cast<DWORD>(image.size());require(QueryFullProcessImageNameW(process.h,0,image.data(),&count),"Runtime image unavailable");auto expected=safe_path(root/L"Bin"/L"SFXRuntime.exe");
    require(_wcsicmp(safe_path(fs::path(image.data())).c_str(),expected.c_str())==0&&digest(expected)==SFX_RUNTIME_SHA256,"peer is not the accepted installed Runtime");return pid;}
static std::string field(SOCKET socket,std::size_t bound){std::string value;auto until=GetTickCount64()+5000;while(!stopping&&GetTickCount64()<until){char c{};auto n=recv(socket,&c,1,0);
    if(n==1){if(!c)return value;require(value.size()<bound&&static_cast<unsigned char>(c)>=32&&static_cast<unsigned char>(c)<127,"invalid RLogin field");value+=c;}
    else if(n==0)break;else if(WSAGetLastError()!=WSAEWOULDBLOCK)break;Sleep(2);}throw std::runtime_error("RLogin handshake timeout/close");}
static void send_all(SOCKET socket,const std::string& data){std::size_t at=0;auto until=GetTickCount64()+3000;while(at<data.size()&&!stopping&&GetTickCount64()<until){auto n=send(socket,data.data()+at,static_cast<int>(data.size()-at),0);
    if(n>0)at+=n;else if(n==SOCKET_ERROR&&WSAGetLastError()!=WSAEWOULDBLOCK)break;else Sleep(2);}require(at==data.size(),"caller output blocked/closed");}
static std::string message(HANDLE pipe){std::array<char,4000> bytes{};DWORD n{};require(ReadFile(pipe,bytes.data(),static_cast<DWORD>(bytes.size()),&n,nullptr),"driver message read failed");return {bytes.data(),n};}
static std::unique_ptr<Handle> named_event(Acl& acl,const std::string& session,unsigned node,const char* kind,bool manual){auto h=std::make_unique<Handle>(CreateEventA(&acl.attributes,manual,FALSE,name(session,node,kind).c_str()));require(h->h&&GetLastError()!=ERROR_ALREADY_EXISTS,"private event must be new");return h;}
static void driver_path(const std::string& report,HANDLE expected){auto values=split(report,'|');require(values.size()==5&&values[0]=="SFXPAIR1"&&values[1]=="SFXEXEC-20261024-01","driver pair report mismatch");
    auto actual_path=safe_path(fs::path(values[4]));same_pinned_file(actual_path,expected,SFX_DLL_SHA256);
    std::cout<<"DRIVER path="<<actual_path.string()<<" sha256="<<SFX_DLL_SHA256<<"\n"<<std::flush;}
static std::string startup_approval(const std::string& report,const std::string& session,unsigned node,DWORD pid,HANDLE pinned){
    auto values=split(report,'|');require(values.size()==5&&values[2]==session&&values[3]==std::to_string(node),"startup session/node mismatch");
    driver_path(report,pinned);actual_vdm_module(pid,pinned);
    return "SFXSTART2|"+session+"|"+std::to_string(node);
}
static void startup_acknowledged(HANDLE output,HANDLE credit,HANDLE process,const std::string& expected){
    std::string ack;auto until=GetTickCount64()+3000;
    while(GetTickCount64()<until&&!stopping){DWORD next{},count{};
        require(GetMailslotInfo(output,nullptr,&next,&count,nullptr),"startup acknowledgment channel failed");
        // The game can exit immediately after writing its acknowledgment.
        // Consume an already queued message even when the owned process exited.
        if(count){ack=message(output);require(ReleaseSemaphore(credit,1,nullptr),"startup acknowledgment credit failed");break;}
        if(WaitForSingleObject(process,0)!=WAIT_TIMEOUT)break;Sleep(10);}
    require(ack==expected,"matched launcher startup acknowledgment missing/mismatched");
}
struct Child{Handle process,thread,job;bool owned{},assigned{};~Child(){if(owned){if(assigned)TerminateJobObject(job.h,0x535846);else if(process.h!=INVALID_HANDLE_VALUE)TerminateProcess(process.h,0x535846);}}};
static void serve(SOCKET client,const fs::path& base,const fs::path& root,const std::map<std::string,std::string>& cfg,Acl& acl,std::set<std::string>& claimed,const PairPins& pins){
    const auto get=[&](const char* key)->const std::string&{auto i=cfg.find(key);require(i!=cfg.end()&&text(i->second,240),"missing profile field");return i->second;};
    require(field(client,0).empty(),"invalid RLogin initial byte");auto secret=field(client,127);auto alias=field(client,127);auto terminal=field(client,127);
    bool allowed=secret==alias&&alias==get("caller_alias")&&terminal=="sfxdos/"+get("profile");SecureZeroMemory(secret.data(),secret.size());require(allowed,"test identity/profile mismatch; use no password flags");
    const auto runtime=runtime_peer(client,root,acl.sid);Context current;bool found=false;auto until=GetTickCount64()+3000;
    while(!found&&GetTickCount64()<until){auto entries=contexts(root);auto now=static_cast<std::uint64_t>(std::time(nullptr));auto pending=eligible(entries,alias,get("door_name"),claimed,now);
        require(pending.size()<=1,"ambiguous caller/node; stop duplicate test logins");if(pending.size()==1){current=pending[0];found=true;}else Sleep(20);}
    require(found,"fresh typed Runtime Door entry unavailable; check test name/file logging");claimed.insert(current.key());auto session=session_id();
    require(claimed.size()<=4096,"candidate session limit reached; restart this test host");
    auto work=private_work(base,get("work_directory"));
    require(GetFileAttributesW((work/L"SFXEXEC.DLL").c_str())==INVALID_FILE_ATTRIBUTES&&GetLastError()==ERROR_FILE_NOT_FOUND,"game-directory SFX driver present/inaccessible; expected only System32 driver");
    auto command=get("command"),program=command.substr(0,command.find(' '));require(program.find_first_of("\\/:\"")==program.npos&&program.size()>4&&_stricmp(program.c_str()+program.size()-4,".EXE")==0,"direct private DOS EXE required");
    Handle game(CreateFileW((work/fs::path(program)).c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));require(game.h!=INVALID_HANDLE_VALUE,"private game executable missing");
    BY_HANDLE_FILE_INFORMATION game_info{};require(GetFileInformationByHandle(game.h,&game_info)&&!(game_info.dwFileAttributes&(FILE_ATTRIBUTE_REPARSE_POINT|FILE_ATTRIBUTE_DIRECTORY))&&game_info.nNumberOfLinks==1,"private game must be an ordinary independent file");
    auto parent=base/L"NODE";if(!fs::exists(parent))require(CreateDirectoryW(parent.c_str(),&acl.attributes),"candidate Node creation failed");auto node=safe_path(parent/fs::path(std::to_string(current.node)));
    if(!fs::exists(node))require(CreateDirectoryW(node.c_str(),&acl.attributes),"candidate node creation failed");
    node/=fs::path(session.substr(0,8));require(CreateDirectoryW(node.c_str(),&acl.attributes),"fresh candidate session directory required; existing results retained");auto nodepath=dospath(node)+"\\";
    for(auto suffix:{L"SFXRUN.RET",L"SFXRUN.ERR"})require(GetFileAttributesW((node/suffix).c_str())==INVALID_FILE_ATTRIBUTES,"prior launcher result retained; review/move it before retrying");
    for(std::size_t pos=0;(pos=command.find("{NODEDIR}",pos))!=command.npos;pos+=nodepath.size())command.replace(pos,9,nodepath);
    require(command.size()<=120&&command.find_first_of("\r\n\"")==command.npos,"DOS game command exceeds candidate bound");OwnedFiles files(acl);
    SYSTEMTIME local{};GetLocalTime(&local);char date[16],clock[16];sprintf_s(date,"%02u/%02u/%02u",local.wMonth,local.wDay,local.wYear%100);sprintf_s(clock,"%02u:%02u",local.wHour,local.wMinute);
    Drop drop{current,unsigned(number(get("security_level"),999)),unsigned(number(get("minutes"),60)),nodepath,get("sysop_name"),get("location"),date,clock,unsigned(local.wHour*3600+local.wMinute*60+local.wSecond)};
    auto format=get("dropfile");require(format=="doorsys"||format=="sfdoors","unknown drop-file format");files.create(format=="doorsys"?node/L"DOOR.SYS":work/L"SFDOORS.DAT",format=="doorsys"?doorsys(drop):sfdoors(drop));
    std::cout<<"SFXLOAD fresh dropfile prepared format="<<format<<" path="<<(format=="doorsys"?node/L"DOOR.SYS":work/L"SFDOORS.DAT").string()<<"\n"<<std::flush;
    auto env=node/L"SFXRUN.ENV",ini=node/L"SFXRUN.INI";files.create(env,command+"\r\n");files.create(ini,"CanDisconnect=false\r\n[UART]\r\nComPort=1\r\n");
    Handle output(CreateMailslotA(name(session,current.node,"from_dos").c_str(),4000,0,&acl.attributes));require(output.h!=INVALID_HANDLE_VALUE,"private output channel failed");
    auto hungup=named_event(acl,session,current.node,"hungup",true),hangup=named_event(acl,session,current.node,"hangup",true),carrier=named_event(acl,session,current.node,"carrier",false);
    Handle from_credit(CreateSemaphoreA(&acl.attributes,64,64,name(session,current.node,"from_credit").c_str()));require(from_credit.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh output credits required");
    Handle to_credit(CreateSemaphoreA(&acl.attributes,64,64,name(session,current.node,"to_credit").c_str()));require(to_credit.h&&GetLastError()!=ERROR_ALREADY_EXISTS,"fresh input credits required");
    auto mode=get("mode");require(mode=="fossil"||mode=="uart","select fossil or uart");auto tail=dospath(env)+" NT "+std::to_string(current.node)+(mode=="fossil"?" 8 ":" 4 ")+dospath(ini)+" "+session;
    require(tail.size()<=125,"DOS tail exceeds125 bytes; shorten candidate path");auto launcher=base/L"BIN"/L"SFXDOOR.EXE";auto line="\""+dospath(launcher)+"\" "+tail;std::vector<char> mutable_line(line.begin(),line.end());mutable_line.push_back(0);
    Child child;child.job.h=CreateJobObjectW(&acl.attributes,nullptr);require(child.job.h,"private job creation failed");JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(SetInformationJobObject(child.job.h,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"private job limits failed");FILETIME start{};GetSystemTimeAsFileTime(&start);STARTUPINFOA si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
    require(CreateProcessA(dospath(launcher).c_str(),mutable_line.data(),nullptr,nullptr,FALSE,CREATE_NEW_CONSOLE|CREATE_SEPARATE_WOW_VDM|CREATE_SUSPENDED,nullptr,dospath(work).c_str(),&si,&pi),"native DOS launch failed; requires Windows x86 NTVDM");
    child.process.h=pi.hProcess;child.thread.h=pi.hThread;FILETIME born{},exit{},kernel{},user{};
    require(GetProcessTimes(child.process.h,&born,&exit,&kernel,&user)&&CompareFileTime(&born,&start)>=0,"shared/pre-existing VDM refused; do not terminate production processes");
    child.owned=true;require(AssignProcessToJobObject(child.job.h,child.process.h),"new private VDM job assignment failed");child.assigned=true;require(ResumeThread(child.thread.h)!=DWORD(-1),"private VDM resume failed");
    std::array<wchar_t,512> vdm_image{};DWORD image_size=static_cast<DWORD>(vdm_image.size());
    if(QueryFullProcessImageNameW(child.process.h,0,vdm_image.data(),&image_size)){
        try{auto path=safe_path(fs::path(vdm_image.data()));std::cout<<"SFXLOAD native process pid="<<pi.dwProcessId<<" image="<<path.string()<<" sha256="<<digest(path)<<" owner_matches_host="<<same_owner(child.process.h,acl.sid)<<"\n";
            auto exports=pe32_exports(read_file(path,16*1024*1024));std::cout<<"SFXLOAD read-only PE export check; missing core NTVDM imports:";
            for(auto api:{"VDDDeInstallIOHook","setAX","getES","getDI","getCX","getBH","getBL","getAX","MGetVdmPointer","call_ica_hw_interrupt","VDDInstallIOHook"})if(!exports.count(api))std::cout<<" "<<api;
            std::cout<<" [end]; metadata only, not registration proof\n"<<std::flush;
        }catch(const std::exception& e){std::cout<<"SFXLOAD process image metadata unavailable: "<<e.what()<<"\n"<<std::flush;}}
    auto opened=GetTickCount64()+8000;std::string hello;
    while(GetTickCount64()<opened&&WaitForSingleObject(child.process.h,0)==WAIT_TIMEOUT&&!stopping){DWORD next{},count{};require(GetMailslotInfo(output.h,nullptr,&next,&count,nullptr),"output status failed");if(count){hello=message(output.h);require(ReleaseSemaphore(from_credit.h,1,nullptr),"handshake credit failed");break;}Sleep(10);}
    auto values=split(hello,'|');if(values.size()!=5||values[2]!=session||values[3]!=std::to_string(current.node)){DWORD exit_code{};GetExitCodeProcess(child.process.h,&exit_code);std::cout<<"SFXLOAD report gate failed; VDM process exit/status="<<exit_code<<"; see preceding launcher stage (BOP/pair/open)\n"<<std::flush;throw std::runtime_error("private driver startup report missing/mismatched");}
    require(same_owner(child.process.h,acl.sid),"owned VDM identity mismatch");
    const auto approval=startup_approval(hello,session,current.node,pi.dwProcessId,pins.driver.h);
    Handle input(CreateFileA(name(session,current.node,"to_dos").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));require(input.h!=INVALID_HANDLE_VALUE,"private VDD input missing");
    DWORD approval_sent{};
    require(WaitForSingleObject(to_credit.h,0)==WAIT_OBJECT_0,"startup input credit unavailable");
    require(WriteFile(input.h,approval.data(),static_cast<DWORD>(approval.size()),&approval_sent,nullptr)&&approval_sent==approval.size(),"private startup approval failed");
    std::cout<<"SFXLOAD System32 file/module/pair VERIFIED; private startup approval sent\n"<<std::flush;
    startup_acknowledged(output.h,from_credit.h,child.process.h,"SFXSTARTED2|"+session+"|"+std::to_string(current.node));
    send_all(client,std::string(1,'\0'));
    std::cout<<"ACTIVE node="<<current.node<<" account="<<current.account<<" generation="<<current.generation<<" runtime_pid="<<runtime<<" vdm_pid="<<pi.dwProcessId<<" session="<<session<<"\n"<<std::flush;
    auto deadline=GetTickCount64()+drop.minutes*60000ull;std::string pending;bool live=true;
    while(live&&!stopping&&GetTickCount64()<deadline&&WaitForSingleObject(child.process.h,0)==WAIT_TIMEOUT){
        char probe{};auto peek=recv(client,&probe,1,MSG_PEEK);
        if(peek==0||(peek==SOCKET_ERROR&&WSAGetLastError()!=WSAEWOULDBLOCK)){live=false;break;}
        if(!pending.empty()){auto n=send(client,pending.data(),static_cast<int>(pending.size()),0);if(n>0)pending.erase(0,n);else if(WSAGetLastError()!=WSAEWOULDBLOCK)live=false;}
        for(unsigned batch=0;batch<64;++batch){DWORD next{},count{};require(GetMailslotInfo(output.h,nullptr,&next,&count,nullptr)&&count<=64&&(next==MAILSLOT_NO_MESSAGE||next<=4000),"driver output exceeds bounded protocol");
            if(!count||pending.size()+next>32768)break;
            pending+=message(output.h);require(ReleaseSemaphore(from_credit.h,1,nullptr),"output credit mismatch");}
        if(WaitForSingleObject(to_credit.h,0)==WAIT_OBJECT_0){std::array<char,256> bytes{};auto n=recv(client,bytes.data(),static_cast<int>(bytes.size()),0);
            if(n>0){DWORD sent{};require(WriteFile(input.h,bytes.data(),n,&sent,nullptr)&&sent==DWORD(n),"private caller input failed");}
            else{ReleaseSemaphore(to_credit.h,1,nullptr);if(n==0||(n==SOCKET_ERROR&&WSAGetLastError()!=WSAEWOULDBLOCK))live=false;}}
        if(WaitForSingleObject(hangup->h,0)==WAIT_OBJECT_0)live=false;Sleep(2);
    }
    // A normal game exit may race its final output message. Drain only that
    // owned queue, with a finite deadline; cancellation never waits for it.
    if(live&&!stopping&&WaitForSingleObject(child.process.h,0)==WAIT_OBJECT_0){auto drain=GetTickCount64()+1000;
        while(GetTickCount64()<drain){DWORD next{},count{};require(GetMailslotInfo(output.h,nullptr,&next,&count,nullptr)&&count<=64&&(next==MAILSLOT_NO_MESSAGE||next<=4000),"invalid final driver output");
            if(!count)break;pending+=message(output.h);require(ReleaseSemaphore(from_credit.h,1,nullptr),"final output credit failed");
            if(!pending.empty()){send_all(client,pending);pending.clear();}}
        if(!pending.empty())send_all(client,pending);}
    SetEvent(hungup->h);SetEvent(carrier->h);shutdown(client,SD_BOTH);if(WaitForSingleObject(child.process.h,2000)!=WAIT_OBJECT_0)TerminateJobObject(child.job.h,0x535846);
    require(WaitForSingleObject(child.process.h,3000)==WAIT_OBJECT_0,"owned VDM did not stop within bound");
    auto returned=node/L"SFXRUN.RET";
    if(GetFileAttributesW(returned.c_str())!=INVALID_FILE_ATTRIBUTES)std::cout<<"GAME RESULT "<<read_file(returned,512)<<"; retained at "<<returned.string()<<"\n"<<std::flush;
    if(GetFileAttributesW((node/L"SFXRUN.ERR").c_str())!=INVALID_FILE_ATTRIBUTES)std::cerr<<"Launcher error retained in "<<node.string()<<"\n"<<std::flush;
    std::cout<<"RETURN node="<<current.node<<" session="<<session<<"\n"<<std::flush;
}
#if defined(SFX_NORMAL_HOST)
#include "normal_host.hpp"
#else
int main(int argc,char** argv){try{
    require(argc==3&&std::string(argv[1])=="--profile","usage: SFXDOSHost --profile <private profile.ini>");BOOL wow{};require(IsWow64Process(GetCurrentProcess(),&wow)&&!wow,"native DOS candidate requires 32-bit Windows; x64 unsupported");
    std::array<wchar_t,512> image{};require(GetModuleFileNameW(nullptr,image.data(),static_cast<DWORD>(image.size())),"bridge path unavailable");auto base=safe_path(fs::path(image.data()).parent_path().parent_path());
    PairPins pins(base,system_driver());auto cfg=config(read_file(safe_path(fs::path(argv[2])),8192));
    auto get=[&](const char* key)->const std::string&{auto i=cfg.find(key);require(i!=cfg.end()&&text(i->second,240),"profile incomplete");return i->second;};auto root=safe_path(fs::path(get("sfx_root")));auto port=unsigned(number(get("listen_port"),65535));
    require(port>=1024&&get("caller_alias")!="CHANGE_ME","fill designated test profile first");std::set<std::string> claimed;for(auto& c:contexts(root))claimed.insert(c.key());Acl acl;WSADATA wsa{};require(WSAStartup(MAKEWORD(2,2),&wsa)==0,"Winsock startup failed");SetConsoleCtrlHandler(stop_console,TRUE);
    Socket listener;listener.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);require(listener.s!=INVALID_SOCKET,"listener socket failed");BOOL exclusive=TRUE;require(setsockopt(listener.s,SOL_SOCKET,SO_EXCLUSIVEADDRUSE,reinterpret_cast<char*>(&exclusive),sizeof(exclusive))==0,"exclusive loopback port unavailable");
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);address.sin_port=htons(static_cast<u_short>(port));require(bind(listener.s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener.s,1)==0,"loopback bind/listen failed");u_long nonblocking=1;ioctlsocket(listener.s,FIONBIO,&nonblocking);
    std::cout<<"READY rlogin 127.0.0.1:"<<port<<" -q -P -t sfxdos/"<<get("profile")<<"\n"<<std::flush;
    while(!stopping){Socket client;client.s=accept(listener.s,nullptr,nullptr);if(client.s==INVALID_SOCKET){require(WSAGetLastError()==WSAEWOULDBLOCK,"accept failed");Sleep(20);continue;}ioctlsocket(client.s,FIONBIO,&nonblocking);
        BOOL nodelay=TRUE;require(setsockopt(client.s,IPPROTO_TCP,TCP_NODELAY,reinterpret_cast<char*>(&nodelay),sizeof(nodelay))==0,"candidate loopback nodelay failed");
        try{serve(client.s,base,root,cfg,acl,claimed,pins);}catch(const std::exception& e){std::cerr<<"SESSION STOP: "<<e.what()<<"\n"<<std::flush;shutdown(client.s,SD_BOTH);}}
    return 0;
}catch(const std::exception& e){std::cerr<<"SFX DOS CANDIDATE STOP: "<<e.what()<<" (Windows error "<<GetLastError()<<")\n";return 1;}}
#endif
