#include "linux_runtime_store.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
namespace app=sfx::application;
namespace native=sfx::platform::linux_host;
namespace rt=sfx::apps::runtime;
namespace {
struct Clock final:app::IClock{
    std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
    std::uint64_t monotonic_millis()const noexcept override{return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());}
};
struct Revocations final:app::ISessionRevocationSink{bool revoke(const app::SessionRevocationCommand&)override{return true;}};
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
}
int main(int argc,char** argv){try{
    require(argc==6&&geteuid()!=0,"isolated existing board, timezone, node and two high loopback ports required");
    const auto root=std::filesystem::absolute(argv[1]);const auto node=std::stoul(argv[3]),port=std::stoul(argv[4]),peer_port=std::stoul(argv[5]);
    require((node==1||node==2)&&port>1024&&port<65536&&peer_port>1024&&peer_port<65536,"isolated fixture numeric bounds");
    require(native::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"private timezone");
    require(mkdir((root/"ftn").c_str(),0700)==0,"fresh FTN fixture root only");
    Clock clock;Revocations revocations;rt::LinuxRuntimeStore store(root,revocations,clock,[]{return false;});
    require(store.open().code==app::LocalControlResponseCode::ok,"prepared stopped board opens");
    const app::MessageConfigurationActor owner{true,{}};auto config=store.storage()->ftn_configuration(owner);require(config&&config.value,"read current FTN configuration");
    require(config.value->networks.empty(),"preserve existing non-fixture network configuration");
    auto& candidate=*config.value;candidate.settings.mode=app::FtnMailerMode::native;candidate.settings.bind_address="127.0.0.1";candidate.settings.port=static_cast<std::uint16_t>(port);
    candidate.settings.listener_enabled=true;candidate.settings.tls_listener_enabled=false;
    app::FtnNetwork network;network.code="TEST";network.name="Native Test Network";network.address="2:100/"+std::to_string(node)+"@test";network.enabled=true;candidate.networks.push_back(network);
    app::FtnLink link;link.network="TEST";link.code="PEER";link.name="Native Test Peer";link.address="2:100/"+std::to_string(node==1?2:1)+"@test";link.host="127.0.0.1";link.port=static_cast<std::uint16_t>(peer_port);link.enabled=true;link.poll_minutes=0;
    const auto secret=native::protect_ftn_secret(root.u8string(),"TEST","PEER",native::FtnCredentialKind::session,"isolated-runtime-loopback-peer-83");require(bool(secret),"private fixture credential");link.credentials.session=*secret;candidate.links.push_back(link);
    require(bool(store.storage()->change_ftn_configuration(owner,store.identifiers().generate().bytes,candidate)),"save exact loopback fixture network");
    require(store.close_for_maintenance().code==app::LocalControlResponseCode::ok,"close fixture store before Runtime");std::cout<<"Prepared isolated FTN peer "<<node<<'\n';return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
