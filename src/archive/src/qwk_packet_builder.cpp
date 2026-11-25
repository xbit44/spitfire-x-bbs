#include "sfx/archive/qwk_packet_builder.hpp"
#include "sfx/application/message_source.hpp"
#include "sfx/application/ftn.hpp"
#include <sodium.h>
#include <algorithm>
#include <deque>
#include <limits>
#include <stdexcept>

namespace sfx::archive {
namespace {
namespace app=sfx::application;
struct Refusal {Result result;app::StorageStatus storage;app::QwkCodecError codec_error{app::QwkCodecError::none};};
[[noreturn]] void refuse(Error e,std::string message){throw Refusal{{e,std::move(message)},{}};}
template<class T> T value(app::StorageResult<T> result){if(!result||!result.value)throw Refusal{{Error::source_changed,"Packet preparation could not verify current storage/authority."},result.status};return std::move(*result.value);}
void checkpoint(const CancelCheck& cancel){if(cancel&&cancel())refuse(Error::cancelled,"Packet preparation cancelled; nothing delivered.");}
std::string plain_export(std::string_view source,bool pipes,bool colors){
    constexpr unsigned vga_to_ansi[]{0,4,2,6,1,5,3,7};std::string result;std::size_t column{};unsigned attribute=7;bool colored=false;
    for(std::size_t n=0;n<source.size();++n){const auto c=static_cast<unsigned char>(source[n]);
        if(c=='\r'||c=='\n'){if(c=='\r'&&n+1<source.size()&&source[n+1]=='\n')++n;result+='\n';column=0;}
        else if(c=='\t'){const auto spaces=8-column%8;result.append(spaces,' ');column+=spaces;}
        else if(pipes&&c=='|'&&n+2<source.size()&&source[n+1]>='0'&&source[n+1]<='9'&&source[n+2]>='0'&&source[n+2]<='9'&&((source[n+1]-'0')*10+source[n+2]-'0')<=31){
            const auto code=static_cast<unsigned>((source[n+1]-'0')*10+source[n+2]-'0');n+=2;attribute=code<16?(attribute&240)|code:(attribute&15)|((code-16)<<4);
            if(colors){colored=true;result+="\x1b[0";if(attribute&8)result+=";1";if(attribute&128)result+=";5";result+=';'+std::to_string(30+vga_to_ansi[attribute&7]);result+=';'+std::to_string(40+vga_to_ansi[(attribute>>4)&7]);result+='m';}
        }else{if(c<32||c==127)refuse(Error::invalid_request,"Unsupported plain message control.");result+=static_cast<char>(c);++column;}
        if(result.size()>app::kMessageBodyBytes)refuse(Error::limit_exceeded,"A complete plain rendition cannot fit the supported body limit.");
    }
    if(colored)result+="\x1b[0m";
    return result;
}
std::string rendition(const app::MessageCanvas& c,bool color){
    std::string result;
    for(std::size_t first=0;first<c.extent;first+=100){
        auto page=app::message_canvas_page(c,first,std::min<std::size_t>(100,c.extent-first),color);
        if(!page)refuse(Error::invalid_request,"Supported message presentation cannot be exported safely.");
        for(const auto& op:page->operations){
            if(op.kind==app::TerminalDisplayOperationKind::cp437_text||op.kind==app::TerminalDisplayOperationKind::ansi_sequence)result+=op.bytes;
            else if(op.kind==app::TerminalDisplayOperationKind::logical_newline)result+='\n';
            else refuse(Error::invalid_request,"Unsupported message presentation operation.");
            if(result.size()>app::kMessageBodyBytes)refuse(Error::limit_exceeded,"A complete message rendition cannot fit the supported body limit.");
        }
    }
    return result;
}
app::QwkRecord record(app::IQwkStorage& storage,const app::MessageActor& a,const app::QwkPacketPlan& plan,
    const app::OpenedMessage& opened,const QwkDateFormatter& date,std::uint32_t reference){
    const auto& h=opened.header;app::QwkRecord m;
    if(h.target.kind==app::MessageKind::private_local){
        if(h.target.recipient!=a.account||h.inbox_removed)refuse(Error::source_changed,"Received-mail ownership changed.");
        m.private_mail=true;m.read=h.recipient_opened;
        if(h.remote_author){if(!plan.netmail_conference)refuse(Error::source_changed,"FTN NetMail capability unavailable.");m.conference=*plan.netmail_conference;}
    }else{
        const auto echo=std::find_if(plan.catalog.begin(),plan.catalog.end(),[&](const auto& e){return e.echo==h.target.echo;});
        if(echo==plan.catalog.end())refuse(Error::source_changed,"Packet target is no longer accessible.");
        m.conference=echo->conference;
    }
    m.number=reference;m.message_id=app::qwk_message_identifier(plan.policy.board_id,m.number);
    if(h.parent){
        auto parent=storage.qwk_reference(a,*h.parent);
        if(parent){m.reference=*parent.value;m.in_reply_to=app::qwk_message_identifier(plan.policy.board_id,m.reference);}
        else if(parent.status.code!=app::StorageErrorCode::not_found)throw Refusal{{Error::source_changed,"Reply metadata authority changed."},parent.status};
    }
    m.from=h.from;m.to=h.to;m.subject=h.subject;const auto stamp=date(h.identity.utc_seconds);m.date=stamp.date;m.time=stamp.time;m.when_written=stamp.when_written;
    const auto& prefs=plan.settings.preferences;
    if(opened.received_canvas){m.artwork=true;m.body=rendition(*opened.received_canvas,prefs.preserve_colors);}
    else if(opened.original_source){
        const auto rendered=app::message_source_rendition(*opened.original_source,opened.pipe_colors,opened.ansi_colors);
        if(!rendered)refuse(Error::source_changed,"Stored message presentation is invalid.");
        const auto& canvas=*rendered;m.artwork=opened.original_source->format==app::MessageSourceFormat::ansi_canvas;
        // Generated supported SGR/CP437 only. No untrusted cursor controls,
        // display includes/tokens or host commands are exported/evaluated.
        m.body=m.artwork?rendition(canvas,prefs.preserve_colors&&(opened.ansi_colors||opened.pipe_colors)):plain_export(opened.original_source->bytes,opened.pipe_colors,prefs.preserve_colors);
    }else if(opened.pipe_colors){
        const auto& original=opened.plain_rendition_source?*opened.plain_rendition_source:opened.body;
        const auto canvas=app::message_plain_canvas(original,true);if(!canvas)refuse(Error::source_changed,"Stored plain color rendition is invalid.");m.body=rendition(*canvas,prefs.preserve_colors);
    }else m.body=opened.body.canonical_lf;
    if(h.remote_author&&h.target.kind==app::MessageKind::private_local){
        // QWKE subject-line addressing: correspondent address in Subject,
        // actual subject in a body kludge. HEADERS keeps the address too.
        m.body=(m.from.size()>25?"From: "+m.from+"\n":std::string{})+(m.to.size()>25?"To: "+m.to+"\n":std::string{})+"Subject: "+m.subject+"\n\n"+m.body;
        auto address=app::parse_ftn_address(h.remote_author->address);if(!address)refuse(Error::source_changed,"FTN correspondent address invalid.");address->domain.clear();m.subject="@"+app::format_ftn_address(*address);
    }
    return m;
}
std::size_t hex_digits(std::size_t n){std::size_t digits=1;while(n>=16){++digits;n/=16;}return digits;}
struct SingleSize {std::uint64_t charge{},message_bytes{},base{};};
SingleSize single_size(app::QwkCodecResult& packet,std::size_t actual_offset){
    SingleSize r;for(const auto& file:packet.files){
        if(file.name=="CONTROL.DAT"||file.name=="DOOR.ID"||file.name=="TOREADER.EXT")r.base+=file.bytes.size();
        else if(file.name=="MESSAGES.DAT"){if(file.bytes.size()<128)refuse(Error::archive_invalid,"Invalid complete message encoding.");r.message_bytes=file.bytes.size()-128;r.charge+=r.message_bytes;}
        else{r.charge+=file.bytes.size();if(file.name=="HEADERS.DAT")r.charge+=hex_digits(actual_offset)-hex_digits(128);}
    }
    r.base+=128;return r;
}
} // namespace
QwkPreparedDownload prepare_qwk_download(app::IQwkStorage& storage,const app::MessageActor& actor,
    app::Identifier128 operation,app::QwkPacketMetadata metadata,QwkDateFormatter date,CancelCheck cancelled,QwkPreparationObserver progress){
    try{
        if(!date)refuse(Error::invalid_request,"Packet timestamp formatter is unavailable.");
        checkpoint(cancelled);
        const auto initial=value(storage.begin_qwk_packet(actor,operation));auto cursor=initial;
        std::vector<bool> announced(initial.queues.size());
        const auto scanned=[&](std::size_t queue){
            if(!progress||announced[queue])return;
            announced[queue]=true;
            const auto echo=initial.queues[queue].echo;
            QwkPreparationEvent event{QwkPreparationStage::scanning,echo,{},"Private mail"};
            if(echo.value){
                const auto area=std::find_if(initial.catalog.begin(),initial.catalog.end(),[&](const auto& item){return item.echo==echo;});
                if(area==initial.catalog.end())refuse(Error::source_changed,"Packet area unavailable.");
                event.group_name=area->group_name;event.area_name=area->echo_name;
            }else if(initial.settings.preferences.private_mail==app::QwkPrivateMode::none)return;
            checkpoint(cancelled);
            if(!progress(event))refuse(Error::cancelled,"Packet preparation cancelled.");
        };
        metadata.board_id=initial.policy.board_id;metadata.conferences={{0,"Private local mail"}};metadata.netmail_conference=initial.netmail_conference;
        if(initial.netmail_conference)metadata.conferences.push_back({*initial.netmail_conference,"FTN NetMail"});
        for(const auto& e:initial.catalog)metadata.conferences.push_back({e.conference,e.group_name+" / "+e.echo_name});
        std::vector<app::QwkRecord> records;std::vector<app::QwkDeliveredItem> delivered;
        struct Pending {app::MessageId id;std::size_t queue{};std::uint32_t reference{};};
        std::deque<Pending> pending;
        std::uint64_t charges{},base{};std::size_t offset=128,at=initial.next_queue,rebases{};bool more=false;
        while(records.size()<initial.policy.maximum_messages){
            checkpoint(cancelled);
            if(pending.empty()){
                auto probe=cursor;auto queue=at;std::size_t empty{};std::vector<app::MessageId> ids;
                while(ids.size()<app::kQwkReferenceBatchMaximum&&records.size()+ids.size()<initial.policy.maximum_messages){
                    const auto maximum=std::min(app::kQwkSelectionQueuesMaximum,probe.queues.size()-empty);
                    checkpoint(cancelled);auto selection=value(storage.select_qwk_candidate(actor,probe,queue,maximum));checkpoint(cancelled);
                    if(selection.scanned==0||selection.scanned>maximum||selection.queue!=(queue+selection.scanned-1)%probe.queues.size())refuse(Error::source_changed,"Packet selection cursor changed.");
                    for(std::size_t n=0;n<selection.scanned;++n)scanned((queue+n)%probe.queues.size());
                    queue=selection.queue;const auto& selected=selection.selected;
                    if(!selected){empty+=selection.scanned;if(empty==probe.queues.size())break;queue=(queue+1)%probe.queues.size();continue;}
                    empty=0;ids.push_back(selected->message);pending.push_back({selected->message,queue,0});
                    probe.queues[queue].after_arrival=selected->arrival;queue=(queue+1)%probe.queues.size();
                    // Production peeks retain only ID/arrival metadata. Legacy
                    // ports may load a body in the compatibility fallback; it
                    // is released here, never retained across the batch.
                }
                if(ids.empty())break;
                checkpoint(cancelled);const auto refs=value(storage.qwk_references(actor,ids));checkpoint(cancelled);
                if(refs.size()!=pending.size())refuse(Error::source_changed,"Packet references changed during preparation.");
                for(std::size_t n=0;n<refs.size();++n){if(!refs[n])refuse(Error::source_changed,"Packet reference unavailable.");pending[n].reference=refs[n];}
            }
            // Freshly repeat the ordinary round-robin selection and full read
            // authorization before encoding each record. Authority failures
            // refuse; harmless forward read progress rebuilds the pending IDs.
            std::optional<app::OpenedMessage> opened;std::size_t empty{};
            for(;;){
                const auto maximum=std::min(app::kQwkSelectionQueuesMaximum,cursor.queues.size()-empty);
                checkpoint(cancelled);auto selection=value(storage.select_qwk_message(actor,cursor,at,maximum));checkpoint(cancelled);
                if(selection.scanned==0||selection.scanned>maximum||selection.queue!=(at+selection.scanned-1)%cursor.queues.size())refuse(Error::source_changed,"Packet selection cursor changed.");
                at=selection.queue;opened=std::move(selection.selected);empty+=selection.scanned;
                if(opened||empty==cursor.queues.size())break;
                at=(at+1)%cursor.queues.size();
            }
            if(!opened||pending.front().queue!=at||pending.front().id.value!=opened->header.identity.id.value){
                // An online read or retention may legitimately move selection
                // forward. Rebuild only the content-free pending batch from
                // delivered cursors; authorization failures above still refuse.
                pending.clear();if(++rebases>32)refuse(Error::source_changed,"Packet selection kept changing during preparation.");continue;
            }
            auto m=record(storage,actor,initial,*opened,date,pending.front().reference);
            auto encoded=app::encode_qwk(metadata,initial.settings.preferences,{m},initial.policy.maximum_messages);
            if(!encoded)throw Refusal{{encoded.error==app::QwkCodecError::limit_exceeded?Error::limit_exceeded:Error::invalid_request,encoded.detail},{},encoded.error};
            const auto size=single_size(encoded,offset);base=size.base;
            // Exact complete-record accounting. CONTROL count digits and
            // HEADERS hexadecimal offsets are charged at their final widths.
            const auto count_extra=std::to_string(records.size()+1).size()-1;
            if(size.charge>app::kQwkExpandedBytes||base+charges+size.charge+count_extra>app::kQwkExpandedBytes){
                if(records.empty())refuse(Error::limit_exceeded,"A complete message plus metadata cannot fit this packet.");
                more=true;break;
            }
            charges+=size.charge;offset+=static_cast<std::size_t>(size.message_bytes);
            delivered.push_back({opened->header.identity.id,opened->header.target.echo,opened->header.identity.arrival_ordinal,opened->header.identity.area_number});
            records.push_back(std::move(m));cursor.queues[at].after_arrival=opened->header.identity.arrival_ordinal;
            at=(at+1)%cursor.queues.size();cursor.next_queue=static_cast<std::uint16_t>(at);
            pending.pop_front();
        }
        if(records.empty()){QwkPreparedDownload empty;empty.notice=std::string(app::kQwkEmptyNotice);return empty;}
        if(!more){for(std::size_t n=0;n<cursor.queues.size();++n){checkpoint(cancelled);if(value(storage.next_qwk_message(actor,cursor,n))){more=true;break;}}}
        checkpoint(cancelled);
        if(progress&&!progress({QwkPreparationStage::packing,{},{},{}}))refuse(Error::cancelled,"Packet preparation cancelled.");
        for(;;){
            checkpoint(cancelled);auto packet=app::encode_qwk(metadata,initial.settings.preferences,records,initial.policy.maximum_messages);
            if(!packet)throw Refusal{{Error::invalid_request,packet.detail},{},packet.error};
            std::uint64_t expanded{};std::vector<QwkArchiveFile> files;files.reserve(packet.files.size());
            for(auto& file:packet.files){expanded+=file.bytes.size();files.push_back({std::move(file.name),std::move(file.bytes)});}
            auto archive=write_qwk_zip(files,cancelled);
            if(archive.result.ok()){
                checkpoint(cancelled);app::QwkDeliveryCommit commit;commit.initial=initial;commit.delivered=std::move(delivered);commit.final_queues=initial.queues;
                std::size_t last{};for(const auto& item:commit.delivered){const auto q=std::find_if(commit.final_queues.begin(),commit.final_queues.end(),[&](const auto& x){return x.echo==item.echo;});if(q==commit.final_queues.end())refuse(Error::invalid_request,"Packet queue identity mismatch.");q->after_arrival=item.arrival;last=static_cast<std::size_t>(q-commit.final_queues.begin());}
                commit.next_queue=static_cast<std::uint16_t>((last+1)%commit.final_queues.size());commit.more=more;commit.compressed_bytes=archive.zip.size();commit.expanded_bytes=expanded;commit.cancelled=cancelled;
                if(sodium_init()<0||crypto_hash_sha256(commit.zip_sha256.data(),reinterpret_cast<const unsigned char*>(archive.zip.data()),archive.zip.size())!=0)refuse(Error::archive_io,"Packet digest unavailable.");
                QwkPreparedDownload result;result.zip=std::move(archive.zip);result.progress=std::move(commit);if(more)result.notice=std::string(app::kQwkMoreNotice);return result;
            }
            if(archive.result.error!=Error::limit_exceeded)throw Refusal{archive.result,{}};
            if(records.size()==1)refuse(Error::limit_exceeded,"A complete message plus metadata cannot fit the compressed packet limit.");
            // Keep a complete fair prefix. Never truncate or skip the next
            // message; actual ZIP output, not a compression estimate, decides.
            const auto keep=std::max<std::size_t>(1,records.size()/2);records.resize(keep);delivered.resize(keep);more=true;
        }
    }catch(const Refusal& e){QwkPreparedDownload result;result.result=e.result;result.storage=e.storage;result.codec_error=e.codec_error;return result;}
    catch(...){QwkPreparedDownload result;result.result={Error::archive_io,"Packet preparation failed; nothing delivered."};return result;}
}
} // namespace sfx::archive
