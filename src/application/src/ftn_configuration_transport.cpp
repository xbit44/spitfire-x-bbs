#include "sfx/application/ftn_configuration_transport.hpp"
#include <algorithm>
#include <utility>

namespace sfx::application {
namespace {
bool token_valid(const FtnTransferToken& t){return std::any_of(t.begin(),t.end(),[](auto b){return b!=0;});}
bool request_valid(const FtnConfigurationRequest& r){
    if(static_cast<unsigned>(r.operation)>6||!token_valid(r.token)||r.revision<0||r.revision==INT64_MAX||
       r.offset>kFtnConfigurationMaximumBytes||r.total>kFtnConfigurationMaximumBytes||r.bytes.size()>kFtnConfigurationPageBytes)return false;
    if(r.operation==FtnConfigurationOperation::append)return !r.bytes.empty()&&r.revision>=1&&r.total>=r.bytes.size()&&r.offset<=r.total-r.bytes.size();
    if(!r.bytes.empty())return false;
    if(r.operation==FtnConfigurationOperation::begin)return r.revision>=1&&r.offset==0&&r.total>0;
    if(r.operation==FtnConfigurationOperation::commit||r.operation==FtnConfigurationOperation::preview_retention)return r.revision>=1&&r.offset==0&&r.total>0;
    if(r.operation==FtnConfigurationOperation::cancel||r.operation==FtnConfigurationOperation::reconcile)return r.offset==0&&r.total==0;
    return r.total==0;
}
void put(std::vector<std::uint8_t>& b,std::uint64_t v,unsigned count){for(unsigned i=0;i<count;++i)b.push_back(static_cast<std::uint8_t>(v>>(i*8)));}
std::uint64_t get(const std::vector<std::uint8_t>& b,std::size_t& pos,unsigned count){std::uint64_t v{};for(unsigned i=0;i<count;++i)v|=static_cast<std::uint64_t>(b[pos++])<<(i*8);return v;}
FtnConfigurationResponse failure(FtnConfigurationCode c){FtnConfigurationResponse r;r.code=c;return r;}
}
std::vector<std::uint8_t> encode_ftn_configuration_request(const FtnConfigurationRequest& r){
    if(!request_valid(r))return {};
    std::vector<std::uint8_t> b{1,static_cast<std::uint8_t>(r.operation)};b.insert(b.end(),r.token.begin(),r.token.end());put(b,static_cast<std::uint64_t>(r.revision),8);put(b,r.offset,4);put(b,r.total,4);b.insert(b.end(),r.bytes.begin(),r.bytes.end());return b;
}
std::optional<FtnConfigurationRequest> decode_ftn_configuration_request(const std::vector<std::uint8_t>& b){
    if(b.size()<34||b.size()>34+kFtnConfigurationPageBytes||b[0]!=1)return {};
    FtnConfigurationRequest r;r.operation=static_cast<FtnConfigurationOperation>(b[1]);std::copy_n(b.begin()+2,16,r.token.begin());std::size_t pos=18;
    const auto revision=get(b,pos,8);if(revision>static_cast<std::uint64_t>(INT64_MAX))return {};r.revision=static_cast<std::int64_t>(revision);
    r.offset=static_cast<std::uint32_t>(get(b,pos,4));r.total=static_cast<std::uint32_t>(get(b,pos,4));r.bytes={b.begin()+pos,b.end()};if(!request_valid(r))return {};return r;
}
std::vector<std::uint8_t> encode_ftn_configuration_response(const FtnConfigurationResponse& r){
    if(static_cast<unsigned>(r.code)>4||r.revision<0||r.offset>r.total||r.total>kFtnConfigurationMaximumBytes||r.bytes.size()>kFtnConfigurationPageBytes||r.bytes.size()>r.total-r.offset)return {};
    std::vector<std::uint8_t> b{1,static_cast<std::uint8_t>(r.code)};put(b,static_cast<std::uint64_t>(r.revision),8);put(b,r.offset,4);put(b,r.total,4);b.insert(b.end(),r.bytes.begin(),r.bytes.end());return b;
}
std::optional<FtnConfigurationResponse> decode_ftn_configuration_response(const std::vector<std::uint8_t>& b){
    if(b.size()<18||b.size()>18+kFtnConfigurationPageBytes||b[0]!=1)return {};
    FtnConfigurationResponse r;r.code=static_cast<FtnConfigurationCode>(b[1]);std::size_t pos=2;const auto revision=get(b,pos,8);if(revision>static_cast<std::uint64_t>(INT64_MAX))return {};r.revision=static_cast<std::int64_t>(revision);r.offset=static_cast<std::uint32_t>(get(b,pos,4));r.total=static_cast<std::uint32_t>(get(b,pos,4));r.bytes={b.begin()+pos,b.end()};if(encode_ftn_configuration_response(r).empty())return {};return r;
}
FtnConfigurationPublisher::FtnConfigurationPublisher(Read read,Change change,Reconcile reconcile,Change preview):read_(std::move(read)),change_(std::move(change)),reconcile_(std::move(reconcile)),preview_(std::move(preview)){}
FtnConfigurationResponse FtnConfigurationPublisher::handle(const FtnConfigurationRequest& r,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);
    if(!request_valid(r))return failure(FtnConfigurationCode::invalid);
    for(auto* state:{&reader_,&writer_})if(*state&&(now<(*state)->touched||now-(*state)->touched>60000))state->reset();
    if(r.operation==FtnConfigurationOperation::reconcile)return reconcile_?reconcile_(r.token):failure(FtnConfigurationCode::unavailable);
    if(r.operation==FtnConfigurationOperation::cancel){
        if(reader_&&reader_->token==r.token)reader_.reset();
        if(writer_&&writer_->token==r.token)writer_.reset();
        return failure(FtnConfigurationCode::ok);
    }
    if(r.operation==FtnConfigurationOperation::read){
        if(!reader_){
            if(r.offset)return failure(FtnConfigurationCode::conflict);
            const auto c=read_();if(!c)return failure(FtnConfigurationCode::unavailable);
            auto bytes=encode_ftn_configuration(*c);if(bytes.empty())return failure(FtnConfigurationCode::invalid);
            if(r.revision&&r.revision!=c->revision)return failure(FtnConfigurationCode::conflict);
            reader_=Transfer{r.token,c->revision,static_cast<std::uint32_t>(bytes.size()),now,std::move(bytes)};
        }
        if(reader_->token!=r.token)return failure(FtnConfigurationCode::busy);
        if((r.revision&&r.revision!=reader_->revision)||r.offset>reader_->total)return failure(FtnConfigurationCode::conflict);
        reader_->touched=now;const auto count=std::min<std::size_t>(kFtnConfigurationPageBytes,reader_->total-r.offset);
        return {FtnConfigurationCode::ok,reader_->revision,r.offset,reader_->total,{reader_->bytes.begin()+r.offset,reader_->bytes.begin()+r.offset+count}};
    }
    if(r.operation==FtnConfigurationOperation::begin){
        if(writer_)return failure(FtnConfigurationCode::busy);
        const auto old=read_();if(!old)return failure(FtnConfigurationCode::unavailable);
        if(old->revision!=r.revision)return failure(FtnConfigurationCode::conflict);
        writer_=Transfer{r.token,r.revision,r.total,now,{}};writer_->bytes.reserve(r.total);return {FtnConfigurationCode::ok,r.revision,0,r.total,{}};
    }
    if(!writer_||writer_->token!=r.token)return failure(FtnConfigurationCode::conflict);
    if(writer_->revision!=r.revision||writer_->total!=r.total)return failure(FtnConfigurationCode::conflict);
    writer_->touched=now;
    if(r.operation==FtnConfigurationOperation::append){
        if(r.offset<writer_->bytes.size()){
            if(r.offset+r.bytes.size()>writer_->bytes.size()||!std::equal(r.bytes.begin(),r.bytes.end(),writer_->bytes.begin()+r.offset))return failure(FtnConfigurationCode::conflict);
        }else{
            if(r.offset!=writer_->bytes.size())return failure(FtnConfigurationCode::conflict);
            writer_->bytes.insert(writer_->bytes.end(),r.bytes.begin(),r.bytes.end());
        }
        return {FtnConfigurationCode::ok,r.revision,static_cast<std::uint32_t>(writer_->bytes.size()),r.total,{}};
    }
    if(writer_->bytes.size()!=writer_->total)return failure(FtnConfigurationCode::invalid);
    const auto c=decode_ftn_configuration(writer_->bytes);
    if(!c||c->revision!=writer_->revision){writer_.reset();return failure(FtnConfigurationCode::invalid);}
    if(r.operation==FtnConfigurationOperation::preview_retention)return preview_?preview_(writer_->token,*c):failure(FtnConfigurationCode::unavailable);
    const auto result=change_(writer_->token,*c);writer_.reset();return result;
}
FtnConfigurationResponse FtnReadOnlyPublisher::handle(const FtnConfigurationRequest& r,std::uint64_t now){
    std::lock_guard<std::mutex> lock(mutex_);if(!request_valid(r)||(r.operation!=FtnConfigurationOperation::read&&r.operation!=FtnConfigurationOperation::cancel))return failure(FtnConfigurationCode::invalid);
    if(snapshot_&&(now<touched_||now-touched_>60000))snapshot_.reset();
    if(r.operation==FtnConfigurationOperation::cancel){if(token_==r.token)snapshot_.reset();return failure(FtnConfigurationCode::ok);}
    if(!snapshot_){if(r.offset)return failure(FtnConfigurationCode::conflict);snapshot_=read_();if(!snapshot_)return failure(FtnConfigurationCode::unavailable);if(snapshot_->first<1||snapshot_->second.empty()||snapshot_->second.size()>kFtnConfigurationMaximumBytes){snapshot_.reset();return failure(FtnConfigurationCode::invalid);}token_=r.token;}
    if(token_!=r.token)return failure(FtnConfigurationCode::busy);
    if((r.revision&&r.revision!=snapshot_->first)||r.offset>snapshot_->second.size())return failure(FtnConfigurationCode::conflict);
    touched_=now;const auto& bytes=snapshot_->second;const auto end=std::min(bytes.size(),r.offset+kFtnConfigurationPageBytes);return {FtnConfigurationCode::ok,snapshot_->first,r.offset,static_cast<std::uint32_t>(bytes.size()),{bytes.begin()+r.offset,bytes.begin()+end}};
}
} // namespace sfx::application
