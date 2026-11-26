#include "sfx/application/chat.hpp"
#include <algorithm>
#include <limits>
#include <set>

namespace sfx::application {
std::vector<std::uint8_t> encode_chat_result(const ChatConfigurationResult& r){
    std::vector<std::uint8_t> out{static_cast<std::uint8_t>(r.code)};
    if(r.configuration){auto data=encode_chat_configuration(*r.configuration);if(data.empty())return {};out.insert(out.end(),data.begin(),data.end());}return out;
}
std::optional<ChatConfigurationResult> decode_chat_result(const std::vector<std::uint8_t>& bytes){
    if(bytes.empty()||bytes[0]>static_cast<unsigned>(ChatResult::invalid))return {};
    ChatConfigurationResult result{static_cast<ChatResult>(bytes[0])};
    if(bytes.size()>1){result.configuration=decode_chat_configuration({bytes.begin()+1,bytes.end()});if(!result.configuration)return {};}
    if(result.code==ChatResult::ok&&!result.configuration)return {};
    return result;
}
bool ChatIdentity::operator==(const ChatIdentity& o)const noexcept{return session==o.session&&account==o.account&&node==o.node;}
std::string chat_safe_text(std::string_view text,std::size_t maximum){
    std::string out;for(unsigned char c:text){if(out.size()==maximum)break;if(c>=32&&c!=127&&c!=0x9b)out+=static_cast<char>(c);}return out;
}
std::string chat_node(std::uint16_t node,std::uint16_t count){auto text=std::to_string(node);const auto width=std::max<std::size_t>(2,std::to_string(count).size());if(text.size()<width)text.insert(0,width-text.size(),'0');return text;}
std::string_view chat_result_text(ChatResult r)noexcept{
    switch(r){case ChatResult::ok:return "Sent.";case ChatResult::dnd:return "That caller has Do Not Disturb on.";
    case ChatResult::busy:return "Caller is busy; retry later.";case ChatResult::full:return "The caller's queue is full. Try again later.";
    case ChatResult::conflict:return "Settings changed. Reload and try again.";case ChatResult::invalid:return "Check the name, selection and security level.";
    case ChatResult::denied:return "Access denied.";default:return "That caller or room is no longer available.";}
}
bool valid_chat_configuration(const ChatConfiguration& c)noexcept{
    if(c.revision<1||c.revision==std::numeric_limits<std::int64_t>::max()||c.rooms.empty()||c.rooms.size()>128||c.commands.size()>130)return false;
    std::set<ChatRoomId> ids;bool entry=false;
    for(const auto& r:c.rooms){if(!r.id||!ids.insert(r.id).second||!valid_door_text(r.name,64)||!valid_door_requirements(r.access))return false;if(r.id==c.entry){entry=true;if(!r.enabled)return false;}}
    std::set<std::pair<MenuId,char>> keys;
    for(const auto& p:c.commands){if(p.menu!=MenuId::main&&p.menu!=MenuId::messages&&p.menu!=MenuId::files&&p.menu!=MenuId::chat&&p.menu!=MenuId::doors)return false;
        if(p.key<'A'||p.key>'Z'||!p.door||p.door>999||!valid_door_text(p.name,64)||!valid_door_requirements(p.access)||!keys.emplace(p.menu,p.key).second)return false;
        if(!available_door_command_key(p.menu,p.key))return false;
    }return entry;
}
bool chat_room_access(const ChatRoom& r,const ChatParticipant& p)noexcept{return r.enabled&&meets_door_requirements(r.access,{p.level,p.sysop_threshold})&&(!r.access.sysop_only||p.sysop);}
namespace {
struct Put {std::vector<std::uint8_t> b;void n(std::uint64_t x,unsigned bytes){for(unsigned i=0;i<bytes;++i)b.push_back(static_cast<std::uint8_t>(x>>(i*8)));}void text(std::string_view s){n(s.size(),2);b.insert(b.end(),s.begin(),s.end());}void req(const DoorRequirements& a){n(static_cast<unsigned>(a.mode),1);n(a.level,2);n(a.sysop_only,1);}};
struct Get {const std::vector<std::uint8_t>& b;std::size_t p{};bool ok{true};std::uint64_t n(unsigned bytes){if(bytes>b.size()-p){ok=false;return 0;}std::uint64_t x{};for(unsigned i=0;i<bytes;++i)x|=static_cast<std::uint64_t>(b[p++])<<(i*8);return x;}bool flag(){auto v=n(1);if(v>1)ok=false;return v==1;}std::string text(){auto len=n(2);if(len>64||len>b.size()-p){ok=false;return {};}std::string s(b.begin()+p,b.begin()+p+static_cast<std::size_t>(len));p+=static_cast<std::size_t>(len);return s;}DoorRequirements req(){DoorRequirements a;a.mode=static_cast<DoorSecurityMode>(n(1));a.level=static_cast<std::uint16_t>(n(2));a.sysop_only=flag();return a;}};
}
std::vector<std::uint8_t> encode_chat_configuration(const ChatConfiguration& c){if(!valid_chat_configuration(c))return {};Put w;w.n(1,1);w.n(static_cast<std::uint64_t>(c.revision),8);w.n(c.entry,2);w.n(c.exclude_sysop_statistics,1);w.n(c.rooms.size(),2);for(const auto& r:c.rooms){w.n(r.id,2);w.text(r.name);w.n(r.enabled,1);w.req(r.access);}w.n(c.commands.size(),2);for(const auto& p:c.commands){w.n(static_cast<unsigned>(p.menu),1);w.n(static_cast<unsigned char>(p.key),1);w.n(p.door,2);w.text(p.name);w.n(p.enabled,1);w.req(p.access);}return w.b;}
std::optional<ChatConfiguration> decode_chat_configuration(const std::vector<std::uint8_t>& b){if(b.size()>32768)return {};Get r{b};if(r.n(1)!=1)return {};ChatConfiguration c;c.rooms.clear();c.revision=static_cast<std::int64_t>(r.n(8));c.entry=static_cast<ChatRoomId>(r.n(2));c.exclude_sysop_statistics=r.flag();auto count=r.n(2);if(count>128)return {};for(std::uint64_t i=0;i<count&&r.ok;++i){ChatRoom v;v.id=static_cast<ChatRoomId>(r.n(2));v.name=r.text();v.enabled=r.flag();v.access=r.req();c.rooms.push_back(std::move(v));}count=r.n(2);if(count>130)return {};for(std::uint64_t i=0;i<count&&r.ok;++i){ChatDoorCommand v;v.menu=static_cast<MenuId>(r.n(1));v.key=static_cast<char>(r.n(1));v.door=static_cast<DoorId>(r.n(2));v.name=r.text();v.enabled=r.flag();v.access=r.req();c.commands.push_back(std::move(v));}if(!r.ok||r.p!=b.size()||!valid_chat_configuration(c))return {};return c;}
ChatHub::Session* ChatHub::find(const ChatIdentity& id){const auto it=sessions_.find(id.node);return it!=sessions_.end()&&it->second.caller.identity==id?&it->second:nullptr;}
const ChatHub::Session* ChatHub::find(const ChatIdentity& id)const{const auto it=sessions_.find(id.node);return it!=sessions_.end()&&it->second.caller.identity==id?&it->second:nullptr;}
bool ChatHub::push(Session& s,ChatEvent e){if(s.events.size()>=64){
        if(e.kind!=ChatEventKind::ended&&e.kind!=ChatEventKind::started)return false;
        // Reserve four slots for bounded lifecycle control; never discard accepted
        // private text or notices to make room for a private-chat transition.
        if(s.events.size()>=68){const auto old=std::find_if(s.events.begin(),s.events.end(),[](const auto& v){return v.kind==ChatEventKind::ended||v.kind==ChatEventKind::started;});if(old==s.events.end())return false;s.events.erase(old);}
    }s.events.push_back(std::move(e));return true;}
bool ChatHub::visible_to(const Session& viewer,const Session& target,bool room_only)const{
    if(room_only)return viewer.caller.room&&viewer.caller.room==target.caller.room;
    return viewer.caller.sysop||!target.caller.preferences.ghost||viewer.caller.identity==target.caller.identity;
}
ChatConfiguration ChatHub::configuration()const{std::lock_guard<std::mutex> lock(mutex_);return config_;}
bool ChatHub::configure(const ChatConfiguration& c){std::lock_guard<std::mutex> lock(mutex_);if(!sessions_.empty()||!valid_chat_configuration(c))return false;config_=c;return true;}
ChatResult ChatHub::change_configuration(const ChatConfiguration& c,const std::function<bool()>& commit){
    std::lock_guard<std::mutex> lock(mutex_);if(!valid_chat_configuration(c)||c.entry!=config_.entry)return ChatResult::invalid;if(c.revision!=config_.revision)return ChatResult::conflict;
    for(const auto& old:config_.rooms){const auto it=std::find_if(c.rooms.begin(),c.rooms.end(),[&](const auto& r){return r.id==old.id;});
        if(it!=c.rooms.end()&&it->name==old.name&&it->enabled==old.enabled&&it->access.mode==old.access.mode&&it->access.level==old.access.level&&it->access.sysop_only==old.access.sysop_only)continue;
        for(const auto& pair:sessions_)if(pair.second.caller.room==old.id)return ChatResult::busy;
    }
    if(!commit())return ChatResult::conflict;
    config_=c;++config_.revision;return ChatResult::ok;
}
bool ChatHub::attach(ChatParticipant p){std::lock_guard<std::mutex> lock(mutex_);if(!p.identity.session.valid()||p.identity.account<=0||!p.identity.node||p.identity.node>128||sessions_.count(p.identity.node)||!p.level||p.level>999)return false;
    p.name=chat_safe_text(p.name,64);p.activity=chat_safe_text(p.activity,96);p.room=0;p.preferences.ghost=p.preferences.ghost&&p.sysop;
    for(auto& item:sessions_)if(item.second.caller.preferences.notices&&(!p.preferences.ghost||item.second.caller.sysop))push(item.second,{ChatEventKind::presence,p.identity,p.name,p.name+" (Node "+chat_node(p.identity.node)+") logged on."});
    sessions_.emplace(p.identity.node,Session{std::move(p),{}, {}});return true;
}
bool ChatHub::refresh(const ChatParticipant& p){std::lock_guard<std::mutex> lock(mutex_);auto* s=find(p.identity);if(!s)return false;s->caller.level=p.level;s->caller.sysop=p.sysop;s->caller.sysop_threshold=p.sysop_threshold;s->caller.preferences=p.preferences;s->caller.preferences.ghost=p.preferences.ghost&&p.sysop;s->caller.name=chat_safe_text(p.name,64);s->caller.activity=chat_safe_text(p.activity,96);
    s->caller.private_chat_minutes=p.private_chat_minutes;s->caller.private_seconds_remaining_today=p.private_seconds_remaining_today;
    if(s->caller.room){const auto r=std::find_if(config_.rooms.begin(),config_.rooms.end(),[&](const auto& v){return v.id==s->caller.room;});if(r==config_.rooms.end()||!chat_room_access(*r,s->caller))leave_locked(*s);}return true;}
void ChatHub::safe(const ChatIdentity& id,bool value){std::lock_guard<std::mutex> lock(mutex_);if(auto* s=find(id)){
    s->caller.safe=value;
    if(!value){const auto it=operators_.find(s->operator_token);if(it!=operators_.end()&&it->second.pending)
        end_operator_locked(it->second,it->second.started,"Caller is busy; retry later.");}
}}
std::optional<ChatParticipant> ChatHub::participant(const ChatIdentity& id)const{std::lock_guard<std::mutex> lock(mutex_);const auto* s=find(id);return s?std::optional<ChatParticipant>(s->caller):std::nullopt;}
std::vector<ChatParticipant> ChatHub::visible(const ChatIdentity& id,bool room_only)const{std::lock_guard<std::mutex> lock(mutex_);std::vector<ChatParticipant> out;const auto* s=find(id);if(!s)return out;for(const auto& item:sessions_)if(visible_to(*s,item.second,room_only)){auto p=item.second.caller;if(p.room){p.activity="Chat Lobby";for(const auto& r:config_.rooms)if(r.id==p.room&&chat_room_access(r,s->caller))p.activity="Chat Lobby ["+r.name+"]";}out.push_back(std::move(p));}return out;}
std::vector<ChatRoom> ChatHub::rooms(const ChatIdentity& id)const{std::lock_guard<std::mutex> lock(mutex_);std::vector<ChatRoom> out;const auto* s=find(id);if(s)for(const auto& r:config_.rooms)if(chat_room_access(r,s->caller))out.push_back(r);return out;}
void ChatHub::room_notice(const ChatParticipant& p,std::string_view action){
    for(auto& item:sessions_){
        const auto& caller=item.second.caller;
        if(caller.room==p.room&&caller.identity!=p.identity)
            push(item.second,{ChatEventKind::public_text,p.identity,p.name,p.name+std::string(action),0,p.room,false,ChatLineStyle::presence});
    }
}
void ChatHub::leave_locked(Session& s){if(!s.caller.room)return;room_notice(s.caller," left the room.");s.caller.room=0;s.events.erase(std::remove_if(s.events.begin(),s.events.end(),[](const auto& e){return e.kind==ChatEventKind::public_text;}),s.events.end());}
void ChatHub::leave(const ChatIdentity& id){std::lock_guard<std::mutex> lock(mutex_);if(auto* s=find(id))leave_locked(*s);}
ChatResult ChatHub::join(const ChatIdentity& id,ChatRoomId room){std::lock_guard<std::mutex> lock(mutex_);auto* s=find(id);if(!s||s->peer||s->operator_token)return ChatResult::unavailable;for(const auto& r:config_.rooms)if(r.id==room){if(!chat_room_access(r,s->caller))return ChatResult::denied;if(s->caller.room==room)return ChatResult::ok;leave_locked(*s);s->caller.room=room;room_notice(s->caller," joined the room.");return ChatResult::ok;}return ChatResult::unavailable;}
ChatResult ChatHub::say(const ChatIdentity& id,std::string_view text,bool action){std::lock_guard<std::mutex> lock(mutex_);auto* s=find(id);const auto clean=chat_safe_text(text);if(!s||clean.empty())return ChatResult::invalid;
    const auto line=action?clean:s->caller.name+": "+clean;
    if(s->operator_token){const auto it=operators_.find(s->operator_token);if(it==operators_.end()||!it->second.active)return ChatResult::busy;
        if(s->events.size()>=64||!operator_line(it->second,line))return ChatResult::full;
        push(*s,{ChatEventKind::public_text,id,s->caller.name,line});return ChatResult::ok;}
    if(s->peer){auto* other=find(*s->peer);if(!other)return ChatResult::unavailable;if(other->events.size()>=64||s->events.size()>=64)return ChatResult::full;push(*other,{ChatEventKind::public_text,id,s->caller.name,line});push(*s,{ChatEventKind::public_text,id,s->caller.name,line});return ChatResult::ok;}
    if(!s->caller.room)return ChatResult::unavailable;
    for(const auto& item:sessions_)if(item.second.caller.room==s->caller.room&&item.second.events.size()>=64)return ChatResult::full;
    for(auto& item:sessions_)if(item.second.caller.room==s->caller.room)
        push(item.second,{ChatEventKind::public_text,id,s->caller.name,line,0,s->caller.room,false,action?ChatLineStyle::emote:ChatLineStyle::caller});
    return ChatResult::ok;}
ChatResult ChatHub::text(const ChatIdentity& from,const ChatIdentity& to,std::string_view text,bool whisper,bool override_dnd){std::lock_guard<std::mutex> lock(mutex_);auto* a=find(from);auto* b=find(to);if(!a||!b||from==to||!visible_to(*a,*b,whisper))return ChatResult::unavailable;
    if(b->caller.preferences.dnd&&!(override_dnd&&a->caller.sysop))return ChatResult::dnd;
    const auto clean=chat_safe_text(text);if(clean.empty())return ChatResult::invalid;
    return push(*b,{ChatEventKind::private_text,from,a->caller.name,clean,0,whisper?a->caller.room:ChatRoomId{},override_dnd&&a->caller.sysop})?ChatResult::ok:ChatResult::full;}
ChatResult ChatHub::reply_text(const ChatIdentity& from,std::uint64_t token,std::string_view text,bool override_dnd){
    std::lock_guard<std::mutex> lock(mutex_);
    auto* sender=find(from);
    if(!sender)return ChatResult::unavailable;
    const auto reply=sender->replies.find(token);
    if(reply==sender->replies.end())return ChatResult::unavailable;
    auto* recipient=find(reply->second);
    sender->replies.erase(reply);
    if(!recipient)return ChatResult::unavailable;
    const bool overridden=override_dnd&&sender->caller.sysop;
    if(recipient->caller.preferences.dnd&&!overridden)return ChatResult::dnd;
    const auto clean=chat_safe_text(text);
    if(clean.empty())return ChatResult::invalid;
    return push(*recipient,{ChatEventKind::private_text,from,sender->caller.name,clean,
        0,0,overridden,ChatLineStyle::text,0,true})?ChatResult::ok:ChatResult::full;
}
void ChatHub::discard_reply(const ChatIdentity& id,std::uint64_t token){
    std::lock_guard<std::mutex> lock(mutex_);
    if(auto* caller=find(id))caller->replies.erase(token);
}
ChatRequestResult ChatHub::invite(const ChatIdentity& from,const ChatIdentity& to,std::uint64_t now,bool override_dnd){std::lock_guard<std::mutex> lock(mutex_);auto* a=find(from);auto* b=find(to);if(!a||!b||from==to||!visible_to(*a,*b))return {};
    if(a->caller.private_seconds_remaining_today==0||b->caller.private_seconds_remaining_today==0)return {ChatResult::denied,0};
    if(b->caller.preferences.dnd&&!(override_dnd&&a->caller.sysop))return {ChatResult::dnd,0};
    if(a->peer||b->peer||a->operator_token||b->operator_token||a->page||b->page||!b->caller.safe)return {ChatResult::busy,0};
    for(const auto& it:invites_)if(it.second.from==from||it.second.to==from||it.second.to==to||it.second.from==to)return {ChatResult::busy,0};
    if(next_request_==std::numeric_limits<std::uint64_t>::max())return {ChatResult::unavailable,0};
    const auto request=next_request_++;
    if(!push(*b,{ChatEventKind::invite,from,a->caller.name,{},request,0,override_dnd&&a->caller.sysop}))return {ChatResult::full,0};
    invites_.emplace(request,Invite{request,now+60000,from,to,override_dnd&&a->caller.sysop});return {ChatResult::ok,request};}
void ChatHub::finish_invite(std::map<std::uint64_t,Invite>::iterator it,std::string_view text){const auto i=it->second;for(const auto& id:{i.from,i.to})if(auto* s=find(id)){s->events.erase(std::remove_if(s->events.begin(),s->events.end(),[&](const auto& e){return e.request==i.id;}),s->events.end());push(*s,{ChatEventKind::ended,{}, {},std::string(text),i.id});}invites_.erase(it);}
ChatResult ChatHub::answer(const ChatIdentity& id,std::uint64_t request,bool accept,std::uint64_t now){std::lock_guard<std::mutex> lock(mutex_);auto it=invites_.find(request);if(it==invites_.end()||it->second.to!=id)return ChatResult::unavailable;const auto i=it->second;auto* a=find(i.from);auto* b=find(i.to);
    if(a&&b&&(a->caller.private_seconds_remaining_today==0||b->caller.private_seconds_remaining_today==0)){finish_invite(it,"Private chat time is unavailable.");return ChatResult::denied;}
    if(!accept||now>=i.expires||!a||!b||a->peer||b->peer||a->operator_token||b->operator_token||a->page||b->page||!b->caller.safe||!visible_to(*a,*b)||(b->caller.preferences.dnd&&!(i.override_dnd&&a->caller.sysop))){finish_invite(it,accept?"Chat request expired or is no longer available.":"Chat request declined.");return ChatResult::unavailable;}
    a->suspended_room=a->caller.room;b->suspended_room=b->caller.room;leave_locked(*a);leave_locked(*b);a->peer=i.to;b->peer=i.from;a->private_started=b->private_started=now;invites_.erase(it);push(*a,{ChatEventKind::started,i.to,b->caller.name,{},request});push(*b,{ChatEventKind::started,i.from,a->caller.name,{},request});return ChatResult::ok;}
void ChatHub::cancel(const ChatIdentity& id,std::uint64_t request){std::lock_guard<std::mutex> lock(mutex_);auto it=invites_.find(request);if(it!=invites_.end()&&(it->second.from==id||it->second.to==id))finish_invite(it,"Chat request cancelled.");}
bool ChatHub::invite_pending(const ChatIdentity& id,std::uint64_t request) const {
    std::lock_guard<std::mutex> lock(mutex_);const auto it=invites_.find(request);
    return it!=invites_.end()&&(it->second.from==id||it->second.to==id);
}
void ChatHub::tick(std::uint64_t now){std::lock_guard<std::mutex> lock(mutex_);
    for(auto& entry:sessions_){auto& s=entry.second;if(!s.peer)continue;
        const auto* other=find(*s.peer);std::uint64_t deadline=UINT64_MAX;
        for(const auto* caller:std::initializer_list<const Session*>{&s,other})if(caller&&caller->caller.private_chat_minutes)
            deadline=std::min(deadline,caller->private_started+static_cast<std::uint64_t>(*caller->caller.private_chat_minutes)*60000);
        if(now>=deadline)end_locked(s,"Private chat time has expired.",deadline);
        else if(s.caller.private_seconds_remaining_today==0||(other&&other->caller.private_seconds_remaining_today==0))
            end_locked(s,"Private chat time has expired.",now);
    }
    for(auto it=invites_.begin();it!=invites_.end();){auto next=std::next(it);if(now>=it->second.expires)finish_invite(it,"Chat request timed out.");it=next;}
    for(auto& entry:sessions_)if(entry.second.page&&now>=entry.second.page_expires)entry.second.page=entry.second.page_expires=0;
    for(auto it=operators_.begin();it!=operators_.end();){auto& op=it->second;
        if(op.pending&&now>=op.claim_by)end_operator_locked(op,now,"Caller is busy; retry later.");
        if(now>=op.lease){if(op.active||op.pending)end_operator_locked(op,op.lease,"Operator connection lost. Chat ended.");else{it=operators_.erase(it);continue;}}
        ++it;
    }
}
void ChatHub::restore_room(Session& s){
    const auto previous=s.suspended_room;s.suspended_room=0;
    for(const auto& room:config_.rooms)if(room.id==previous&&chat_room_access(room,s.caller)){s.caller.room=previous;room_notice(s.caller," joined the room.");break;}
}
void ChatHub::end_locked(Session& s,std::string_view text,std::uint64_t now,bool departure){if(!s.peer)return;auto* other=find(*s.peer);
    const auto finish=[&](Session& caller){
        if(now>=caller.private_started)caller.private_millis+=now-caller.private_started;
        caller.private_started=0;caller.peer.reset();restore_room(caller);
        // The departing caller already chose to leave. Only the remaining
        // caller needs the departure acknowledgement, inside the live chat.
        if(!departure||&caller!=&s)push(caller,{ChatEventKind::ended,s.caller.identity,s.caller.name,
            departure?s.caller.name+" has left chat":std::string(text)});
    };
    finish(s);if(other)finish(*other);
}
void ChatHub::end(const ChatIdentity& id,std::uint64_t now){std::lock_guard<std::mutex> lock(mutex_);if(auto* s=find(id))end_locked(*s,"Chat ended.",now,true);}
std::optional<std::uint64_t> ChatHub::private_usage_millis(const ChatIdentity& id,std::uint64_t now)const{
    std::lock_guard<std::mutex> lock(mutex_);const auto* s=find(id);if(!s)return {};
    return s->private_millis+(s->peer&&now>=s->private_started?now-s->private_started:0);
}
std::optional<ChatIdentity> ChatHub::peer(const ChatIdentity& id)const{std::lock_guard<std::mutex> lock(mutex_);const auto* s=find(id);return s?s->peer:std::nullopt;}
void ChatHub::detach(const ChatIdentity& id,std::uint64_t now){std::lock_guard<std::mutex> lock(mutex_);auto* s=find(id);if(!s)return;const auto p=s->caller;s->suspended_room=0;leave_locked(*s);end_locked(*s,"Chat ended.",now,true);const auto op=operators_.find(s->operator_token);if(op!=operators_.end())end_operator_locked(op->second,now,"The caller disconnected. Chat ended.");for(auto it=invites_.begin();it!=invites_.end();){auto next=std::next(it);if(it->second.from==id||it->second.to==id)finish_invite(it,"The caller disconnected.");it=next;}sessions_.erase(id.node);
    for(auto& item:sessions_)if(item.second.caller.preferences.notices&&(!p.preferences.ghost||item.second.caller.sysop))push(item.second,{ChatEventKind::presence,id,p.name,p.name+" (Node "+chat_node(id.node)+") logged off."});}
std::vector<ChatEvent> ChatHub::poll(const ChatIdentity& id,bool boundary){std::lock_guard<std::mutex> lock(mutex_);std::vector<ChatEvent> out;auto* s=find(id);if(!s||!boundary)return out;
    while(!s->events.empty()){auto e=std::move(s->events.front());s->events.pop_front();const auto* sender=find(e.sender);
        if(e.kind==ChatEventKind::private_text){
            if(!sender||(!e.direct_reply&&!visible_to(*sender,*s,e.room!=0))||
                (s->caller.preferences.dnd&&!(e.override_dnd&&sender->caller.sysop))||
                (e.room&&(sender->caller.room!=e.room||s->caller.room!=e.room)))continue;
            if(!e.room){
                if(s->replies.size()>=64)s->replies.erase(s->replies.begin());
                e.reply_token=next_request_++;
                s->replies.emplace(e.reply_token,e.sender);
            }
        }
        if(e.kind==ChatEventKind::invite&&!invites_.count(e.request))continue;
        if(e.kind==ChatEventKind::presence&&(!s->caller.preferences.notices||(sender&&sender->caller.preferences.ghost&&!s->caller.sysop)))continue;
        if(e.kind==ChatEventKind::public_text&&e.room&&e.room!=s->caller.room)continue;
        out.push_back(std::move(e));
    }return out;}
std::vector<ChatEvent> ChatHub::poll_request(const ChatIdentity& id,std::uint64_t request){
    std::lock_guard<std::mutex> lock(mutex_);std::vector<ChatEvent> out;auto* s=find(id);if(!s||!request)return out;
    // Waiting for consent must not drain accepted text into an unbounded second
    // queue. Lifecycle controls have reserved space and can bypass queued text.
    for(auto it=s->events.begin();it!=s->events.end();){
        if(it->request==request&&(it->kind==ChatEventKind::started||it->kind==ChatEventKind::ended)){out.push_back(std::move(*it));it=s->events.erase(it);}else ++it;
    }return out;
}
ChatResult ChatHub::notice(std::optional<ChatIdentity> target,ChatRoomId room,std::string_view text,bool override_dnd){std::lock_guard<std::mutex> lock(mutex_);const auto clean=chat_safe_text(text);if(clean.empty())return ChatResult::invalid;if(target){auto* s=find(*target);if(!s)return ChatResult::unavailable;if(s->caller.preferences.dnd&&!override_dnd)return ChatResult::dnd;return push(*s,{ChatEventKind::notice,{}, {},"Sysop notice: "+clean})?ChatResult::ok:ChatResult::full;}
    for(const auto& it:sessions_)if((!room||it.second.caller.room==room)&&it.second.events.size()>=64)return ChatResult::full;
    for(auto& it:sessions_)if(!room||it.second.caller.room==room)push(it.second,{ChatEventKind::notice,{}, {},"Sysop notice: "+clean});
    return ChatResult::ok;}
std::vector<ChatEmote> parse_chat_emotes(std::string_view text){std::vector<ChatEmote> out;if(text.size()>65536)return out;std::set<std::string> names;while(!text.empty()){auto n=text.find('\n');auto line=text.substr(0,n);if(n==text.npos)text={};else text.remove_prefix(n+1);if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);if(line.empty()||line.front()=='#'||line.front()==';')continue;auto a=line.find('|'),b=a==line.npos?line.npos:line.find('|',a+1);if(a==line.npos||b==line.npos)continue;auto name=std::string(line.substr(0,a));for(auto& ch:name)if(ch>='a'&&ch<='z')ch=static_cast<char>(ch-'a'+'A');if(name=="HELP"||name.empty()||name.size()>24||!std::all_of(name.begin(),name.end(),[](char c){return c>='A'&&c<='Z';})||!names.insert(name).second)continue;auto mode=line.substr(a+1,b-a-1);if(mode!="NONE"&&mode!="TARGET")continue;auto body=chat_safe_text(line.substr(b+1));if(body.empty())continue;out.push_back({name,mode=="TARGET",body});if(out.size()==128)break;}return out;}
std::optional<std::string> render_chat_emote(const ChatEmote& e,std::string_view caller,std::string_view target){if(e.targeted&&target.empty())return {};std::string out;for(std::size_t i=0;i<e.text.size();){
    if(e.text.compare(i,8,"{caller}")==0){out+=chat_safe_text(caller,64);i+=8;}
    else if(e.text.compare(i,8,"{target}")==0){out+=chat_safe_text(target,64);i+=8;}
    else out+=e.text[i++];
}return chat_safe_text(out);}
#include "chat_operator.inc"
} // namespace sfx::application
