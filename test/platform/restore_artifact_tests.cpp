#include "setup_engine.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include "sfx/platform/win32/file_backup.hpp"
#include <map>
#include <array>
#include <cstring>
#include <algorithm>
#include <iostream>
#include <Aclapi.h>
#include <sddl.h>
namespace fixture {
using namespace sfx::platform::win32::installation;
using sfx::apps::setup::Plan;
namespace fs=std::filesystem;namespace archive_engine=sfx::archive;
namespace platform=sfx::platform;
bool absent(const Path& path){std::error_code error;const auto exists=fs::exists(path,error);require(!error,"fixture_path_unavailable");return !exists;}
#include "restore_artifacts.inc"
}
namespace ins=sfx::platform::win32::installation;namespace arc=sfx::archive;
std::filesystem::path pref(std::filesystem::path p){p.make_preferred();return p;}
unsigned checks{};
void check(bool value,const char* label){++checks;ins::require(value,label);}
const std::wstring test_reader=L"S-1-5-21-314159-265358-979323-1901";
const std::wstring test_denied=L"S-1-5-21-314159-265358-979323-1902";
void set_fixture_acl(const std::filesystem::path& path,const std::wstring& text){
    auto held=ins::open_object(pref(path),std::filesystem::is_directory(path),READ_CONTROL|WRITE_DAC,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);
    void* raw{};check(ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(),SDDL_REVISION_1,&raw,nullptr),"encode retained fixture DACL");
    ins::Descriptor sd(raw);PACL acl{};BOOL present{},defaulted{};
    check(GetSecurityDescriptorDacl(sd.get(),&present,&acl,&defaulted)&&present&&acl,"read retained fixture DACL");
    check(SetSecurityInfo(held.get(),SE_FILE_OBJECT,DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,nullptr,nullptr,acl,nullptr)==ERROR_SUCCESS,"set only retained fixture DACL");
}
std::wstring fixture_acl(const std::filesystem::path& path){auto held=ins::open_object(pref(path),std::filesystem::is_directory(path),READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE);return sfx::platform::win32::BackupDaclSnapshot(held.get()).sddl();}
void seed_session_only_set(const std::filesystem::path& set){
    const auto text=L"D:P(D;;FR;;;"+test_denied+L")(A;;FA;;;SY)(A;;FA;;;BA)(A;;0x1200a9;;;S-1-5-5-0-271478)(A;;FR;;;"+test_reader+L")";
    for(const auto& entry:std::filesystem::recursive_directory_iterator(set/L"FileContents"))set_fixture_acl(entry.path(),text);
    set_fixture_acl(set/L"FileContents",text);set_fixture_acl(set/L"SFX-BACKUP-INFO.txt",text);set_fixture_acl(set/L"SFX-BACKUP-MANIFEST.txt",text);
}
std::map<std::filesystem::path,std::wstring> retained_acls(const std::filesystem::path& set){
    std::map<std::filesystem::path,std::wstring> result;
    for(const auto& entry:std::filesystem::recursive_directory_iterator(set/L"FileContents"))result.emplace(entry.path().lexically_relative(set),fixture_acl(entry.path()));
    for(const auto* name:{L"FileContents",L"SFX-BACKUP-INFO.txt",L"SFX-BACKUP-MANIFEST.txt"})result.emplace(name,fixture_acl(set/name));return result;
}
std::filesystem::path make_case(const std::filesystem::path& root,const std::wstring& who){
    auto sd=ins::mutable_descriptor(who);ins::create_directory(root,sd.get());
    for(const auto* leaf:{L"Backup",L"Work",L"Work/Seed",L"Work/Seed/FileContents",L"Work/Seed/FileContents/Objects",L"FileContents",L"FileContents/Objects"})ins::create_directory(pref(root/leaf),sd.get());
    const std::vector<std::uint8_t> payload{'l','e','g','a','c','y'};const auto hash=ins::sha256(payload);
    arc::ContentIndex index;index.areas.push_back({1,1,"Test area","Files/Test"});index.files.push_back({1,"test.zip",payload.size(),hash,{},{},"FileContents/Objects/"+hash});
    ins::create_file(pref(root/L"Work/Seed/FileContents/INDEX.txt"),arc::encode_content_index(index),sd.get());
    ins::create_file(pref(root/L"Work/Seed/FileContents/Objects"/hash),payload,sd.get());
    arc::CreateBackupRequest request;request.source_root=root/L"Work/Seed";request.partial_path=root/L"Work/legacy.partial";request.destination_path=root/L"Work/legacy.zip";request.included_root_names={"FileContents"};request.metadata.backup_type="integration-test";request.metadata.transaction_id="0123456789ABCDEF";request.metadata.sfx_version="1.0.0-alpha.2";request.metadata.source_generation="1";request.metadata.created=arc::current_timestamp();
    check(arc::create_backup(request).ok(),"create genuine legacy extraction reference");const auto archive=arc::inspect_backup(request.destination_path);check(archive.result.ok(),"reference backup manifest verified");
    for(const auto& file:archive.files){const auto member=arc::read_zip_member(request.destination_path,file.path);check(member.result.ok(),"reference member CRC verified");ins::create_file(pref(root/std::filesystem::u8path(file.path)),member.bytes,sd.get());}
    const auto manifest=arc::read_zip_member(request.destination_path,arc::backup_manifest_name);check(manifest.result.ok(),"archive self-manifest read separately");ins::create_file(root/L"SFX-BACKUP-MANIFEST.txt",manifest.bytes,sd.get());
    const auto info=arc::read_zip_member(request.destination_path,arc::backup_info_name);check(info.result.ok(),"archive info control read separately");ins::create_file(root/L"SFX-BACKUP-INFO.txt",info.bytes,sd.get());
    return pref(root/L"FileContents/Objects"/hash);
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2,"new isolated evidence root required");const auto base=pref(std::filesystem::absolute(argv[1]));check(!std::filesystem::exists(base),"preserve prior fixtures");std::filesystem::create_directories(base);const auto owner=ins::operator_sid();
    for(unsigned n=0;n<3;++n){const auto root=base/std::to_wstring(n);const auto object=make_case(root,owner);const auto bytes=ins::read_file(object,64);const auto hash=ins::file_sha256(pref(root/L"Work/legacy.zip"));
        sfx::apps::setup::Plan plan;plan.root=root;plan.transaction=L"0123456789ABCDEF0123456789ABCDEF";
        if(n==1){auto sd=ins::mutable_descriptor(owner);ins::create_file(pref(root/L"FileContents/unknown"),{'x'},sd.get());}
        if(n==2){auto held=ins::open_object(object,false,GENERIC_WRITE,0);DWORD written{};const char changed='X';check(WriteFile(held.get(),&changed,1,&written,nullptr)&&written==1,"seed corrupt loose object without changing archive");}
        fixture::relocate_restore_artifacts(plan,owner);
        if(n==0){const auto retained=root/L"Backup/RestoreFiles"/(L"Restore-"+plan.transaction);
            check(!std::filesystem::exists(root/L"FileContents")&&!std::filesystem::exists(root/L"SFX-BACKUP-INFO.txt")&&!std::filesystem::exists(root/L"SFX-BACKUP-MANIFEST.txt"),"verified loose set leaves top level");
            check(ins::read_file(pref(retained/L"FileContents/Objects"/object.filename()),64)==bytes,"exact payload retained under approved destination");
            check(std::filesystem::exists(retained/L"SFX-BACKUP-MANIFEST.txt"),"index/control records retained with payload");fixture::relocate_restore_artifacts(plan,owner);
            check(std::distance(std::filesystem::directory_iterator(root/L"Backup/RestoreFiles"),std::filesystem::directory_iterator{})==1,"repeat is a no-op with no duplicate set");
        }else check(std::filesystem::exists(root/L"FileContents")&&!std::filesystem::exists(root/L"Backup/RestoreFiles"),"unrecognized or changed loose payload preserved in place");
        check(ins::file_sha256(pref(root/L"Work/legacy.zip"))==hash,"original backup ZIP untouched");
    }
    // Setup's real elevated role can read the original Admin/SYSTEM-only set.
    // The separate SID-only suite proves the repaired owner does not need that
    // administrative role for backup afterwards.
    check(ins::elevated(),"extended observed-ACL cases require Setup's real elevated test role");
    for(unsigned n=3;n<7;++n){const auto root=base/std::to_wstring(n);auto object=make_case(root,owner);
        sfx::apps::setup::Plan plan;plan.root=root;plan.transaction=L"0123456789ABCDEF0123456789ABCDEF";
        auto set=root;
        if(n==3){fixture::relocate_restore_artifacts(plan,owner);set=pref(root/L"Backup/RestoreFiles"/(L"Restore-"+plan.transaction));object=pref(set/L"FileContents/Objects"/object.filename());
            const auto parent=root/L"Backup/RestoreFiles";
            set_fixture_acl(parent,L"D:P(A;OICI;0x1301ff;;;"+owner+L")(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;"+test_reader+L")");}
        seed_session_only_set(set);const auto before=retained_acls(set);const auto original=ins::read_file(object,64);
        auto object_pin=ins::open_object(object,false,READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_DELETE);const auto original_id=ins::file_id(object_pin.get());object_pin.reset();
        if(n==5||n==6){bool refused{};
            try{fixture::retain_restore_artifact_set(plan,owner,set,false,[&](std::size_t at){
                if(n==5&&at==0)throw std::runtime_error("injected_partial_acl_failure");
                if(n==6&&at+1==before.size()){auto sd=ins::mutable_descriptor(owner);
                    ins::create_file(pref(root/L"Backup/RestoreFiles"/(L"Restore-"+plan.transaction)/L"SFX-BACKUP-INFO.txt"),{'x'},sd.get());}
            });}catch(const std::exception&){refused=true;}
            check(refused,"injected ACL or partial rename failure refused");
            check(retained_acls(set)==before,"every original custom DACL restored after ACL/rename failure");
            check(ins::read_file(object,64)==original,"rollback leaves original payload bytes exact");
            check(std::filesystem::exists(root/L"FileContents")&&std::filesystem::exists(root/L"SFX-BACKUP-INFO.txt"),"rollback preserves original source names");
            continue;
        }
        fixture::relocate_restore_artifacts(plan,owner);const auto retained=pref(root/L"Backup/RestoreFiles"/(L"Restore-"+plan.transaction));
        for(const auto& row:before){auto held=ins::open_object(retained/row.first,std::filesystem::is_directory(retained/row.first),READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_DELETE);
            check(sfx::platform::win32::backup_owner_has_read(held.get(),owner,std::filesystem::is_directory(retained/row.first)),"stable owner read established for every retained object");
            const auto after=fixture_acl(retained/row.first);check(after.find(test_reader)!=std::wstring::npos&&after.find(test_denied)!=std::wstring::npos&&after.find(L"S-1-5-5-0-271478")!=std::wstring::npos,"original reader,deny and session ACEs retained");}
        const auto retained_object=pref(retained/L"FileContents/Objects"/object.filename());auto held=ins::open_object(retained_object,false,GENERIC_READ,FILE_SHARE_READ);
        check(ins::file_id(held.get())==original_id&&ins::read_file(retained_object,64)==original,"retained payload identity and bytes unchanged");held.reset();
        const auto stable=retained_acls(retained);fixture::relocate_restore_artifacts(plan,owner);check(retained_acls(retained)==stable,"already-retained repeat preserves exact final ACLs");
        bool journal{};for(const auto& entry:std::filesystem::directory_iterator(retained))if(entry.path().filename().native().find(L"SFX-RETAINED-ACL-BEFORE-")==0)journal=true;
        check(journal,"private durable original-ACL journal retained");
    }
    std::cout<<"PASS "<<checks<<" loose/existing retained sets, stable owner/custom ACLs, identity, repeat, scope and ACL/rename rollback checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}
