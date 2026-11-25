// Existing preparation primitives exercised on a fresh owned copied fixture.
// No registry, service, full Prepare or installed-generation claim.
#include "ftn_external_files.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include "../../src/platform/win32/src/portable_preparation.cpp"
#include "sfx/platform/win32/ftn_secrets.hpp"
namespace native=sfx::platform::win32;
#else
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include "sfx/platform/linux/installation_layout.hpp"
namespace native=sfx::platform::linux_host;
#endif
namespace app=sfx::application;
namespace ext=sfx::platform::external_ftn;
namespace fs=std::filesystem;
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
fs::path physical(std::string_view path){
#ifdef _WIN32
    return fs::u8path(path);
#else
    return native::layout::physical_relative(path);
#endif
}
int main(int argc,char** argv){try{
    if(argc==3&&std::string(argv[2])=="--seed-fixture"){
        const auto seed=fs::absolute(argv[1]).lexically_normal();check(seed.filename()=="Prepared Source Fixture","fresh isolated seed fixture only");
#ifdef _WIN32
        const auto result=native::refresh_portable_file_snapshot(seed,native::installation::operator_sid(),true);
        if(!result.published)for(const auto& why:result.diagnostics)std::cerr<<why<<'\n';
        check(result.published&&!result.deferred,"normal fresh fixture identity snapshot");
#else
        check(bool(native::LinuxFilePayloadBindings::initialize_fresh(seed)),"normal fresh fixture identity snapshot");
#endif
        std::cout<<"PASS separate fixture initialization\n";return 0;
    }
    check(argc==2,"existing isolated copied fixture required");const auto root=fs::absolute(argv[1]).lexically_normal();
    check(root.filename()=="Copied Board With Spaces"&&fs::is_regular_file(root/physical("Data/SFX.DB")),"exact copied test root");
#ifdef _WIN32
    const auto owner=native::installation::operator_sid();const auto content=native::installation::mutable_descriptor(owner);
    std::vector<native::Object> objects;native::collect(root/"FTN",objects,{});check(objects.size()>20,"normal preparation inventory includes all External descendants");
    for(auto& object:objects)native::set_access(object,content.get());objects.clear();
    // Config is copied by the test driver under current-owner private access.
    const auto prepared=native::prepare_copied_file_bindings(root,owner);
    if(!prepared.published||prepared.deferred)for(const auto& why:prepared.diagnostics)std::cerr<<why<<'\n';
    check(prepared.published&&!prepared.deferred,"existing stopped copied-file binding preparation succeeds");
#else
    const auto prepared=native::LinuxFilePayloadBindings::prepare(root);check(prepared&&prepared->deferred_records()==0,"existing stopped Linux copied-file binding preparation succeeds");
#endif
    unsigned records{};
    for(const auto& entry:fs::directory_iterator(root/physical("FTN/External/Records"))){
        if(entry.path().extension()!=".rec")continue;
        const auto relative=entry.path().lexically_relative(root).generic_u8string();const auto record=app::decode_ftn_external_receive(ext::read_small(root,relative));check(record.has_value(),"copied exact receive record decodes");
        std::uint64_t size{};ext::read_completed(root,*record,[&](auto bytes){size+=bytes.size();},[]{return true;});check(size==record->size,"copied raw original matches full recorded digest");++records;
    }
    check(records==5&&ext::pending_records(root,"").empty(),"all five original consumed records remain consumed after copy");
    const auto profile=root/physical("FTN/External/binkd-test.cfg");std::fstream owner_access(profile,std::ios::binary|std::ios::in|std::ios::out);check(bool(owner_access),"ordinary Sysop can read and write copied profile after access rebase");owner_access.close();
    sfx::test::FakeStoragePathPolicy paths;sfx::adapters::storage::SqliteStorageSettings settings;settings.data_directory=root/physical("Data");
#ifndef _WIN32
    settings.layout_profile=sfx::adapters::storage::SqliteStorageLayoutProfile::linux_lowercase_v1;
#endif
    sfx::adapters::storage::SqliteStorage db(settings,paths);check(db.start().succeeded(),"copied current database opens");
    const app::MessageConfigurationActor owner_actor{true,{}};const auto c=db.ftn_configuration(owner_actor);check(c&&c.value&&c.value->settings.mode==app::FtnMailerMode::external_binkd,"copied external configuration preserved");
    const auto& link=c.value->links.front();const auto packet=native::unprotect_ftn_secret(root.u8string(),link.network,link.code,native::FtnCredentialKind::packet,link.credentials.packet);const auto area=native::unprotect_ftn_secret(root.u8string(),link.network,link.code,native::FtnCredentialKind::areafix,link.credentials.areafix);
    check(packet&&area&&packet->view()=="PACKET12"&&area->view()=="AREA-KEY","separate portable credentials survive exact whole-folder copy");
    const auto handoff=db.pending_ftn_external_handoff(owner_actor);check(handoff&&handoff.value->empty(),"committed and released reply is never republished by copied database");
    const auto queue=db.ftn_queue_page(owner_actor,0);check(queue&&queue.value->outbound[5]==1&&queue.value->outbound[3]==0,"copied reply keeps Handed to BinkD without fake Delivered");check(db.shutdown().succeeded(),"copied store closed cleanly");
    const auto run=ext::prepare_run(root,"FTN/External/binkd-test.cfg",
#ifdef _WIN32
        "Bin/SFXBinkD.exe"
#else
        "bin/sfxbinkd"
#endif
    );check(!run.id.empty()&&fs::is_regular_file(root/fs::u8path(run.configuration_path)),"new private run uses same relative profile under moved root with spaces");
    std::cout<<"PASS External copied folder / existing access and custody preparation "<<checks<<" checks; no service registration or full Windows Prepare\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
