// Seeds isolated stopped-board FTN choices; no network sessions or delivery.
#include "linux_runtime_store.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include <sodium.h>
#include <unistd.h>
#include <iostream>
namespace app=sfx::application;namespace rt=sfx::apps::runtime;namespace host=sfx::platform::linux_host;
namespace {void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}struct Clock:app::IClock{std::int64_t unix_seconds()const noexcept override{return 1794268800;}std::uint64_t monotonic_millis()const noexcept override{return 1000;}};struct Revocations:app::ISessionRevocationSink{bool revoke(const app::SessionRevocationCommand&)override{return true;}};}
int main(int argc,char** argv){try{
    require(argc==3&&geteuid()!=0&&sodium_init()>=0,"normal user fixture arguments");const std::filesystem::path root=argv[1];require(root.is_absolute()&&root.string().find("filemanager-")!=std::string::npos,"named File Manager fixture only");require(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"time zone");Clock clock;Revocations sink;rt::LinuxRuntimeStore store(root,sink,clock,[]{return false;});require(store.open().code==app::LocalControlResponseCode::ok,"open isolated stopped board");auto& db=*store.storage();const app::MessageConfigurationActor owner{true,{}};
    const auto current=db.ftn_configuration(owner);require(current&&current.value&&current.value->networks.empty(),"empty FTN fixture only");auto config=*current.value;config.settings.mode=app::FtnMailerMode::native;config.settings.listener_enabled=false;config.settings.tls_listener_enabled=false;
    app::FtnNetwork network;network.code="files";network.name="File Manager Test";network.address="2:100/1@test";network.enabled=true;network.default_link="PERMITTED";config.networks.push_back(network);
    for(const auto* name:{"PERMITTED","UNPERMITTED"}){app::FtnLink link;link.network=network.code;link.code=name;link.name=name;link.address=std::string(name)=="PERMITTED"?"2:100/2@test":"2:100/3@test";link.host="127.0.0.1";link.port=65432;link.poll_minutes=0;link.contact_on_mail=false;link.enabled=true;
        const auto session=host::protect_ftn_secret(root.string(),network.code,name,host::FtnCredentialKind::session,"Fixture session secret"),tic=host::protect_ftn_secret(root.string(),network.code,name,host::FtnCredentialKind::tic,"Fixture TIC secret");require(session&&tic,"fixture credential sealing");link.credentials.session=*session;link.credentials.tic=*tic;config.links.push_back(link);}
    const auto files=db.file_configuration({true,{}});require(files&&files.value&&!files.value->areas.empty(),"fresh file area");config.file_echoes.push_back({network.code,"TEST.FILES","Fixture outbound files",files.value->areas.front().area.id.value});config.permissions.push_back({network.code,"PERMITTED","TEST.FILES",true,false,true,false});
    require(db.change_ftn_configuration(owner,store.identifiers().generate().bytes,config).succeeded(),"save exact fixture permissions");require(store.close_for_maintenance().code==app::LocalControlResponseCode::ok,"close seed ownership");std::cout<<"PASS isolated permitted and unpermitted FTN choices; no BBS or network start\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
