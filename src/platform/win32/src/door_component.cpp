#include "sfx/platform/win32/door_component.hpp"
#include <shlobj.h>
#include <sddl.h>
#include <algorithm>
#include <charconv>
#include <set>

namespace sfx::platform::win32::installation {
namespace {
constexpr std::size_t limit=1024*1024;
struct State {DoorComponentIdentity image;std::vector<Receipt> consumers;};
struct Journal {State before,after;Receipt consumer;bool remove{},changed{};Path stage,backup;};
bool absent(const Path& p){if(GetFileAttributesW(p.c_str())!=INVALID_FILE_ATTRIBUTES)return false;const auto e=GetLastError();require(e==ERROR_FILE_NOT_FOUND||e==ERROR_PATH_NOT_FOUND,"door_component_inspection_failed");return true;}
std::string ascii(const std::wstring& s){require(std::all_of(s.begin(),s.end(),[](wchar_t c){return c>=32&&c<127;}),"door_component_ascii_path_required");std::string out;out.reserve(s.size());for(auto c:s)out+=static_cast<char>(c);return out;}
std::wstring wide(const std::string& s){return {s.begin(),s.end()};}
bool hash(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='A'&&c<='F');});}
std::string hex(const std::vector<std::uint8_t>& b){constexpr char digits[]="0123456789ABCDEF";std::string s;s.reserve(b.size()*2);for(auto c:b){s+=digits[c>>4];s+=digits[c&15];}return s;}
std::vector<std::uint8_t> unhex(const std::string& s){require(s.size()%2==0&&s.size()<=limit,"door_component_hex_invalid");const auto digit=[](char c)->unsigned {require((c>='0'&&c<='9')||(c>='A'&&c<='F'),"door_component_hex_invalid");return c<='9'?c-'0':c-'A'+10;};std::vector<std::uint8_t> b;for(std::size_t i=0;i<s.size();i+=2)b.push_back(static_cast<std::uint8_t>((digit(s[i])<<4)|digit(s[i+1])));return b;}
std::vector<std::uint8_t> bytes(const std::string& s){require(s.size()<=limit,"door_component_record_oversized");return {s.begin(),s.end()};}
struct Reader {
    std::vector<std::string> rows;std::size_t at{};
    explicit Reader(const std::vector<std::uint8_t>& b){require(!b.empty()&&b.size()<=limit&&b.back()=='\n',"door_component_record_invalid");std::string row;for(auto c:b){if(c=='\n'){rows.push_back(row);row.clear();}else{require(c>=32&&c<127,"door_component_record_character");row+=static_cast<char>(c);}}}
    std::string get(){require(at<rows.size(),"door_component_record_truncated");return rows[at++];}
    std::uint64_t number(){const auto s=get();std::uint64_t n{};const auto r=std::from_chars(s.data(),s.data()+s.size(),n);require(r.ec==std::errc{}&&r.ptr==s.data()+s.size()&&std::to_string(n)==s,"door_component_number_invalid");return n;}
    bool flag(){const auto n=number();require(n<=1,"door_component_flag_invalid");return n!=0;}
    void end(){require(at==rows.size(),"door_component_trailing_fields");}
};
std::vector<std::uint8_t> encode(const State& s){
    require(s.consumers.size()<=128,"door_component_consumer_limit");const auto& i=s.image;
    std::string text="SFX-SHARED-DOS-1\n"+std::to_string(i.registered)+"\n"+std::to_string(i.introduced)+"\n"+ascii(i.path.native())+"\n"+std::to_string(i.volume)+"\n"+std::to_string(i.file_id)+"\n"+i.sha256+"\n"+i.compatibility+"\n"+std::to_string(s.consumers.size())+"\n";
    require(!i.registered||(i.path.is_absolute()&&!_wcsicmp(i.path.filename().c_str(),L"SFXEXEC.DLL")&&i.volume&&i.file_id&&hash(i.sha256)&&i.compatibility==door_component_compatibility),"door_component_identity_invalid");
    require(i.registered||(!i.introduced&&i.path.empty()&&!i.volume&&!i.file_id&&i.sha256.empty()&&i.compatibility.empty()&&s.consumers.empty()),"door_component_empty_state_invalid");
    std::set<std::wstring> ids;for(const auto& c:s.consumers){require(ids.insert(c.id).second&&c.program_count==current_program_count&&c.committed&&c.door_component.registered&&c.door_component.sha256==i.sha256&&c.door_component.file_id==i.file_id&&c.door_component.volume==i.volume&&c.door_component.path==i.path&&c.door_component.compatibility==i.compatibility,"door_component_consumer_binding_invalid");text+=hex(encode_receipt(c))+"\n";}return bytes(text);
}
State decode_state(const std::vector<std::uint8_t>& b){Reader r(b);require(r.get()=="SFX-SHARED-DOS-1","door_component_receipt_version");State s;auto& i=s.image;i.registered=r.flag();i.introduced=r.flag();i.path=wide(r.get());const auto vol=r.number();require(vol<=MAXDWORD,"door_component_volume_invalid");i.volume=static_cast<DWORD>(vol);i.file_id=r.number();i.sha256=r.get();i.compatibility=r.get();const auto count=r.number();require(count<=128,"door_component_consumer_limit");for(std::uint64_t n=0;n<count;++n)s.consumers.push_back(decode_receipt(unhex(r.get())));r.end();require(encode(s)==b,"door_component_noncanonical");return s;}
std::vector<std::uint8_t> encode(const Journal& j){return bytes("SFX-SHARED-DOS-JOURNAL-1\n"+std::to_string(j.remove)+"\n"+std::to_string(j.changed)+"\n"+ascii(j.stage.native())+"\n"+ascii(j.backup.native())+"\n"+hex(encode(j.before))+"\n"+hex(encode(j.after))+"\n"+hex(encode_receipt(j.consumer))+"\n");}
Journal decode_journal(const std::vector<std::uint8_t>& b){Reader r(b);require(r.get()=="SFX-SHARED-DOS-JOURNAL-1","door_component_journal_version");Journal j;j.remove=r.flag();j.changed=r.flag();j.stage=wide(r.get());j.backup=wide(r.get());j.before=decode_state(unhex(r.get()));j.after=decode_state(unhex(r.get()));j.consumer=decode_receipt(unhex(r.get()));r.end();require(encode(j)==b,"door_component_journal_noncanonical");return j;}
DoorComponentIdentity identify(const Path& p,bool introduced){auto f=open_object(p,false,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE);return {true,introduced,p,volume_serial(f.get()),file_id(f.get()),file_sha256(p),door_component_compatibility};}
bool matches(const Path& p,const DoorComponentIdentity& i){if(!i.registered||absent(p))return false;auto f=open_object(p,false,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE);return volume_serial(f.get())==i.volume&&file_id(f.get())==i.file_id&&file_sha256(p)==i.sha256;}
void erase_exact(const Path& p,const DoorComponentIdentity& i){require(matches(p,i),"door_component_changed_preserved");auto f=open_object(p,false,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE);require(volume_serial(f.get())==i.volume&&file_id(f.get())==i.file_id,"door_component_identity_changed");require(DeleteFileW(p.c_str())!=FALSE,"door_component_in_use_preserved");}
}
// NTFS persists mapped file rights, not generic GR/GX bits. Keep strict
// descriptor comparisons and spell out FILE_GENERIC_READ|FILE_GENERIC_EXECUTE.
Descriptor shared_component_descriptor(){PSECURITY_DESCRIPTOR raw{};require(ConvertStringSecurityDescriptorToSecurityDescriptorW(L"O:BAG:BAD:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)",SDDL_REVISION_1,&raw,nullptr)!=FALSE,"door_component_descriptor_failed");return Descriptor(raw);}
DoorComponentLocation system_door_component_location(){SYSTEM_INFO info{};GetNativeSystemInfo(&info);if(info.wProcessorArchitecture!=PROCESSOR_ARCHITECTURE_INTEL)return {};wchar_t system[MAX_PATH]{},data[MAX_PATH]{};const auto n=GetSystemDirectoryW(system,MAX_PATH);require(n&&n<MAX_PATH&&SUCCEEDED(SHGetFolderPathW(nullptr,CSIDL_COMMON_APPDATA,nullptr,SHGFP_TYPE_CURRENT,data)),"door_component_system_directory_unavailable");return {Path(system)/L"SFXEXEC.DLL",Path(data)/L"Spitfire-X Shared Components"};}
struct DoorComponentChange::Impl {
    DoorComponentLocation location;Descriptor descriptor_storage;void* descriptor{};Handle directory,system_directory,lock;Journal journal;bool finished{},retained{};
    explicit Impl(const DoorComponentLocation& l,void* sd):location(l){
        require(sd&&IsValidSecurityDescriptor(sd),"door_component_descriptor_invalid");const auto size=GetSecurityDescriptorLength(sd);require(size&&size<65536,"door_component_descriptor_invalid");descriptor_storage.reset(LocalAlloc(LMEM_FIXED,size));require(descriptor_storage!=nullptr,"door_component_descriptor_allocation_failed");CopyMemory(descriptor_storage.get(),sd,size);descriptor=descriptor_storage.get();
        require(l.image.is_absolute()&&l.records.is_absolute()&&!_wcsicmp(l.image.filename().c_str(),L"SFXEXEC.DLL")&&l.image.parent_path()!=l.records,"door_component_location_invalid");
        system_directory=open_object(l.image.parent_path(),true);
        if(absent(l.records))create_directory(l.records,sd);directory=open_object(l.records,true);require(exact_security(directory.get(),sd),"door_component_records_security_invalid");
        const auto path=l.records/L"operation.lock";if(absent(path)){try{create_file(path,{'S','F','X'},sd);}catch(...){if(absent(path))throw;}}
        lock=open_object(path,false,GENERIC_READ|GENERIC_WRITE,0);require(exact_security(lock.get(),sd),"door_component_lock_security_invalid");
    }
    Path record()const{return location.records/L"component.receipt";}Path journal_path()const{return location.records/L"component.journal";}
    std::vector<std::uint8_t> read(const Path& p){auto f=open_object(p,false,GENERIC_READ,FILE_SHARE_READ);require(exact_security(f.get(),descriptor),"door_component_record_security_invalid");return read_file(p,limit);}
    State state(){return absent(record())?State{}:decode_state(read(record()));}
    void save(const State& s){const auto previous=absent(record())?std::vector<std::uint8_t>{}:read(record());const auto p=location.records/(L"receipt.new-"+unique_id());create_file(p,encode(s),descriptor);if(!previous.empty())require(read(record())==previous,"door_component_record_changed");require(MoveFileExW(p.c_str(),record().c_str(),MOVEFILE_WRITE_THROUGH|(previous.empty()?0:MOVEFILE_REPLACE_EXISTING))!=FALSE,"door_component_record_commit_failed");require(read(record())==encode(s),"door_component_record_readback_failed");}
    void validate_journal(){const auto& j=journal;require(j.consumer.root.is_absolute(),"door_component_journal_consumer_invalid");for(const auto* s:{&j.before,&j.after})require(!s->image.registered||s->image.path==location.image,"door_component_journal_path_changed");
        if(j.changed){require(!j.remove&&j.stage.parent_path()==location.image.parent_path()&&j.backup.parent_path()==location.image.parent_path()&&j.stage.filename().native().rfind(L"SFXEXEC.DLL.new-",0)==0&&j.backup.filename().native().rfind(L"SFXEXEC.DLL.previous-",0)==0&&j.after.image.registered,"door_component_journal_staging_invalid");}
        else require(j.stage.empty()&&j.backup.empty(),"door_component_unexpected_staging");
    }
    void clear_journal(){require(read(journal_path())==encode(journal),"door_component_journal_changed");require(DeleteFileW(journal_path().c_str())!=FALSE,"door_component_journal_cleanup_failed");finished=true;}
    bool retiring_last_image()const{return journal.remove&&journal.after.consumers.empty()&&journal.after.image.introduced;}
    void validate_current_state(bool committing=false){const auto current=encode(state());
        const bool retired=committing&&retiring_last_image()&&current==encode(State{})&&absent(location.image);
        require(retired||current==encode(journal.before)||current==encode(journal.after),"door_component_recovery_record_changed_preserved");}
    void finish(){validate_journal();validate_current_state(true);auto after=journal.after;
        if(journal.changed)require(matches(location.image,after.image),"door_component_commit_image_changed");
        if(retiring_last_image()){
            // Recovery can resume after deletion, after the empty receipt save,
            // or before journal retirement. Never recreate a phantom identity.
            if(absent(location.image))after={};
            else try{erase_exact(location.image,after.image);after={};}catch(...){retained=true;}
        }else retained=after.image.registered;
        save(after);clear_journal(); // previous bytes stay retained for recovery evidence
    }
    void undo(){validate_journal();validate_current_state();const auto& j=journal;
        if(j.remove)require(matches(location.image,j.before.image),"door_component_removed_image_cannot_be_rolled_back");
        if(j.changed){
            if(matches(location.image,j.after.image))erase_exact(location.image,j.after.image);
            else require(absent(location.image)||matches(location.image,j.before.image),"door_component_rollback_conflict_preserved");
            if(j.before.image.registered&&!matches(location.image,j.before.image)){
                require(absent(location.image)&&matches(j.backup,j.before.image),"door_component_previous_image_missing_preserved");require(MoveFileExW(j.backup.c_str(),location.image.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"door_component_restore_failed");
            }
            if(!absent(j.stage))erase_exact(j.stage,j.after.image);
        }
        save(j.before);clear_journal();
    }
};
DoorComponentChange::DoorComponentChange(std::unique_ptr<Impl> p):impl_(std::move(p)){}
DoorComponentChange::~DoorComponentChange()=default;
const DoorComponentIdentity& DoorComponentChange::identity()const{return impl_->journal.after.image;}
bool DoorComponentChange::image_retained()const{return impl_->retained;}
void DoorComponentChange::commit(){require(!impl_->finished,"door_component_transaction_finished");impl_->finish();}
void DoorComponentChange::rollback(){require(!impl_->finished,"door_component_transaction_finished");impl_->undo();}
std::unique_ptr<DoorComponentChange> DoorComponentChange::prepare(const DoorComponentLocation& location,Receipt consumer,const std::vector<std::uint8_t>& image,const DoorComponentPolicy& policy,void* sd,bool remove){
    auto p=std::make_unique<Impl>(location,sd);require(absent(p->journal_path()),"door_component_recovery_pending");auto& j=p->journal;j.before=p->state();j.after=j.before;j.consumer=consumer;j.remove=remove;
    require(consumer.program_count==current_program_count&&consumer.committed,"door_component_consumer_not_current");
    if(j.before.image.registered)require(matches(location.image,j.before.image),"door_component_registered_image_changed");
    const auto found=std::find_if(j.after.consumers.begin(),j.after.consumers.end(),[&](const auto& c){return c.id==consumer.id;});
    const bool registered=found!=j.after.consumers.end();
    if(registered){require(found->root==consumer.root&&found->root_id==consumer.root_id&&found->volume==consumer.volume&&found->owner==consumer.owner,"door_component_consumer_root_changed");j.after.consumers.erase(found);}
    if(remove){require(j.before.image.registered&&registered,"door_component_consumer_missing");}
    else{
        require(hash(policy.incoming_sha256)&&sha256(image)==policy.incoming_sha256,"door_component_incoming_hash_invalid");
        if(!j.after.image.registered&&!absent(location.image)){j.after.image=identify(location.image,false);require(j.after.image.sha256==policy.incoming_sha256,"door_component_unknown_image_preserved");}
        if(!j.after.image.registered||j.after.image.sha256!=policy.incoming_sha256){
            if(j.after.image.registered){require(j.after.image.introduced&&std::find(policy.compatible_predecessors.begin(),policy.compatible_predecessors.end(),j.after.image.sha256)!=policy.compatible_predecessors.end(),"door_component_unknown_predecessor_preserved");
                for(const auto& c:j.after.consumers)require(c.door_component.compatibility==door_component_compatibility&&policy.stopped_consumer&&policy.stopped_consumer(c),"door_component_consumer_live_or_incompatible");
                // Other receipts bind the exact old image identity. A version
                // replacement needs their separately journaled receipt update.
                require(j.after.consumers.empty(),"door_component_shared_upgrade_requires_all_consumer_receipts");
            }
            j.changed=true;const auto transaction=unique_id();j.stage=location.image.native()+L".new-"+transaction;j.backup=location.image.native()+L".previous-"+transaction;
            create_file(j.stage,image,sd);j.after.image=identify(j.stage,true);j.after.image.path=location.image;
        }
        consumer.door_component=j.after.image;j.consumer=consumer;j.after.consumers.push_back(consumer);
    }
    p->validate_journal();create_file(p->journal_path(),encode(j),sd);
    if(j.changed){if(j.before.image.registered){require(matches(location.image,j.before.image),"door_component_predecessor_changed");auto in_use=open_object(location.image,false,GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ|FILE_SHARE_DELETE);require(MoveFileExW(location.image.c_str(),j.backup.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"door_component_live_predecessor_preserved");}
        require(absent(location.image)&&MoveFileExW(j.stage.c_str(),location.image.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE,"door_component_install_failed_recovery_retained");require(matches(location.image,j.after.image),"door_component_installed_identity_changed");}
    p->save(j.after);return std::unique_ptr<DoorComponentChange>(new DoorComponentChange(std::move(p)));
}
void DoorComponentChange::recover(const DoorComponentLocation& location,void* sd,const std::function<std::optional<bool>(const Receipt&,bool)>& committed){
    auto p=std::make_unique<Impl>(location,sd);if(absent(p->journal_path()))return;p->journal=decode_journal(p->read(p->journal_path()));p->validate_journal();require(static_cast<bool>(committed),"door_component_recovery_decision_required");const auto decision=committed(p->journal.consumer,p->journal.remove);require(decision.has_value(),"door_component_recovery_state_unknown_preserved");if(*decision)p->finish();else p->undo();
}
} // namespace sfx::platform::win32::installation
