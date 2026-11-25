// Test-only configuration of two exact disposable normal-install boards.
// The installed shipping Runtime, helper and stock peer perform the smoke.
#include "fixture_paths.hpp"
#include "sfx/application/ftn.hpp"
#include "sfx/application/ftn_wire.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ctime>
#ifdef _WIN32
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#else
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/linux/private_files.hpp"
#endif
namespace app=sfx::application;
void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv)try{
 require(argc==2||(argc==3&&std::string(argv[2])=="--packet-only"),"exact owned board required");const bool packet_only=argc==3;const auto root=std::filesystem::absolute(argv[1]).lexically_normal();
#ifdef _WIN32
 namespace win=sfx::platform::win32;namespace ins=win::installation;
 require(root==std::filesystem::path("C:/Program Files/SFX-RC12-Test-20261110-r1"),"only rc12 disposable board");
 const auto owner=ins::operator_sid();require(!ins::elevated(),"ordinary owner only");ins::verify_runtime_installation(root,owner);
 const auto previous=win::offline_ftn_configuration(root,owner);
#else
 require(root==(sfx::test::fixture_account_home() / "sfx-tests/linux-runtime-web-20261108-r1/rc12-installed-first-run-r1/board"),"only rc12 disposable native board");
 namespace linux_host=sfx::platform::linux_host;
 linux_host::InstallationLock lock;require(lock.acquire(root/"Data","SFXRuntime.lock"),"stopped owned board lock");
 linux_host::LinuxLocalStoragePathPolicy policy;sfx::adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/"Data";settings.utc_now=[]{return static_cast<std::int64_t>(std::time(nullptr));};
 sfx::adapters::storage::SqliteStorage store(settings,policy);require(bool(store.start()),"private installed store opens");
 const auto previous=store.ftn_configuration({true,{}});
#endif
 require(bool(previous)&&previous.value.has_value(),"current configuration available");auto config=*previous.value;
 if(packet_only){require(config.settings.mode==app::FtnMailerMode::external_binkd&&config.networks.size()==1&&config.networks.front().code=="TEST"&&config.networks.front().address=="2:100/1@test","only retained rc12 fixture packet repair");}
 else {
 require(config.networks.empty(),"fresh board without existing FTN only");config.settings.mode=app::FtnMailerMode::external_binkd;
 app::FtnNetwork network;network.code="TEST";network.name="RC12 isolated stock smoke";network.address="2:100/1@test";network.enabled=true;config.networks.push_back(network);
 app::FtnLink link;link.network="TEST";link.code="PEER";link.name="Loopback only";link.address="2:100/2@test";link.enabled=true;link.host.clear();link.port=0;config.links.push_back(link);
 app::FtnMessageEcho echo;echo.network="TEST";echo.tag="EXTERNAL.TEST";echo.description="RC12 isolated echo";config.message_echoes.push_back(echo);
 config.permissions.push_back({"TEST","PEER","EXTERNAL.TEST",false,true,true,false});app::FtnTransferToken token{};token[0]=0x72;token[1]=0x12;
#ifdef _WIN32
 require(bool(win::offline_change_ftn_configuration(root,owner,token,config)),"normal offline FTN save");
#else
 require(bool(store.change_ftn_configuration({true,{}},token,config)),"normal stored FTN save");
#endif
 }
#ifndef _WIN32
 require(bool(store.shutdown()),"installed store closed");
#endif
 app::FtnPacket packet;packet.header.origin=*app::parse_ftn_address("2:100/2@test");packet.header.destination=*app::parse_ftn_address("2:100/1@test");packet.header.year=2026;packet.header.month=11;packet.header.day=10;
 app::FtnPackedMessage message;message.origin_net=message.destination_net=100;message.origin_node=2;message.destination_node=1;message.date="10 Nov 26  20:00:00";message.from="Synthetic Peer";message.to="All";message.subject="RC12 installed stock smoke";message.text="AREA:EXTERNAL.TEST\r\1MSGID: 2:100/2 rc12-installed-1\r\1CHRS: CP437 2\rInstalled payload stock peer smoke\r";packet.messages.push_back(message);
 const auto wire=app::encode_ftn_packet(packet);require(bool(wire),"synthetic packet encoded");const auto path=root/"Data/rc12-peer-input.pkt";require(!std::filesystem::exists(path),"new packet only");std::ofstream out(path,std::ios::binary);out.write(wire->data(),static_cast<std::streamsize>(wire->size()));out.close();require(bool(out),"packet written");
#ifndef _WIN32
 std::filesystem::permissions(path,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
#endif
 std::cout<<"PASS isolated installed FTN configured; no transfer claimed\n";return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
