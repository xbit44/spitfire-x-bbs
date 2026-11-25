#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <iostream>
#include <algorithm>
bool without_birth{};
bool metadata_only{};
extern "C" int __real_statx(int,const char*,int,unsigned,struct statx*);
extern "C" int __wrap_statx(int fd,const char* path,int flags,unsigned mask,struct statx* output){const auto result=__real_statx(fd,path,flags,mask,output);if(result==0&&without_birth)output->stx_mask&=~STATX_BTIME;return result;}
namespace fs=std::filesystem;namespace host=sfx::platform::linux_host;namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void refused(F fn,const char* why){bool failed=false;try{fn();}catch(const std::runtime_error&){failed=true;}check(failed,why);}
void directory(const fs::path& path){check(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"private fixture directory");}
void put(const fs::path& path,std::string_view text,bool replace=false){const auto parent=host::open_owned_directory(path.parent_path());host::publish_private_file(parent.get(),path.filename().string(),{text.begin(),text.end()},replace);}
std::vector<std::uint8_t> bytes(const fs::path& path){const auto parent=host::open_owned_directory(path.parent_path());return host::read_private_file(parent.get(),path.filename().string(),1024*1024);}
bool same(const app::FileObjectIdentity& a,const app::FilePayloadIdentity& b){return a.volume==b.volume&&a.file_id==b.file_id;}
void run(const fs::path& root){
    check(geteuid()!=0&&root.is_absolute()&&!fs::exists(root),"fresh normal-user fixture");directory(root);const auto board=root/"board";
    directory(board);directory(board/"config");directory(board/"files");directory(board/"files/Public");directory(board/"files/Other");
    put(board/"config/first-run.v1","SFX_LINUX_FIRST_RUN=1\n");check(static_cast<bool>((sfx::test::mark_provisioned_linux_fixture(board),host::initialize_fresh_portable_board_key(board))),"fresh portable key");
    put(board/"config/file-identities.v1","Frozen Windows map must remain byte-identical\n");
    refused([&]{host::LinuxFilePayloadBindings::open(board);},"missing Linux map is not silently generated");
    auto map=host::LinuxFilePayloadBindings::initialize_fresh(board);app::FilePayloadIdentity identity;
    {
        auto payloads=host::FilePayloadDirectory::private_store(board/"files/Public",map);auto file=payloads.create("one.txt",4096);
        const std::string body="portable native payload\n";file.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());identity=file.identity();
        check(identity.bytes==body.size()&&identity.volume.rfind("sfx-linux-",0)==0,"new file gets portable logical identity and checkpoint");
    }
    const auto original_map=bytes(board/"config/file-identities-linux.v1");map->refresh();map.reset();
    map=host::LinuxFilePayloadBindings::open(board);
    {auto payloads=host::FilePayloadDirectory::private_store(board/"files/Public",map);auto file=payloads.read("one.txt");check(same(file.object_identity(),identity)&&file.identity().sha256==identity.sha256,"reopen preserves portable ID and bytes");}
    // A real publication refusal must latch before an unchanged checkpoint can
    // take its no-op branch. Reopen only after the injected unsafe link is gone.
    {
    auto payloads=host::FilePayloadDirectory::private_store(board/"files/Public",map);auto held=payloads.read("one.txt");
    fs::create_hard_link(board/"config/file-identities-linux.v1",board/"config/map-test-link");
    refused([&]{map->refresh();},"unsafe map publication fails");fs::remove(board/"config/map-test-link");
    refused([&]{held.identity();},"unchanged checkpoint cannot skip latched failed publication");
    }
    refused([&]{auto payloads=host::FilePayloadDirectory::private_store(board/"files/Public",map);payloads.read("one.txt");},"publication failure remains latched after physical fault removed");
    map.reset();map=host::LinuxFilePayloadBindings::open(board);
    {auto payloads=host::FilePayloadDirectory::private_store(board/"files/Public",map);auto file=payloads.read("one.txt");check(same(file.object_identity(),identity),"explicit reopen recovers exact durable unchanged binding");}
    const auto external=root/"external library";directory(external);
    {auto payloads=host::FilePayloadDirectory::private_store(external,map);auto file=payloads.create("outside.txt",256);const std::string text="external resource";file.append(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());const auto id=file.identity();check(id.volume.rfind("sfx-linux-",0)!=0,"external area retains native identity without owned map adoption");}
    {auto payloads=host::FilePayloadDirectory::private_store(external,map);auto file=payloads.read("outside.txt");check(file.byte_count()==17,"external configured file remains readable");}
    {auto payloads=host::FilePayloadDirectory::private_store(external,map);auto destination=host::FilePayloadDirectory::private_store(board/"files/Public",map);auto file=payloads.read("outside.txt",true);refused([&]{file.rename_to(destination,"external-move.txt");},"boundary-changing move requires copy before namespace mutation");check(fs::exists(external/"outside.txt")&&!fs::exists(board/"files/Public/external-move.txt"),"refused boundary move preserves actual source and destination");}
    const auto copy=root/"copied board";fs::copy(board,copy,fs::copy_options::recursive);
    refused([&]{host::LinuxFilePayloadBindings::open(copy);},"copied inode/root requires explicit Prepare");
    check(bytes(copy/"config/file-identities-linux.v1")==original_map,"refused copy leaves authenticated map exact");
    auto prepared=host::LinuxFilePayloadBindings::prepare(copy);
    {auto payloads=host::FilePayloadDirectory::private_store(copy/"files/Public",prepared);auto file=payloads.read("one.txt");check(same(file.object_identity(),identity)&&file.identity().sha256==identity.sha256,"explicit verified copy preserves logical ID and content");}
    check(bytes(copy/"config/file-identities.v1")==bytes(board/"config/file-identities.v1"),"Windows snapshot untouched");
    check(prepared->resolve_path(board/"files/Public/one.txt")==copy/"files/Public/one.txt","historical absolute File path resolves to copied board");
    check(prepared->resolve_path("files/Public/one.txt")==copy/"files/Public/one.txt","safe relative File path resolves under current board");
    check(prepared->logical_path(copy/"files/Public/one.txt")==board/"files/Public/one.txt","logical history path retains original root");
    check(prepared->resolve_path(external/"outside.txt")==external/"outside.txt","external configured path remains external");
    check(prepared->resolve_path(board/"Unknown/file")==board/"Unknown/file","unknown old-root role is not silently redirected");
    refused([&]{prepared->resolve_path("files/../config");},"relative traversal refused before path resolution");
    refused([&]{prepared->resolve_path(board/"files/../config");},"absolute traversal refused before path resolution");
    prepared.reset();prepared=host::LinuxFilePayloadBindings::open(copy);
    check(prepared->resolve_path(board/"files/Public/one.txt")==copy/"files/Public/one.txt","authenticated root history survives reopen");
    {auto area=host::FilePayloadDirectory::area(copy,board/"files/Public",{board/"files/Other"},{"data","config"},prepared);auto file=area.read("one.txt");check(area.path()==copy/"files/Public"&&same(file.object_identity(),identity),"actual area capability opens copied object from historical configured path");}
    refused([&]{host::FilePayloadDirectory::area(copy,board/"files/Public",{copy/"files/Public"},{},prepared);},"area overlap is checked after alias resolution");
    refused([&]{host::FilePayloadDirectory::import_source(board/"files/Public",{copy/"files/Public"},prepared,false);},"import protection is checked after alias resolution");
    {auto source=host::FilePayloadDirectory::import_source(external,{board/"files/Public"},prepared,false);check(source.path()==external,"configured external import keeps its actual directory");}
    const auto second=root/"second copy";fs::copy(copy,second,fs::copy_options::recursive);
    {auto next=host::LinuxFilePayloadBindings::prepare(second);
    check(next->resolve_path(board/"files/Public/one.txt")==second/"files/Public/one.txt"&&next->resolve_path(copy/"files/Public/one.txt")==second/"files/Public/one.txt","two moves retain each historical owned root");
    check(next->logical_path(second/"files/Public/one.txt")==board/"files/Public/one.txt","multiple moves preserve original history key");
    const auto near=fs::path(board.u8string()+"-external")/"files/item";check(next->resolve_path(near)==near,"root prefix alone cannot capture external sibling");}
    const auto nested=board/"files/nested-board";fs::copy(copy,nested,fs::copy_options::recursive);
    {auto next=host::LinuxFilePayloadBindings::prepare(nested);
    check(next->resolve_path(nested/"Unknown/file")==nested/"Unknown/file","longest containing root preserves unknown current role");
    check(next->resolve_path(nested/"files/Public/one.txt")==nested/"files/Public/one.txt","nested current owned path is not duplicated");
    check(next->resolve_path(board/"files/Public/one.txt")==nested/"files/Public/one.txt","nested destination still resolves historical File path");}
    if(metadata_only)return;
    const auto ambiguous=root/"ambiguous copy";fs::copy(board,ambiguous,fs::copy_options::recursive);
    auto ambiguity=host::LinuxFilePayloadBindings::prepare(ambiguous);app::FilePayloadIdentity left,right;
    {auto files=host::FilePayloadDirectory::private_store(ambiguous/"files/Public",ambiguity);const std::string body="same candidate bytes";
    {auto file=files.create("left.txt",256);file.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());left=file.identity();}
    {auto file=files.create("right.txt",256);file.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());right=file.identity();}}
    ambiguity.reset();fs::rename(ambiguous/"files/Public/left.txt",ambiguous/"files/Public/candidate.txt");fs::remove(ambiguous/"files/Public/right.txt");
    host::FileBindingPrepareProofs conflicting;conflicting.moves={{"files/Public/left.txt","files/Public/candidate.txt",left},{"files/Public/right.txt","files/Public/candidate.txt",right}};
    ambiguity=host::LinuxFilePayloadBindings::prepare(ambiguous,{},conflicting);check(ambiguity->deferred_records()==2&&fs::exists(ambiguous/"files/Public/candidate.txt"),"ambiguous candidate defers every claimant and preserves actual bytes");
    {auto files=host::FilePayloadDirectory::private_store(ambiguous/"files/Public",ambiguity);refused([&]{files.read("candidate.txt");},"ambiguous candidate gets no first-match logical identity");check(same(files.read("one.txt").object_identity(),identity),"unrelated proved file remains usable beside ambiguity");}
    ambiguity.reset();fs::rename(ambiguous/"files/Public/candidate.txt",ambiguous/"files/Public/left.txt");put(ambiguous/"files/Public/right.txt","same candidate bytes");
    ambiguity=host::LinuxFilePayloadBindings::prepare(ambiguous);check(ambiguity->deferred_records()==0,"explicit retry resolves independently restored ambiguous records");
    const auto raced=root/"changed-after-hash";fs::copy(board,raced,fs::copy_options::recursive);unsigned ticks{};
    auto race=host::LinuxFilePayloadBindings::prepare(raced,[&]{if(++ticks==3){const auto parent=host::open_owned_directory(raced/"files/Public");const int fd=openat(parent.get(),"one.txt",O_WRONLY|O_TRUNC|O_CLOEXEC|O_NOFOLLOW);check(fd>=0,"owned mutation fixture opens held file");check(write(fd,"changed",7)==7&&fsync(fd)==0,"actual content mutation after hashing");close(fd);}return false;});
    check(ticks>=3&&race->deferred_records()==1,"post-hash content mutation is deferred before map publication");
    const auto late=root/"cancel-at-publication";fs::copy(board,late,fs::copy_options::recursive);const auto late_map=bytes(late/"config/file-identities-linux.v1");unsigned cancel_ticks{};
    refused([&]{host::LinuxFilePayloadBindings::prepare(late,[&]{return ++cancel_ticks==4;});},"Cancel during final validation prevents publication");
    check(cancel_ticks==4&&bytes(late/"config/file-identities-linux.v1")==late_map,"late cancellation preserves exact prior authenticated snapshot");
    {auto payloads=host::FilePayloadDirectory::private_store(copy/"files/Public",prepared);auto destination=host::FilePayloadDirectory::private_store(copy/"files/Other",prepared);auto file=payloads.read("one.txt",true);file.rename_to(destination,"moved.txt");check(same(file.object_identity(),identity),"actual rename retains logical ID");}
    prepared.reset();prepared=host::LinuxFilePayloadBindings::open(copy);
    {auto payloads=host::FilePayloadDirectory::private_store(copy/"files/Other",prepared);auto file=payloads.read("moved.txt",true);check(same(file.object_identity(),identity),"renamed binding survives process reopen");file.remove_exact();}
    prepared.reset();prepared=host::LinuxFilePayloadBindings::open(copy);put(copy/"files/Other/moved.txt","portable native payload\n");
    {auto payloads=host::FilePayloadDirectory::private_store(copy/"files/Other",prepared);refused([&]{payloads.read("moved.txt");},"replacement after recorded deletion is never adopted");}
    const auto changed=root/"changed copy";fs::copy(board,changed,fs::copy_options::recursive);put(changed/"files/Public/one.txt","changed bytes\n",true);const auto changed_map=bytes(changed/"config/file-identities-linux.v1");
    auto deferred=host::LinuxFilePayloadBindings::prepare(changed);check(deferred->deferred_records()==1&&bytes(changed/"config/file-identities-linux.v1")!=changed_map&&fs::exists(changed/"files/Public/one.txt"),"changed copied payload is preserved with honest attention state");
    refused([&]{auto files=host::FilePayloadDirectory::private_store(changed/"files/Public",deferred);files.read("one.txt");},"changed copied payload never acquires old live identity");
    deferred.reset();deferred=host::LinuxFilePayloadBindings::open(changed);check(deferred->deferred_records()==1,"deferred expected evidence survives ordinary reopen");
    deferred.reset();put(changed/"files/Public/one.txt","portable native payload\n",true);deferred=host::LinuxFilePayloadBindings::prepare(changed);
    {auto files=host::FilePayloadDirectory::private_store(changed/"files/Public",deferred);auto file=files.read("one.txt");check(deferred->deferred_records()==0&&same(file.object_identity(),identity),"explicit retry resolves restored expected content with same logical ID");}
    const auto cancelled=root/"cancelled copy";fs::copy(board,cancelled,fs::copy_options::recursive);const auto cancelled_map=bytes(cancelled/"config/file-identities-linux.v1");
    refused([&]{host::LinuxFilePayloadBindings::prepare(cancelled,[]{return true;});},"cancelled Prepare refused");check(bytes(cancelled/"config/file-identities-linux.v1")==cancelled_map,"cancelled Prepare publishes nothing");
    const auto tampered=root/"tampered copy";fs::copy(board,tampered,fs::copy_options::recursive);auto damaged=bytes(tampered/"config/file-identities-linux.v1");damaged[20]^=1;
    put(tampered/"config/file-identities-linux.v1",{reinterpret_cast<const char*>(damaged.data()),damaged.size()},true);
    refused([&]{host::LinuxFilePayloadBindings::prepare(tampered);},"unauthenticated snapshot refused");check(bytes(tampered/"config/file-identities-linux.v1")==damaged,"tampered evidence preserved");
    {auto payloads=host::FilePayloadDirectory::private_store(board/"files/Public",map);fs::rename(board/"files/Public/one.txt",board/"files/Public/original-preserved.txt");put(board/"files/Public/one.txt","portable native payload\n");refused([&]{payloads.read("one.txt");},"byte-identical native replacement never acquires old logical ID");}
    refused([&]{map->refresh();},"snapshot refresh refuses replaced native file");
}
void ftn_identities(const fs::path& root){
    directory(root);const auto board=root/"board";directory(board);directory(board/"config");
    directory(board/"ftn");directory(board/"ftn/inbound");directory(board/"files");directory(board/"files/Public");
    put(board/"config/first-run.v1","SFX_LINUX_FIRST_RUN=1\n");
    check(bool((sfx::test::mark_provisioned_linux_fixture(board),host::initialize_fresh_portable_board_key(board))),"FTN fixture portable board key");
    auto map=host::LinuxFilePayloadBindings::initialize_fresh(board);app::FilePayloadIdentity native,old;
    const auto decimal=[](const std::string& value){return !value.empty()&&value.size()<=20&&std::all_of(value.begin(),value.end(),[](char c){return c>='0'&&c<='9';});};
    {
        auto ftn=host::FilePayloadDirectory::private_store(board/"ftn/inbound",map);
        auto files=host::FilePayloadDirectory::private_store(board/"files/Public",map);
        auto packet=ftn.create("new.pkt",128);const std::string body="synthetic FTN packet bytes";
        packet.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());native=packet.identity();
        check(decimal(native.volume)&&decimal(native.file_id),"new FTN identity fits accepted decimal database schema");
        auto legacy=files.create("old.pkt",128);const std::string previous="existing hex identity bytes";
        legacy.append(reinterpret_cast<const std::uint8_t*>(previous.data()),previous.size());old=legacy.identity();
        check(old.volume.rfind("sfx-linux-",0)==0,"ordinary Files identity unchanged");
        legacy.rename_to(ftn,"old.pkt");check(same(legacy.object_identity(),old),"preexisting hex FTN identity retained without conversion");
    }
    map.reset();map=host::LinuxFilePayloadBindings::open(board);
    {auto files=host::FilePayloadDirectory::private_store(board/"ftn/inbound",map);
        check(same(files.read("new.pkt").object_identity(),native)&&same(files.read("old.pkt").object_identity(),old),"mixed numeric and old hex identities reopen exactly");}
    const auto copy=root/"copied";fs::copy(board,copy,fs::copy_options::recursive);
    refused([&]{host::LinuxFilePayloadBindings::open(copy);},"copied FTN files require explicit Prepare");
    auto prepared=host::LinuxFilePayloadBindings::prepare(copy);
    {auto files=host::FilePayloadDirectory::private_store(copy/"ftn/inbound",prepared);
        auto packet=files.read("new.pkt",true);check(same(packet.object_identity(),native)&&packet.identity().sha256==native.sha256,"FTN decimal identity survives real copy and Prepare");
        check(same(files.read("old.pkt").object_identity(),old),"old FTN identity survives same Prepare");
        auto published=host::FilePayloadDirectory::private_store(copy/"files/Public",prepared);packet.rename_to(published,"published.pkt");
        check(same(packet.object_identity(),native),"FTN-to-file-area publication preserves decimal identity");}
    prepared.reset();prepared=host::LinuxFilePayloadBindings::open(copy);
    {auto published=host::FilePayloadDirectory::private_store(copy/"files/Public",prepared);
        check(same(published.read("published.pkt").object_identity(),native),"published numeric identity survives reopen outside FTN");}
    const auto next=root/"copied-again";fs::copy(copy,next,fs::copy_options::recursive);
    auto again=host::LinuxFilePayloadBindings::prepare(next);
    {auto files=host::FilePayloadDirectory::private_store(next/"files/Public",again);
        check(same(files.read("published.pkt").object_identity(),native),"published numeric identity survives another relocation");}
    fs::rename(next/"files/Public/published.pkt",next/"files/Public/original-preserved.pkt");
    put(next/"files/Public/published.pkt","synthetic FTN packet bytes");
    refused([&]{auto files=host::FilePayloadDirectory::private_store(next/"files/Public",again);files.read("published.pkt");},"byte-identical replacement cannot inherit numeric identity");
}
}
int main(int argc,char** argv){try{check(argc==2||(argc==3&&(std::string_view(argv[2])=="--no-birth"||std::string_view(argv[2])=="--metadata-only")),"fixture path and optional metadata test mode required");without_birth=argc==3&&std::string_view(argv[2])=="--no-birth";metadata_only=argc==3&&std::string_view(argv[2])=="--metadata-only";run(argv[1]);ftn_identities(fs::path(argv[1])/"ftn-identities");std::cout<<"PASS "<<checks<<" native portable file identity/copy/Prepare/preservation checks; no-birth="<<without_birth<<" metadata-only="<<metadata_only<<'\n';return 0;}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
