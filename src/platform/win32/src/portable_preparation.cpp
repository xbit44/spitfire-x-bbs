#include "sfx/platform/win32/portable_preparation.hpp"
#include "sfx/platform/win32/backup_read_access.hpp"
#include "sfx/platform/win32/portable_installation.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/service_file_owner.hpp"
#include "sfx/platform/win32/service_mode.hpp"
#include "sfx/platform/win32/door_component.hpp"
#include "sfx/platform/win32/clamav_installation.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <algorithm>
#include <sstream>
namespace sfx::platform::win32 {
namespace {
namespace ins=installation;
using ins::Path;using ins::Handle;using ins::require;
bool absent(const Path& p){if(GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES)return false;const auto e=GetLastError();require(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND,"preparation_object_unavailable_preserved");return true;}
bool hex(const std::string& s){return s.size()==32&&s.find_first_not_of("0123456789ABCDEF")==s.npos;}
std::string ascii(std::wstring_view value){std::string out;for(auto c:value){require(c>=32&&c<127,"preparation_text_invalid");out+=static_cast<char>(c);}return out;}
struct Intent{bool service{};DWORD policy{SERVICE_DISABLED};};
Intent copied_intent(const Path& root,const ins::Receipt& source){
    for(const auto* leaf:{L"service-registration.pending",L"service-registration.v2",L"service-mode.pending",L"service-mode.v1"}){
        const auto path=root/L"Bin"/leaf;if(absent(path))continue;
        const auto bytes=ins::read_file(path,4096);std::istringstream input(std::string(bytes.begin(),bytes.end()));std::string magic,id,owner,selected,revision,policy,extra;
        require(std::getline(input,magic)&&std::getline(input,id)&&std::getline(input,owner)&&id==ascii(source.id)&&owner==ascii(source.owner),"preparation_source_service_metadata_invalid_preserved");
        if(magic=="SFX-SERVICE-MODE-1"){require(!std::getline(input,extra),"preparation_source_service_metadata_invalid_preserved");return {true,SERVICE_AUTO_START};}
        require(magic=="SFX-SERVICE-REGISTRATION-2"&&std::getline(input,selected)&&(selected=="SERVICE"||selected=="DESKTOP")&&std::getline(input,revision)&&hex(revision)&&std::getline(input,policy)&&(policy=="2"||policy=="3"||policy=="4")&&!std::getline(input,extra),"preparation_source_service_metadata_invalid_preserved");
        require(selected=="SERVICE"||policy=="4","preparation_source_service_intent_invalid");return {selected=="SERVICE",DWORD(policy[0]-'0')};
    }return {};
}
struct Object{Path path;bool directory{};Handle pin;};
// The backup/restore impersonation scope is local to this attested worker.
// Metadata handles pin each exact current object, never follow a junction or
// a hardlink, and never authorize a content rewrite/deletion.
Handle metadata_pin(const Path& path,bool directory){
    const DWORD access=READ_CONTROL|WRITE_DAC|WRITE_OWNER|FILE_READ_ATTRIBUTES|(directory?MAXIMUM_ALLOWED:0);
    Handle h(CreateFileW(path.c_str(),access,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(h.get()==INVALID_HANDLE_VALUE)throw std::runtime_error("preparation_metadata_access_failed_preserved: Win32 "+std::to_string(GetLastError())+" ("+path.filename().u8string()+")");BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(h.get(),&info)&&!(info.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)&&bool(info.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)==directory&&(directory||info.nNumberOfLinks==1),"preparation_redirected_or_shared_object_preserved");
    wchar_t final[32768]{};const auto count=GetFinalPathNameByHandleW(h.get(),final,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
    require(count&&count<32768&&!_wcsicmp(final,(L"\\\\?\\"+path.native()).c_str()),"preparation_current_object_path_changed");return h;
}
void collect(const Path& path,std::vector<Object>& objects,const std::function<void()>& tick,unsigned depth=0){
    require(depth<64&&objects.size()<200000,"preparation_tree_limit_preserved");if(tick)tick();
    const auto attributes=GetFileAttributesW(path.c_str());require(attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT),"preparation_redirected_or_unavailable_path_preserved");
    const bool directory=(attributes&FILE_ATTRIBUTE_DIRECTORY)!=0;objects.push_back({path,directory,metadata_pin(path,directory)});
    if(directory)for(const auto& entry:std::filesystem::directory_iterator(path))collect(entry.path(),objects,tick,depth+1);
}
void set_access(Object& object,void* descriptor,const std::wstring& prior_owner){
    PSID owner{};PACL dacl{};BOOL defaulted{},present{};
    require(GetSecurityDescriptorOwner(descriptor,&owner,&defaulted)&&GetSecurityDescriptorDacl(descriptor,&present,&dacl,&defaulted)&&present&&dacl,"preparation_descriptor_invalid");
    // MAXIMUM_ALLOWED directory handles prevent automatic descendant ACL
    // propagation. Every child was inventoried/pinned and is set explicitly.
    auto readers=capture_backup_read_grants(object.pin.get(),prior_owner,object.directory);
    wchar_t* owner_text{};require(ConvertSidToStringSidW(owner,&owner_text)!=FALSE,"preparation_owner_sid_text_failed");
    const std::wstring host_owner(owner_text);LocalFree(owner_text);
    auto host_readers=capture_backup_read_grants(object.pin.get(),host_owner,object.directory);
    readers.insert(readers.end(),host_readers.begin(),host_readers.end());
    auto merged=merge_backup_read_grants(dacl,readers);
    require(SetSecurityInfo(object.pin.get(),SE_FILE_OBJECT,OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION|PROTECTED_DACL_SECURITY_INFORMATION,owner,nullptr,reinterpret_cast<PACL>(merged.data()),nullptr)==ERROR_SUCCESS,"preparation_access_update_incomplete_retry");
}
void replace_identity(const Path& root,const std::wstring& id,void* descriptor){
    const auto path=root/L"Bin"/L"installation.id";const auto text=ascii(id);const std::vector<std::uint8_t> wanted(text.begin(),text.end());
    if(!absent(path)){
        const auto old=ins::read_file(path,32);if(old==wanted)return;
        require(old.size()==32&&hex(std::string(old.begin(),old.end())),"preparation_source_identity_invalid_preserved");
        require(MoveFileExW(path.c_str(),(root/L"Bin"/(L"installation-source-"+id+L".id")).c_str(),MOVEFILE_WRITE_THROUGH),"preparation_source_identity_retention_failed");
    }ins::create_file(path,wanted,descriptor);
}
bool same_root(const Path& a,const Path& b){return !_wcsicmp(a.c_str(),b.c_str());}

std::pair<Intent, ins::Receipt> read_preparation_journal(const Path& path) {
    const auto bytes = ins::read_file(path, ins::maximum_receipt_bytes + 128);
    const std::string text(bytes.begin(), bytes.end());
    std::istringstream input(text);
    std::string magic, mode, policy;
    require(std::getline(input, magic) && std::getline(input, mode) && std::getline(input, policy) &&
        magic == "SFX-PREPARATION-1" && (mode == "SERVICE" || mode == "DESKTOP"),
        "preparation_journal_invalid_preserved");
    require(policy == "2" || policy == "3" || policy == "4", "preparation_journal_policy_invalid");
    const auto offset = magic.size() + mode.size() + policy.size() + 3;
    require(offset < bytes.size(), "preparation_journal_invalid_preserved");
    return {{mode == "SERVICE", DWORD(policy[0] - '0')}, ins::decode_receipt({bytes.begin() + offset, bytes.end()})};
}

Handle hold_preparation_history(const Path& path, void* descriptor) {
    auto held = ins::open_object(path, false, GENERIC_READ | READ_CONTROL, FILE_SHARE_READ);
    BY_HANDLE_FILE_INFORMATION info{};
    require(GetFileInformationByHandle(held.get(), &info) && info.nNumberOfLinks == 1 &&
        ins::exact_security(held.get(), descriptor), "preparation_history_protection_invalid_preserved");
    return held;
}

std::wstring read_preparation_predecessor(const ins::Receipt& prepared, void* descriptor) {
    const auto path = prepared.root / L"Bin" / (L"installation-source-" + prepared.id + L".id");
    const auto held = hold_preparation_history(path, descriptor);
    const auto bytes = ins::read_file(path, 32);
    require(hex({bytes.begin(), bytes.end()}), "preparation_history_identity_invalid_preserved");
    std::wstring predecessor(bytes.begin(), bytes.end());
    require(predecessor != prepared.id, "preparation_history_identity_invalid_preserved");
    return predecessor;
}
}

std::optional<std::wstring> confirmed_preparation_predecessor(const ins::Receipt& live) {
    require(live.committed && hex(ascii(live.id)), "preparation_live_installation_invalid");
    const DWORD access = FILE_READ_ATTRIBUTES | READ_CONTROL;
    const auto root = ins::open_object(live.root, true, access, FILE_SHARE_READ | FILE_SHARE_WRITE);
    const auto bin = ins::open_object(live.root / L"Bin", true, access, FILE_SHARE_READ | FILE_SHARE_WRITE);
    const auto descriptor = ins::structural_descriptor(live.owner);
    require(ins::volume_serial(root.get()) == live.volume && ins::file_id(root.get()) == live.root_id &&
        ins::file_id(bin.get()) == live.bin_id && ins::exact_security(root.get(), descriptor.get()) &&
        ins::exact_security(bin.get(), descriptor.get()), "preparation_live_installation_changed_preserved");
    const auto journal = live.root / L"Bin" / (L"preparation-" + live.id + L".confirmed");
    if (absent(journal)) return {};
    const auto held = hold_preparation_history(journal, descriptor.get());
    const auto prepared = read_preparation_journal(journal).second;
    require(prepared.committed && prepared.id == live.id && prepared.owner == live.owner && prepared.root == live.root &&
        prepared.generation == 1 && prepared.volume == live.volume && prepared.root_id == live.root_id &&
        prepared.bin_id == live.bin_id, "preparation_confirmed_installation_mismatch_preserved");
    return read_preparation_predecessor(prepared, descriptor.get());
}

PreparationDirectoryScope::PreparationDirectoryScope(){
    wchar_t current[32768]{},windows[32768]{};
    const auto n=GetCurrentDirectoryW(32768,current);
    const auto m=GetWindowsDirectoryW(windows,32768);
    ins::require(n&&n<32768&&m&&m<32768,"preparation_working_directory_unavailable");
    previous_=current;
    ins::require(SetCurrentDirectoryW(windows),"preparation_working_directory_release_failed");
}
PreparationDirectoryScope::~PreparationDirectoryScope(){
    if(!previous_.empty())SetCurrentDirectoryW(previous_.c_str());
}
ins::Receipt prepare_portable_installation(const Path& root,const std::wstring& owner,const std::function<void()>& progress,const std::function<void()>& attention,
    const std::function<void()>& clamav_attention){
    require(ins::elevated()&&ins::canonical_root(root)==root,"preparation_requires_consented_current_host_worker");
    PreparationDirectoryScope directory_scope;
    activate_preparation_file_owner(owner);ServiceFileOwnerScope scope(owner);require(scope.valid()&&scope.active(),"preparation_metadata_scope_unavailable");
    auto token=ins::process_token();auto ancestors=ins::protect_ancestors(root,token.get());
    auto source=ins::read_portable_installation_input(root);
    // A known running source copy on this host is a conflict. This only uses
    // the existing kernel exclusion name, never requires the old drive/PC.
    std::unique_ptr<OfflineInstallationGuard> source_guard;
    if(!same_root(source.root,root)){source_guard=std::make_unique<OfflineInstallationGuard>();require(source_guard->acquire(source.root,source.owner).succeeded(),"preparation_source_board_active_stop_it_first");}
    OfflineInstallationGuard guard;require(guard.acquire(root,owner).succeeded(),"preparation_stop_bbs_or_wait_for_current_transaction");
    if(!absent(root/L"Data"/L"SFXRuntime.lock")){auto lock=ins::open_object(root/L"Data"/L"SFXRuntime.lock",false,FILE_READ_ATTRIBUTES,0);}
    std::vector<Object> objects;collect(root,objects,{});
    const auto config_object=std::find_if(objects.begin(),objects.end(),[&](const auto& object){return same_root(object.path,root/L"Config");});
    require(config_object!=objects.end()&&config_object->directory,"preparation_config_directory_missing");
    const auto config_volume=ins::volume_serial(config_object->pin.get());const auto config_id=ins::file_id(config_object->pin.get());
    // No host authority or permission mutation until all current programs have
    // been checked against the carried generation under non-writable pins.
    std::vector<Handle> programs;
    for(std::size_t i=0;i<ins::current_program_count;++i){const auto path=root/L"Bin"/ins::programs[i];programs.push_back(ins::open_object(path,false,GENERIC_READ,FILE_SHARE_READ));require(ins::file_sha256(path)==source.program_hashes[i],"preparation_program_changed_run_matching_setup_repair");}
    const auto pending=root/L"Bin"/L"preparation.pending";Intent intent;ins::Receipt next;
    auto structure=ins::structural_descriptor(owner),content=ins::mutable_descriptor(owner);
    if(absent(pending)){
        intent=copied_intent(root,source);next=source;next.root=root;next.owner=owner;next.id=ins::unique_id();next.generation=1;next.door_component={};
        auto root_pin=ins::open_object(root,true,FILE_READ_ATTRIBUTES);auto bin=ins::open_object(root/L"Bin",true,FILE_READ_ATTRIBUTES);auto ftn=ins::open_object(root/L"FTN",true,FILE_READ_ATTRIBUTES);
        next.volume=ins::volume_serial(root_pin.get());next.root_id=ins::file_id(root_pin.get());next.bin_id=ins::file_id(bin.get());next.ftn_volume=ins::volume_serial(ftn.get());next.ftn_directory_id=ins::file_id(ftn.get());
        for(std::size_t i=0;i<programs.size();++i)next.program_ids[i]=ins::file_id(programs[i].get());
        // Current-root structural access is needed for the exclusive journal.
        for(auto& object:objects)if(object.path==root||object.path==root/L"Bin")set_access(object,structure.get(),source.owner);
        const auto header=std::string("SFX-PREPARATION-1\n")+(intent.service?"SERVICE\n":"DESKTOP\n")+std::to_string(intent.policy)+'\n';auto bytes=ins::encode_receipt(next);bytes.insert(bytes.begin(),header.begin(),header.end());ins::create_file(pending,bytes,structure.get());
    }else{
        auto journal = read_preparation_journal(pending);
        intent = journal.first;
        next = std::move(journal.second);
        require(next.root==root&&next.owner==owner&&next.program_count==ins::current_program_count&&next.program_hashes==source.program_hashes,"preparation_journal_current_copy_mismatch_preserved");
        require(next.volume==ins::volume_serial(objects.front().pin.get())&&next.root_id==ins::file_id(objects.front().pin.get()),"preparation_journal_current_root_changed_preserved");
        for(std::size_t i=0;i<programs.size();++i)require(next.program_ids[i]==ins::file_id(programs[i].get()),"preparation_program_replaced_during_retry");
    }
    // Repeatable metadata-only phase. No external configured paths are touched.
    for(auto& object:objects){
        const auto relative=object.path.lexically_relative(root);auto part=relative.begin();
        const auto first=part==relative.end()?std::wstring{}:part->native();
        const bool bin=!_wcsicmp(first.c_str(),L"Bin");
        const bool ini=bin&&object.path==root/L"Bin"/L"sfxexec.ini";
        // Setup's retained recovery transactions are protected structural
        // material, including on a replacement host. Preserve that boundary.
        const bool recovery=!_wcsicmp(first.c_str(),L"Backup")&&++part!=relative.end()&&!_wcsicmp(part->native().c_str(),L"Recovery");
        set_access(object,(object.path==root||(bin&&!ini)||recovery)?structure.get():content.get(),source.owner);
    }
    // Release metadata handles before bounded subsystem reads/publications;
    // program/ancestor pins and installation exclusion remain held.
    objects.clear();
    // MAXIMUM_ALLOWED metadata handles include DELETE and cannot coexist with
    // ordinary no-delete read pins. Switch phases, then revalidate native
    // identity before any callback/publication; replacement during the switch
    // is refused. These read pins stay held through all remaining work.
    auto root_hold=ins::open_object(root,true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE);
    auto bin_hold=ins::open_object(root/L"Bin",true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE);
    auto config_hold=ins::open_object(root/L"Config",true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE);
    auto ftn_hold=ins::open_object(root/L"FTN",true,FILE_READ_ATTRIBUTES|FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE);
    require(next.volume==ins::volume_serial(root_hold.get())&&next.root_id==ins::file_id(root_hold.get())&&next.bin_id==ins::file_id(bin_hold.get()),"preparation_current_root_changed_preserved");
    require(config_volume==ins::volume_serial(config_hold.get())&&config_id==ins::file_id(config_hold.get())&&next.ftn_volume==ins::volume_serial(ftn_hold.get())&&next.ftn_directory_id==ins::file_id(ftn_hold.get()),"preparation_current_data_boundary_changed_preserved");
    if(progress)progress();
    // Established missing/damaged key or proof remains unavailable. Preparing
    // host access never regenerates board material or invents file outcomes.
    const auto bindings=prepare_copied_file_bindings(root,owner);
    if((!bindings.published||bindings.deferred)&&attention)attention();
    replace_identity(root,next.id,structure.get());
    bool clamav_needs_attention = false;
    if (clamav_available()) {
        try {
            if (clamav_installed(root)) {
                const auto predecessor = read_preparation_predecessor(next, structure.get());
                require(source.id == predecessor || source.id == next.id, "preparation_clamav_source_mismatch_preserved");
                const auto result = rebind_clamav_receipt(root, owner, next.id, predecessor, source.root);
                clamav_needs_attention = result.configuration_preserved;
            }
        } catch (const std::exception&) {
            clamav_needs_attention = true;
        }
    }
    if (clamav_needs_attention && clamav_attention) clamav_attention();
    // BEFORE receipt/SCM publication, including restored Automatic intent.
    ins::require_prepared_first_start(next,next.id);
    const auto location=ins::system_door_component_location();std::unique_ptr<ins::DoorComponentChange> component;
    if(!location.image.empty()){
        auto descriptor=ins::shared_component_descriptor();ins::DoorComponentChange::recover(location,descriptor.get(),[](const ins::Receipt& expected,bool remove)->std::optional<bool>{if(remove)return {};try{return ins::encode_receipt(ins::read_receipt(expected.root,expected.owner))==ins::encode_receipt(expected);}catch(...){return false;}});
        const auto bytes=ins::read_file(root/L"Bin"/L"Support"/L"SFXEXEC.DLL",4U*1024U*1024U);ins::DoorComponentPolicy policy;policy.incoming_sha256=ins::qualified_door_driver_sha256;
        component=ins::DoorComponentChange::prepare(location,next,bytes,policy,descriptor.get());next.door_component=component->identity();
    }
    bool registered=false;try{const auto existing=ins::read_receipt(root,owner);registered=ins::encode_receipt(existing)==ins::encode_receipt(next);require(registered,"preparation_registered_generation_changed_preserved");}catch(const std::exception& e){if(std::string(e.what())=="preparation_registered_generation_changed_preserved")throw;}
    if(!registered)ins::create_receipt(next,true);
    // A receipt commit can survive an interrupted portable-input publication.
    // Every successful retry must carry the current host generation forward,
    // otherwise a second copy would see mismatched service/source identities.
    ins::write_portable_installation_input(next);
    if(component)component->commit();
    // Copied metadata is history, never authority to control the old service.
    for(const auto* leaf:{L"service-registration.pending",L"service-registration.v2",L"service-mode.pending",L"service-mode.v1"}){
        const auto path=root/L"Bin"/leaf;if(absent(path))continue;
        // A retry after this operation's SCM publication retains its metadata.
        try{const auto current=ins::read_service_registration(root,true);if(current&&current->id==next.id)break;}catch(...){}
        require(MoveFileExW(path.c_str(),(root/L"Bin"/(L"service-source-"+next.id+L"-"+leaf)).c_str(),MOVEFILE_WRITE_THROUGH),"preparation_service_source_retention_incomplete_retry");
    }
    ins::configure_owned_service(root,owner,intent.service,next.id,intent.policy);
    require(ins::query_owned_service(*ins::read_service_registration(root)).state==SERVICE_STOPPED,"preparation_service_must_remain_stopped");
    require(MoveFileExW(pending.c_str(),(root/L"Bin"/(L"preparation-"+next.id+L".confirmed")).c_str(),MOVEFILE_WRITE_THROUGH),"preparation_commit_not_confirmed_retry");
    ins::verify_structure(next);return next;
}
}
