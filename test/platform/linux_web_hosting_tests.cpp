#include "linux_web_hosting.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>

namespace {
namespace web=sfx::apps::runtime::web;
namespace native=sfx::platform::linux_host;
using Json=nlohmann::json;
unsigned checks{};
void check(bool value,const char* name){++checks;if(!value)throw std::runtime_error(name);}
template<class F>void refuses(F action){bool refused{};try{action();}catch(const std::exception&){refused=true;}check(refused,"invalid or unsafe hosting input refused");}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"normal-user context");const std::filesystem::path root(argv[1]);
    check(root.is_absolute()&&!std::filesystem::exists(root),"fresh owned fixture");
    check(mkdir(root.c_str(),0700)==0&&mkdir((root/"config").c_str(),0700)==0&&mkdir((root/"management").c_str(),0700)==0,"private fixture created");
    const auto fresh=web::read_hosting(root);check(fresh.revision==1&&!fresh.start_bbs_at_boot,"fresh BBS auto-start off");
    check(!std::filesystem::exists(root/"config/hosting-linux.v1"),"reading defaults does not create policy");
    auto request=Json{{"revision","1"},{"values",{{"start_bbs_at_boot",true}}}};
    auto settings=web::hosting_update(request);check(settings.revision==2&&settings.start_bbs_at_boot,"typed boot preference");
    web::write_hosting(root,settings,1);check(web::read_hosting(root).start_bbs_at_boot,"saved preference read back");
    struct stat info{};check(stat((root/"config/hosting-linux.v1").c_str(),&info)==0&&(info.st_mode&0777)==0600&&info.st_uid==geteuid(),"saved preference owner-private");
    refuses([&]{web::write_hosting(root,settings,1);});check(web::read_hosting(root).revision==2,"stale save leaves revision unchanged");
    settings.revision=3;settings.start_bbs_at_boot=false;
    {native::InstallationLock held;check(held.acquire(root/"management","hosting-linux.lock"),"writer lock held");refuses([&]{web::write_hosting(root,settings,2);});}
    web::write_hosting(root,settings,2);check(!web::read_hosting(root).start_bbs_at_boot,"preference can be switched off");
    for(const auto& value:Json::array({0,"0","01","+1","-1"," 1","1 ","18446744073709551615","18446744073709551614"})){auto invalid=request;invalid["revision"]=value;refuses([&]{web::hosting_update(invalid);});}
    for(const auto& value:Json::array({0,1,"true",nullptr})){auto invalid=request;invalid["values"]["start_bbs_at_boot"]=value;refuses([&]{web::hosting_update(invalid);});}
    auto invalid=request;invalid["start_now"]=true;refuses([&]{web::hosting_update(invalid);});invalid=request;invalid["values"]["start_now"]=true;refuses([&]{web::hosting_update(invalid);});
    request["revision"]="9007199254740993";check(web::hosting_update(request).revision==9007199254740994ULL,"revision exact beyond browser integer precision");
    const auto foreground=web::hosting_page(settings,false,"Stopped at startup.");check(!foreground.at("service_invocation").get<bool>()&&foreground.at("fields")[0].at("value")==false,"foreground page does not invent service registration");
    check(web::hosting_page(settings,true,"Start refused.").at("activation").get<std::string>().find("Start refused.")!=std::string::npos,"startup failure remains visible");
    const auto plan=web::hosting_service_plan(root/"bin/sfxweb",root,root/"Time/data",1001,1001,false);const auto unit=plan.at("unit").get<std::string>();
    check(plan.at("installed")==false&&unit.find("User=1001\nGroup=1001\n")!=std::string::npos,"plan binds normal identity without claiming installation");
    check(unit.find(" --port 0 --service\n")!=std::string::npos&&unit.find("WorkingDirectory="+root.string()+"\n")!=std::string::npos,"unit binds explicit board and service invocation");
    check(unit.find("CapabilityBoundingSet=\nAmbientCapabilities=\n")!=std::string::npos&&unit.find("NoNewPrivileges=yes")!=std::string::npos,"default service grants no capabilities");
    check(unit.find("Restart=no")!=std::string::npos&&unit.find("SendSIGKILL=no")!=std::string::npos&&unit.find("KillMode=mixed")!=std::string::npos,"unit permits orderly shutdown without automatic recovery restart or forced kill");
    const auto low=web::hosting_service_plan(root/"bin/sfxweb",root,root/"Time/data",1001,1001,true);
    check(low.at("unit_name")==plan.at("unit_name")&&low.at("unit").get<std::string>().find("CapabilityBoundingSet=CAP_NET_BIND_SERVICE\nAmbientCapabilities=CAP_NET_BIND_SERVICE\n")!=std::string::npos,"same service identity with only optional bind capability");
    check(web::hosting_service_plan(root/"bin/sfxweb",root/"moved",root/"Time/data",1001,1001,false).at("unit_name")!=plan.at("unit_name"),"relocation prepares a separate registration");
    const auto quoted=web::hosting_service_plan("/home/a space/$HOME%u/\"bin/SFXWeb","/home/a space/$HOME%u/board","/home/a\\b/time",1001,1001,false).at("unit").get<std::string>();
    check(quoted.find("ExecStart=:/usr/bin/env -- \"/home/a space/$HOME%%u/\\\"bin/SFXWeb\"")!=std::string::npos&&quoted.find("/home/a\\\\b/time")!=std::string::npos,"paths quoted and environment/specifier expansion disabled");
    check(quoted.find("WorkingDirectory=/home/a space/$HOME%%u/board\n")!=std::string::npos,"working directory uses raw path with escaped specifiers");
    refuses([&]{web::hosting_service_plan("/bin/a",root,"/tmp/tz",0,1001,false);});refuses([&]{web::hosting_service_plan("/bin/a",root,"/tmp/tz",1001,0,false);});
    for(const auto* bad:{"relative","/home/a/../b","/home/a\nExecStart=/bad","/home/a\tb","/home/a ","/home/a\\"})refuses([&]{web::hosting_service_plan("/bin/a",bad,"/tmp/tz",1001,1001,false);});
    const auto config=native::open_owned_directory(root/"config");const auto replace=[&](const std::string& text){native::publish_private_file(config.get(),"hosting-linux.v1",{text.begin(),text.end()},true);};
    replace("{\"revision\":\"3\",\"start_bbs_at_boot\":true,\"start_bbs_at_boot\":false}");refuses([&]{web::read_hosting(root);});
    replace("{broken");refuses([&]{web::read_hosting(root);});refuses([&]{web::write_hosting(root,settings,2);});
    replace(web::hosting_json(settings).dump());check(chmod((root/"config/hosting-linux.v1").c_str(),0644)==0,"permissions changed in owned fixture");refuses([&]{web::read_hosting(root);});
    check(chmod((root/"config/hosting-linux.v1").c_str(),0600)==0,"private permissions restored");
    std::filesystem::rename(root/"config/hosting-linux.v1",root/"config/retained-policy.v1");
    check(symlink("retained-policy.v1",(root/"config/hosting-linux.v1").c_str())==0,"owned symlink fixture created");refuses([&]{web::read_hosting(root);});
    check(std::filesystem::remove(root/"config/hosting-linux.v1"),"only created symlink retired");std::filesystem::rename(root/"config/retained-policy.v1",root/"config/hosting-linux.v1");
    check(web::read_hosting(root).revision==3,"fixture policy restored");std::cout<<"PASS "<<checks<<" Linux hosting checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
