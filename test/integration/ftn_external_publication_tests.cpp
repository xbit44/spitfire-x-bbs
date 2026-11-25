// Actual private files and SQLite handoff recovery; source callback is a
// controlled host observation, not a Native lease or stock-mailer exchange.
#define main retained_external_file_fixture_main
#include "file_storage_tests.cpp"
#undef main
#include "ftn_external_files.hpp"
#include <fstream>
#include <sodium.h>
#ifndef _WIN32
#include "sfx/test/linux_layout_fixture.hpp"
#endif
namespace ext=sfx::platform::external_ftn;
namespace {
std::filesystem::path physical(std::string_view path){
#ifdef _WIN32
    return std::filesystem::u8path(path);
#else
    return sfx::platform::linux_host::layout::physical_relative(path);
#endif
}
template<class F>void refuses(F f,const char* why){bool refused=false;try{f();}catch(const std::exception&){refused=true;}check(refused,why);}
std::string hex(const app::Identifier128& value){std::string out;for(auto b:value.bytes){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;}
void make_private(const std::filesystem::path& p){std::filesystem::create_directories(p);
#ifndef _WIN32
    std::filesystem::permissions(p,std::filesystem::perms::owner_all);
#endif
}
void run(const std::filesystem::path& root,std::string_view fault,sfx::test::FakeStoragePathPolicy& paths){
    make_private(root);make_private(root/physical("FTN"));
#ifndef _WIN32
    make_private(root/"config");sfx::test::mark_provisioned_linux_fixture(root);
#endif
    ext::prepare_profile(root,"TEST");
    make_private(root/physical("Data"));auto configured=settings(root/physical("Data"));
#ifndef _WIN32
    configured.layout_profile=storage::SqliteStorageLayoutProfile::linux_lowercase_v1;
#endif
    storage::SqliteStorage db(configured,paths);check(db.start().succeeded(),"publication store opens");
    const app::MessageConfigurationActor owner{true,{}};auto c=*db.ftn_configuration(owner).value;c.settings.mode=app::FtnMailerMode::external_binkd;
    app::FtnNetwork n;n.code="TEST";n.name="Publication fixture";n.address="2:100/1@test";n.enabled=true;c.networks.push_back(n);
    app::FtnLink l;l.network=n.code;l.code="PEER";l.name="Peer";l.address="2:100/2@test";l.enabled=true;c.links.push_back(l);
    check(db.change_ftn_configuration(owner,id(10).bytes,c).succeeded(),"external fixture configured");c=*db.ftn_configuration(owner).value;
    const std::string payload="retained original payload bytes",tic="File payload.zip\r\nArea TEST.FILES\r\n";
    app::FtnOutboundJob p;p.id=id(20);p.operation=id(22);p.network=n.code;p.link=l.code;p.kind=app::FtnOutboundKind::payload;p.file={"payload.zip",payload.size(),1700000000};crypto_hash_sha256(p.digest.data(),reinterpret_cast<const unsigned char*>(payload.data()),payload.size());
    auto t=p;t.id=id(21);t.ordinal=1;t.kind=app::FtnOutboundKind::tic;t.file={"control.tic",tic.size(),1700000000};crypto_hash_sha256(t.digest.data(),reinterpret_cast<const unsigned char*>(tic.data()),tic.size());
    check(db.queue_ftn_jobs(owner,c.revision,{p,t}).succeeded(),"actual queue transaction");
    const auto claim=id(31);check(db.claim_ftn_jobs(owner,n.code,l.code,id(30),claim,64,true).value->size()==2,"actual same-link claim");
    auto batch=*db.prepare_ftn_external_handoff(owner,c.revision,claim).value;
    const auto flow=batch[0].flow_path,busy=batch[0].busy_path;
    ext::publish_once(root,flow,"existing-transfer.zip\r\n");
    bool partial=fault=="partial-copy";unsigned reads{};
    const ext::ReadOutbound source=[&](const auto& job,const auto& consume){
        ++reads;const auto& text=job.kind==app::FtnOutboundKind::payload?payload:tic;
        consume(std::string_view(text).substr(0,7));
        if(partial){partial=false;throw std::runtime_error("injected interrupted source read");}
        consume(std::string_view(text).substr(7));
    };
    const auto running=[]{return true;};
    if(fault=="foreign-busy"){
        ext::publish_once(root,busy,"12345\n");auto result=ext::publish_handoff(root,db,batch,source,running);
        check(result.busy&&!result.published&&!result.uncertain&&reads==0,"foreign busy prevents all outgoing writes");
        check(ext::read_small(root,busy)=="12345\n"&&ext::read_small(root,flow)=="existing-transfer.zip\r\n","foreign busy and prior flow preserved");
        check(db.shutdown().succeeded(),"busy fixture stopped with pending work retained");return;
    }
    const bool policy=fault=="disabled-link"||fault=="disabled-network"||fault=="retargeted-link"||fault=="disabled-mode";
    const auto injected=fault=="lost-lock"||policy?std::string_view("after-publication-intent"):fault;
    refuses([&]{ext::publish_handoff(root,db,batch,source,running,[&](auto at){if(at==injected)throw std::runtime_error("injected process boundary");});},"publication interruption retained");
    const auto journal="FTN/External/test/Journal/"+hex(claim)+".intent";
    check(std::filesystem::exists(root/physical(journal)),"durable intent precedes every interrupted write");
    check(db.shutdown().succeeded()&&db.start().succeeded(),"actual database restart after interruption");
    batch=*db.pending_ftn_external_handoff(owner).value;check(batch.size()==2,"recovery returns complete exact batch");
    if(policy){
        auto altered=*db.ftn_configuration(owner).value;
        if(fault=="disabled-link")altered.links.front().enabled=false;
        if(fault=="disabled-network")altered.networks.front().enabled=false;
        if(fault=="retargeted-link")altered.links.front().address="2:100/3@test";
        if(fault=="disabled-mode")altered.settings.mode=app::FtnMailerMode::disabled;
        check(db.change_ftn_configuration(owner,id(40).bytes,altered).succeeded(),"policy changes after interrupted preparation");
        const auto before=reads;
        refuses([&]{ext::publish_handoff(root,db,batch,source,running);},"changed policy prevents uncommitted handoff recovery");
        check(reads==before&&ext::read_small(root,flow)=="existing-transfer.zip\r\n"&&std::filesystem::exists(root/physical(busy)),"held policy preserves original flow and owned recovery lock without reading payload");
        c.revision=db.ftn_configuration(owner).value->revision;
        check(db.change_ftn_configuration(owner,id(41).bytes,c).succeeded(),"owner can restore exact destination policy");
    }
    if(fault=="lost-lock"){
        std::filesystem::remove(root/physical(busy));
        auto result=ext::publish_handoff(root,db,batch,source,running);
        check(result.uncertain&&!result.published&&reads==0,"missing lock after durable intent is held without replay");
        auto page=db.ftn_queue_page(owner,0);check(page&&page.value->outbound[4]==2,"uncertain files remain visible together");
        check(ext::read_small(root,flow)=="existing-transfer.zip\r\n","uncertain case never exposes fresh flow");
        check(db.shutdown().succeeded(),"uncertain fixture stopped");return;
    }
    const bool committed=fault=="after-handoff-commit"||fault=="after-busy-release";
    if(committed){
        check(batch[0].state==1,"committed handoff recovered as release-only");
        auto disabled=*db.ftn_configuration(owner).value;disabled.settings.mode=app::FtnMailerMode::disabled;
        check(db.change_ftn_configuration(owner,id(42).bytes,disabled).succeeded(),"disabled mode still permits committed lock cleanup");
        // Simulate BinkD consuming its disposable copies after a released lock.
        if(fault=="after-busy-release"){
            for(const auto& row:batch)std::filesystem::remove(root/physical(row.copy_path));
            std::filesystem::remove(root/physical(flow));ext::publish_once(root,busy,"next-mailer-owner\n");
        }
    }
    const auto before_reads=reads;
    auto result=ext::publish_handoff(root,db,batch,source,running);
    check(result.published&&!result.busy&&!result.uncertain,"exact interrupted publication recovers");
    if(committed)check(reads==before_reads,"committed handoff never reads or recopies original");
    if(fault=="after-busy-release"){
        check(ext::read_small(root,busy)=="next-mailer-owner\n","later mailer busy file never removed");
        check(!std::filesystem::exists(root/physical(flow))&&!std::filesystem::exists(root/physical(batch[0].copy_path)),"consumed handoff never reappears");
    }else{
        check(!std::filesystem::exists(root/physical(busy)),"own lock released after database commit");
        const auto expected="existing-transfer.zip\r\n^"+physical(batch[0].copy_path).generic_string()+"\r\n^"+physical(batch[1].copy_path).generic_string()+"\r\n";
        check(ext::read_small(root,flow)==expected,"prior flow preserved and each payload precedes TIC exactly once");
        check(ext::read_small(root,batch[0].copy_path)==payload&&ext::read_small(root,batch[1].copy_path)==tic,"disposable copies contain exact original bytes");
    }
    check(db.pending_ftn_external_handoff(owner).value->empty(),"finished lock release leaves no replay work");
    auto page=db.ftn_queue_page(owner,0);check(page&&page.value->outbound[5]==2&&page.value->outbound[3]==0&&page.value->rows.empty(),"handoff is recorded separately from delivery");
    check(scalar(root/physical("Data/SFX.DB"),"SELECT count(*) FROM pragma_foreign_key_check")==0,"recovered store references clean");
    check(db.shutdown().succeeded(),"publication fixture stopped");
}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh fixture root required");const auto root=std::filesystem::absolute(argv[1]).lexically_normal();check(!std::filesystem::exists(root),"retain earlier cases");
    sfx::test::FakeStoragePathPolicy paths;
    for(const auto* fault:{"after-publication-intent","partial-copy","after-transfer-copy","after-flow-publication","before-handoff-commit","after-handoff-commit","after-busy-release","foreign-busy","lost-lock","disabled-link","disabled-network","retargeted-link","disabled-mode"})run(root/fault,fault,paths);
    std::cout<<"PASS external FTN publication "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
