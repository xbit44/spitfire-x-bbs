// Private owner packets are arguments, never source-package fixtures.
#define main retained_rescan_file_fixture_main
#include "file_storage_tests.cpp"
#undef main
#include "sfx/platform/win32/ftn_native_toss.hpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
#include "sfx/application/ftn_wire.hpp"
#include "sfx/application/message_source.hpp"
#include <sodium.h>
#include <fstream>
#include <iterator>
namespace native=sfx::platform::win32;
namespace ins=native::installation;
namespace {app::FtnDigest hash(std::string_view v){app::FtnDigest out{};crypto_hash_sha256(out.data(),reinterpret_cast<const unsigned char*>(v.data()),v.size());return out;}}
int main(int argc,char** argv){try{
    check(argc==3,"fresh native test root and sanitized fixture directory required");
    const auto root=std::filesystem::absolute(argv[1]),inputs=std::filesystem::absolute(argv[2]);check(!std::filesystem::exists(root),"retain prior recovery fixtures");
    std::filesystem::create_directories(root.parent_path());const auto who=ins::operator_sid();const auto sd=ins::mutable_descriptor(who);ins::create_directory(root,sd.get());
    for(const auto* role:{L"Data",L"FTN",L"FTN/Inbound",L"FTN/Outbound",L"FTN/Bad",L"FTN/Processed",L"FTN/Nonsecure",L"FTN/Inbound/FIDO",L"FTN/Outbound/FIDO",L"FTN/Bad/FIDO",L"FTN/Processed/FIDO"})ins::create_directory((root/role).make_preferred(),sd.get());
    ins::Receipt installed;installed.root=root;installed.owner=who;installed.program_count=12;installed.ftn_layout=true;{auto h=ins::open_object(root/L"FTN",true);installed.volume=installed.ftn_volume=ins::volume_serial(h.get());installed.ftn_directory_id=ins::file_id(h.get());}
    sfx::test::FakeStoragePathPolicy paths;auto options=settings(root/L"Data");const auto database=options.data_directory/L"SFX.DB";{storage::SqliteStorage db(options,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"current native schema bootstraps");}seed(database);
    storage::SqliteStorage db(options,paths);check(db.start().succeeded(),"native store starts after seed");const app::MessageConfigurationActor owner{true,{}};
    std::ifstream header_input(inputs/"rescan-e930cd00.pkt",std::ios::binary);check(bool(header_input),"local sanitized packet supplies its own endpoint addresses");const std::string header_bytes{std::istreambuf_iterator<char>(header_input),{}};const auto header_packet=app::decode_ftn_packet(header_bytes,{},true);check(header_packet.packet.has_value(),"bounded local fixture header decodes without embedded owner profile");
    auto c=*db.ftn_configuration(owner).value;app::FtnNetwork network;network.code="FIDO";network.name="FidoNet";network.address=app::format_ftn_address(header_packet.packet->header.destination);network.enabled=true;network.default_link="HUB";c.networks.push_back(network);
    app::FtnLink link;link.network=network.code;link.code="HUB";link.name="Synthetic hub";link.address=app::format_ftn_address(header_packet.packet->header.origin);link.host="127.0.0.1";link.enabled=true;link.credentials.session=*native::protect_ftn_secret(root.u8string(),network.code,link.code,native::FtnCredentialKind::session,"synthetic-session-secret");c.links.push_back(link);
    for(const auto* tag:{"NET105CHAT","FIDOTEST","SYNCHRONET","MYSTIC","SPITFIRE","BBS_ADS"}){app::FtnMessageEcho echo;echo.network=network.code;echo.tag=tag;echo.description=tag;echo.local_name=tag;echo.access_level=echo.read_level=echo.post_level=10;c.message_echoes.push_back(echo);c.permissions.push_back({network.code,link.code,tag,false,true,true,false});}
    check(bool(db.change_ftn_configuration(owner,id(40000).bytes,c)),"six authoritative echo mappings and hub grants");c=*db.ftn_configuration(owner).value;
    auto incoming=native::FilePayloadDirectory::private_store(root/L"FTN/Inbound/FIDO",who);const auto live=[] {return true;};
    const auto receive=[&](unsigned number,const char* input,const char* name){std::ifstream in(inputs/input,std::ios::binary);check(bool(in),"private packet fixture readable");const std::string bytes{std::istreambuf_iterator<char>(in),{}};
        app::FtnIngressReceipt receipt;receipt.id=id(number);receipt.session=id(number+1000);receipt.identity=hash("rescan-recovery-"+std::to_string(number));receipt.network=network.code;receipt.link=link.code;receipt.proof=app::FtnIngressProof::inbound_cram;receipt.configuration_revision=c.revision;receipt.file={name,bytes.size(),1700000000+number};
        auto receiving=native::FtnNativeReceive::open(incoming,db,receipt,live);for(std::size_t at=0;at<bytes.size();){const auto count=std::min<std::size_t>(16384,bytes.size()-at);receiving.append(std::string_view(bytes).substr(at,count));at+=count;}receiving.finish();return receiving.receipt();};
    const auto original=receive(41000,"rescan.zip","e930cd00.mo0");const auto result=native::toss_ftn_native_receipt(db,c,installed,original,live);
    std::cout<<"RESCAN imported="<<result.imported<<" replayed="<<result.replayed<<" held="<<result.held<<" deferred="<<result.deferred<<" reason="<<result.reason<<"\n";for(const auto& note:result.diagnostics)std::cout<<note<<'\n';
    check(!result.deferred&&result.held&&result.imported>2000&&result.imported+result.replayed+result.diagnostics.size()==2174,"every original message independently imports, reconciles or receives a classified hold");
    const auto progress=db.ftn_toss_progress(owner,original.id);check(bool(progress)&&progress.value->size()==result.imported+result.replayed,"each import or duplicate has durable ordinal-bound progress");
    auto bad=native::FilePayloadDirectory::private_store(root/L"FTN/Bad",who);{auto retained=bad.read(original.file.name);const auto actual=retained.identity();check(actual.sha256==original.digest&&actual.bytes==original.file.size,"recognizable Bad original retains exact bytes");}
    const app::FtnReprocessRequest retry{id(42000),original.id,{},c.revision};check(bool(db.request_ftn_reprocess(owner,retry)),"explicit retry queues held original");const auto retried=native::reprocess_ftn_native_receipt(db,c,installed,retry.operation,live);
    check(retried.held&&!retried.deferred&&!retried.imported&&retried.replayed==result.imported+result.replayed,"retry imports no duplicates and preserves every committed ordinal");
    check(scalar(database,"SELECT count(*) FROM sfx_local_messages WHERE author_account_id IS NULL")==result.imported,"native store has exactly one copy of safe original messages");
    check(scalar(database,"SELECT count(*) FROM sfx_local_messages WHERE length(subject)=71")==1,"full 71-byte subject stored without truncation");
    check(scalar(database,"SELECT count(*) FROM sfx_ftn_message_metadata WHERE length(wire_to)=0")==4,"four blank public recipients retain exact empty wire fields");
    const auto single=receive(43000,"single.zip","e931d109.mo0");const auto small=native::toss_ftn_native_receipt(db,c,installed,single,live);check(small.processed&&small.imported==1,"one-message owner original still imports normally");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"schema and data survive restart after partial bundle retry");
    c=*db.ftn_configuration(owner).value;for(auto& echo:c.message_echoes)echo.ansi_colors=false;
    check(bool(db.change_ftn_configuration(owner,id(44000).bytes,c)),"outgoing ANSI disabled before received artwork read");
    std::size_t art{},colored{},recovered_plain{},truecolor{};
    {internal::SqliteConnection read(database,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
        internal::SqliteStatement rows(read.get(),"SELECT m.message_id,m.wire_text,s.format,s.source FROM sfx_ftn_message_metadata m JOIN sfx_message_sources s USING(message_id)","review retained original artwork without data rewrites");
        const app::MessageActor sysop{id(1),1,1,1};
        while(rows.row()){const auto raw_original=rows.blob(1);if(std::find(raw_original.begin(),raw_original.end(),27)==raw_original.end())continue;
            const auto opened=db.quote_source(sysop,{rows.int64(0)});check(opened&&opened.value,"authorized received original remains readable");
            if(opened.value->received_canvas){++art;const auto& canvas=*opened.value->received_canvas;const bool colors=std::any_of(canvas.cells.begin(),canvas.cells.end(),[](const auto& cell){return cell.glyph!=' '&&cell.attribute!=7;});if(colors){++colored;if(rows.int64(2)==static_cast<int>(app::MessageSourceFormat::plain))++recovered_plain;}if(std::any_of(canvas.foreground_rgb.begin(),canvas.foreground_rgb.end(),[](auto value){return value!=0xffffffffU;})||std::any_of(canvas.background_rgb.begin(),canvas.background_rgb.end(),[](auto value){return value!=0xffffffffU;}))++truecolor;
                check(opened.value->ansi_colors&&canvas.extent<=app::kMessageCanvasRows,"incoming colors independent of outgoing Off with bounded extent");}
            internal::SqliteStatement exact(read.get(),"SELECT wire_text,source FROM sfx_ftn_message_metadata JOIN sfx_message_sources USING(message_id) WHERE message_id=?1","verify source unchanged after read");exact.bind(1,rows.int64(0));check(exact.row()&&exact.blob(0)==raw_original&&exact.blob(1)==rows.blob(3),"wire and immutable source remain byte-exact after derived view");
        }
    }
    check(art>0&&colored>0&&recovered_plain>0,"actual retained rescan artwork regains authored colors including plain import fallbacks");
    check(truecolor>0,"actual supplied ads retain their CTerm RGB colors");
    std::cout<<"Received ANSI canvases="<<art<<" colored="<<colored<<" recovered plain fallbacks="<<recovered_plain<<" RGB="<<truecolor<<'\n';
    check(db.shutdown().succeeded(),"fixture closes cleanly");
    std::cout<<"PASS "<<checks<<" native original/rescan/partial-hold/retry/long-subject checks; isolated synthetic provenance, no live hub or owner acceptance\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
