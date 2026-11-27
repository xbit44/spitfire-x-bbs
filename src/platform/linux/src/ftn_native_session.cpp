#include <sodium.h>
#include "sfx/platform/linux/ftn_native_session.hpp"
#include "sfx/platform/linux/ftn_crypto.hpp"
#include "sfx/platform/linux/ftn_secrets.hpp"
#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

namespace sfx::platform::linux_host {
namespace {
namespace app=sfx::application;
using Clock=std::chrono::steady_clock;
const app::MessageConfigurationActor owner{true,{}};
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
app::Identifier128 fresh_id(){app::Identifier128 id;randombytes_buf(id.bytes.data(),id.bytes.size());require(id.valid(),"ftn_session_random_failed");return id;}
struct Erase {std::string& value;~Erase(){if(!value.empty())sodium_memzero(value.data(),value.size());}};
std::string safe_reason(const std::exception& error){const std::string_view value(error.what());if(value.substr(0,4)!="ftn_"||value.size()>64)return "ftn_session_local_failure";for(char c:value)if((c<'a'||c>'z')&&(c<'0'||c>'9')&&c!='_')return "ftn_session_local_failure";return std::string(value);}
const app::FtnLink* configured_link(const app::FtnConfiguration& c,const app::FtnPeerMatch& match){for(const auto& link:c.links)if(link.network==match.network&&link.code==match.link&&link.enabled)return &link;return nullptr;}
bool claimed_configured(const app::FtnConfiguration& c,const std::vector<app::FtnAddress>& claims){
    for(const auto& link:c.links)if(link.enabled){auto aliases=link.additional_addresses;aliases.push_back(link.address);for(const auto& raw:aliases)if(const auto configured=app::parse_ftn_address(raw))for(const auto& claim:claims)if(app::same_ftn_address(*configured,claim,true))return true;}
    return false;
}
class Session final {
public:
    Session(FtnNativeChannel& channel,FtnNativeSessionPorts p):channel_(channel),ports_(std::move(p)),session_(fresh_id()){
        result_.outbound=ports_.dialled_link.has_value();require(ports_.configuration.settings.mode==app::FtnMailerMode::native&&ports_.configuration.revision>0&&ports_.generation.valid()&&ports_.running&&ports_.running(),"ftn_session_owner_policy_invalid");
        if(ports_.dialled_link){link_=configured_link(ports_.configuration,*ports_.dialled_link);require(link_&&app::ftn_outbound(*link_),"ftn_session_outbound_link_unavailable");require(channel_.matches_outbound_link(*link_),"ftn_session_outbound_endpoint_mismatch");load_secret();}
        else{const auto challenge=ftn_cram_challenge();require(challenge.has_value(),"ftn_session_challenge_failed");challenge_=*challenge;}
    }
    FtnNativeSessionResult run(){
        try{
            channel_.handshake(ports_.running);send(app::BinkpCommand::nul,"SYS Spitfire-X");send(app::BinkpCommand::nul,"VER Spitfire-X/1.0 binkp/1.1");send(app::BinkpCommand::nul,"OPT NR");
            if(!result_.outbound)send(app::BinkpCommand::nul,"OPT CRAM-MD5-"+challenge_);
            std::string addresses;std::set<std::string> unique;
            for(const auto& network:ports_.configuration.networks)if(network.enabled&&(!link_||network.code==link_->network)){auto aliases=network.additional_addresses;aliases.insert(aliases.begin(),network.address);for(const auto& address:aliases)if(unique.insert(address).second){require(unique.size()<=64,"ftn_session_local_aka_limit");if(!addresses.empty())addresses+=' ';addresses+=address;}}
            require(!addresses.empty(),"ftn_session_local_aka_missing");send(app::BinkpCommand::adr,addresses);
            const auto handshake_deadline=Clock::now()+std::chrono::seconds(15);auto last=Clock::now();
            while(!result_.completed){
                require(ports_.running(),"ftn_session_cancelled");require(Clock::now()-last<std::chrono::seconds(60),"ftn_session_idle_timeout");require(authenticated_||Clock::now()<handshake_deadline,"ftn_session_handshake_timeout");
                const auto before=result_.sent_bytes;if(authenticated_)pump();if(result_.sent_bytes!=before)last=Clock::now();if(result_.completed)break;
                auto read=channel_.read(sending_&&!awaiting_ack_&&!awaiting_get_?std::chrono::milliseconds(1):std::chrono::milliseconds(100),ports_.running);Erase erase_read{read.bytes};
                if(read.outcome==FtnReadOutcome::closed)throw std::runtime_error("ftn_session_peer_closed");
                if(read.outcome==FtnReadOutcome::idle)continue;
                last=Clock::now();
                require(decoder_.feed(read.bytes,[&](app::BinkpFrame frame){require(!result_.completed,"ftn_session_trailing_after_complete");handle(std::move(frame));return true;}),"ftn_session_frame_invalid");
            }
            notify();return result_;
        }catch(const std::exception& error){
            result_.completed=false;result_.reason=safe_reason(error);try{send(app::BinkpCommand::err,result_.reason);}catch(...){}
            for(const auto& job:jobs_)if(!settled_.count(ftn_native_artifact_name(job.id)))ports_.storage.finish_ftn_job(owner,job.id,session_,app::FtnOutboundState::queued,"interrupted_preserved");
            notify();return result_;
        }
    }
private:
    FtnNativeChannel& channel_;FtnNativeSessionPorts ports_;app::Identifier128 session_;FtnNativeSessionResult result_;
    app::BinkpDecoder decoder_;app::BinkpOptions options_;const app::FtnLink* link_{};std::optional<app::RloginSecret> secret_;
    std::string challenge_;bool saw_adr_{},sent_password_{},authenticated_{},local_eob_{},remote_eob_{};std::uint32_t commands_{},batch_commands_{},batches_{};
    std::optional<FtnNativeReceive> receiving_;app::BinkpFile incoming_;bool awaiting_file_{};std::uint64_t discarded_{};
    std::vector<app::FtnOutboundJob> jobs_;std::size_t next_job_{};std::optional<FilePayloadLease> sending_;app::FtnOutboundJob outgoing_;std::uint64_t send_offset_{};bool awaiting_ack_{},awaiting_get_{};
    std::set<std::string> offered_,settled_,acknowledged_;
    Clock::time_point progress_at_{};
    void notify(){
        progress_at_=Clock::now();
        if(sending_)result_.send={outgoing_.file.name,outgoing_.file.size,send_offset_,true,awaiting_ack_};else{result_.send.active=false;result_.send.awaiting_ack=false;}
        if(receiving_)result_.receive={incoming_.name,incoming_.size,receiving_->offset(),true,false};else result_.receive.active=false;
        if(ports_.observe)try{ports_.observe(result_);}catch(...){}
    }
    void send(app::BinkpCommand command,std::string_view argument={}){
        auto bytes=app::binkp_command(command,argument);Erase erase{bytes};require(!bytes.empty(),"ftn_session_command_bound");channel_.write(bytes,ports_.running);if(authenticated_&&command!=app::BinkpCommand::nul)++batch_commands_;
    }
    void load_secret(){if(!link_->credentials.session.empty()){secret_=unprotect_ftn_secret(ports_.installation,link_->network,link_->code,FtnCredentialKind::session,link_->credentials.session);require(secret_.has_value(),"ftn_session_secret_unavailable");}}
    void authenticate(app::FtnIngressProof proof){
        batch_commands_=0;result_.proof=proof;result_.peer_verified=app::ftn_verified_peer(proof,result_.outbound);if(link_){result_.network=link_->network;result_.link=link_->code;}
        if(ports_.directory){ports_.inbound=ports_.directory(result_.network,false);if(link_)ports_.outbound=ports_.directory(result_.network,true);}
        authenticated_=true;result_.handshake_complete=true;secret_.reset();notify();
    }
    void handshake(app::BinkpFrame& frame){
        const auto command=static_cast<app::BinkpCommand>(frame.id);require(++commands_<=1024,"ftn_session_handshake_command_limit");
        if(command==app::BinkpCommand::nul) {
            require(app::observe_binkp_information(options_,frame.bytes,true),"ftn_session_option_invalid");
            if(result_.advertised_system.empty()) {
                result_.advertised_system=app::binkp_advertised_system(frame.bytes);
                if(!result_.advertised_system.empty())notify();
            }
            return;
        }
        if(command==app::BinkpCommand::adr){
            require(!saw_adr_,"ftn_session_duplicate_adr");const auto addresses=app::parse_binkp_addresses(frame.bytes);require(addresses.has_value(),"ftn_session_adr_invalid");saw_adr_=true;
            result_.advertised_addresses=*addresses;
            notify();
            const auto match=app::resolve_ftn_peer(ports_.configuration,*addresses,ports_.dialled_link?ports_.dialled_link->network:std::string_view{},ports_.dialled_link?ports_.dialled_link->link:std::string_view{});
            require(match.has_value()||(!result_.outbound&&!claimed_configured(ports_.configuration,*addresses)&&ports_.configuration.settings.allow_direct_local_netmail),"ftn_session_unknown_or_ambiguous_aka");
            if(match){link_=configured_link(ports_.configuration,*match);require(link_&&(result_.outbound?app::ftn_outbound(*link_):app::ftn_inbound(*link_)),"ftn_session_link_direction_refused");
                // Address match identifies a candidate, not an authenticated peer.
                // Directory/queue access still waits for authenticate().
                result_.network=link_->network;result_.link=link_->code;notify();if(!result_.outbound)load_secret();}
            if(result_.outbound){
                if(secret_){if(!options_.challenge_hex.empty()){auto response=ftn_cram_response(secret_->view(),options_.challenge_hex);require(response.has_value(),"ftn_session_cram_failed");send(app::BinkpCommand::pwd,response->view());}
                    else {require(link_->allow_legacy_plain_auth,"ftn_session_cram_required");send(app::BinkpCommand::pwd,secret_->view());}}
                else send(app::BinkpCommand::pwd,"-");
                sent_password_=true;
            }return;
        }
        if(command==app::BinkpCommand::pwd){
            require(!result_.outbound&&saw_adr_,"ftn_session_unexpected_password");app::FtnIngressProof proof=link_?app::FtnIngressProof::inbound_tcp:app::FtnIngressProof::nonsecure;
            if(secret_){if(frame.bytes.substr(0,9)=="CRAM-MD5-"){
                switch(check_ftn_cram(secret_->view(),challenge_,frame.bytes)){
                    case FtnCramCheck::accepted:break;
                    case FtnCramCheck::length:throw std::runtime_error("ftn_session_cram_response_length");
                    case FtnCramCheck::trailing_nul:throw std::runtime_error("ftn_session_cram_trailing_nul");
                    case FtnCramCheck::encoding:throw std::runtime_error("ftn_session_cram_response_encoding");
                    case FtnCramCheck::digest_mismatch:throw std::runtime_error("ftn_session_cram_digest_mismatch");
                    case FtnCramCheck::unavailable:throw std::runtime_error("ftn_session_cram_crypto_unavailable");
                }proof=app::FtnIngressProof::inbound_cram;}
                else{require(link_->allow_legacy_plain_auth&&app::equal_ftn_secret(secret_->view(),frame.bytes),"ftn_session_legacy_refused");proof=app::FtnIngressProof::inbound_legacy;}}
            // With no expected secret, M_PWD is anonymous admission only.
            // Peers may send a CRAM response even for an empty session policy;
            // retain nonsecure/inbound_tcp proof and its queue/import limits.
            send(app::BinkpCommand::ok,"accepted");authenticate(proof);return;
        }
        if(command==app::BinkpCommand::ok){require(result_.outbound&&saw_adr_&&sent_password_,"ftn_session_unexpected_ok");authenticate(channel_.verified_server_tls()?app::FtnIngressProof::outbound_verified_tls:app::FtnIngressProof::outbound_tcp);return;}
        require(command!=app::BinkpCommand::err&&command!=app::BinkpCommand::bsy,"ftn_peer_refused");throw std::runtime_error("ftn_session_handshake_order");
    }
    void handle(app::BinkpFrame frame){
        Erase erase{frame.bytes};
        if(!authenticated_){require(frame.command,"ftn_session_data_before_auth");handshake(frame);return;}
        if(!frame.command){
            // IREX emits an empty data frame after the final file block. It
            // carries no bytes or custody transition; keep it bounded as a no-op.
            if(frame.bytes.empty()){require(commands_++<65536,"ftn_session_command_limit");return;}
            if(awaiting_file_){discarded_+=frame.bytes.size();require(discarded_<=incoming_.size,"ftn_session_stale_data_limit");return;}
            require(receiving_.has_value(),"ftn_session_unsolicited_data");const auto before=result_.received_bytes;receiving_->append(frame.bytes);result_.received_bytes+=frame.bytes.size();if(receiving_->offset()==incoming_.size)received();else if(before/262144!=result_.received_bytes/262144||Clock::now()-progress_at_>=std::chrono::milliseconds(200))notify();return;
        }
        require(commands_++<65536,"ftn_session_command_limit");
        const auto command=static_cast<app::BinkpCommand>(frame.id);if(command!=app::BinkpCommand::nul)++batch_commands_;
        switch(command){
            case app::BinkpCommand::nul:return;
            case app::BinkpCommand::file:incoming(frame.bytes);return;
            case app::BinkpCommand::get:restart(frame.bytes);return;
            case app::BinkpCommand::got:case app::BinkpCommand::skip:acknowledge(frame.bytes,command==app::BinkpCommand::got);return;
            case app::BinkpCommand::eob:require(frame.bytes.empty()&&!remote_eob_&&!receiving_,"ftn_session_eob_during_file");remote_eob_=true;batch();return;
            case app::BinkpCommand::err:case app::BinkpCommand::bsy:throw std::runtime_error("ftn_peer_refused");
            default:throw std::runtime_error("ftn_session_command_unexpected");
        }
    }
    void incoming(std::string_view descriptor){
        require(!remote_eob_&&result_.received_files<4096,"ftn_session_inbound_batch_bound");const auto file=app::parse_binkp_file(descriptor,true,options_.version11);require(file.has_value(),"ftn_session_file_descriptor_invalid");
        if(receiving_){require(awaiting_file_&&!file->request_offset&&app::same_binkp_file(*file,incoming_)&&file->offset==receiving_->offset(),"ftn_session_file_restart_invalid");awaiting_file_=false;incoming_=*file;if(receiving_->offset()==file->size)received();return;}
        require(file->request_offset||file->offset==0,"ftn_session_unrequested_file_offset");app::FtnIngressReceipt request;request.id=fresh_id();request.session=session_;request.configuration_revision=ports_.configuration.revision;request.proof=result_.proof;request.outbound=result_.outbound;request.network=result_.network;request.link=result_.link;request.file=*file;request.file.offset=0;request.file.request_offset=false;
        crypto_hash_sha256(request.identity.data(),reinterpret_cast<const unsigned char*>("native-session-v1"),17);incoming_=*file;discarded_=0;awaiting_file_=false;
        try {receiving_.emplace(FtnNativeReceive::open(ports_.inbound,ports_.storage,request,ports_.running,ports_.completed_inbound?&*ports_.completed_inbound:nullptr));}
        catch(const std::exception& e){if(std::string_view(e.what())!="ftn_inbound_name_collision")throw;send(app::BinkpCommand::skip,app::format_binkp_file(*file,false));awaiting_file_=true;result_.reason="ftn_inbound_name_collision";notify();return;}
        result_.last_ingress=receiving_->receipt().id;
        if(file->request_offset||receiving_->offset()!=file->offset){auto get=*file;get.request_offset=false;get.offset=receiving_->offset();send(app::BinkpCommand::get,app::format_binkp_file(get,true));awaiting_file_=true;}
        else if(file->size==0)received();
        notify();
    }
    void received(){
        require(receiving_&&!awaiting_file_,"ftn_session_receive_order");receiving_->finish();result_.receive={incoming_.name,incoming_.size,incoming_.size,false,false};++result_.received_files;send(app::BinkpCommand::got,app::format_binkp_file(incoming_,false));receiving_.reset();notify();
    }
    void restart(std::string_view descriptor){
        const auto get=app::parse_binkp_file(descriptor,true,options_.version11);require(get&&!get->request_offset&&sending_&&app::same_binkp_file(*get,outgoing_.file),"ftn_session_get_not_current_file");require(!local_eob_,"ftn_session_get_after_eob");send_offset_=get->offset;auto resumed=outgoing_.file;resumed.offset=send_offset_;send(app::BinkpCommand::file,app::format_binkp_file(resumed,true));awaiting_get_=false;awaiting_ack_=send_offset_==resumed.size;
    }
    void acknowledge(std::string_view descriptor,bool got){
        const auto file=app::parse_binkp_file(descriptor,false,options_.version11);require(file.has_value(),"ftn_session_ack_descriptor_invalid");const auto key=app::format_binkp_file(*file,false);
        if(!sending_||!app::same_binkp_file(*file,outgoing_.file)){require(got&&acknowledged_.count(key),"ftn_session_ack_not_current_file");return;}
        // A peer may report an already-held duplicate in response to the NR
        // offer. That is a real remote GOT, unlike a local missing spool file.
        require(!got||awaiting_ack_||awaiting_get_,"ftn_session_early_got");require(ports_.storage.finish_ftn_job(owner,outgoing_.id,session_,got?app::FtnOutboundState::delivered:app::FtnOutboundState::queued,got?std::string_view{}:std::string_view{"peer_skipped"}).succeeded(),"ftn_session_ack_commit_failed");settled_.insert(ftn_native_artifact_name(outgoing_.id));if(got){acknowledged_.insert(key);++result_.sent_files;}sending_.reset();awaiting_ack_=awaiting_get_=false;notify();batch();
    }
    void offer(){
        if(!app::ftn_queue_offer_allowed(result_.proof,result_.outbound)||!link_){next_job_=jobs_.size();return;}
        if(jobs_.empty()){const auto claimed=ports_.storage.claim_ftn_jobs(owner,result_.network,result_.link,ports_.generation,session_,64,ports_.include_hold);require(bool(claimed)&&claimed.value.has_value(),"ftn_session_queue_claim_failed");jobs_=*claimed.value;}
        while(next_job_<jobs_.size()){
            outgoing_=jobs_[next_job_++];const auto key=app::format_binkp_file(outgoing_.file,false);bool valid=offered_.insert(key).second;
            try{if(valid){sending_.emplace(ports_.outbound.read(ftn_existing_spool_name(ports_.outbound,outgoing_)));const auto actual=sending_->identity([&](auto,auto){return ports_.running();});valid=actual.bytes==outgoing_.file.size&&sodium_memcmp(actual.sha256.data(),outgoing_.digest.data(),actual.sha256.size())==0;}}catch(...){valid=false;}
            if(!valid){sending_.reset();require(ports_.storage.finish_ftn_job(owner,outgoing_.id,session_,app::FtnOutboundState::held,"artifact_missing_or_changed").succeeded(),"ftn_session_queue_hold_failed");settled_.insert(ftn_native_artifact_name(outgoing_.id));continue;}
            auto descriptor=outgoing_.file;descriptor.offset=0;descriptor.request_offset=options_.version11&&options_.request_nr;awaiting_get_=descriptor.request_offset;awaiting_ack_=!awaiting_get_&&descriptor.size==0;send_offset_=0;send(app::BinkpCommand::file,app::format_binkp_file(descriptor,true));notify();return;
        }
    }
    void pump(){
        const auto observed=result_.sent_bytes;
        if(!sending_&&!local_eob_)offer();
        if(sending_&&!awaiting_get_&&!awaiting_ack_){const auto bytes=sending_->read(send_offset_,static_cast<std::size_t>(std::min<std::uint64_t>(app::kBinkpFrameMaximumBytes,outgoing_.file.size-send_offset_)));require(!bytes.empty(),"ftn_session_outbound_read_failed");channel_.write(app::binkp_data(std::string_view(reinterpret_cast<const char*>(bytes.data()),bytes.size())),ports_.running);send_offset_+=bytes.size();result_.sent_bytes+=bytes.size();if(send_offset_==outgoing_.file.size){awaiting_ack_=true;require(ports_.storage.finish_ftn_job(owner,outgoing_.id,session_,app::FtnOutboundState::awaiting_ack,{}).succeeded(),"ftn_session_awaiting_ack_commit_failed");}}
        if(observed/262144!=result_.sent_bytes/262144||(observed!=result_.sent_bytes&&Clock::now()-progress_at_>=std::chrono::milliseconds(200))||(sending_&&awaiting_ack_))notify();
        if(!sending_&&next_job_==jobs_.size()&&!local_eob_){send(app::BinkpCommand::eob);local_eob_=true;batch();}
    }
    void batch(){
        if(!local_eob_||!remote_eob_||receiving_||sending_)return;
        ++result_.completed_batches;
        if(!options_.version11||batch_commands_<=2){result_.completed=true;result_.reason.clear();return;}
        require(++batches_<=64,"ftn_session_batch_limit");batch_commands_=0;local_eob_=remote_eob_=false;jobs_.clear();next_job_=0;
    }
};
}
std::string ftn_native_spool_name(const application::FtnOutboundJob& job){
    if(!job.id.valid()||!application::valid_ftn_leaf_name(job.file.name))return {};
    const auto token=ftn_native_artifact_name(job.id).substr(0,32);
    if(job.file.name==token+".pkt" || job.file.name==token+".tic")return job.file.name;
    const auto dot=job.file.name.rfind('.');const auto ext=dot==std::string::npos?std::string{}:job.file.name.substr(dot);
    // Preserve the actual type while making simultaneous same-name payloads
    // unambiguous. The advertised wire filename and durable job ID are unchanged.
    if(ext.size()>16)return job.file.name.substr(0,95)+"-"+token;
    const auto stem=job.file.name.substr(0,dot);return stem.substr(0,128-33-ext.size())+"-"+token+ext;
}
std::string ftn_existing_spool_name(const FilePayloadDirectory& directory,const application::FtnOutboundJob& job){
    const auto name=ftn_native_spool_name(job);if(!directory.absent(name))return name;
    const auto old=ftn_native_artifact_name(job.id);return directory.absent(old)?name:old;
}
std::string ftn_native_artifact_name(const application::Identifier128& id){if(!id.valid())return {};std::string out;for(const auto byte:id.bytes){out+="0123456789abcdef"[byte>>4];out+="0123456789abcdef"[byte&15];}return out+".dat";}
FtnNativeSessionResult run_ftn_native_session(FtnNativeChannel& channel,FtnNativeSessionPorts ports){try{return Session(channel,std::move(ports)).run();}catch(const std::exception& error){FtnNativeSessionResult result;result.reason=safe_reason(error);return result;}}
}
