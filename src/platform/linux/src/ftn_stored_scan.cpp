#include "sfx/platform/linux/ftn_stored_scan.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/ftn_native_session.hpp"
#include "sfx/application/ftn_stored_message.hpp"
#include "sfx/application/ftn_mail.hpp"
#include "sfx/application/ftn_setup.hpp"
#include "sfx/application/qwk.hpp"
#include <sodium.h>
#include <algorithm>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cerrno>
#include <stdexcept>
namespace sfx::platform::linux_host {
namespace {
namespace app=application;
const app::MessageConfigurationActor owner{true,{}};
void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
std::string lower(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
app::FtnDigest hash(std::string_view bytes){app::FtnDigest d{};require(sodium_init()>=0&&crypto_hash_sha256(d.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())==0,"ftn_msg_hash_failed");return d;}
app::Identifier128 token(std::string_view bytes){const auto d=hash(bytes);app::Identifier128 id;std::copy_n(d.begin(),16,id.bytes.begin());require(id.valid(),"ftn_msg_identity_failed");return id;}
std::string binary(const app::FtnDigest& d){return {reinterpret_cast<const char*>(d.data()),d.size()};}
#include "ftn_stored_file.inc"

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
std::filesystem::path attachment(const FtnInstallation& receipt,const std::filesystem::path& folder,std::string_view specification){
    auto p=std::filesystem::u8path(app::qwk_cp437_to_utf8(specification));
    require(!p.empty()&&p.has_filename(),"ftn_msg_attachment_path_invalid");if(!p.is_absolute())p=folder/p;
    require(p.is_absolute(),"ftn_msg_attachment_path_invalid");return receipt.bindings->resolve_path(p);
}
}
FtnStoredScanResult scan_ftn_stored_messages(app::IFtnMailStorage& storage,const app::FtnConfiguration& config,const FtnInstallation& receipt,std::string& after,const std::function<bool()>& running){
    FtnStoredScanResult result;
    const auto map=std::dynamic_pointer_cast<LinuxFilePayloadBindings>(receipt.bindings);
    if(!map){result.failed=1;result.reason="ftn_msg_bindings_unavailable";return result;}
    // The common Windows source collector folds case and separators. POSIX
    // directory identity must instead be resolved before any deduplication.
    std::vector<app::FtnStoredDirectory> directories;
    for(const auto& network:config.networks)if(network.enabled&&network.msg_netmail_enabled.value_or(false)&&!network.msg_netmail_directory.empty())
        directories.push_back({network.msg_netmail_directory,{network.code}});
    try{
    std::vector<app::FtnStoredDirectory> resolved;
    for(auto source:directories){auto path=std::filesystem::u8path(source.directory);if(path.is_relative())require(!path.has_root_name(),"ftn_msg_directory_drive_relative");path=receipt.bindings->resolve_path(path);source.directory=path.lexically_normal().u8string();const auto found=std::find_if(resolved.begin(),resolved.end(),[&](const auto& prior){return prior.directory==source.directory;});if(found==resolved.end())resolved.push_back(std::move(source));else for(const auto& network:source.networks)if(std::find(found->networks.begin(),found->networks.end(),network)==found->networks.end())found->networks.push_back(network);}
    directories=std::move(resolved);
    }catch(...){++result.failed;result.reason="ftn_msg_directory_unavailable";return result;}if(directories.empty()){after.clear();return result;}
    std::size_t source_index{};std::vector<std::string> source_keys;
    const auto advance=[&]{if(directories.size()==1)after.clear();else after="@"+source_keys[(source_index+1)%directories.size()]+":";};
    try{
        for(const auto& source:directories)source_keys.push_back(ftn_native_artifact_name(token("SFX/StoredMSG/source/v2/"+map->logical_path(std::filesystem::u8path(source.directory)).u8string())));
        if(!after.empty()&&after.front()=='@'){
            const auto split=after.find(':');const auto found=split==std::string::npos?source_keys.end():std::find(source_keys.begin(),source_keys.end(),after.substr(1,split-1));
            if(found==source_keys.end())after.clear();else{source_index=static_cast<std::size_t>(found-source_keys.begin());after.erase(0,split+1);}
        }
        const auto& scope=directories[source_index];
        const std::vector<std::filesystem::path> protected_paths{receipt.root/L"bin",receipt.root/L"state",receipt.root/L"config",receipt.root/L"logs",receipt.root/L"data",receipt.root/L"work",receipt.root/L"backup"};
        auto request_protected=protected_paths;
        // A peer-supplied .MSG in a transport spool is not a local game request.
        // A dedicated FTN/NetMail sibling remains a valid owner-selected source.
        for(const auto* role:{L"inbound",L"partial",L"outbound",L"nonsecure",L"bad",L"processed",L"conflicts"})request_protected.push_back(receipt.root/L"ftn"/role);
        auto directory=FilePayloadDirectory::import_source(std::filesystem::u8path(scope.directory),request_protected,receipt.bindings,true);
        auto names=directory.names(10000);std::sort(names.begin(),names.end());std::size_t inspected{};bool end=true;
        for(const auto& name:names){
            if(name<=after)continue;
            if(inspected++==8){end=false;break;}after=name;require(running(),"ftn_msg_cancelled");
            const auto dot=name.find('.');if(dot==name.npos||dot==0||lower(name.substr(dot))!=".msg"||!std::all_of(name.begin(),name.begin()+dot,[](char c){return c>='0'&&c<='9';}))continue;
            try{
                StoredFile message(directory,name);
                const auto attr=static_cast<unsigned char>(message.raw[186])|(static_cast<unsigned char>(message.raw[187])<<8);
                if(!(attr&0x100)||!(attr&0x10)||(attr&8))continue;
                const auto parsed=app::parse_ftn_stored_message(message.raw);require(parsed.has_value(),"ftn_msg_header_or_address_invalid");const auto& request=*parsed;
                const auto eligible=[&](const app::FtnNetwork& n){return n.enabled&&std::find(scope.networks.begin(),scope.networks.end(),n.code)!=scope.networks.end()&&app::ftn_local_address(n,request.origin);};
                if(std::none_of(config.networks.begin(),config.networks.end(),eligible))continue;
                require(!request.truncate_files,"ftn_msg_tfs_not_supported");
                const auto current_policy=[&]{
                    require(running(),"ftn_msg_cancelled");const auto current=storage.ftn_configuration(owner);
                    require(current&&current.value&&current.value->revision==config.revision,"ftn_msg_configuration_changed");
                };
                current_policy();
                message.bind(map,receipt.root,running);
                // Local reading changes Rcvd and timesRead, not the outbound
                // request. Only Sent determines that it has already been sent.
                auto intent=message.raw;intent[164]=intent[165]=0;intent[186]=static_cast<char>(static_cast<unsigned char>(intent[186])&~4U);
                const auto operation=token("SFX/StoredMSG/request/v1/"+lower(map->logical_path(directory.path()/std::filesystem::u8path(name)).u8string())+"/"+message.identity+"/"+binary(hash(intent)));
                const auto prior=storage.ftn_jobs_for_operation(owner,operation);require(prior&&prior.value,"ftn_msg_receipt_unavailable");auto jobs=*prior.value;
                if(jobs.empty()){
                    const app::FtnNetwork* network{};std::string via;
                    for(const auto& n:config.networks)if(eligible(n)){
                        const auto route=app::ftn_route_link(config,n.code,request.destination);if(route){require(!network,"ftn_msg_network_ambiguous");network=&n;via=*route;}}
                    require(network,"ftn_msg_no_configured_route");std::set<std::string> wire_names;
                    std::vector<FilePayloadLease> sources;
                    for(std::size_t ordinal=0;ordinal<request.attachments.size();++ordinal){
                        require(running(),"ftn_msg_cancelled");const auto path=attachment(receipt,directory.path(),request.attachments[ordinal]);
                        auto source_bindings=std::make_shared<StoredSourceBindings>(map,receipt.root,path,running);
                        auto folder=FilePayloadDirectory::import_source(path.parent_path(),protected_paths,source_bindings);
                        sources.push_back(folder.read(path.filename().u8string()));const auto actual=sources.back().identity([&](auto,auto){return running();});
                        app::FtnOutboundJob job;job.operation=operation;job.ordinal=static_cast<std::uint16_t>(ordinal);job.id=payload_id(operation,ordinal,actual);job.network=network->code;job.link=via;job.kind=app::FtnOutboundKind::payload;job.flavor=request.hold?app::FtnMailFlavor::hold:(attr&2)?app::FtnMailFlavor::crash:app::FtnMailFlavor::normal;
                        job.file={path.filename().u8string(),actual.bytes,0};job.digest=actual.sha256;
                        require(app::valid_ftn_leaf_name(job.file.name)&&wire_names.insert(lower(job.file.name)).second&&actual.bytes<=app::kFtnTransferMaximumBytes,"ftn_msg_attachment_descriptor_invalid");
                        auto outgoing=FilePayloadDirectory::private_store(receipt.root/L"ftn"/L"outbound"/std::filesystem::u8path(layout::owned_name(network->code)),receipt.bindings);stage(outgoing,job,sources.back(),running);jobs.push_back(std::move(job));
                    }
                    require(running()&&storage.queue_ftn_jobs(owner,config.revision,jobs).succeeded(),"ftn_msg_queue_commit_failed");++result.queued;
                }else{
                    require(jobs.size()==request.attachments.size(),"ftn_msg_receipt_changed");
                    require(std::all_of(jobs.begin(),jobs.end(),[&](const auto& job){return std::any_of(config.networks.begin(),config.networks.end(),[&](const auto& n){return n.code==job.network&&eligible(n);});}),"ftn_msg_receipt_network_changed");
                    if(!std::all_of(jobs.begin(),jobs.end(),[](const auto& j){return j.state==app::FtnOutboundState::delivered;}))continue;
                    if(request.kill_files)for(std::size_t i=0;i<jobs.size();++i){
                        require(running(),"ftn_msg_cancelled");const auto path=attachment(receipt,directory.path(),request.attachments[i]);auto folder=FilePayloadDirectory::import_source(path.parent_path(),protected_paths,receipt.bindings,true);
                        if(folder.absent(path.filename().u8string()))continue;
                        auto source=folder.read(path.filename().u8string(),true);const auto actual=source.identity([&](auto,auto){return running();});
                        require(jobs[i].ordinal==i&&jobs[i].file.name==path.filename().u8string()&&jobs[i].id==payload_id(operation,i,actual)&&jobs[i].digest==actual.sha256,"ftn_msg_cleanup_source_changed");current_policy();source.remove_exact();
                    }
                    current_policy();message.finish((attr&0x80)!=0);++result.completed;
                }
            }catch(const std::exception& error){++result.failed;const std::string why=error.what();result.reason=why.rfind("ftn_msg_",0)==0?why:"ftn_msg_file_access_failed";}
        }
        if(end)advance();else after="@"+source_keys[source_index]+":"+after;
    }catch(const std::exception& error){++result.failed;const std::string why=error.what();result.reason=why.rfind("ftn_msg_",0)==0?why:"ftn_msg_directory_unavailable";if(source_keys.size()==directories.size())advance();else after.clear();}
    return result;
}
}
