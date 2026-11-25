// Actual Windows NTFS handles, flush/hash/reopen and managed SQLite checkpoint
// transactions. Session/authentication is an explicit synthetic host port; no
// socket, real peer, installed Runtime or physical-platform claim.
#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/ftn_native_custody.hpp"
#include "sfx/platform/win32/ftn_native_session.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include <sodium.h>
#include <iostream>
#include <stdexcept>
namespace app=sfx::application;
namespace native=sfx::platform::win32;
namespace ins=native::installation;
namespace {
unsigned checks{};
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
template<class F>void refused(F action,const char* message){bool threw=false;try{action();}catch(const std::exception&){threw=true;}check(threw,message);}
app::Identifier128 id(unsigned value){app::Identifier128 out;out.bytes[0]=0x6e;out.bytes[15]=static_cast<std::uint8_t>(value);return out;}
app::FtnDigest hash(std::string_view text){app::FtnDigest out{};check(crypto_hash_sha256(out.data(),reinterpret_cast<const unsigned char*>(text.data()),text.size())==0,"independent native test hash");return out;}
void append(native::FilePayloadLease& file,std::string_view text){file.append(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());}
}
int main(int argc,char** argv){try{
    check(argc==2,"new isolated native custody root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"retain every previous fixture");std::filesystem::create_directories(root.parent_path());const auto owner=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(owner);
    for(const auto& path:{root,root/L"Data",root/L"FTN",root/L"Install",root/L"Install"/L"Files",root/L"Install"/L"Files"/L"General"})ins::create_directory(path,descriptor.get());
    sfx::test::FakeStoragePathPolicy paths;sfx::adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};sfx::adapters::storage::SqliteStorage db(settings,paths);check(db.start().succeeded(),"actual managed checkpoint store starts");const app::MessageConfigurationActor host{true,{}};
    auto c=*db.ftn_configuration(host).value;app::FtnNetwork n;n.code="TEST";n.name="Synthetic Native Custody";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);app::FtnLink link;link.network="TEST";link.code="PEER";link.name="Synthetic Host Proof";link.address="2:100/2@test";link.host="127.0.0.1";link.enabled=true;link.credentials.session={1};c.links.push_back(link);check(bool(db.change_ftn_configuration(host,id(10).bytes,c)),"synthetic session policy configured without socket/auth claim");c=*db.ftn_configuration(host).value;
    app::FtnIngressReceipt r;r.id=id(1);r.session=id(2);r.identity=hash("scope-one");r.network="TEST";r.link="PEER";r.proof=app::FtnIngressProof::inbound_cram;r.configuration_revision=c.revision;r.file={"test.pkt",16,1700000000};check(bool(db.begin_ftn_ingress(host,r)),"synthetic host receipt reserved before native creation");
    const auto intake=native::FilePayloadDirectory::private_store(root/L"FTN",owner);app::FilePayloadIdentity checkpoint;
    {
        auto file=intake.create("receipt.data",16);const auto object=file.object_identity();checkpoint.volume=object.volume;checkpoint.file_id=object.file_id;checkpoint.sha256=hash("");file.flush();
        auto wrong=checkpoint;wrong.bytes=1;check(!db.checkpoint_ftn_ingress(host,r.id,0,wrong),"new native custody cannot adopt preexisting unknown bytes");wrong=checkpoint;wrong.sha256.fill(0);check(!db.checkpoint_ftn_ingress(host,r.id,0,wrong),"empty object must carry actual empty-prefix digest");
        check(db.checkpoint_ftn_ingress(host,r.id,0,checkpoint).succeeded(),"actual empty object bound before peer data");append(file,"abcdef");file.flush();checkpoint.bytes=6;checkpoint.sha256=hash("abcdef");
        check(db.checkpoint_ftn_ingress(host,r.id,0,checkpoint).succeeded()&&db.checkpoint_ftn_ingress(host,r.id,0,checkpoint).succeeded(),"flushed native prefix offset/digest persist atomically and replay exactly");wrong=checkpoint;wrong.file_id="0";check(!db.checkpoint_ftn_ingress(host,r.id,6,wrong),"native receipt cannot adopt a replacement file identity");
        check(!db.advance_ftn_ingress(host,r.id,6,7,{},app::FtnIngressState::partial,{}),"generic partial update cannot bypass bound native checkpoint");
        append(file,"TAIL");file.flush();check(std::filesystem::file_size(file.path())==10,"uncommitted tail exists for interrupted recovery");
    }
    check(db.shutdown().succeeded()&&db.start().succeeded(),"actual database owner reopen preserves native prefix identity");const auto saved=db.ftn_ingress_checkpoint(host,r.id);check(saved&&saved.value->has_value()&&saved.value->value().file_id==checkpoint.file_id&&saved.value->value().bytes==6&&saved.value->value().sha256==checkpoint.sha256,"durable native identity/hash/offset are the exact original checkpoint");
    check(!db.ftn_ingress_checkpoint({},r.id),"caller/untrusted checkpoint reads refused");
    {
        auto wrong=checkpoint;wrong.sha256[0]^=1;refused([&]{intake.resume_private("receipt.data",16,wrong);},"changed durable prefix refused without overwriting file");check(std::filesystem::file_size(root/L"FTN"/L"receipt.data")==10,"failed prefix verification leaves uncommitted tail untouched");
        wrong=checkpoint;wrong.file_id="0";refused([&]{intake.resume_private("receipt.data",16,wrong);},"same-name replacement is not a resume capability");refused([&]{intake.resume_private("receipt.data",16,checkpoint,[](auto,auto){return false;});},"cancelled prefix verification cannot truncate or append");
        check(std::filesystem::file_size(root/L"FTN"/L"receipt.data")==10,"cancelled native recovery preserves exact file size");
        auto resumed=r;resumed.id=id(3);resumed.session=id(3);const auto receipt=db.begin_ftn_ingress(host,resumed);check(receipt&&receipt.value->id.bytes==r.id.bytes&&receipt.value->received==6,"same authenticated synthetic context resumes the original durable receipt");
        auto file=intake.resume_private("receipt.data",16,checkpoint);check(file.object_identity().file_id==checkpoint.file_id&&std::filesystem::file_size(file.path())==6&&file.read(0)==std::vector<std::uint8_t>({'a','b','c','d','e','f'}),"actual handle-bound recovery drops only uncommitted tail after prefix verification");
        auto outside=CreateFileW(file.path().c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);check(outside==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"resume lease still excludes concurrent writers");if(outside!=INVALID_HANDLE_VALUE)CloseHandle(outside);
        append(file,"ghijklmnop");file.flush();checkpoint.bytes=16;checkpoint.sha256=hash("abcdefghijklmnop");check(db.checkpoint_ftn_ingress(host,r.id,6,checkpoint).succeeded(),"last flushed native prefix committed");
        auto changed=checkpoint.sha256;changed[0]^=1;check(!db.advance_ftn_ingress(host,r.id,16,16,changed,app::FtnIngressState::received,{}),"wrong full digest cannot publish durable custody");const auto complete=file.identity();check(complete.sha256==checkpoint.sha256&&complete.bytes==16,"independent actual locked-file full hash verifies incremental checkpoint");
        check(db.advance_ftn_ingress(host,r.id,16,16,complete.sha256,app::FtnIngressState::received,{}).succeeded(),"only flushed checkpoint plus full native hash publishes complete custody");refused([&]{append(file,"x");},"sealed resumed file cannot mutate after custody publication");
    }
    {
        auto low_space=native::FilePayloadDirectory::private_store(root/L"FTN",owner,[]{return native::kFilePayloadFreeReserve-1;});refused([&]{low_space.resume_private("receipt.data",32,checkpoint);},"resume cannot bypass shared native free-space reserve");
        auto area=native::FilePayloadDirectory::area(root/L"Install",L"Files/General",{},{},owner);app::FilePayloadIdentity area_identity;{auto file=area.create("receipt.data",16);append(file,"abcdefghijklmnop");area_identity=file.identity();}refused([&]{area.resume_private("receipt.data",32,area_identity);},"valid caller file-area object cannot acquire private-spool recovery authority");check(area.read("receipt.data").identity().sha256==area_identity.sha256,"refused area recovery preserves the actual existing payload");auto input=native::FilePayloadDirectory::import_source(root/L"FTN",{},owner);refused([&]{input.resume_private("receipt.data",16,checkpoint);},"import-source capability cannot append or truncate");
    }
    auto incomplete=r;incomplete.id=id(4);incomplete.session=id(4);incomplete.identity=hash("scope-two");check(bool(db.begin_ftn_ingress(host,incomplete)),"separate partial receipt for IO failure");check(db.advance_ftn_ingress(host,incomplete.id,0,0,{},app::FtnIngressState::held,"native_io_failure").succeeded(),"incomplete native failure can be held without false full-size/digest claim");
    const auto pending=db.pending_ftn_ingress(host,16);check(pending&&pending.value->size()==1&&pending.value->front().id.bytes==r.id.bytes&&pending.value->front().digest==checkpoint.sha256,"only complete verified custody reaches pending tosser; incomplete HOLD is excluded");
    auto request=r;request.id=id(20);request.session=id(20);request.identity=hash("native-engine-one");request.file={"engine.pkt",10,1700000001};app::Identifier128 engine_id;
    {auto receive=native::FtnNativeReceive::open(intake,db,request,[]{return true;});engine_id=receive.receipt().id;check(receive.offset()==0,"engine reserves native empty file before bytes");receive.append("abc");receive.checkpoint();receive.append("zz");refused([&]{receive.finish();},"incomplete native engine transfer cannot publish Received/authorize GOT");}
    request.id=id(21);request.session=id(21);
    {auto receive=native::FtnNativeReceive::open(intake,db,request,[]{return true;});check(receive.offset()==3&&receive.receipt().id.bytes==engine_id.bytes,"engine resumes only committed prefix in same verified context");receive.append("defghij");refused([&]{receive.append("x");},"engine actual byte count cannot exceed wire declaration");const auto result=receive.finish();check(result.bytes==10&&result.sha256==hash("abcdefghij")&&receive.receipt().state==app::FtnIngressState::received,"native engine full independent hash then durable Received");refused([&]{receive.append("x");},"engine sealed custody cannot accept later bytes");}
    request.id=id(22);request.session=id(22);
    {auto repeat=native::FtnNativeReceive::open(intake,db,request,[]{return true;});check(repeat.offset()==0&&repeat.receipt().id.bytes!=engine_id.bytes,"completed name/size/time never blindly ACKs a new peer transfer");}
    request.id=id(23);request.session=id(23);request.identity=hash("native-empty");request.file={"empty.pkt",0,1700000002};
    {auto empty=native::FtnNativeReceive::open(intake,db,request,[]{return true;});const auto result=empty.finish();check(result.bytes==0&&result.sha256==hash(""),"zero-length transfer obtains actual empty native custody before ACK");}
    c=*db.ftn_configuration(host).value;auto empty_link=link;empty_link.code="EMPTY";empty_link.address="2:100/3@test";empty_link.credentials={};c.links.push_back(empty_link);check(bool(db.change_ftn_configuration(host,id(30).bytes,c)),"separate intentionally empty synthetic session policy configured");c=*db.ftn_configuration(host).value;
    request.configuration_revision=c.revision;request.link="EMPTY";request.proof=app::FtnIngressProof::inbound_tcp;request.id=id(24);request.session=id(24);request.identity=hash("native-unverified");request.file={"plain.pkt",10,1700000003};app::Identifier128 unverified_id;
    {auto unverified=native::FtnNativeReceive::open(intake,db,request,[]{return true;});unverified_id=unverified.receipt().id;unverified.append("abc");unverified.checkpoint();}
    request.id=id(25);request.session=id(25);
    {auto another=native::FtnNativeReceive::open(intake,db,request,[]{return true;});check(another.offset()==0&&another.receipt().id.bytes!=unverified_id.bytes,"known unverified inbound TCP cannot resume another session's bytes");}
    request.id=id(26);request.session=id(26);request.link="PEER";request.proof=app::FtnIngressProof::inbound_cram;
    {auto changed=native::FtnNativeReceive::open(intake,db,request,[]{return true;});check(changed.offset()==0&&changed.receipt().id.bytes!=unverified_id.bytes,"later verified context cannot upgrade old unverified partial custody");const auto held=changed.receipt().id;check(db.advance_ftn_ingress(host,held,0,0,{},app::FtnIngressState::held,"native_io_failure").succeeded(),"native engine HOLD fixture");}
    request.id=id(27);request.session=id(27);refused([&]{native::FtnNativeReceive::open(intake,db,request,[]{return true;});},"engine does not silently clear a durable HOLD");
    refused([&]{native::FtnNativeReceive::open(intake,db,request,[]{return false;});},"cancelled session cannot reserve native work");
    #include "rc8_inbound_checks.inc"
    check(db.shutdown().succeeded(),"actual native custody fixture stopped");std::cout<<"PASS "<<checks<<" actual NTFS/SQLite FTN checkpoint/recovery checks; synthetic session proof, no socket/Runtime claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<" win32="<<GetLastError()<<'\n';return 1;}}
