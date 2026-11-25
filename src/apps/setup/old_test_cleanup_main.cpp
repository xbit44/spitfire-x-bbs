// Owner-run one-time cleanup companion. No general recovery/legacy UI and no
// raw SQL writes. The existing offline owner guard and exact journaled file
// operation implement deletion and preserve immutable catalog history.
#include "sfx/platform/win32/offline_file_administration.hpp"
#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/archive/archive_engine.hpp"
#include <objbase.h>
#include <algorithm>
#include <iostream>
#include <set>
#include <string>
namespace app=sfx::application;namespace native=sfx::platform::win32;namespace ins=native::installation;
namespace {
app::Identifier128 fresh(){app::Identifier128 value;ins::require(CoCreateGuid(reinterpret_cast<GUID*>(value.bytes.data()))==S_OK,"cleanup_identifier_failed");return value;}
bool same(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b){return a.bytes==b.bytes&&a.sha256==b.sha256&&a.volume==b.volume&&a.file_id==b.file_id;}
}
int wmain(int argc,wchar_t** argv)try{
    ins::require((argc==3||argc==7)&&!ins::elevated(),"Usage: SFX-OldTestCleanup.exe SFX-root --preview [key1 key2 key3 key4] or --delete-four key1 key2 key3 key4. Start normally with SFX stopped.");
    const std::wstring mode=argv[2];ins::require(mode==L"--preview"||mode==L"--delete-four","cleanup_explicit_mode_required");
    ins::require(mode==L"--preview"||argc==7,"cleanup_deletion_requires_four_explicit_reviewed_keys");
    std::set<std::string> keys;
    for(int i=3;i<argc;++i){const std::wstring wide=argv[i];ins::require(wide.size()==32&&wide.find_first_not_of(L"0123456789abcdef")==wide.npos,"cleanup_exact_legacy_key_required");std::string key;for(const auto c:wide)key+=static_cast<char>(c);keys.insert(std::move(key));}
    ins::require(keys.empty()||keys.size()==4,"cleanup_four_distinct_keys_required");
    const auto root=ins::canonical_root(argv[1]);const auto owner=ins::operator_sid();
    std::string diagnostic;auto session=native::OfflineFileAdministration::begin(root,owner,diagnostic);
    ins::require(bool(session),diagnostic.empty()?"cleanup_offline_owner_unavailable":diagnostic.c_str());
    app::FileManagementQuery query;query.state=app::FileCatalogState::removed;
    std::vector<app::ManagedFile> selected;app::FileId cursor;
    for(unsigned page=0;page<1000;++page){const auto result=session->page(query,cursor,100);ins::require(result&&result.value,"cleanup_catalog_read_failed");
        for(const auto& row:result.value->files)if(keys.count(row.removed_key)||(keys.empty()&&!row.purged&&app::file_name_matches(row.file.name,"20meg*.zip")))selected.push_back(row);
        if(!result.value->next.value)break;cursor=result.value->next;ins::require(page!=999,"cleanup_catalog_limit");}
    ins::require(selected.size()==4,"cleanup_four_exact_catalog_matches_required_no_files_deleted");
    auto directory=native::FilePayloadDirectory::private_store(root/L"Data"/L"FileRemoved",owner);
    std::vector<native::FilePayloadLease> pinned;std::uint64_t total{};unsigned already{};
    for(const auto& row:selected){
        ins::require(!row.operation_pending&&row.file.state==app::FileCatalogState::removed,"cleanup_pending_or_active_file_refused");
        if(row.purged){ins::require(directory.absent(row.removed_key),"cleanup_purged_key_reoccupied_preserved");++already;continue;}
        ins::require(app::file_name_matches(row.file.name,"20meg*.zip"),"cleanup_test_archive_name_mismatch");
        auto file=directory.read(row.removed_key);ins::require(same(file.identity(),row.payload),"cleanup_native_identity_or_hash_changed");
        const auto zip=sfx::archive::inspect_zip(file.path());ins::require(zip.result.ok()&&zip.files.size()==1&&zip.directory_count==0&&zip.files[0].path=="dummy20.bin"&&zip.files[0].size==20971520,"cleanup_exact_test_zip_contents_required");
        const auto contents=sfx::archive::read_zip_member(file.path(),"dummy20.bin",20971520);ins::require(contents.result.ok()&&contents.bytes.size()==20971520,"cleanup_test_zip_crc_or_size_failed");
        total+=row.payload.bytes;std::cout<<row.removed_key<<"  "<<row.file.name<<"  "<<row.payload.bytes<<" bytes\n";pinned.push_back(std::move(file));
    }
    std::cout<<"Verified "<<(4-already)<<" retained test archives; "<<already<<" already deleted. Bytes eligible: "<<total<<"\n";
    if(mode==L"--preview"||already==4){std::cout<<"No files deleted by this run.\n";
        if(already!=4){const auto quote=[](const std::string& input){std::string out="'";for(auto c:input){out+=c;if(c=='\'')out+='\'';}return out+"'";};
            std::cout<<"After checking these are the four old test files, run in PowerShell:\n& "<<quote(ins::module_path().u8string())<<' '<<quote(root.u8string())<<" --delete-four";for(const auto& row:selected)std::cout<<' '<<row.removed_key;std::cout<<'\n';}
        return 0;}
    std::cout<<"Delete only the verified files above? Type DELETE FOUR: "<<std::flush;std::string confirm;std::getline(std::cin,confirm);
    ins::require(confirm=="DELETE FOUR","cleanup_cancelled_no_files_deleted");pinned.clear();
    unsigned completed{};
    for(const auto& row:selected){if(row.purged)continue;
        app::FileManagementTask task;task.job=fresh();task.operation.operation=fresh();task.operation.file=row.file.id;task.operation.expected_revision=row.file.revision;task.operation.action=app::FileOperationAction::purge;
        auto status=session->start(task);const auto started=GetTickCount64();
        while(status.state==app::FileManagementJobState::running&&GetTickCount64()-started<300000){Sleep(25);status=session->status(task.job);}
        ins::require(status.state==app::FileManagementJobState::completed&&status.journal_confirmed,"cleanup_unconfirmed_stop_do_not_blindly_repeat");
        const auto after=session->detail(row.file.id);ins::require(after&&after.value&&after.value->purged&&directory.absent(row.removed_key),"cleanup_deletion_not_confirmed");++completed;
    }
    std::cout<<"Confirmed deleted: "<<completed<<"; reclaimed "<<total<<" bytes. Catalog history retained.\n";return 0;
}catch(const std::exception& error){std::cerr<<"Stopped: "<<error.what()<<"\nNo unrelated file is eligible. Retain this output if a deletion was unconfirmed.\n";return 1;}
