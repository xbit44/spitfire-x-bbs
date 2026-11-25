#include <windows.h>
#include <bcrypt.h>
#include <sodium.h>
#include "sfx/platform/win32/ftn_native_custody.hpp"
#include <algorithm>
#include <stdexcept>

namespace sfx::platform::win32 {
namespace {
namespace app=sfx::application;
const app::MessageConfigurationActor host{true,{}};
void require(bool value,const char* reason){if(!value)throw std::runtime_error(reason);}
app::Identifier128 fresh_id(){app::Identifier128 id;require(BCryptGenRandom(nullptr,id.bytes.data(),static_cast<ULONG>(id.bytes.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)>=0&&id.valid(),"ftn_custody_random_failed");return id;}
std::string filename(const app::Identifier128& id){std::string out;for(const auto c:id.bytes){out+="0123456789abcdef"[c>>4];out+="0123456789abcdef"[c&15];}return out+".dat";}
app::FtnDigest scope(const app::FtnIngressReceipt& r,bool renewed){
    // Fixed-size input identity + length-prefixed configuration fields. Even
    // an accidentally reused host identity cannot upgrade proof/direction or
    // resume an unauthenticated transfer from another native session.
    crypto_hash_sha256_state h{};crypto_hash_sha256_init(&h);
    const auto add=[&](std::string_view value){const auto n=static_cast<std::uint64_t>(value.size());unsigned char length[8]{};for(unsigned i=0;i<8;++i)length[i]=static_cast<unsigned char>(n>>(i*8));crypto_hash_sha256_update(&h,length,8);crypto_hash_sha256_update(&h,reinterpret_cast<const unsigned char*>(value.data()),value.size());};
    add("SFX native FTN custody v1");crypto_hash_sha256_update(&h,r.identity.data(),r.identity.size());
    if(r.external_session_authenticated)add("external_session_authenticated");
    add(r.network);add(r.link);add(std::to_string(static_cast<unsigned>(r.proof)));add(r.outbound?"out":"in");add(std::to_string(r.configuration_revision));
    add(r.file.name);add(std::to_string(r.file.size));add(std::to_string(r.file.timestamp));
    if(!app::ftn_verified_peer(r.proof,r.outbound))crypto_hash_sha256_update(&h,r.session.bytes.data(),r.session.bytes.size());
    if(renewed)crypto_hash_sha256_update(&h,r.id.bytes.data(),r.id.bytes.size());
    app::FtnDigest result{};crypto_hash_sha256_final(&h,result.data());return result;
}
}
struct FtnNativeReceive::Impl {
    app::IFtnMailStorage& storage;app::FtnIngressReceipt current;std::function<bool()> running;
    std::optional<FilePayloadLease> file;crypto_hash_sha256_state hash{};
    app::FilePayloadIdentity prefix;std::uint64_t written{};bool finished{};std::optional<FilePayloadDirectory> completed;
    Impl(const FilePayloadDirectory& directory,app::IFtnMailStorage& db,app::FtnIngressReceipt request,std::function<bool()> live,const FilePayloadDirectory* destination):storage(db),running(std::move(live)){
        require(running&&running(),"ftn_custody_cancelled");
        require(request.id.valid()&&request.session.valid()&&!request.received&&request.state==app::FtnIngressState::partial&&!request.file.offset&&!request.file.request_offset,"ftn_custody_context_invalid");
        auto candidate=request;candidate.identity=scope(request,false);auto reserved=storage.begin_ftn_ingress(host,candidate);require(bool(reserved)&&reserved.value.has_value(),"ftn_custody_reservation_failed");
        if(reserved.value->state!=app::FtnIngressState::partial){
            // A matching name/size/time from an earlier completed transfer is
            // not proof of this peer's new bytes. Receive a new candidate;
            // downstream immutable content/MSGID receipts handle duplicates.
            require(reserved.value->state!=app::FtnIngressState::held,"ftn_custody_held");candidate=request;candidate.id=fresh_id();candidate.identity=scope(candidate,true);reserved=storage.begin_ftn_ingress(host,candidate);require(bool(reserved)&&reserved.value&&reserved.value->state==app::FtnIngressState::partial,"ftn_custody_fresh_reservation_failed");
        }
        current=*reserved.value;const auto saved=storage.ftn_ingress_checkpoint(host,current.id);require(bool(saved)&&saved.value.has_value(),"ftn_custody_checkpoint_unavailable");
        const auto name=filename(current.id);const auto maximum=std::max<std::uint64_t>(1,current.file.size);
        if(destination&&!current.network.empty()){
            completed=*destination;
            if(!completed->absent(current.file.name)){
                // A fresh arrival has no object to recover. Report the name
                // conflict before requesting DELETE access to the original,
                // which may legitimately be held open by a reader/tosser.
                require(saved.value->has_value(),"ftn_inbound_name_collision");
                auto published=completed->read(current.file.name,true);const auto object=published.object_identity();
                require(object.volume==(**saved.value).volume&&object.file_id==(**saved.value).file_id&&directory.absent(name),"ftn_inbound_name_collision");
                published.rename_to(directory,name); // exact interrupted rename-before-SQL recovery
            }
        }
        if(saved.value->has_value()){
            prefix=**saved.value;require(prefix.bytes==current.received,"ftn_custody_checkpoint_mismatch");file.emplace(directory.resume_private(name,maximum,prefix,[&](auto,auto){return running();}));
        }else{
            require(!current.received,"ftn_custody_native_binding_missing");
            if(directory.absent(name))file.emplace(directory.create(name,maximum));
            else{
                // The only admissible create-before-bind crash is the exact
                // still-empty private object. Never adopt unknown peer bytes.
                {auto orphan=directory.read(name);prefix=orphan.identity();require(prefix.bytes==0,"ftn_custody_unbound_nonempty_object");}
                file.emplace(directory.resume_private(name,maximum,prefix,[&](auto,auto){return running();}));
            }
            const auto object=file->object_identity();prefix.volume=object.volume;prefix.file_id=object.file_id;prefix.bytes=0;crypto_hash_sha256(prefix.sha256.data(),nullptr,0);file->flush();
            require(storage.checkpoint_ftn_ingress(host,current.id,0,prefix).succeeded(),"ftn_custody_native_binding_failed");
        }
        crypto_hash_sha256_init(&hash);while(written<prefix.bytes){require(running(),"ftn_custody_cancelled");const auto bytes=file->read(written,static_cast<std::size_t>(std::min<std::uint64_t>(65536,prefix.bytes-written)));require(!bytes.empty(),"ftn_custody_resume_read_failed");crypto_hash_sha256_update(&hash,bytes.data(),bytes.size());written+=bytes.size();}
    }
    void checkpoint(){
        require(!finished&&file&&running(),"ftn_custody_not_writable");if(written==prefix.bytes)return;
        file->flush();auto next=prefix;next.bytes=written;auto copy=hash;crypto_hash_sha256_final(&copy,next.sha256.data());
        require(storage.checkpoint_ftn_ingress(host,current.id,prefix.bytes,next).succeeded(),"ftn_custody_checkpoint_failed");prefix=std::move(next);current.received=written;
    }
};
FtnNativeReceive::FtnNativeReceive(std::unique_ptr<Impl> impl):impl_(std::move(impl)){}
FtnNativeReceive FtnNativeReceive::open(const FilePayloadDirectory& directory,app::IFtnMailStorage& storage,app::FtnIngressReceipt request,const std::function<bool()>& running,const FilePayloadDirectory* completed){return FtnNativeReceive(std::make_unique<Impl>(directory,storage,std::move(request),running,completed));}
FtnNativeReceive::~FtnNativeReceive()=default;
FtnNativeReceive::FtnNativeReceive(FtnNativeReceive&&)noexcept=default;
FtnNativeReceive& FtnNativeReceive::operator=(FtnNativeReceive&&)noexcept=default;
std::uint64_t FtnNativeReceive::offset()const noexcept{return impl_->written;}
const app::FtnIngressReceipt& FtnNativeReceive::receipt()const noexcept{return impl_->current;}
void FtnNativeReceive::append(std::string_view bytes){
    require(!impl_->finished&&impl_->running()&&bytes.size()<=app::kBinkpFrameMaximumBytes&&impl_->written<=impl_->current.file.size&&bytes.size()<=impl_->current.file.size-impl_->written,"ftn_custody_frame_bound");
    impl_->file->append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());crypto_hash_sha256_update(&impl_->hash,reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size());impl_->written+=bytes.size();
    if(impl_->written-impl_->prefix.bytes>=262144)impl_->checkpoint();
}
void FtnNativeReceive::checkpoint(){impl_->checkpoint();}
app::FilePayloadIdentity FtnNativeReceive::finish(){
    require(!impl_->finished&&impl_->written==impl_->current.file.size,"ftn_custody_incomplete");impl_->checkpoint();
    const auto actual=impl_->file->identity([&](auto,auto){return impl_->running();});require(actual.bytes==impl_->written&&actual.volume==impl_->prefix.volume&&actual.file_id==impl_->prefix.file_id&&sodium_memcmp(actual.sha256.data(),impl_->prefix.sha256.data(),actual.sha256.size())==0,"ftn_custody_final_identity_mismatch");
    if(impl_->completed)impl_->file->rename_to(*impl_->completed,impl_->current.file.name);
    require(impl_->storage.advance_ftn_ingress(host,impl_->current.id,impl_->written,impl_->written,actual.sha256,app::FtnIngressState::received,{}).succeeded(),"ftn_custody_publication_failed");impl_->current.state=app::FtnIngressState::received;impl_->current.digest=actual.sha256;impl_->finished=true;return actual;
}
#include "ftn_inbound_layout.inc"
}
