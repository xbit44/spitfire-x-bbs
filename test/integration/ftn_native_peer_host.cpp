// Finite original-binkd interoperability fixture. Literal loopback only.
// Synthetic payload/DPAPI identity; no Runtime, owner endpoint or tosser claim.
#include <windows.h>
#include <sodium.h>
#include "sfx/platform/win32/ftn_native_session.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include <chrono>
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
namespace native=sfx::platform::win32;
namespace ins=native::installation;
namespace {
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
app::Identifier128 id(unsigned n){app::Identifier128 result;result.bytes[0]=0x72;result.bytes[15]=static_cast<std::uint8_t>(n);return result;}
}
int main(int argc,char** argv){try{
    require(argc==3||argc==4,"new fixture root and inbound/outbound [loopback port or --wrong-key] required");const auto root=std::filesystem::absolute(argv[1]);require(!std::filesystem::exists(root),"fixture root already exists");const std::string mode=argv[2];require(mode=="inbound"||mode=="outbound","bounded fixture mode required");const bool outbound=mode=="outbound";const bool wrong_key=!outbound&&argc==4&&std::string_view(argv[3])=="--wrong-key";require(outbound?argc==4:(argc==3||wrong_key),"exact argument count required");
    std::uint16_t port{};if(outbound){unsigned value{};for(const char* at=argv[3];*at;++at){require(*at>='0'&&*at<='9'&&value<=65535,"loopback port invalid");value=value*10+static_cast<unsigned>(*at-'0');}require(value>0&&value<=65535,"loopback port invalid");port=static_cast<std::uint16_t>(value);}
    std::filesystem::create_directories(root.parent_path());const auto owner=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(owner);for(const auto& path:{root,root/L"Data",root/L"In",root/L"Out"})ins::create_directory(path,descriptor.get());
    auto input=native::FilePayloadDirectory::private_store(root/L"In",owner),output=native::FilePayloadDirectory::private_store(root/L"Out",owner);sfx::test::FakeStoragePathPolicy paths;sfx::adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};sfx::adapters::storage::SqliteStorage db(settings,paths);require(db.start().succeeded(),"managed fixture startup failed");const app::MessageConfigurationActor owner_actor{true,{}};auto config=*db.ftn_configuration(owner_actor).value;
    app::FtnNetwork network;network.code="TEST";network.name="Independent Original Peer Fixture";network.address="2:100/1@test";network.enabled=true;config.networks.push_back(network);app::FtnLink link;link.network="TEST";link.code="PEER";link.name="Original binkd isolated peer";link.address="2:100/2@test";link.host="127.0.0.1";link.enabled=true;if(port)link.port=port;const auto credential=native::protect_ftn_secret(root.u8string(),"TEST","PEER",native::FtnCredentialKind::session,wrong_key?"different-fixture-key":"fixture-session-key");require(credential.has_value(),"fixture DPAPI protection failed");link.credentials.session=*credential;config.links.push_back(link);require(bool(db.change_ftn_configuration(owner_actor,id(1).bytes,config)),"fixture config failed");config=*db.ftn_configuration(owner_actor).value;
    const std::string content=std::string("SFX native independent-peer fixture\r\n")+std::string(65537,'s');app::FtnOutboundJob job;job.id=id(2);job.operation=id(2);job.network="TEST";job.link="PEER";job.kind=app::FtnOutboundKind::payload;job.file={"sfx-native.bin",content.size(),1700000001};
    {auto file=output.create(native::ftn_native_artifact_name(job.id),content.size());for(std::size_t at=0;at<content.size();){const auto count=std::min<std::size_t>(65536,content.size()-at);file.append(reinterpret_cast<const std::uint8_t*>(content.data()+at),count);at+=count;}job.digest=file.identity().sha256;}require(db.queue_ftn_jobs(owner_actor,config.revision,{job}).succeeded(),"fixture queue failed");
    const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(25);const auto live=[&]{return std::chrono::steady_clock::now()<until;};std::unique_ptr<native::FtnNativeChannel> channel;std::unique_ptr<native::FtnNativeListener> listener;
    if(outbound)channel=native::FtnNativeChannel::connect_ipv4({127,0,0,1},port,config.links.front(),owner,live);
    else {listener=native::FtnNativeListener::open("127.0.0.1",0,false,config.settings,owner);std::cout<<"READY "<<listener->port()<<'\n'<<std::flush;while(!channel&&live())channel=listener->accept(std::chrono::milliseconds(100),live);}
    require(channel!=nullptr,"finite fixture accept timed out");native::FtnNativeSessionPorts ports{db,config,input,output,root.u8string(),id(3),outbound?std::optional<app::FtnPeerMatch>{{"TEST","PEER"}}:std::nullopt,false,live,{}};const auto result=native::run_ftn_native_session(*channel,std::move(ports));channel.reset();listener.reset();
    const auto received=db.pending_ftn_ingress(owner_actor,16);bool exact=false;if(received&&received.value&&received.value->size()==1){const auto receipt=received.value->front();const auto actual=input.read(native::ftn_native_artifact_name(receipt.id)).identity();const std::string expected(65537,'p');app::FtnDigest digest{};crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(expected.data()),expected.size());exact=actual.bytes==expected.size()&&actual.sha256==digest&&receipt.digest==digest;}
    std::cout<<"RESULT {\"completed\":"<<(result.completed?"true":"false")<<",\"peer_verified\":"<<(result.peer_verified?"true":"false")<<",\"proof\":"<<static_cast<unsigned>(result.proof)<<",\"reason\":\""<<result.reason<<"\",\"received_files\":"<<result.received_files<<",\"sent_files\":"<<result.sent_files<<",\"batches\":"<<result.completed_batches<<",\"received_exact\":"<<(exact?"true":"false")<<"}\n"<<std::flush;
    require(db.shutdown().succeeded(),"fixture shutdown failed");return result.completed&&result.received_files==1&&result.sent_files==1&&exact?0:1;
}catch(const std::exception& error){std::cerr<<"FAIL isolated native peer fixture: "<<error.what()<<'\n';return 1;}}
