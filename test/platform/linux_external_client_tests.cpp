#include "linux_external_client.hpp"
#include <sodium.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fstream>
#include <iostream>

namespace {
namespace web=sfx::apps::runtime::web;
namespace native=sfx::platform::linux_host;
using Json=nlohmann::json;
unsigned checks{};
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
template<class F>void refuses(F action){bool refused{};try{action();}catch(const std::exception&){refused=true;}check(refused,"invalid client input refused");}
std::string read(const std::filesystem::path& path){std::ifstream file(path);return {std::istreambuf_iterator<char>(file),{}};}
void write(const std::filesystem::path& path,const std::string& text,mode_t mode=0700){std::ofstream file(path);file<<text;file.close();check(chmod(path.c_str(),mode)==0,"owned file permissions");}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0&&sodium_init()>=0,"normal user fixture");const std::filesystem::path root(argv[1]);
    check(root.is_absolute()&&!std::filesystem::exists(root),"new fixture");
    check(mkdir(root.c_str(),0700)==0&&mkdir((root/"config").c_str(),0700)==0&&mkdir((root/"management").c_str(),0700)==0,"private fixture directories");
    const auto fresh=web::read_desktop_client(root);check(fresh.revision==1&&!fresh.type,"fresh empty client");
    check(!std::filesystem::exists(root/"config/desktop-client-linux.v1"),"default read has no side effect");
    const auto program=root/"literal client ' one";write(program,"#!/bin/sh\nprintf '%s\\n' \"$@\" > \"$SFX_TEST_ARGUMENTS\"\n");
    web::DesktopClientProfile profile{2,2,program.string(),{"{HOST}","{PORT}","literal spaces","$(touch MUST_NOT_EXIST);$HOME*'\\\""}};
    const auto parsed=web::parse_desktop_client(web::desktop_client_json(profile));check(parsed.arguments==profile.arguments,"literal custom tokens preserved");
    const auto arguments=web::desktop_client_arguments(profile,43210);check(arguments[0]=="127.0.0.1"&&arguments[1]=="43210"&&arguments[2]=="literal spaces"&&arguments[3]==profile.arguments[3],"only host and port substituted");
    web::write_desktop_client(root,profile,1);check(web::desktop_client_json(web::read_desktop_client(root))==web::desktop_client_json(profile),"profile saved and read exactly");
    struct stat info{};check(stat((root/"config/desktop-client-linux.v1").c_str(),&info)==0&&(info.st_mode&0777)==0600&&info.st_uid==geteuid(),"profile owner private");
    refuses([&]{web::write_desktop_client(root,profile,1);});
    {native::InstallationLock held;check(held.acquire(root/"management","desktop-client-linux.lock"),"profile lock");auto next=profile;++next.revision;refuses([&]{web::write_desktop_client(root,next,2);});}
    const auto pin=web::pin_desktop_client(profile);check(pin.script&&web::same_desktop_client(pin,web::pin_desktop_client(profile)),"same script inode and bytes");
    check(setenv("SFX_TEST_ARGUMENTS",(root/"arguments.txt").c_str(),1)==0,"fixture output env");const auto child=web::spawn_desktop_client(pin,profile,43210);int status{};
    check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"pinned shebang executes");
    check(read(root/"arguments.txt")=="127.0.0.1\n43210\nliteral spaces\n"+profile.arguments[3]+"\n","actual argv has no shell expansion");
    check(!std::filesystem::exists("MUST_NOT_EXIST"),"argument command text never executed");
    write(program,"#!/bin/sh\nexit 0\n");check(!web::same_desktop_client(pin,web::pin_desktop_client(profile)),"in-place changed program detected");
    std::filesystem::rename(program,root/"retained-client");write(program,"#!/bin/sh\nexit 0\n");check(!web::same_desktop_client(pin,web::pin_desktop_client(profile)),"replaced inode detected");
    const auto link=root/"client-link";check(symlink(program.c_str(),link.c_str())==0,"installed symlink fixture");auto linked=profile;linked.executable=link.string();check(web::same_desktop_client(web::pin_desktop_client(profile),web::pin_desktop_client(linked)),"installed executable symlink pinned to target");
    check(chmod(program.c_str(),0777)==0,"unsafe program fixture");refuses([&]{web::pin_desktop_client(profile);});check(chmod(program.c_str(),0600)==0,"nonexecutable fixture");refuses([&]{web::pin_desktop_client(profile);});
    write(program,"not a program\n");refuses([&]{web::spawn_desktop_client(web::pin_desktop_client(profile),profile,43210);});
    check(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD,"failed exec child reaped");
    auto sync=profile;sync.type=1;sync.executable="/usr/bin/syncterm";sync.arguments.clear();check(web::desktop_client_arguments(sync,43210)==std::vector<std::string>({"-4","telnet://127.0.0.1:43210"}),"SyncTERM accepted argument convention");
    const auto good=web::desktop_client_json(profile);
    for(const auto& revision:Json::array({0,"0","01","-1","18446744073709551615"})){auto bad=good;bad["revision"]=revision;refuses([&]{web::parse_desktop_client(bad);});}
    for(const auto& kind:Json::array({true,"2",-1,3})){auto bad=good;bad["type"]=kind;refuses([&]{web::parse_desktop_client(bad);});}
    for(const auto* path:{"relative","/a/../b","/a\nb","/a\tb"}){auto bad=good;bad["executable"]=path;refuses([&]{web::parse_desktop_client(bad);});}
    for(const auto& tokens:Json::array({Json::array(),Json::array({"{HOST}"}),Json::array({"{HOST}","{PORT}","{HOST}"}),Json::array({"{HOST}","{PORT}","{BAD}"}),Json::array({"{HOST}","{PORT}",""}),Json::array({"{HOST}","{PORT}","bad\nline"}),Json::array({"{HOST}","{PORT}",std::string(513,'a')})})){
        auto bad=good;bad["arguments"]=tokens;refuses([&]{web::parse_desktop_client(bad);});}
    auto bad=good;bad["arguments"]=std::vector<std::string>(9,"a");refuses([&]{web::parse_desktop_client(bad);});bad=good;bad["extra"]=0;refuses([&]{web::parse_desktop_client(bad);});
    auto large=profile;large.executable="/"+std::string(3000,'x');large.arguments={"{HOST}","{PORT}",std::string(512,'a'),std::string(512,'b'),std::string(512,'c')};refuses([&]{web::desktop_client_arguments(large,43210);});
    refuses([&]{web::desktop_client_arguments(profile,0);});refuses([&]{web::desktop_client_arguments(fresh,43210);});
    const auto request=Json{{"revision","2"},{"values",{{"type",2},{"executable",program.string()},{"arguments","{HOST}\n{PORT}\nliteral spaces"}}}};
    check(web::desktop_client_update(request).arguments==std::vector<std::string>({"{HOST}","{PORT}","literal spaces"}),"separate UI argument lines");
    auto invalid=request;invalid["values"]["arguments"]="{HOST}\n{PORT}\n";refuses([&]{web::desktop_client_update(invalid);});
    const auto page=web::desktop_client_page(profile,true,root,root/"sfxweb");check(page.at("can_save")==true&&page.at("desktop_command").get<std::string>().find(" --node 2")!=std::string::npos,"local profile and concrete desktop command");
    const auto remote=web::desktop_client_page(profile,false,root,root/"sfxweb");check(remote.at("can_save")==true&&remote.at("desktop_command")=="","LAN can configure server profile but has no desktop launch command");
    auto cleared=fresh;cleared.revision=3;web::write_desktop_client(root,cleared,2);check(!web::read_desktop_client(root).type&&std::filesystem::exists(program),"clear resets profile and preserves program");
    const auto directory=native::open_owned_directory(root/"config");const std::string duplicate="{\"revision\":\"3\",\"type\":0,\"type\":0,\"executable\":\"\",\"arguments\":[]}";
    native::publish_private_file(directory.get(),"desktop-client-linux.v1",{duplicate.begin(),duplicate.end()},true);refuses([&]{web::read_desktop_client(root);});
    const auto restored=web::desktop_client_json(cleared).dump();native::publish_private_file(directory.get(),"desktop-client-linux.v1",{restored.begin(),restored.end()},true);
    check(chmod((root/"config/desktop-client-linux.v1").c_str(),0644)==0,"nonprivate profile fixture");refuses([&]{web::read_desktop_client(root);});
    check(chmod((root/"config/desktop-client-linux.v1").c_str(),0600)==0,"profile permissions restored");
    std::cout<<"PASS "<<checks<<" Linux desktop client checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
