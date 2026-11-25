#include "sfx/platform/win32/ftn_stored_scan.hpp"
#include "sfx/platform/win32/portable_file_bindings.hpp"
#include "sfx/platform/win32/ftn_native_session.hpp"
#include "sfx/application/ftn_stored_message.hpp"
#include "sfx/application/ftn_mail.hpp"
#include "sfx/application/ftn_setup.hpp"
#include "sfx/application/qwk.hpp"
#include <sodium.h>
#include <algorithm>
#include <set>
namespace sfx::platform::win32 {
namespace {
namespace app=application;namespace ins=installation;
const app::MessageConfigurationActor owner{true,{}};
void require(bool v,const char* why){ins::require(v,why);}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
app::FtnDigest hash(std::string_view bytes){app::FtnDigest d{};require(sodium_init()>=0&&crypto_hash_sha256(d.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())==0,"ftn_msg_hash_failed");return d;}
app::Identifier128 token(std::string_view bytes){const auto d=hash(bytes);app::Identifier128 id;std::copy_n(d.begin(),16,id.bytes.begin());require(id.valid(),"ftn_msg_identity_failed");return id;}
std::string binary(const app::FtnDigest& d){return {reinterpret_cast<const char*>(d.data()),d.size()};}
struct StoredFile {
    ins::Handle handle;std::string raw,identity;
    std::filesystem::path path;app::FileObjectIdentity logical;
    StoredFile(const std::filesystem::path& input):path(input){
        handle.reset(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE|DELETE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
        require(handle.get()!=INVALID_HANDLE_VALUE,"ftn_msg_open_failed");BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        require(GetFileInformationByHandle(handle.get(),&info)&&!(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))&&info.nNumberOfLinks==1&&GetFileSizeEx(handle.get(),&size)&&size.QuadPart>=191&&size.QuadPart<=1024*1024,"ftn_msg_invalid_file");
        std::wstring actual(32768,L'\0');const auto n=GetFinalPathNameByHandleW(handle.get(),actual.data(),static_cast<DWORD>(actual.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);
        require(n>4&&n<actual.size(),"ftn_msg_final_path_failed");actual.resize(n);require(!_wcsicmp(actual.c_str(),(L"\\\\?\\"+path.native()).c_str()),"ftn_msg_path_changed");
        raw.resize(static_cast<std::size_t>(size.QuadPart));DWORD got{};require(ReadFile(handle.get(),raw.data(),static_cast<DWORD>(raw.size()),&got,nullptr)&&got==raw.size(),"ftn_msg_read_failed");
        logical=portable_logical_file_identity(path,handle.get());identity=logical.volume+"/"+logical.file_id;
    }
    void finish(bool kill){
        if(kill){
            FILE_DISPOSITION_INFO disposition{TRUE};require(SetFileInformationByHandle(handle.get(),FileDispositionInfo,&disposition,sizeof(disposition))!=FALSE,"ftn_msg_remove_failed");
            require(CloseHandle(handle.get())!=FALSE,"ftn_msg_close_unconfirmed_preserved");handle.release();
            const auto flags=GetFileAttributesW(path.c_str());const auto error=GetLastError();
            require(flags==INVALID_FILE_ATTRIBUTES&&(error==ERROR_FILE_NOT_FOUND||error==ERROR_PATH_NOT_FOUND),"ftn_msg_removal_absence_unconfirmed");
            record_portable_removed_file(path,logical);
        }
        else{const auto attr=static_cast<std::uint16_t>(static_cast<unsigned char>(raw[186])|(static_cast<unsigned char>(raw[187])<<8)|8);const unsigned char sent[]{static_cast<unsigned char>(attr),static_cast<unsigned char>(attr>>8)};LARGE_INTEGER at{};at.QuadPart=186;DWORD wrote{};require(SetFilePointerEx(handle.get(),at,nullptr,FILE_BEGIN)&&WriteFile(handle.get(),sent,2,&wrote,nullptr)&&wrote==2&&FlushFileBuffers(handle.get()),"ftn_msg_sent_mark_failed");}
    }
};
app::Identifier128 payload_id(const app::Identifier128& operation,std::size_t ordinal,const app::FilePayloadIdentity& p){
    return token("SFX/StoredMSG/payload/v1/"+ftn_native_artifact_name(operation)+"/"+std::to_string(ordinal)+"/"+p.volume+"/"+p.file_id+"/"+std::to_string(p.bytes)+"/"+binary(p.sha256));
}
void stage(const FilePayloadDirectory& directory,const app::FtnOutboundJob& job,const FilePayloadLease& source,const std::function<bool()>& running){
    const auto name=ftn_existing_spool_name(directory,job);std::optional<FilePayloadLease> out;std::uint64_t offset{};
    if(directory.absent(name))out.emplace(directory.create(name,std::max<std::uint64_t>(1,job.file.size)));
    else{
        // Like deterministic generated FTN packets, an unpublished prefix is
        // reusable only after every byte matches this still-locked source.
        app::FilePayloadIdentity prefix;{auto old=directory.read(name);prefix=old.identity();require(prefix.bytes<=job.file.size,"ftn_msg_staged_size_changed");
            for(std::uint64_t at=0;at<prefix.bytes;){require(running(),"ftn_msg_cancelled");const auto a=old.read(at,static_cast<std::size_t>(std::min<std::uint64_t>(65536,prefix.bytes-at))),b=source.read(at,a.size());require(a==b&&!a.empty(),"ftn_msg_staged_bytes_changed");at+=a.size();}}
        if(prefix.bytes==job.file.size){require(prefix.sha256==job.digest,"ftn_msg_staged_hash_changed");return;}
        out.emplace(directory.resume_private(name,job.file.size,prefix));offset=prefix.bytes;
    }
    while(offset<job.file.size){require(running(),"ftn_msg_cancelled");const auto bytes=source.read(offset,static_cast<std::size_t>(std::min<std::uint64_t>(65536,job.file.size-offset)));require(!bytes.empty(),"ftn_msg_source_changed");out->append(bytes.data(),bytes.size());offset+=bytes.size();}
    const auto final=out->identity();require(final.bytes==job.file.size&&final.sha256==job.digest,"ftn_msg_staged_hash_changed");
}
std::filesystem::path attachment(const std::filesystem::path& root,const std::filesystem::path& folder,std::string_view specification){
    auto p=std::filesystem::u8path(app::qwk_cp437_to_utf8(specification));
    require(!p.empty()&&p.has_filename(),"ftn_msg_attachment_path_invalid");if(!p.is_absolute())p=folder/p;
    require(p.is_absolute(),"ftn_msg_attachment_path_invalid");return resolve_portable_owned_path(root,p);
}
}
FtnStoredScanResult scan_ftn_stored_messages(app::IFtnMailStorage& storage,const app::FtnConfiguration& config,const ins::Receipt& receipt,std::string& after,const std::function<bool()>& running){
    FtnStoredScanResult result;
    auto directories=app::ftn_stored_directories(config);
    try{
    std::vector<app::FtnStoredDirectory> resolved;
    for(auto source:directories){auto path=std::filesystem::u8path(source.directory);if(path.is_relative())require(!path.has_root_name(),"ftn_msg_directory_drive_relative");path=resolve_portable_owned_path(receipt.root,path);source.directory=path.lexically_normal().u8string();const auto found=std::find_if(resolved.begin(),resolved.end(),[&](const auto& prior){return lower(prior.directory)==lower(source.directory);});if(found==resolved.end())resolved.push_back(std::move(source));else for(const auto& network:source.networks)if(std::find(found->networks.begin(),found->networks.end(),network)==found->networks.end())found->networks.push_back(network);}
    directories=std::move(resolved);
    }catch(...){++result.failed;result.reason="ftn_msg_directory_unavailable";return result;}if(directories.empty()){after.clear();return result;}
    std::size_t source_index{};std::vector<std::string> source_keys;
    const auto advance=[&]{if(directories.size()==1)after.clear();else after="@"+source_keys[(source_index+1)%directories.size()]+":";};
    try{
        for(const auto& source:directories)source_keys.push_back(ftn_native_artifact_name(token("SFX/StoredMSG/source/v2/"+lower(portable_logical_owned_path(receipt.root,std::filesystem::u8path(source.directory)).u8string()))));
        if(!after.empty()&&after.front()=='@'){
            const auto split=after.find(':');const auto found=split==std::string::npos?source_keys.end():std::find(source_keys.begin(),source_keys.end(),after.substr(1,split-1));
            if(found==source_keys.end())after.clear();else{source_index=static_cast<std::size_t>(found-source_keys.begin());after.erase(0,split+1);}
        }
        const auto& scope=directories[source_index];
        const std::vector<std::filesystem::path> protected_paths{receipt.root/L"Bin",receipt.root/L"State",receipt.root/L"Config",receipt.root/L"Logs",receipt.root/L"Data",receipt.root/L"Work",receipt.root/L"Backup"};
        auto request_protected=protected_paths;
        // A peer-supplied .MSG in a transport spool is not a local game request.
        // A dedicated FTN/NetMail sibling remains a valid owner-selected source.
        for(const auto* role:{L"Inbound",L"Partial",L"Outbound",L"Nonsecure",L"Bad",L"Processed",L"Conflicts"})request_protected.push_back(receipt.root/L"FTN"/role);
        auto directory=FilePayloadDirectory::import_source(std::filesystem::u8path(scope.directory),request_protected,receipt.owner,true);
        auto names=directory.names(10000);std::sort(names.begin(),names.end());std::size_t inspected{};bool end=true;
        for(const auto& name:names){
            if(name<=after)continue;if(inspected++==8){end=false;break;}after=name;require(running(),"ftn_msg_cancelled");
            const auto dot=name.find('.');if(dot==name.npos||dot==0||lower(name.substr(dot))!=".msg"||!std::all_of(name.begin(),name.begin()+dot,[](char c){return c>='0'&&c<='9';}))continue;
            try{
                StoredFile message(directory.path()/std::filesystem::u8path(name));
                const auto attr=static_cast<unsigned char>(message.raw[186])|(static_cast<unsigned char>(message.raw[187])<<8);
                if(!(attr&0x100)||!(attr&0x10)||(attr&8))continue;
                const auto parsed=app::parse_ftn_stored_message(message.raw);require(parsed.has_value(),"ftn_msg_header_or_address_invalid");const auto& request=*parsed;
                const auto eligible=[&](const app::FtnNetwork& n){return n.enabled&&std::find(scope.networks.begin(),scope.networks.end(),n.code)!=scope.networks.end()&&app::ftn_local_address(n,request.origin);};
                if(std::none_of(config.networks.begin(),config.networks.end(),eligible))continue;
                require(!request.truncate_files,"ftn_msg_tfs_not_supported");
                // Local reading changes Rcvd and timesRead, not the outbound
                // request. Only Sent determines that it has already been sent.
                auto intent=message.raw;intent[164]=intent[165]=0;intent[186]=static_cast<char>(static_cast<unsigned char>(intent[186])&~4U);
                const auto operation=token("SFX/StoredMSG/request/v1/"+lower(portable_logical_owned_path(receipt.root,directory.path()/std::filesystem::u8path(name)).u8string())+"/"+message.identity+"/"+binary(hash(intent)));
                const auto prior=storage.ftn_jobs_for_operation(owner,operation);require(prior&&prior.value,"ftn_msg_receipt_unavailable");auto jobs=*prior.value;
                if(jobs.empty()){
                    const app::FtnNetwork* network{};std::string via;
                    for(const auto& n:config.networks)if(eligible(n)){
                        const auto route=app::ftn_route_link(config,n.code,request.destination);if(route){require(!network,"ftn_msg_network_ambiguous");network=&n;via=*route;}}
                    require(network,"ftn_msg_no_configured_route");std::set<std::string> wire_names;
                    std::vector<FilePayloadLease> sources;
                    for(std::size_t ordinal=0;ordinal<request.attachments.size();++ordinal){
                        require(running(),"ftn_msg_cancelled");const auto path=attachment(receipt.root,directory.path(),request.attachments[ordinal]);
                        auto folder=FilePayloadDirectory::import_source(path.parent_path(),protected_paths,receipt.owner);
                        sources.push_back(folder.read(path.filename().u8string()));const auto actual=sources.back().identity([&](auto,auto){return running();});
                        app::FtnOutboundJob job;job.operation=operation;job.ordinal=static_cast<std::uint16_t>(ordinal);job.id=payload_id(operation,ordinal,actual);job.network=network->code;job.link=via;job.kind=app::FtnOutboundKind::payload;job.flavor=request.hold?app::FtnMailFlavor::hold:(attr&2)?app::FtnMailFlavor::crash:app::FtnMailFlavor::normal;
                        job.file={path.filename().u8string(),actual.bytes,0};job.digest=actual.sha256;
                        require(app::valid_ftn_leaf_name(job.file.name)&&wire_names.insert(lower(job.file.name)).second&&actual.bytes<=app::kFtnTransferMaximumBytes,"ftn_msg_attachment_descriptor_invalid");
                        auto outgoing=FilePayloadDirectory::private_store(receipt.root/L"FTN"/L"Outbound"/std::filesystem::u8path(network->code),receipt.owner);stage(outgoing,job,sources.back(),running);jobs.push_back(std::move(job));
                    }
                    require(running()&&storage.queue_ftn_jobs(owner,config.revision,jobs).succeeded(),"ftn_msg_queue_commit_failed");++result.queued;
                }else{
                    require(jobs.size()==request.attachments.size(),"ftn_msg_receipt_changed");
                    require(std::all_of(jobs.begin(),jobs.end(),[&](const auto& job){return std::any_of(config.networks.begin(),config.networks.end(),[&](const auto& n){return n.code==job.network&&eligible(n);});}),"ftn_msg_receipt_network_changed");
                    if(!std::all_of(jobs.begin(),jobs.end(),[](const auto& j){return j.state==app::FtnOutboundState::delivered;}))continue;
                    if(request.kill_files)for(std::size_t i=0;i<jobs.size();++i){
                        require(running(),"ftn_msg_cancelled");const auto path=attachment(receipt.root,directory.path(),request.attachments[i]);auto folder=FilePayloadDirectory::import_source(path.parent_path(),protected_paths,receipt.owner,true);
                        if(folder.absent(path.filename().u8string()))continue;
                        auto source=folder.read(path.filename().u8string(),true);const auto actual=source.identity([&](auto,auto){return running();});
                        require(jobs[i].ordinal==i&&jobs[i].file.name==path.filename().u8string()&&jobs[i].id==payload_id(operation,i,actual)&&jobs[i].digest==actual.sha256,"ftn_msg_cleanup_source_changed");source.remove_exact();
                    }
                    require(running(),"ftn_msg_cancelled");message.finish((attr&0x80)!=0);++result.completed;
                }
            }catch(const std::exception& error){++result.failed;const std::string why=error.what();result.reason=why.rfind("ftn_msg_",0)==0?why:"ftn_msg_file_access_failed";}
        }
        if(end)advance();else after="@"+source_keys[source_index]+":"+after;
    }catch(const std::exception& error){++result.failed;const std::string why=error.what();result.reason=why.rfind("ftn_msg_",0)==0?why:"ftn_msg_directory_unavailable";if(source_keys.size()==directories.size())advance();else after.clear();}
    return result;
}
}
