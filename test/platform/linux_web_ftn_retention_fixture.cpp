// Actual native receive-file custody and toss for web retention qualification.
// Session provenance is synthetic; no socket, peer or running-BBS claim.
#include "linux_runtime_store.hpp"
#include "sfx/platform/linux/bbs_time_zone_provider.hpp"
#include "sfx/platform/linux/ftn_native_toss.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include <sodium.h>
#include <sys/stat.h>
#include <unistd.h>
#include <chrono>
#include <iostream>
namespace app=sfx::application;namespace rt=sfx::apps::runtime;namespace host=sfx::platform::linux_host;namespace fs=std::filesystem;
namespace {
void require(bool value,const char* why){if(!value)throw std::runtime_error(why);}
struct Clock final:app::IClock{
 std::int64_t unix_seconds()const noexcept override{return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();}
 std::uint64_t monotonic_millis()const noexcept override{return 1000;}
};
struct Revocations final:app::ISessionRevocationSink{bool revoke(const app::SessionRevocationCommand&)override{return true;}};
}
int main(int argc,char** argv){try{
 require(argc==3&&geteuid()!=0&&sodium_init()>=0,"normal user fixture arguments");const fs::path root=argv[1];require(root.is_absolute()&&fs::is_directory(root)&&!fs::is_symlink(root),"existing isolated board");require(host::LinuxBbsTimeZoneRuleProvider::initialize(argv[2]),"fixture time zone");Clock clock;require(clock.unix_seconds()>0,"valid positive fixture time");Revocations revocations;rt::LinuxRuntimeStore store(root,revocations,clock,[]{return false;});require(store.open().code==app::LocalControlResponseCode::ok,"open stopped isolated board");auto& db=*store.storage();const app::MessageConfigurationActor owner{true,{}};
 auto current=db.ftn_configuration(owner);require(current&&current.value&&current.value->networks.empty(),"fresh empty FTN catalog only");auto c=*current.value;c.settings.mode=app::FtnMailerMode::native;c.settings.listener_enabled=false;c.settings.tls_listener_enabled=false;
 app::FtnNetwork net;net.code="RETAIN";net.name="Retained Mail Fixture";net.address="2:100/1@test";net.enabled=true;net.keep_processed=true;net.retention_days=0;net.default_link="HUB";c.networks.push_back(net);
 app::FtnLink hub;hub.network=net.code;hub.code="HUB";hub.name="Fixture Hub";hub.address="2:100/2@test";hub.host="127.0.0.1";hub.enabled=true;hub.poll_minutes=0;hub.contact_on_mail=false;
 for(const auto kind:{host::FtnCredentialKind::session,host::FtnCredentialKind::packet}){const auto secret=host::protect_ftn_secret(root.string(),net.code,hub.code,kind,"retkey");require(bool(secret),"protect fixture credentials");if(kind==host::FtnCredentialKind::session)hub.credentials.session=*secret;else hub.credentials.packet=*secret;}
 c.links.push_back(hub);app::FtnMessageEcho echo;echo.network=net.code;echo.tag="RETAIN.TEST";echo.description="Retained original test";echo.store_locally=true;c.message_echoes.push_back(echo);c.permissions.push_back({net.code,hub.code,echo.tag,false,true,false,false});
 require(bool(db.change_ftn_configuration(owner,store.identifiers().generate().bytes,c)),"save Forever fixture configuration");c=*db.ftn_configuration(owner).value;
 for(const auto* role:{"ftn/inbound","ftn/processed","ftn/bad","ftn/inbound/retain","ftn/processed/retain","ftn/bad/retain"}){const auto path=root/role;if(!fs::exists(path))require(mkdir(path.c_str(),0700)==0,"private native FTN role");}
 const host::FtnInstallation installation{root,store.file_bindings()};auto incoming=host::FilePayloadDirectory::private_store(root/"ftn/inbound/retain",store.file_bindings());const auto live=[]{return true;};
 for(unsigned i=1;i<=2;++i){
  app::FtnPacket packet;packet.header.origin=*app::parse_ftn_address(hub.address);packet.header.destination=*app::parse_ftn_address(net.address);packet.header.year=2026;packet.header.month=11;packet.header.day=9;std::copy_n("retkey",6,packet.header.password.begin());
  app::FtnPackedMessage message;message.origin_net=message.destination_net=100;message.origin_node=2;message.destination_node=1;message.date="09 Nov 26  12:00:00";message.from="Retention Fixture";message.to="All";message.subject="Actual retained original "+std::to_string(i);message.text="AREA:RETAIN.TEST\r\1MSGID: 2:100/2 web-retention-"+std::to_string(i)+"\r\1CHRS: CP437 2\rPreserved imported content.\r";packet.messages.push_back(message);const auto wire=app::encode_ftn_packet(packet);require(bool(wire),"valid fixture packet");
  app::FtnIngressReceipt ingress;ingress.id=store.identifiers().generate();ingress.session=store.identifiers().generate();require(crypto_hash_sha256(ingress.identity.data(),reinterpret_cast<const unsigned char*>(wire->data()),wire->size())==0,"packet hash");ingress.network=net.code;ingress.link=hub.code;ingress.proof=app::FtnIngressProof::inbound_cram;ingress.configuration_revision=c.revision;ingress.file={"retained"+std::to_string(i)+".pkt",wire->size(),static_cast<std::uint64_t>(clock.unix_seconds())};
  {auto receiver=host::FtnNativeReceive::open(incoming,db,ingress,live);receiver.append(*wire);receiver.finish();ingress=receiver.receipt();}
  const auto result=host::toss_ftn_native_receipt(db,c,installation,ingress,live);if(!result.processed)std::cerr<<result.reason<<'\n';require(result.processed&&result.imported==1,"actual native toss imports and retains original");
 }
 auto proposed=c;proposed.networks.front().retention_days=1;const auto preview=db.preview_ftn_retention(owner,store.identifiers().generate().bytes,proposed);require(preview&&preview.value->changed==2&&preview.value->shortened==2&&preview.value->networks.size()==1&&preview.value->networks[0].from_forever==2&&preview.value->networks[0].expire_later==2,"two actual Forever originals eligible for review only");
 require(store.close_for_maintenance().code==app::LocalControlResponseCode::ok,"close fixture ownership");std::cout<<"PASS two actual native processed originals and imported messages, Forever protection unchanged; synthetic session provenance, no network or BBS start\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
