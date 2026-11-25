#include "ftn_external_files.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#include "sfx/platform/win32/storage_dacl_policy.hpp"
#include <windows.h>
#include <sddl.h>
#else
#include "sfx/platform/linux/installation_layout.hpp"
#endif
namespace app=sfx::application;
namespace ext=sfx::platform::external_ftn;
namespace {
unsigned checks{};
#ifdef _WIN32
constexpr auto style=app::FtnExternalPathStyle::canonical;
constexpr const char* recorder="Bin/SFXBinkD";
#else
constexpr auto style=app::FtnExternalPathStyle::linux_lowercase_v1;
constexpr const char* recorder="bin/sfxbinkd";
#endif
std::filesystem::path physical(std::string_view path){
    if constexpr(style==app::FtnExternalPathStyle::canonical)return std::filesystem::u8path(path);
    if(path=="FTN")return "ftn";
    if(path.substr(0,13)=="ftn/external/")return std::filesystem::u8path(path);
    auto mapped=app::ftn_external_physical_path(path,style);
    if(mapped.empty()){
        // Keep deliberate unsupported-payload negative fixtures intact.
        const auto slash=path.rfind('/');
        if(slash==path.npos)return std::filesystem::u8path(path);
        const auto parent=app::ftn_external_physical_path(path.substr(0,slash),style);
        if(!parent.empty())return std::filesystem::u8path(parent)/std::filesystem::u8path(path.substr(slash+1));
    }
    return std::filesystem::u8path(mapped);
}
std::string native_profile(std::string text){
    if constexpr(style==app::FtnExternalPathStyle::canonical)return text;
    for(const auto path:{"FTN/External/test/Outbound","FTN/External/Inbound/Secure","FTN/External/Inbound/Nonsecure","FTN/External/Temp"}){
        const auto native=app::ftn_external_physical_path(path,style);
        for(auto at=text.find(path);at!=text.npos;at=text.find(path,at+native.size()))text.replace(at,std::string_view(path).size(),native);
    }
    return text;
}
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>void refuses(F operation,const char* why){bool refused=false;try{operation();}catch(const std::exception&){refused=true;}check(refused,why);}
void write(const std::filesystem::path& path,std::string_view text){std::ofstream file(path,std::ios::binary|std::ios::trunc);file.write(text.data(),static_cast<std::streamsize>(text.size()));file.close();check(bool(file),"fixture bytes written");
#ifndef _WIN32
    std::filesystem::permissions(path,std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
#endif
}
void run(const std::filesystem::path& root){
    check(!std::filesystem::exists(root),"fresh fixture required; earlier evidence retained");
    std::filesystem::create_directories(root/physical("FTN"));
#ifdef _WIN32
    check(sfx::platform::win32::WindowsStorageDaclPolicy(sfx::platform::win32::current_process_user_sid_string()).apply(root/physical("FTN"),sfx::platform::win32::StorageDaclObjectKind::directory).succeeded(),"fresh fixture FTN private defaults");
#endif
#ifndef _WIN32
    std::filesystem::permissions(root,std::filesystem::perms::owner_all);
    std::filesystem::permissions(root/physical("FTN"),std::filesystem::perms::owner_all);
    sfx::platform::linux_host::layout::initialize_or_verify(root);
#endif
    ext::prepare_profile(root,"TEST");ext::prepare_profile(root,"TEST");
    const auto relative="FTN/External/test/Inbound/Secure/mail.pkt";
    const auto incoming=root/physical(relative);write(incoming,"packet-bytes");
    const std::vector<app::FtnAddress> akas{*app::parse_ftn_address("2:100/2@test")};
    const auto r=ext::record_completed(root,"TEST",relative,false,akas);
    check(r.size==12&&!r.protected_session,"legacy unprotected local context does not gain authentication");
    const auto duplicate=ext::record_completed(root,"TEST",relative,false,akas);check(duplicate.id==r.id,"repeated exact hook is idempotent");
    auto pending=ext::pending_records(root,"TEST");check(pending.size()==1&&pending[0].digest==r.digest,"one complete atomic receive record visible");
    write(root/physical("FTN/External/test/Records/00000000000000000000000000000000.rec"),"malformed");unsigned issues{};
    pending=ext::pending_records(root,"TEST",64,[&](auto){++issues;});
    check(issues==1&&pending.size()==1&&pending[0].id==r.id,"malformed earlier record reported without starving later valid record");
    std::string received;ext::read_completed(root,r,[&](auto bytes){received+=bytes;},[]{return true;});check(received=="packet-bytes","same recorded bytes read for custody");
    refuses([&]{ext::read_completed(root,r,[](auto){},[]{return false;});},"cancelled read stops before custody completion");
    const auto unbound=ext::record_completed(root,"TEST",relative,true,akas);
    check(unbound.protected_session&&unbound.id!=r.id,"legacy P=1 does not equal unprotected context");
    check(ext::pending_records(root,"TEST").size()==1,"unbound legacy P=1 retained instead of trusted");ext::mark_record_consumed(root,unbound);
    refuses([&]{ext::record_completed(root,"OTHER",relative,true,akas);},"wrong profile cannot receive file");
    refuses([&]{ext::record_completed(root,"TEST","../mail.pkt",true,akas);},"hook traversal refused");
    write(incoming,"changed-data");refuses([&]{ext::read_completed(root,r,[](auto){},[]{return true;});},"same-size changed payload cannot become recorded custody");
    write(incoming,"packet-bytes");ext::mark_record_consumed(root,r);ext::mark_record_consumed(root,r);
    check(ext::pending_records(root,"TEST").empty(),"consumption marker prevents repeat import");
    check(std::filesystem::exists(incoming),"recorder and consumption marker retain received original");
    ext::publish_once(root,"FTN/External/test/Journal/exact","one");
    refuses([&]{ext::publish_once(root,"FTN/External/test/Journal/exact","two");},"conflicting durable record preserved");
    check(ext::read_small(root,"FTN/External/test/Journal/exact")=="one","original record unchanged");
    write(root/physical("FTN/External/test/Journal/partial.tmp"),"incomplete");
    refuses([&]{ext::publish_once(root,"FTN/External/test/Journal/partial","complete");},"interrupted different stage retained");
    check(!std::filesystem::exists(root/physical("FTN/External/test/Journal/partial")),"partial metadata not published");
    refuses([&]{ext::publish_once(root,"FTN/Outbound/original.pkt","wrong");},"publication cannot target canonical originals");
    const std::string profile="domain test FTN/External/test/Outbound 2\naddress 2:100/1@test\nlisten 127.0.0.1:24554\ninbound FTN/External/Inbound/Secure\ninbound-nonsecure FTN/External/Inbound/Nonsecure\ntemp-inbound FTN/External/Temp\nnode 2:100/2@test -nr 127.0.0.1:24555 synthetic-only-key i - -\n";
    ext::publish_once(root,"FTN/External/stock.cfg",native_profile(profile));
#ifdef _WIN32
    const auto sid=sfx::platform::win32::current_process_user_sid_string();
    const auto broad=L"D:P(A;;FA;;;"+sid+L")(A;;FA;;;SY)(A;;FA;;;BA)(A;;FR;;;WD)";PSECURITY_DESCRIPTOR descriptor{};
    check(ConvertStringSecurityDescriptorToSecurityDescriptorW(broad.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"synthetic broad-read profile descriptor");
    const bool changed=SetFileSecurityW((root/physical("FTN/External/stock.cfg")).c_str(),DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,descriptor)!=FALSE;LocalFree(descriptor);check(changed,"only new fixture profile made public for negative check");
#else
    std::filesystem::permissions(root/physical("FTN/External/stock.cfg"),std::filesystem::perms::owner_read|std::filesystem::perms::owner_write|std::filesystem::perms::group_read|std::filesystem::perms::others_read);
#endif
    refuses([&]{ext::prepare_run(root,"FTN/External/stock.cfg",recorder);},"publicly readable secret input refused before snapshot");
#ifdef _WIN32
    check(sfx::platform::win32::WindowsStorageDaclPolicy(sid).apply(root/physical("FTN/External/stock.cfg"),sfx::platform::win32::StorageDaclObjectKind::file).succeeded(),"restore only fixture profile private defaults");
#else
    std::filesystem::permissions(root/physical("FTN/External/stock.cfg"),std::filesystem::perms::owner_read|std::filesystem::perms::owner_write);
#endif
    const auto prepared=ext::prepare_run(root,"FTN/External/stock.cfg",recorder);
    const auto shared="FTN/External/Inbound/Secure/shared.pkt";write(root/physical(shared),"recorded shared bytes");
    auto aliases=akas;aliases.push_back(*app::parse_ftn_address("2:100/99@test"));
    const auto bound=ext::record_run_completed(root,prepared.id,shared,true,aliases);
    check(bound.network.empty()&&bound.remote.size()==2&&bound.authenticated_remote.size()==1&&bound.run==prepared.id,"shared run trusts only explicit protected alias");
    check(ext::record_run_completed(root,prepared.id,shared,true,aliases).id==bound.id,"exact run hook replay stays idempotent");
    check(ext::record_run_completed(root,prepared.id,physical(shared).generic_u8string(),true,aliases).id==bound.id,"native hook path maps to the same canonical receive identity without duplicate publication");
    check(ext::pending_records(root,"").size()==1,"run manifest qualifies shared record");
    const auto manifest_path="FTN/External/Runs/"+prepared.id+"/Manifest";const auto manifest=ext::read_small(root,manifest_path,65536);
    std::map<std::string,std::string> audit;
    const auto inspect=[&]{audit.clear();ext::audit_inbox(root,[&](auto name,auto reason){audit[std::string(name)]=std::string(reason);});};
    inspect();check(audit[shared].empty(),"valid recorded completed file has no missing-metadata warning");
    const auto orphan="FTN/External/Inbound/Nonsecure/orphan.pkt";write(root/physical(orphan),"no receive metadata");
    write(root/physical("FTN/External/Temp/in-progress.dt"),"unfinished download");
    write(root/physical("FTN/External/Inbound/Nonsecure/unsupported;.pkt"),"unsupported hook filename");
    inspect();check(audit[orphan]=="ftn_external_hook_metadata_missing"&&audit["FTN/External/Inbound/Nonsecure/unsupported;.pkt"]=="ftn_external_hook_filename_unsupported","missing and unsupported hook files visible without granting custody");
    check(audit.size()==3&&ext::pending_records(root,"").size()==1,"diagnostic excludes partial downloads and creates no record");
    const auto recovered=ext::record_run_completed(root,prepared.id,orphan,false,akas);inspect();check(audit[orphan].empty(),"real later hook metadata clears diagnostic");ext::mark_record_consumed(root,recovered);
    check(std::filesystem::exists(root/physical(orphan))&&std::filesystem::exists(root/physical("FTN/External/Inbound/Nonsecure/unsupported;.pkt"))&&std::filesystem::exists(root/physical("FTN/External/Temp/in-progress.dt")),"audit never deletes or moves any file");
    check(manifest.find("synthetic-only-key")==manifest.npos,"nonsecret run manifest contains no password");
    const auto active_profile=ext::read_small(root,prepared.configuration_path,65536);
    write(root/prepared.configuration_path,active_profile+"# changed after launch\n");
    refuses([&]{ext::record_run_completed(root,prepared.id,shared,true,aliases);},"changed active profile cannot record new authenticated context");
    write(root/physical("FTN/External/stock.cfg"),"changed saved source\n");
    check(ext::pending_records(root,"").size()==1,"already recorded exact run survives later active and saved profile edits");
    write(root/physical(manifest_path),"changed manifest\n");issues=0;
    check(ext::pending_records(root,"",64,[&](auto){++issues;}).empty()&&issues==1,"changed run binding holds record visibly");
    write(root/physical(manifest_path),manifest);check(ext::pending_records(root,"").size()==1,"restored exact run manifest reconciles original record");
    write(root/prepared.configuration_path,active_profile);
    app::FtnTic tic;tic.area="FILE.TEST";tic.file="PAYLOAD.ZIP";tic.origin=tic.from=akas.front();tic.to=*app::parse_ftn_address("2:100/1@test");tic.size=7;tic.crc=app::ftn_crc32("payload");tic.password="synthetic-tic-key";
    tic.path.push_back({tic.from,std::uint64_t{1700000000},{}});
    const auto tic_bytes=app::encode_ftn_tic(tic);check(tic_bytes.has_value(),"private indexed TIC fixture encoded");
    write(root/physical("FTN/External/Inbound/Secure/control.tic"),*tic_bytes);
    check(ext::tic_records_for_payload(root,"payload.zip").empty(),"payload before TIC waits without guessing a network");
    const auto tic_record=ext::record_run_completed(root,prepared.id,"FTN/External/Inbound/Secure/control.tic",true,akas);
    auto indexed=ext::tic_records_for_payload(root,"payload.zip");check(indexed.size()==1&&indexed.front().id==tic_record.id,"TIC index binds exact original record with case-insensitive payload name");
    ext::mark_record_consumed(root,tic_record);indexed=ext::tic_records_for_payload(root,"PAYLOAD.ZIP");check(indexed.size()==1&&indexed.front().digest==tic_record.digest,"consumed TIC remains routable for later payload arrival");
    check(ext::record_run_completed(root,prepared.id,"FTN/External/Inbound/Secure/control.tic",true,akas).id==tic_record.id&&ext::tic_records_for_payload(root,"payload.zip").size()==1,"TIC hook replay does not duplicate index");
    write(root/physical("FTN/External/Inbound/Secure/interrupted.tic"),*tic_bytes);
    refuses([&]{ext::record_run_completed(root,prepared.id,"FTN/External/Inbound/Secure/interrupted.tic",true,akas,[](auto){throw std::runtime_error("injected TIC index interruption");});},"index-before-Records interrupted publication retained");
    issues=0;indexed=ext::tic_records_for_payload(root,"payload.zip",[&](auto,auto why){if(!why.empty())++issues;});
    check(issues==1&&indexed.size()==1&&indexed.front().id==tic_record.id,"orphan index visible without starving valid matching TIC");
    write(root/physical("FTN/External/Inbound/Secure/later.tic"),*tic_bytes);const auto later=ext::record_run_completed(root,prepared.id,"FTN/External/Inbound/Secure/later.tic",true,akas);
    issues=0;indexed=ext::tic_records_for_payload(root,"payload.zip",[&](auto,auto why){if(!why.empty())++issues;});
    check(issues==1&&indexed.size()==2&&std::any_of(indexed.begin(),indexed.end(),[&](const auto& item){return item.id==later.id;}),"later valid TIC is discoverable despite retained orphan");
    refuses([&]{ext::tic_records_for_payload(root,"../outside.zip");},"payload index path cannot escape its pool");
#ifndef _WIN32
    const auto fault="FTN/External/test/Journal/link-crash";
    refuses([&]{ext::publish_once(root,fault,"complete metadata",[](auto){throw std::runtime_error("injected publication crash");});},"link-before-unlink failure retained");
    check(std::filesystem::hard_link_count(root/physical(fault))==2,"exact linked publication window reproduced");
    ext::publish_once(root,fault,"complete metadata");
    check(ext::read_small(root,fault)=="complete metadata"&&std::filesystem::hard_link_count(root/physical(fault))==1&&!std::filesystem::exists(root/physical(std::string(fault)+".tmp")),"exact owned pair reconciles without general hardlink acceptance");
    std::filesystem::create_hard_link(root/physical("FTN/External/test/Journal/exact"),root/physical("FTN/External/test/Journal/unrelated"));
    refuses([&]{ext::read_small(root,"FTN/External/test/Journal/exact");},"unrelated hardlink refused and retained");
    check(std::filesystem::exists(root/physical("FTN/External/test/Journal/unrelated")),"unrelated link never removed");
    std::filesystem::create_symlink(incoming,root/physical("FTN/External/test/Inbound/Secure/link.pkt"));
    refuses([&]{ext::record_completed(root,"TEST","FTN/External/test/Inbound/Secure/link.pkt",true,akas);},"symlink incoming refused");
#endif
#ifndef _WIN32
    for(const auto& entry:std::filesystem::recursive_directory_iterator(root/"ftn")){
        const auto name=entry.path().filename().string();
        if(entry.is_directory())check(name.find_first_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ")==name.npos,"generated external-mailer directories are recursively lowercase");
    }
    check(std::filesystem::exists(root/"ftn/external/test/profile")&&std::filesystem::exists(root/"ftn/external/runs"/prepared.id/"manifest"),"actual lazy profile and run manifest filenames use lower native names");
#endif
    check(!std::filesystem::exists(root/"Data")&&!std::filesystem::exists(root/"data")&&!std::filesystem::exists(root/"SFX.DB"),"offline recorder needs no database or BBS process");
}
}
int main(int argc,char** argv){try{check(argc==2,"owned fixture root required");run(std::filesystem::absolute(argv[1]).lexically_normal());std::cout<<"PASS external FTN files "<<checks<<" checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
