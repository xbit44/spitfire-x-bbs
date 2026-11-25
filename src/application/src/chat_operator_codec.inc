namespace {
struct ChatWireWriter {
    std::vector<std::uint8_t> b;
    void n(std::uint64_t value,unsigned bytes){for(unsigned i=0;i<bytes;++i)b.push_back(static_cast<std::uint8_t>(value>>(i*8)));}
    void id(Identifier128 value){b.insert(b.end(),value.bytes.begin(),value.bytes.end());}
    void text(std::string_view text){n(text.size(),2);b.insert(b.end(),text.begin(),text.end());}
};
struct ChatWireReader {
    const std::vector<std::uint8_t>& b;std::size_t p{};bool ok{true};
    std::uint64_t n(unsigned bytes){if(bytes>b.size()-p){ok=false;return 0;}std::uint64_t out{};for(unsigned i=0;i<bytes;++i)out|=static_cast<std::uint64_t>(b[p++])<<(i*8);return out;}
    Identifier128 id(){Identifier128 out;for(auto& c:out.bytes)c=static_cast<std::uint8_t>(n(1));return out;}
    bool flag(){auto value=n(1);if(value>1)ok=false;return value==1;}
    std::string text(std::size_t maximum){const auto len=n(2);if(len>maximum||len>b.size()-p){ok=false;return {};}
        std::string out(b.begin()+p,b.begin()+p+static_cast<std::size_t>(len));p+=static_cast<std::size_t>(len);if(chat_safe_text(out,maximum)!=out)ok=false;return out;}
};
}
std::vector<std::uint8_t> encode_chat_operator_request(const ChatOperatorRequest& r){
    const bool starting=r.action==ChatOperatorAction::start||r.action==ChatOperatorAction::answer;
    const bool view_action=r.action>=ChatOperatorAction::start;
    if(r.action>ChatOperatorAction::end||starting!=r.target.has_value()||(r.target&&!r.target->valid())||
        view_action!=r.view.valid()||(!view_action&&(r.token||r.page||r.sequence))||
        (starting&&r.token)||(!starting&&view_action&&!r.token)||
        ((r.action==ChatOperatorAction::answer)!=(r.page!=0))||
        (r.action!=ChatOperatorAction::availability&&r.available)||(!starting&&r.override_dnd)||
        (r.action!=ChatOperatorAction::poll&&r.action!=ChatOperatorAction::send&&r.sequence)||
        (r.action==ChatOperatorAction::send?(!r.sequence||r.text.empty()||r.text.size()>512||chat_safe_text(r.text)!=r.text):!r.text.empty()))return {};
    ChatWireWriter w;w.n(1,1);w.n(static_cast<unsigned>(r.action),1);w.n(r.available,1);w.n(r.override_dnd,1);
    if(r.target){w.id(r.target->runtime_epoch);w.n(r.target->node,2);w.n(r.target->generation,8);}
    w.id(r.view);w.n(r.token,8);w.n(r.page,8);w.n(r.sequence,8);w.text(r.text);return w.b;
}
std::optional<ChatOperatorRequest> decode_chat_operator_request(const std::vector<std::uint8_t>& b){
    if(b.size()>600)return {};
    ChatWireReader r{b};if(r.n(1)!=1)return {};ChatOperatorRequest out;
    out.action=static_cast<ChatOperatorAction>(r.n(1));out.available=r.flag();out.override_dnd=r.flag();
    if(out.action==ChatOperatorAction::start||out.action==ChatOperatorAction::answer){NodeAssignmentIdentity id;id.runtime_epoch=r.id();id.node=static_cast<std::uint16_t>(r.n(2));id.generation=r.n(8);out.target=id;}
    out.view=r.id();out.token=r.n(8);out.page=r.n(8);out.sequence=r.n(8);out.text=r.text(512);
    if(!r.ok||r.p!=b.size()||encode_chat_operator_request(out)!=b)return {};
    return out;
}
std::vector<std::uint8_t> encode_chat_operator_snapshot(const ChatOperatorSnapshot& s){
    if(s.code>ChatResult::invalid||s.pages.size()>128||s.lines.size()>65||s.name.size()>64||chat_safe_text(s.name,64)!=s.name)return {};
    ChatWireWriter w;w.n(1,1);w.n(static_cast<unsigned>(s.code),1);w.n(s.available,1);w.n(s.pending,1);w.n(s.active,1);w.n(s.paused,1);w.n(s.token,8);w.text(s.name);w.n(s.pages.size(),2);
    for(const auto& page:s.pages){if(!page.request||!page.caller.session.valid()||page.caller.account<1||!page.caller.node||page.caller.node>128||page.name.size()>64||chat_safe_text(page.name,64)!=page.name)return {};
        w.n(page.request,8);w.id(page.caller.session);w.n(static_cast<std::uint64_t>(page.caller.account),8);w.n(page.caller.node,2);w.text(page.name);}
    w.n(s.lines.size(),2);std::uint64_t last{};
    for(const auto& line:s.lines){if(line.sequence<=last||line.text.size()>1024||chat_safe_text(line.text,1024)!=line.text)return {};last=line.sequence;w.n(line.sequence,8);w.text(line.text);}
    if(w.b.size()>65536)return {};
    return w.b;
}
std::optional<ChatOperatorSnapshot> decode_chat_operator_snapshot(const std::vector<std::uint8_t>& b){
    if(b.size()>65536)return {};
    ChatWireReader r{b};if(r.n(1)!=1)return {};ChatOperatorSnapshot out;
    out.code=static_cast<ChatResult>(r.n(1));out.available=r.flag();out.pending=r.flag();out.active=r.flag();out.paused=r.flag();out.token=r.n(8);out.name=r.text(64);
    auto count=r.n(2);if(count>128)return {};for(std::uint64_t i=0;i<count&&r.ok;++i){ChatPage page;page.request=r.n(8);page.caller.session=r.id();page.caller.account=static_cast<AccountId>(r.n(8));page.caller.node=static_cast<std::uint16_t>(r.n(2));page.name=r.text(64);out.pages.push_back(std::move(page));}
    count=r.n(2);if(count>65)return {};for(std::uint64_t i=0;i<count&&r.ok;++i){ChatOperatorLine line;line.sequence=r.n(8);line.text=r.text(1024);out.lines.push_back(std::move(line));}
    if(!r.ok||r.p!=b.size()||encode_chat_operator_snapshot(out)!=b)return {};
    return out;
}
