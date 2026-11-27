#include "sfx/platform/linux/ftn_native_toss.hpp"

#include "sfx/platform/linux/ftn_native_custody.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include "sfx/platform/linux/ftn_native_tic.hpp"
#include "sfx/application/qwk.hpp"
#include "sfx/application/portable_credential.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <sodium.h>
#include <algorithm>
#include <set>
#include <sstream>
#include <charconv>
#include <ctime>
#include <cstdio>
#include <cstdlib>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;

const app::MessageConfigurationActor owner{true,{}};
void require(bool yes,const char* reason){if(!yes)throw std::runtime_error(reason);}
bool same(std::string_view a,std::string_view b){return a.size()==b.size()&&std::equal(a.begin(),a.end(),b.begin(),[](char x,char y){return (x>='a'&&x<='z'?x-32:x)==(y>='a'&&y<='z'?y-32:y);});}
app::FtnDigest digest(std::string_view bytes){app::FtnDigest out{};crypto_hash_sha256(out.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());return out;}
app::Identifier128 operation(const app::Identifier128& ingress,std::size_t index,std::string_view suffix){
    std::string value="SFX/FTNToss/v1/"+ftn_native_artifact_name(ingress)+"/"+std::to_string(index)+"/"+std::string(suffix);const auto hash=digest(value);app::Identifier128 id;std::copy_n(hash.begin(),id.bytes.size(),id.bytes.begin());require(id.valid(),"ftn_toss_operation_invalid");return id;
}
struct Bytes {std::string value;~Bytes(){if(!value.empty())sodium_memzero(value.data(),value.size());}};
struct TextErase {std::string& value;~TextErase(){if(!value.empty())sodium_memzero(value.data(),value.size());}};
struct PasswordErase {std::array<std::uint8_t,8>& value;~PasswordErase(){sodium_memzero(value.data(),value.size());}};
struct PacketSecretsErase {app::FtnPacket& value;~PacketSecretsErase(){sodium_memzero(value.header.password.data(),value.header.password.size());for(auto& message:value.messages)if(!message.subject.empty())sodium_memzero(message.subject.data(),message.subject.size());}};
void publish_packet(const FilePayloadDirectory& directory,const std::string& name,std::string_view bytes,const std::function<bool()>& running){
    std::optional<FilePayloadLease> output;std::uint64_t offset{};
    if(directory.absent(name))output.emplace(directory.create(name,bytes.size()));
    else {
        application::FilePayloadIdentity prefix;{auto file=directory.read(name);prefix=file.identity();}
        require(prefix.bytes<=bytes.size()&&prefix.sha256==digest(bytes.substr(0,static_cast<std::size_t>(prefix.bytes))),"ftn_packet_staged_content_mismatch");
        if(prefix.bytes==bytes.size())return;
        // Recover only an identical deterministic unpublished prefix, pinned by
        // its actual volume/file identity. Never overwrite a different object.
        output.emplace(directory.resume_private(name,bytes.size(),prefix));offset=prefix.bytes;
    }
    while(offset<bytes.size()){require(running(),"ftn_packet_publication_cancelled");const auto count=std::min<std::size_t>(65536,bytes.size()-static_cast<std::size_t>(offset));output->append(reinterpret_cast<const std::uint8_t*>(bytes.data()+offset),count);offset+=count;}
    const auto actual=output->identity();require(actual.bytes==bytes.size()&&actual.sha256==digest(bytes),"ftn_packet_publication_mismatch");
}
struct PacketSet {std::vector<app::FtnPacket> packets;std::vector<std::string> names;~PacketSet(){for(auto& packet:packets){PacketSecretsErase wipe{packet};}}};
bool packet_name(std::string_view name){return name.size()>4&&same(name.substr(name.size()-4),".pkt");}
const app::FtnNetwork& network_for(const app::FtnConfiguration& c,const app::FtnIngressReceipt& receipt,const app::FtnPacketHeader& header){
    const app::FtnNetwork* found{};
    for(const auto& n:c.networks)if(n.enabled&&(receipt.network.empty()||same(n.code,receipt.network))&&app::ftn_local_address(n,header.destination)) {require(!found,"ftn_toss_ambiguous_network");found=&n;}
    require(found,"ftn_toss_destination_not_local");return *found;
}
std::string reason(const std::exception& error){const std::string value=error.what();if(value.size()>64||value.rfind("ftn_",0)!=0||!std::all_of(value.begin(),value.end(),[](char c){return (c>='a'&&c<='z')||c=='_'||(c>='0'&&c<='9');}))return "ftn_toss_local_failure";return value;}
void decode_bundle(std::string_view bytes,std::string_view domain,PacketSet& out){
    if(bytes.size()>=4&&bytes.substr(0,4)==std::string_view("PK\3\4",4)) {
        struct Reader {::archive* p{archive_read_new()};~Reader(){if(p)archive_read_free(p);}} reader;
        require(reader.p&&archive_read_support_format_zip(reader.p)==ARCHIVE_OK&&archive_read_support_filter_none(reader.p)==ARCHIVE_OK,"ftn_zip_reader_failed");
        require(archive_read_open_memory(reader.p,bytes.data(),bytes.size())==ARCHIVE_OK,"ftn_zip_open_failed");
        archive_entry* entry{};std::size_t total{},count{};std::set<std::string> names;
        for(;;){const auto status=archive_read_next_header(reader.p,&entry);if(status==ARCHIVE_EOF)break;require(status==ARCHIVE_OK&&++count<=128,"ftn_zip_entry_limit_or_corrupt");
            const auto* name=archive_entry_pathname(entry);const auto size=archive_entry_size(entry);
            require(name&&app::valid_ftn_leaf_name(name)&&packet_name(name)&&archive_entry_filetype(entry)==AE_IFREG&&!archive_entry_hardlink(entry)&&!archive_entry_symlink(entry)&&!archive_entry_is_encrypted(entry)&&size>0&&static_cast<std::uint64_t>(size)<=app::kFtnPacketMaximumBytes-total,"ftn_zip_member_not_safe_packet");
            std::string normalized=name;for(auto& ch:normalized)if(ch>='a'&&ch<='z')ch-=32;require(names.insert(normalized).second,"ftn_zip_duplicate_packet_name");
            Bytes member;member.value.resize(static_cast<std::size_t>(size));std::size_t offset{};while(offset<member.value.size()){const auto got=archive_read_data(reader.p,member.value.data()+offset,std::min<std::size_t>(65536,member.value.size()-offset));require(got>0,"ftn_zip_member_truncated");offset+=static_cast<std::size_t>(got);}char extra{};require(archive_read_data(reader.p,&extra,1)==0,"ftn_zip_member_size_mismatch");
            total+=member.value.size();require(total<=std::max<std::size_t>(1024*1024,bytes.size()*128),"ftn_zip_expansion_limit");auto decoded=app::decode_ftn_packet(member.value,domain,true);require(decoded.packet.has_value(),"ftn_zip_packet_invalid");out.names.push_back(name);out.packets.push_back(std::move(*decoded.packet));
        }
        require(!out.packets.empty()&&archive_read_close(reader.p)==ARCHIVE_OK,"ftn_zip_incomplete");
    }else {auto decoded=app::decode_ftn_packet(bytes,domain,true);require(decoded.packet.has_value(),"ftn_packet_invalid");out.names.push_back("packet");out.packets.push_back(std::move(*decoded.packet));}
    std::size_t messages{};for(const auto& packet:out.packets)messages+=packet.messages.size();require(messages<=4096,"ftn_toss_message_limit");
}
bool packet_password(app::FtnPacketHeader& header,const app::FtnLink* link,const std::string& installation){
    auto supplied=header.password;header.password.fill(0);
    struct Erase {std::array<std::uint8_t,8>& b;~Erase(){sodium_memzero(b.data(),b.size());}};
    Erase erase{supplied};
    if(!link||link->credentials.packet.empty())return false;
    auto secret=unprotect_ftn_secret(installation,link->network,link->code,FtnCredentialKind::packet,link->credentials.packet);require(secret&&secret->view().size()<=8,"ftn_packet_secret_unavailable");
    std::array<std::uint8_t,8> expected{};Erase wipe{expected};std::copy_n(secret->view().begin(),std::min(secret->view().size(),expected.size()),expected.begin());require(sodium_memcmp(expected.data(),supplied.data(),8)==0,"ftn_packet_password_wrong");return true;
}
std::set<std::pair<unsigned,unsigned>> seen_by(std::string_view text){
    std::set<std::pair<unsigned,unsigned>> result;unsigned net{};
    while(!text.empty()){const auto end=text.find_first_of("\r\n");const auto line=text.substr(0,end);if(end==text.npos)text={};else text.remove_prefix(end+1);if(line.substr(0,8)!="SEEN-BY:")continue;
        std::istringstream words(std::string(line.substr(8)));std::string word;while(words>>word){const auto slash=word.find('/');const auto parse=[](std::string_view v){unsigned n{};const auto r=std::from_chars(v.data(),v.data()+v.size(),n);require(!v.empty()&&r.ec==std::errc{}&&r.ptr==v.data()+v.size()&&n<=65535,"ftn_seenby_invalid");return n;};unsigned node{};if(slash==word.npos){require(net,"ftn_seenby_missing_net");node=parse(word);}else{net=parse(std::string_view(word).substr(0,slash));node=parse(std::string_view(word).substr(slash+1));}require(result.size()<4096,"ftn_seenby_limit");result.emplace(net,node);}
    }return result;
}
app::FtnOutboundJob forward(const app::FtnConfiguration& c,const FtnInstallation& installation,const app::FtnIngressReceipt& receipt,std::size_t index,const app::FtnNetwork& network,const app::FtnLink& link,const app::FtnPacketHeader& header,const app::FtnPackedMessage& message,const app::FtnMessageMetadata& metadata,const std::set<std::pair<unsigned,unsigned>>& seen){
    app::FtnPacket packet;PasswordErase wipe{packet.header.password};packet.header=header;packet.header.origin=*app::parse_ftn_address(network.address);packet.header.destination=*app::parse_ftn_address(link.address);packet.header.profile=app::FtnPacketProfile::type2plus;packet.header.product=0xfe;packet.header.major=1;packet.header.minor=0;packet.header.password.fill(0);packet.messages.push_back(message);
    if(!link.credentials.packet.empty()){auto secret=unprotect_ftn_secret(installation.root.u8string(),network.code,link.code,FtnCredentialKind::packet,link.credentials.packet);require(secret&&secret->view().size()<=8,"ftn_forward_packet_secret_unavailable");std::copy_n(secret->view().begin(),std::min(secret->view().size(),packet.header.password.size()),packet.header.password.begin());}
    if(!metadata.area.empty()) {
        auto& text=packet.messages.front().text;auto seen_all=seen;if(!packet.header.origin.point)seen_all.emplace(packet.header.origin.net,packet.header.origin.node);if(!packet.header.destination.point)seen_all.emplace(packet.header.destination.net,packet.header.destination.node);
        std::string body,paths;std::string_view rest=text;while(!rest.empty()){const auto end=rest.find_first_of("\r\n");const auto row=rest.substr(0,end);if(row.substr(0,6)=="\1PATH:"){paths.append(row);paths+='\r';}else if(row.substr(0,8)!="SEEN-BY:"){body.append(row);body+='\r';}if(end==rest.npos)rest={};else {const auto ch=rest[end];rest.remove_prefix(end+1);if(ch=='\r'&&!rest.empty()&&rest.front()=='\n')rest.remove_prefix(1);}}
        std::string row="SEEN-BY:";for(const auto& address:seen_all){const auto token=" "+std::to_string(address.first)+"/"+std::to_string(address.second);if(row.size()+token.size()>79){body+=row+'\r';row="SEEN-BY:";}row+=token;}if(!seen_all.empty())body+=row+'\r';body+=paths;if(!packet.header.origin.point)body+="\1PATH: "+std::to_string(packet.header.origin.net)+"/"+std::to_string(packet.header.origin.node)+"\r";text=std::move(body);
    }
    auto encoded=app::encode_ftn_packet(packet);sodium_memzero(packet.header.password.data(),8);require(encoded.has_value(),"ftn_forward_packet_invalid");Bytes bytes{std::move(*encoded)};
    app::FtnOutboundJob job;job.operation=operation(receipt.id,index,"import");job.id=operation(receipt.id,index,link.code+"/"+std::to_string(c.revision));job.network=network.code;job.link=link.code;job.echo_tag=metadata.area;job.kind=app::FtnOutboundKind::packet;job.file={ftn_native_artifact_name(job.id).substr(0,32)+".pkt",bytes.value.size(),receipt.file.timestamp};job.digest=digest(bytes.value);
    if(metadata.area.empty())for(const auto& route:c.routes)if(same(route.network,network.code)&&same(route.via_link,link.code)){const auto destination=app::parse_ftn_address(route.destination);if(destination&&app::same_ftn_address(*destination,metadata.destination,true)){job.flavor=route.flavor;break;}}
    const auto directory=FilePayloadDirectory::private_store(installation.root/L"ftn"/L"outbound"/std::filesystem::u8path(layout::owned_name(network.code)),installation.bindings);const auto name=ftn_existing_spool_name(directory,job);
    publish_packet(directory,name,bytes.value,[]{return true;});
    return job;
}
}
std::vector<std::pair<app::FtnAddress,app::FtnAddress>> inspect_ftn_bundle_routes(std::string_view bytes){
    require(bytes.size()<=app::kFtnPacketMaximumBytes,"ftn_packet_size_limit");PacketSet decoded;decode_bundle(bytes,{},decoded);
    std::vector<std::pair<app::FtnAddress,app::FtnAddress>> routes;
    for(const auto& packet:decoded.packets)routes.emplace_back(packet.header.origin,packet.header.destination);
    return routes;
}
static FtnNativeTossResult scan_native_impl(app::IFtnMailStorage& storage,const app::FtnConfiguration& configuration,const FtnInstallation& installation,const app::FtnScanIntent& intent,const std::function<bool()>& running,const app::FtnControlIntent* control,const app::FtnRescanItem* rescan,const application::BbsLocalTimeAdapters* timezone){
    FtnNativeTossResult result;
    try {
        if(!intent.reason.empty()){result.deferred=true;result.reason=intent.reason;return result;}require(running&&running(),"ftn_scan_cancelled");require(configuration.settings.mode!=app::FtnMailerMode::disabled,"ftn_scan_disabled");verify_ftn_directory(installation);
        const auto network=std::find_if(configuration.networks.begin(),configuration.networks.end(),[&](const auto& n){return n.enabled&&same(n.code,intent.network);});require(network!=configuration.networks.end(),"ftn_scan_network_unavailable");
        const auto source=app::parse_ftn_address(intent.source_aka);require(source&&app::ftn_local_address(*network,*source),"ftn_scan_source_unavailable");const auto destination=intent.tag.empty()?app::parse_ftn_address(intent.destination):std::optional<app::FtnAddress>{};require(!intent.tag.empty()||destination.has_value(),"ftn_scan_target_invalid");
        const auto route=destination?app::ftn_route_link(configuration,network->code,*destination):std::nullopt;
        if(destination)require(route&&(control||same(*route,intent.via_link)),"ftn_scan_route_changed");
        const time_t time=intent.created;tm date{};require(time>=0&&gmtime_r(&time,&date)!=nullptr&&date.tm_year+1900<=9999,"ftn_scan_date_invalid");
        const tm packet_date = date;
        int offset_minutes{};
        if (timezone) {
            const auto local = timezone->project_utc_seconds(intent.created);
            require(local.has_value(),"ftn_scan_bbs_time_zone_unavailable");
            date.tm_year = local->date.year - 1900;
            date.tm_mon = local->date.month - 1;
            date.tm_mday = local->date.day;
            date.tm_hour = local->hour;
            date.tm_min = local->minute;
            date.tm_sec = local->second;
            offset_minutes = local->offset_minutes;
        }
        const auto magnitude = std::abs(offset_minutes);
        char tzutc[16]{};
        std::snprintf(tzutc,sizeof(tzutc),"%s%02d%02d",offset_minutes<0?"-":"",magnitude/60,magnitude%60);
        const char* months[]={"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};char stamp[32]{};std::snprintf(stamp,sizeof(stamp),"%02d %s %02d  %02d:%02d:%02d",date.tm_mday,months[date.tm_mon],(date.tm_year+1900)%100,date.tm_hour,date.tm_min,date.tm_sec);
        std::vector<app::FtnOutboundJob> jobs;
        for(const auto& link:configuration.links) {
            if(!link.enabled||!same(link.network,network->code)||!app::ftn_outbound(link))continue;
            if(rescan&&!same(link.code,intent.via_link))continue;
            if(destination?(!route||!same(*route,link.code)):!app::ftn_echo_permission(configuration,network->code,link.code,intent.tag,false,false))continue;
            require(running(),"ftn_scan_cancelled");const auto target=app::parse_ftn_address(link.address);require(target.has_value(),"ftn_scan_link_invalid");
            app::FtnPacket packet;PasswordErase wipe{packet.header.password};packet.header.origin=*source;packet.header.destination=*target;packet.header.year=static_cast<std::uint16_t>(packet_date.tm_year+1900);packet.header.month=static_cast<std::uint16_t>(packet_date.tm_mon);packet.header.day=static_cast<std::uint16_t>(packet_date.tm_mday);packet.header.hour=static_cast<std::uint16_t>(packet_date.tm_hour);packet.header.minute=static_cast<std::uint16_t>(packet_date.tm_min);packet.header.second=static_cast<std::uint16_t>(packet_date.tm_sec);
            if(!link.credentials.packet.empty()){auto secret=unprotect_ftn_secret(installation.root.u8string(),network->code,link.code,FtnCredentialKind::packet,link.credentials.packet);require(secret&&secret->view().size()<=8,"ftn_scan_packet_secret_unavailable");std::copy_n(secret->view().begin(),std::min(secret->view().size(),packet.header.password.size()),packet.header.password.begin());}
            app::FtnPackedMessage message;message.origin_net=source->net;message.origin_node=source->node;const auto recipient=destination?*destination:*target;message.destination_net=recipient.net;message.destination_node=recipient.node;message.attributes=destination?1:0;message.date=stamp;message.from=intent.from;message.to=intent.to;message.subject=intent.subject;
            if(!intent.tag.empty())message.text="AREA:"+intent.tag+"\r";
            message.text+="\1MSGID: "+intent.msgid+"\r";if(!intent.reply.empty())message.text+="\1REPLY: "+intent.reply+"\r";
            message.text+="\1CHRS: CP437 2\r\1TZUTC: ";
            message.text+=tzutc;
            message.text+='\r';
            if(destination){auto from=*source,to=*destination;from.point=to.point=0;from.domain.clear();to.domain.clear();message.text+="\1INTL "+app::format_ftn_address(to)+" "+app::format_ftn_address(from)+"\r";if(source->point)message.text+="\1FMPT "+std::to_string(source->point)+"\r";if(destination->point)message.text+="\1TOPT "+std::to_string(destination->point)+"\r";}
            bool previous_cr{};for(const unsigned char ch:intent.body){require(ch!=0&&ch!=1,"ftn_scan_body_control_invalid");if(ch=='\r')message.text+='\r';else if(ch!='\n'||!previous_cr)message.text+=ch=='\n'?'\r':static_cast<char>(ch);previous_cr=ch=='\r';}if(message.text.back()!='\r')message.text+='\r';
            if(!intent.tag.empty()){const auto origin=application::qwk_utf8_to_cp437(intent.origin_line);require(origin.has_value(),"ftn_scan_origin_unrepresentable");message.text+="--- Spitfire-X\r * Origin: "+*origin+" ("+intent.source_aka+")\r";std::set<std::pair<unsigned,unsigned>> seen;if(!source->point)seen.emplace(source->net,source->node);for(const auto& peer:configuration.links)if(same(peer.network,network->code)&&app::ftn_echo_permission(configuration,network->code,peer.code,intent.tag,false,false)){const auto address=app::parse_ftn_address(peer.address);if(address&&!address->point)seen.emplace(address->net,address->node);}if(!seen.empty()){std::string row="SEEN-BY:";for(const auto& address:seen){const auto token=" "+std::to_string(address.first)+"/"+std::to_string(address.second);if(row.size()+token.size()>79){message.text+=row+'\r';row="SEEN-BY:";}row+=token;}message.text+=row+'\r';}if(!source->point)message.text+="\1PATH: "+std::to_string(source->net)+"/"+std::to_string(source->node)+"\r";}
            packet.messages.push_back(std::move(message));PacketSecretsErase erase_control{packet};
            if(control&&control->password_subject){const auto provider=std::find_if(configuration.links.begin(),configuration.links.end(),[&](const auto& p){return same(p.network,network->code)&&same(p.code,intent.via_link)&&p.enabled&&app::ftn_outbound(p);});require(provider!=configuration.links.end(),"ftn_control_provider_unavailable");auto secret=unprotect_ftn_secret(installation.root.u8string(),network->code,provider->code,FtnCredentialKind::areafix,provider->credentials.areafix);require(secret&&!secret->view().empty()&&secret->view().size()<=70,"ftn_control_secret_unavailable");packet.messages.front().subject=secret->view();}
            auto encoded=app::encode_ftn_packet(packet);sodium_memzero(packet.header.password.data(),packet.header.password.size());require(encoded.has_value(),"ftn_scan_packet_invalid");Bytes bytes{std::move(*encoded)};
            app::FtnOutboundJob job;job.operation=intent.operation;job.ordinal=static_cast<std::uint16_t>(jobs.size());// New unpublished packet format has its own artifact namespace. Old
            // committed jobs/ACK bindings and interrupted legacy files stay intact.
            std::string binding = offset_minutes == 0 ? "SFX/OriginUTC1/" :
                "SFX/OriginLocal1/" + std::string(stamp) + "/" + tzutc + "/";
            binding += intent.network+"/"+link.code+"/"+std::to_string(configuration.revision);
            binding.append(reinterpret_cast<const char*>(intent.fingerprint.data()),intent.fingerprint.size());job.id=operation(intent.operation,0,binding);job.network=network->code;job.link=link.code;job.echo_tag=intent.tag;job.kind=app::FtnOutboundKind::packet;job.file={ftn_native_artifact_name(job.id).substr(0,32)+".pkt",bytes.value.size(),static_cast<std::uint64_t>(std::min<std::int64_t>(UINT32_MAX,intent.created))};job.digest=digest(bytes.value);
            if(destination)for(const auto& r:configuration.routes)if(same(r.network,network->code)&&same(r.via_link,link.code)){const auto address=app::parse_ftn_address(r.destination);if(address&&app::same_ftn_address(*address,*destination,true)){job.flavor=r.flavor;break;}}
            const auto directory=FilePayloadDirectory::private_store(installation.root/L"ftn"/L"outbound"/std::filesystem::u8path(layout::owned_name(network->code)),installation.bindings);const auto name=ftn_existing_spool_name(directory,job);
            publish_packet(directory,name,bytes.value,running);jobs.push_back(std::move(job));
        }
        require(!jobs.empty(),"ftn_scan_no_permitted_destination");require(running(),"ftn_scan_cancelled");const auto committed=rescan?storage.commit_ftn_rescan(owner,configuration.revision,*rescan,jobs.front()):control?storage.commit_ftn_control(owner,configuration.revision,*control,jobs.front()):storage.commit_ftn_scan(owner,configuration.revision,intent,jobs);require(committed.succeeded(),"ftn_scan_commit_retry");result.processed=true;
    }catch(const std::exception& error){result.reason=reason(error);result.deferred=true;}
    return result;
}
#include "ftn_native_tic.inc"
#include "ftn_native_file_publication.inc"
#include "ftn_native_hatch.inc"
FtnNativeTossResult scan_ftn_native_intent(app::IFtnMailStorage& storage,const app::FtnConfiguration& configuration,const FtnInstallation& installation,const app::FtnScanIntent& intent,const std::function<bool()>& running,const application::BbsLocalTimeAdapters* timezone){return scan_native_impl(storage,configuration,installation,intent,running,nullptr,nullptr,timezone);}
FtnNativeTossResult scan_ftn_native_control(app::IFtnMailStorage& storage,const app::FtnConfiguration& configuration,const FtnInstallation& installation,const app::FtnControlIntent& intent,const std::function<bool()>& running,const application::BbsLocalTimeAdapters* timezone){return scan_native_impl(storage,configuration,installation,intent.text,running,&intent,nullptr,timezone);}
FtnNativeTossResult scan_ftn_native_rescan(app::IFtnMailStorage& storage,const app::FtnConfiguration& configuration,const FtnInstallation& installation,const app::FtnRescanItem& item,const std::function<bool()>& running,const application::BbsLocalTimeAdapters* timezone){
    FtnNativeTossResult result;try{require(running&&running(),"ftn_rescan_cancelled");if(item.text.reason=="ftn_rescan_paused"){result.deferred=true;result.reason=item.text.reason;return result;}
        if(!item.arrival||!item.text.reason.empty()){require(storage.commit_ftn_rescan(owner,configuration.revision,item,{}).succeeded(),"ftn_rescan_finish_retry");result.processed=true;result.reason=item.text.reason;return result;}
        if(!item.original)return scan_native_impl(storage,configuration,installation,item.text,running,nullptr,&item,timezone);
        verify_ftn_directory(installation);require(configuration.settings.mode!=app::FtnMailerMode::disabled,"ftn_rescan_disabled");const auto network=std::find_if(configuration.networks.begin(),configuration.networks.end(),[&](const auto& n){return n.enabled&&same(n.code,item.text.network);});require(network!=configuration.networks.end(),"ftn_rescan_network_unavailable");const auto link=std::find_if(configuration.links.begin(),configuration.links.end(),[&](const auto& l){return same(l.network,network->code)&&same(l.code,item.text.via_link)&&app::ftn_echo_permission(configuration,l.network,l.code,item.text.tag,false,false);});require(link!=configuration.links.end(),"ftn_rescan_link_unavailable");app::FtnPacketHeader header;const time_t now=item.text.created;tm date{};require(now>=0&&gmtime_r(&now,&date)!=nullptr,"ftn_rescan_date_invalid");header.year=static_cast<std::uint16_t>(date.tm_year+1900);header.month=static_cast<std::uint16_t>(date.tm_mon);header.day=static_cast<std::uint16_t>(date.tm_mday);header.origin=item.original_origin;header.destination=*app::parse_ftn_address(network->address);const auto metadata=app::inspect_ftn_message(header,*item.original);require(metadata&&same(metadata->area,item.text.tag),"ftn_rescan_original_invalid");app::FtnIngressReceipt scope;scope.id=item.text.operation;scope.file.timestamp=0;auto job=forward(configuration,installation,scope,0,*network,*link,header,*item.original,*metadata,seen_by(item.original->text));job.operation=item.text.operation;require(running(),"ftn_rescan_cancelled");require(storage.commit_ftn_rescan(owner,configuration.revision,item,job).succeeded(),"ftn_rescan_commit_retry");result.processed=true;
    }catch(const std::exception& error){result.reason=reason(error);result.deferred=true;}return result;
}
FtnNativeTossResult toss_ftn_native_receipt(app::IFtnMailStorage& storage,const app::FtnConfiguration& configuration,const FtnInstallation& installation,const app::FtnIngressReceipt& receipt,const std::function<bool()>& running){
    FtnNativeTossResult result;std::optional<FilePayloadLease> file;std::optional<FilePayloadDirectory> bad,processed;std::optional<app::FtnHeldMessage> observation;std::vector<app::FtnHeldMessage> rejected;bool trusted_file{};std::size_t rejected_count{};
    const auto configuration_retry=[&](const app::StorageStatus& status){if(status.code!=app::StorageErrorCode::conflict)return false;const auto latest=storage.ftn_configuration(owner);if(latest&&latest.value&&latest.value->revision==configuration.revision)return false;result.deferred=true;result.configuration_changed=latest&&latest.value&&latest.value->revision!=configuration.revision;result.reason="ftn_toss_configuration_retry";return true;};
    try {
        require(running&&running(),"ftn_toss_cancelled");
        if(!app::ftn_message_wire_name(receipt.file.name)){result.deferred=true;return result;}require(configuration.settings.mode!=app::FtnMailerMode::disabled,"ftn_toss_disabled");verify_ftn_directory(installation);
        const auto network=std::find_if(configuration.networks.begin(),configuration.networks.end(),[&](const auto& n){return n.enabled&&same(n.code,receipt.network);});
        if(!receipt.network.empty())require(network!=configuration.networks.end(),"ftn_toss_network_disabled");
        const auto base=installation.root/L"ftn";
        if(network!=configuration.networks.end()){bad=FilePayloadDirectory::private_store(base/L"bad",installation.bindings);processed=FilePayloadDirectory::private_store(base/L"processed"/std::filesystem::u8path(layout::owned_name(network->code)),installation.bindings);}
        const auto name=processed&&processed->absent(receipt.file.name)?receipt.file.name:ftn_bad_original_name(receipt);
        file.emplace(open_ftn_received(storage,installation,receipt,running));
        const auto actual=file->identity([&](auto,auto){return running();});const auto checkpoint=storage.ftn_ingress_checkpoint(owner,receipt.id);
        require(checkpoint&&checkpoint.value&&checkpoint.value->has_value(),"ftn_toss_native_checkpoint_unavailable");const auto& expected=**checkpoint.value;
        require(actual.volume==expected.volume&&actual.file_id==expected.file_id&&actual.bytes==expected.bytes&&actual.sha256==expected.sha256&&actual.bytes==receipt.file.size&&actual.sha256==receipt.digest,"ftn_toss_native_identity_changed");trusted_file=true;
        if(!app::ftn_message_wire_name(receipt.file.name)){result.deferred=true;result.reason="ftn_waiting_tic_pair_or_review";return result;}
        const auto claimed=storage.claim_ftn_message(owner,receipt.id,configuration.revision);
        if(!claimed||!claimed.value||!*claimed.value){result.deferred=true;result.reason="ftn_message_waiting_tic_inspection_or_ambiguity";return result;}
        require(actual.bytes<=app::kFtnPacketMaximumBytes,"ftn_packet_size_limit");Bytes bytes;bytes.value.reserve(static_cast<std::size_t>(actual.bytes));for(std::uint64_t offset=0;offset<actual.bytes;){require(running(),"ftn_toss_cancelled");const auto chunk=file->read(offset,65536);require(!chunk.empty(),"ftn_toss_native_read_failed");bytes.value.append(reinterpret_cast<const char*>(chunk.data()),chunk.size());offset+=chunk.size();}
        PacketSet bundle;const auto address=network==configuration.networks.end()?std::optional<app::FtnAddress>{}:app::parse_ftn_address(network->address);decode_bundle(bytes.value,address?address->domain:std::string{},bundle);
        require(storage.advance_ftn_ingress(owner,receipt.id,receipt.received,receipt.received,receipt.digest,app::FtnIngressState::processing,{}).succeeded(),"ftn_toss_processing_checkpoint_failed");
        const auto prior=storage.ftn_toss_progress(owner,receipt.id);require(prior&&prior.value,"ftn_toss_progress_unavailable");std::size_t index{};
        std::size_t member_index{};for(auto& packet:bundle.packets) {
            const auto member_name=bundle.names.at(member_index++);std::size_t member_message{};
            observation.reset();
            const auto& target=network_for(configuration,receipt,packet.header);const app::FtnLink* source{};for(const auto& link:configuration.links)if(same(link.network,target.code)&&same(link.code,receipt.link)){source=&link;break;}
            if(!receipt.network.empty())require(source&&source->enabled,"ftn_toss_link_unavailable");
            const bool verified=packet_password(packet.header,source,installation.root.u8string());
            if(source){auto addresses=source->additional_addresses;addresses.push_back(source->address);require(std::any_of(addresses.begin(),addresses.end(),[&](const auto& a){const auto parsed=app::parse_ftn_address(a);return parsed&&app::same_ftn_address(*parsed,packet.header.origin,true);}),"ftn_packet_origin_not_same_link");}
            for(const auto& message:packet.messages){observation.reset();require(running(),"ftn_toss_cancelled");++member_message;const auto strict_metadata=app::inspect_ftn_message(packet.header,message);const auto metadata=strict_metadata?strict_metadata:app::inspect_ftn_held_message(packet.header,message);
                const auto current_index=index;try {
                require(metadata.has_value(),"ftn_message_controls_invalid");const auto material=app::ftn_message_fingerprint_material(message,*metadata);
                app::FtnImportRequest request;request.operation=operation(receipt.id,index,"import");request.ingress=receipt.id;request.message_index=static_cast<std::uint16_t>(index);request.expected_configuration_revision=configuration.revision;request.network=target.code;request.header=packet.header;request.packet_password_verified=verified;request.message=message;TextErase request_subject{request.message.subject};request.identity=digest(material.identity);request.content=digest(material.content);
                ++index;const auto completed=std::find_if(prior.value->begin(),prior.value->end(),[&](const auto& p){return p.message_index==request.message_index;});if(completed!=prior.value->end()){require(completed->identity==request.identity&&completed->content==request.content,"ftn_toss_progress_content_mismatch");++result.replayed;continue;}
                const bool authenticated=app::ftn_verified_receipt(receipt)||verified;
                if(metadata->area.empty())require(strict_metadata.has_value(),"ftn_message_identity_or_controls_ambiguous");
                if(metadata->area.empty()&&app::ftn_local_address(target,metadata->destination)&&(same(message.to,"AreaFix")||same(message.to,"AreaMgr"))){
                    if(!metadata->reply.empty()){const auto response=storage.record_ftn_provider_response(owner,request);if(response){if(response.value->replayed)++result.replayed;else ++result.imported;continue;}if(configuration_retry(response.status))return result;if(response.status.code==app::StorageErrorCode::busy||response.status.code==app::StorageErrorCode::backpressure||response.status.code==app::StorageErrorCode::io){result.deferred=true;result.reason="ftn_provider_response_retry";return result;}if(response.status.code!=app::StorageErrorCode::not_found)throw std::runtime_error("ftn_provider_response_held");}
                    const auto peer=app::resolve_ftn_peer(configuration,{metadata->origin},target.code);require(peer.has_value(),"ftn_areafix_requester_unknown");const auto requester=std::find_if(configuration.links.begin(),configuration.links.end(),[&](const auto& l){return same(l.network,peer->network)&&same(l.code,peer->link);});require(requester!=configuration.links.end()&&requester->enabled&&app::ftn_areafix_enabled(*requester)&&!requester->credentials.areafix.empty(),"ftn_areafix_not_enabled");
                    auto secret=unprotect_ftn_secret(installation.root.u8string(),target.code,requester->code,FtnCredentialKind::areafix,requester->credentials.areafix);require(secret&&app::equal_ftn_secret(secret->view(),message.subject),"ftn_areafix_password_wrong");const auto commands=app::parse_ftn_areafix(message.text);require(commands&&!commands->empty(),"ftn_areafix_commands_invalid");
                    app::FtnAreaFixRequest control;control.operation=request.operation;control.ingress=receipt.id;control.message_index=request.message_index;control.expected_configuration_revision=configuration.revision;control.network=target.code;control.requesting_link=requester->code;control.origin=metadata->origin;control.destination=metadata->destination;control.envelope_origin=packet.header.origin;control.envelope_destination=packet.header.destination;const auto sender=app::ftn_message_field(message.from,*metadata,35);require(sender&&!sender->empty(),"ftn_areafix_sender_unsupported");control.from=*sender;control.reply=metadata->msgid;control.identity=request.identity;control.content=request.content;control.commands=*commands;control.password_verified=true;control.packet_password_verified=verified;
                    const auto applied=storage.apply_ftn_areafix(owner,control);if(!applied){if(configuration_retry(applied.status))return result;if(applied.status.code==app::StorageErrorCode::busy||applied.status.code==app::StorageErrorCode::backpressure||applied.status.code==app::StorageErrorCode::io){result.deferred=true;result.reason="ftn_areafix_storage_retry";return result;}throw std::runtime_error("ftn_areafix_request_held");}if(applied.value->replayed)++result.replayed;else ++result.imported;
                    if(applied.value->configuration_changed){result.configuration_changed=true;result.deferred=true;result.reason="ftn_areafix_configuration_changed";return result;}continue;
                }
                const auto observed_from=app::ftn_message_field(message.from,*metadata,35),observed_to=app::ftn_message_field(message.to,*metadata,35);
                if(observed_from&&!observed_from->empty()&&observed_to&&(!observed_to->empty()||!metadata->area.empty())){
                    app::FtnHeldMessage held;held.operation=request.operation;held.ingress=receipt.id;held.message_index=request.message_index;held.configuration_revision=configuration.revision;held.identity=request.identity;held.content=request.content;held.payload=actual;held.network=target.code;held.envelope_destination=app::format_ftn_address(packet.header.destination);held.origin=app::format_ftn_address(metadata->origin);held.destination=app::format_ftn_address(metadata->destination);held.from=app::qwk_cp437_to_utf8(*observed_from);held.to=app::qwk_cp437_to_utf8(observed_to->empty()?"All":*observed_to);held.echo=metadata->area;held.local_private=metadata->area.empty()&&app::ftn_local_address(target,metadata->destination);observation=std::move(held);
                }
                require(strict_metadata.has_value(),"ftn_message_identity_or_controls_ambiguous");
                if(!metadata->area.empty())require(authenticated&&source&&app::ftn_echo_permission(configuration,target.code,source->code,metadata->area,false,true)&&std::any_of(configuration.message_echoes.begin(),configuration.message_echoes.end(),[&](const auto& e){return same(e.network,target.code)&&same(e.tag,metadata->area);}),"ftn_echo_receive_not_authorized");
                if(metadata->area.empty()&&!app::ftn_local_address(target,metadata->destination))require(authenticated,"ftn_transit_not_authenticated");
                const auto duplicate=storage.known_ftn_message(owner,target.code,metadata->area,app::format_ftn_address(metadata->origin),request.identity,request.content);
                if(!duplicate&&(duplicate.status.code==app::StorageErrorCode::busy||duplicate.status.code==app::StorageErrorCode::backpressure||duplicate.status.code==app::StorageErrorCode::io)){result.deferred=true;result.reason="ftn_toss_storage_retry";return result;}
                if(!duplicate&&duplicate.status.code==app::StorageErrorCode::conflict)throw std::runtime_error("ftn_message_id_content_collision");
                require(duplicate&&duplicate.value.has_value(),"ftn_message_duplicate_check_failed");
                const auto seen=metadata->area.empty()?std::set<std::pair<unsigned,unsigned>>{}:seen_by(message.text);
                auto forward_seen=seen;
                if(!metadata->area.empty())for(const auto& recipient:configuration.links)if(!same(recipient.code,receipt.link)&&app::ftn_echo_permission(configuration,target.code,recipient.code,metadata->area,false,false)&&same(recipient.network,target.code)){const auto recipient_address=app::parse_ftn_address(recipient.address);if(recipient_address&&!recipient_address->point)forward_seen.emplace(recipient_address->net,recipient_address->node);}
                const auto route=metadata->area.empty()&&!app::ftn_local_address(target,metadata->destination)?app::ftn_route_link(configuration,target.code,metadata->destination):std::nullopt;
                for(const auto& link:configuration.links)if(!*duplicate.value&&link.enabled&&same(link.network,target.code)&&!same(link.code,receipt.link)) {
                    const auto destination=app::parse_ftn_address(link.address);if(!destination)continue;
                    const bool send=metadata->area.empty()?route&&same(*route,link.code):app::ftn_echo_permission(configuration,target.code,link.code,metadata->area,false,false)&&(destination->point||!seen.count({destination->net,destination->node}));
                    if(send){auto job=forward(configuration,installation,receipt,request.message_index,target,link,packet.header,message,*metadata,forward_seen);job.ordinal=static_cast<std::uint16_t>(request.forwarding.size());request.forwarding.push_back(std::move(job));}
                }
                const auto imported=storage.import_ftn_message(owner,request);if(!imported){if(configuration_retry(imported.status))return result;if(imported.status.code==app::StorageErrorCode::busy||imported.status.code==app::StorageErrorCode::backpressure||imported.status.code==app::StorageErrorCode::io){result.deferred=true;result.reason="ftn_toss_storage_retry";return result;}
                    const std::string hold_reason=imported.status.operation=="FTN local recipient unmatched"?"ftn_recipient_unmatched":imported.status.operation=="FTN recipient ambiguous"?"ftn_recipient_ambiguous":imported.status.operation=="FTN reviewed recipient is stale"?"ftn_recipient_stale":imported.status.operation=="FTN source or header cannot be represented"?"ftn_message_charset_unsupported":imported.status.operation=="FTN source rendition unsupported"?"ftn_message_body_unsupported":imported.status.operation=="FTN duplicate MSGID content collision"?"ftn_message_id_content_collision":"ftn_message_import_held";
                    throw std::runtime_error(hold_reason);
                }if(imported.value->replayed)++result.replayed;else ++result.imported;observation.reset();
                }catch(const std::exception& error){
                    if(index==current_index)++index;
                    const auto why=reason(error);
                    // Only message-local semantic rejection can continue. Native
                    // custody, configuration, authentication and storage failures
                    // retain the conservative whole-original retry boundary.
                    const bool local=why=="ftn_message_controls_invalid"||why=="ftn_message_identity_or_controls_ambiguous"||why=="ftn_echo_receive_not_authorized"||why=="ftn_message_import_held"||why=="ftn_message_charset_unsupported"||why=="ftn_message_body_unsupported"||why=="ftn_message_id_content_collision"||why=="ftn_recipient_unmatched"||why=="ftn_recipient_ambiguous"||why=="ftn_recipient_stale";
                    if(!local)throw;
                    ++rejected_count;if(observation){observation->reason=why;rejected.push_back(*observation);observation.reset();}
                    result.diagnostics.push_back("Held "+receipt.file.name+" / "+member_name+" message "+std::to_string(member_message)+(metadata&&!metadata->area.empty()?" echo "+metadata->area:"")+": "+why+".");
                }
            }
        }
        if(rejected_count){
            // Commit all independent imports first. Marking an observation held
            // earlier would make later imports lose processing authority.
            for(const auto& held:rejected)require(storage.record_ftn_held_message(owner,held).succeeded(),"ftn_held_observation_checkpoint_failed");
            result.reason=rejected.empty()?"ftn_message_controls_invalid":rejected.front().reason;
            if(bad&&file->path().parent_path()!=bad->path())file->rename_to(*bad,bad->absent(receipt.file.name)?receipt.file.name:ftn_bad_original_name(receipt));
            require(storage.advance_ftn_ingress(owner,receipt.id,receipt.received,receipt.received,receipt.digest,app::FtnIngressState::held,result.reason).succeeded(),"ftn_toss_hold_checkpoint_failed");
            result.held=true;return result;
        }
        if(processed&&file->path().parent_path()!=processed->path())file->rename_to(*processed,name);
        require(storage.advance_ftn_ingress(owner,receipt.id,receipt.received,receipt.received,receipt.digest,app::FtnIngressState::processed,{}).succeeded(),"ftn_toss_completion_checkpoint_failed");result.processed=true;
    }catch(const std::exception& error){result.reason=reason(error);
        if(result.reason=="ftn_inbound_file_missing_or_changed" && running && running()){
            result.held=storage.advance_ftn_ingress(owner,receipt.id,receipt.received,receipt.received,receipt.digest,app::FtnIngressState::held,result.reason).succeeded();
            result.deferred=!result.held;return result;
        }
        if(!trusted_file||!running||!running()||result.reason=="ftn_toss_disabled"||result.reason=="ftn_toss_network_disabled"||result.reason=="ftn_toss_link_unavailable"||result.reason.find("checkpoint")!=std::string::npos||result.reason=="ftn_toss_progress_unavailable"){result.deferred=true;return result;}
        if(observation){observation->reason=result.reason;if(!storage.record_ftn_held_message(owner,*observation)){result.deferred=true;result.reason="ftn_held_observation_checkpoint_failed";return result;}}
        try{if(trusted_file&&file&&bad&&file->path().parent_path()!=bad->path())file->rename_to(*bad,bad->absent(receipt.file.name)?receipt.file.name:ftn_bad_original_name(receipt));result.held=storage.advance_ftn_ingress(owner,receipt.id,receipt.received,receipt.received,receipt.digest,app::FtnIngressState::held,result.reason).succeeded();}catch(...){result.reason="ftn_toss_hold_checkpoint_failed";}
    }
    return result;
}
FtnNativeTossResult reprocess_ftn_native_receipt(app::IFtnMailStorage& storage,const app::FtnConfiguration& configuration,const FtnInstallation& installation,const app::Identifier128& request,const std::function<bool()>& running){
    FtnNativeTossResult result;
    if(!running||!running()){result.deferred=true;result.reason="ftn_reprocess_cancelled";return result;}
    const auto checked=storage.check_ftn_reprocess(owner,request);
    if(!checked||!checked.value){
        result.deferred=true;result.reason="ftn_reprocess_review_stale";
        if(checked.status.code!=app::StorageErrorCode::busy&&checked.status.code!=app::StorageErrorCode::backpressure&&checked.status.code!=app::StorageErrorCode::io){
            if(storage.finish_ftn_reprocess(owner,request,false,result.reason).succeeded())result.deferred=false;
        }return result;
    }
    if(checked.value->state==app::FtnIngressState::processed){result.processed=storage.finish_ftn_reprocess(owner,request,true,{}).succeeded();result.completion_reconciled=result.processed;result.deferred=!result.processed;if(result.deferred)result.reason="ftn_reprocess_completion_retry";return result;}
    const auto latest=storage.latest_ftn_reprocess(owner,checked.value->id);
    if(!latest||!latest.value||!latest.value->has_value()||latest.value->value().request.operation.bytes!=request.bytes){result.deferred=true;result.reason="ftn_reprocess_request_unavailable";return result;}
    if(latest.value->value().request.configuration_revision!=configuration.revision){result.deferred=true;result.configuration_changed=true;result.reason="ftn_reprocess_configuration_reload";return result;}
    result=toss_ftn_native_receipt(storage,configuration,installation,*checked.value,running);
    if(!running()||result.configuration_changed)return result;
    // A completed original is authoritative after a lost completion reply.
    const auto actual=storage.ftn_ingress_receipt(owner,checked.value->id);
    if(!actual||!actual.value){result.deferred=true;result.reason="ftn_reprocess_result_retry";return result;}
    // Once native processing has started, transient storage/TIC fences leave
    // the same durable request pending while the sole worker resumes it.
    if(actual.value->state==app::FtnIngressState::processing){result.deferred=true;if(result.reason.empty())result.reason="ftn_reprocess_processing_pending";return result;}
    const bool completed=actual.value->state==app::FtnIngressState::processed;
    const auto why=completed?std::string{}:result.reason.empty()?std::string{"ftn_reprocess_still_held"}:result.reason;
    if(!storage.finish_ftn_reprocess(owner,request,completed,why)){result.deferred=true;result.reason="ftn_reprocess_completion_retry";return result;}
    result.processed=completed;return result;
}
}
