#include "sfx/application/chat_session.hpp"
#include <algorithm>
#include <cctype>

namespace sfx::application {
namespace {
std::string at(unsigned row,unsigned column){return "\x1b["+std::to_string(row)+";"+std::to_string(column)+"H";}
std::string upper(std::string text){for(auto& c:text)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return text;}
bool live_cell(unsigned x,unsigned y){return x>=3&&x<=77&&(y==2||(y>=4&&y<=19)||y==21||y==22);}
bool completed(TerminalOutcome r){return r==TerminalOutcome::completed||r==TerminalOutcome::empty;}
// Notice input is already single-line CP437. Wrap only its presentation, with
// one spare column so terminal autowrap cannot insert an extra physical row.
// Keep the separator on the preceding row: joining the rows recovers every
// original byte, including repeated spaces and the visible Sysop prefix.
void notice_rows(std::string_view text,unsigned columns,const std::function<void(std::string)>& emit){
    const auto width=static_cast<std::size_t>(std::clamp(columns,2U,200U)-1);
    while(text.size()>width){
        const auto space=text.rfind(' ',width-1);
        const auto count=space==std::string_view::npos?width:space+1;
        emit(std::string(text.substr(0,count)));text.remove_prefix(count);
    }
    if(!text.empty())emit(std::string(text));
}
std::string heading(std::string_view title,bool ansi,unsigned columns){
    const auto width=std::max(12U,columns>1?columns-1:1U);
    if(!ansi)return "\r\n"+std::string(title)+"\r\n"+std::string(std::min<unsigned>(width,static_cast<unsigned>(title.size()+10)),'-')+"\r\n\r\n";
    return "\x1b[0m\x1b[2J\x1b[H\x1b[1;30m\xdb\x1b[0;37m\xdb\x1b[1;37m\xdb\x1b[0;37m  "+std::string(title)+
        "  \x1b[1;37m\xdb\x1b[0;37m\xdb\x1b[1;30m\xdb\x1b[0;37m\r\n\r\n"+
        std::string(std::min<unsigned>(width,static_cast<unsigned>(title.size()+10)),static_cast<char>(196))+"\r\n\r\n";
}
std::string who_heading(bool ansi,unsigned columns){
    const auto width=std::min(21U,columns>1?columns-1:1U);
    if(!ansi)return "\r\nWho's on-line\r\n"+std::string(width,'-')+"\r\n\r\n";
    return "\x1b[0m\x1b[2J\x1b[H\x1b[1;30m\xfe\x1b[0;37m\xfe\x1b[1;37m\xfe"
        "\x1b[0;37m Who's on-line \x1b[1;37m\xfe\x1b[0;37m\xfe\x1b[1;30m\xfe\r\n"+
        std::string(width,static_cast<char>(196))+"\x1b[0;37m\r\n\r\n";
}
std::string who_action(char key,std::string_view label,bool ansi){
    return ansi?"\x1b[0;37m[\x1b[1;37m"+std::string(1,key)+"\x1b[0;37m:> "+std::string(label)+"\r\n":
        "["+std::string(1,key)+":> "+std::string(label)+"\r\n";
}
}
bool valid_pchat_frame(std::string_view bytes)noexcept{
    if(bytes.empty()||bytes.size()>32768)return false;
    unsigned x=1,y=1;
    for(std::size_t i=0;i<bytes.size();){const auto c=static_cast<unsigned char>(bytes[i++]);
        if(c==27){const auto start=i-1;if(i==bytes.size()||bytes[i++]!='[')return false;std::vector<unsigned> args;unsigned n{};bool digits=false;
            while(i<bytes.size()&&(bytes[i]==';'||(bytes[i]>='0'&&bytes[i]<='9'))){const auto d=bytes[i++];if(d==';'){args.push_back(n);n=0;digits=false;}else{digits=true;n=n*10+static_cast<unsigned>(d-'0');if(n>999)return false;}}
            if(digits||!args.empty())args.push_back(n);
            if(i==bytes.size()||args.size()>8)return false;
            const auto cmd=bytes[i++];
            if(!valid_terminal_ansi_sequence(bytes.substr(start,i-start)))return false;
            if(cmd=='m')continue;
            if(cmd!='H'&&cmd!='f')return false;
            if(args.size()>2)return false;
            y=args.empty()||!args[0]?1:args[0];x=args.size()<2||!args[1]?1:args[1];
            if(x>79||y>23)return false;
            continue;
        }
        if(c=='\r'){x=1;continue;}if(c=='\n'){if(++y>24)return false;continue;}
        if(c<32||c==127||c==0x9b||x>79||y>23||(live_cell(x,y)&&c!=' '))return false;
        ++x;
    }return true;
}
std::string stock_pchat_frame(){std::string out="\x1b[0;37m";for(unsigned y=1;y<=23;++y){out+=at(y,1);if(y==1||y==3||y==20||y==23){out+=static_cast<char>(y==1?218:y==23?192:195);out+=std::string(77,static_cast<char>(196));out+=static_cast<char>(y==1?191:y==23?217:180);}else{out+=static_cast<char>(179);out+=std::string(77,' ');out+=static_cast<char>(179);}}return out;}
ChatCallerSession::ChatCallerSession(ChatHub& hub,ChatIdentity id,ChatSessionPorts ports):hub_(hub),identity_(id),ports_(std::move(ports)){}
bool ChatCallerSession::present(std::string_view text){if(!ports_.refresh()||!ports_.write(text))return false;++presentations_;return true;}
std::optional<std::string> ChatCallerSession::line(std::string_view prompt,std::size_t maximum){
    hub_.safe(identity_,false);struct Restore{ChatHub& hub;ChatIdentity id;~Restore(){hub.safe(id,true);}} restore{hub_,identity_};
    const auto r=ports_.terminal.read_visible({"chat.input",std::string(prompt),maximum,EmptyInputPolicy::use_default});if(!completed(r.outcome))return {};return r.value;
}
void ChatCallerSession::append(std::string_view text){auto clean=chat_safe_text(text,1024);do{history_.push_back(clean.substr(0,75));++history_version_;clean.erase(0,std::min<std::size_t>(75,clean.size()));while(history_.size()>16)history_.pop_front();}while(!clean.empty());}
bool ChatCallerSession::framed()const{return ports_.ansi()&&ports_.rows()>=24&&ports_.columns()>=80;}
bool ChatCallerSession::redraw(bool private_mode,bool full){
    const auto me=hub_.participant(identity_);if(!me)return false;std::string title;
    if(private_mode){const auto id=hub_.peer(identity_);const auto peer=id?hub_.participant(*id):std::nullopt;title=peer?"Private Chat with "+peer->name+" (Node "+chat_node(peer->identity.node)+")":hub_.operator_active(identity_)?"Private Chat with Sysop":"Private Chat";}
    else{title="Chat Lobby";for(const auto& r:hub_.rooms(identity_))if(r.id==me->room)title=r.name;title+=" - "+std::to_string(hub_.visible(identity_,true).size())+" callers";}
    const auto hint=private_mode?"/Q End Chat   /? Help":"/Q Chat Menu   /? Commands";
    if(framed()){std::string out="\x1b[0m\x1b[2J\x1b[H";auto art=private_mode&&ports_.private_art?ports_.private_art(true):std::nullopt;out+=art&&valid_pchat_frame(*art)?*art:stock_pchat_frame();out+="\x1b[0;37m";
        const auto row=[&](unsigned y,std::string text){text=chat_safe_text(text,75);out+=at(y,3)+text+std::string(75-text.size(),' ');};
        row(2,title);unsigned y=4;for(const auto& h:history_)row(y++,h);while(y<=19)row(y++,{});row(21,draft_.size()>75?draft_.substr(draft_.size()-75):draft_);row(22,hint);out+=at(21,3+static_cast<unsigned>(std::min<std::size_t>(draft_.size(),74)));return present(out);
    }
    if(!full&&plain_ready_){
        const auto count=std::min<std::uint64_t>(history_version_-plain_seen_,history_.size());
        if(!count&&!plain_prompt_pending_)return true;
        std::string out="\r\n";for(auto i=history_.size()-static_cast<std::size_t>(count);i<history_.size();++i)out+=history_[i]+"\r\n";
        plain_seen_=history_version_;plain_prompt_pending_=false;return present(out+"> "+draft_);
    }
    plain_ready_=true;plain_seen_=history_version_;plain_prompt_pending_=false;std::string out="\r\n";
    if(private_mode&&ports_.private_art){const auto art=ports_.private_art(false);if(art){bool valid=art->size()<=32768;unsigned width{},lines{};for(unsigned char c:*art){if(c=='\r')continue;if(c=='\n'){width=0;if(++lines>23)valid=false;}else if(c<32||c==127||c==0x9b||++width>79)valid=false;}if(valid)out+=*art+"\r\n";}}
    out+=title+"\r\n";for(const auto& h:history_)out+=h+"\r\n";return present(out+hint+"\r\n> "+draft_);
}
std::string ChatCallerSession::caller_row(const ChatParticipant& p,std::uint16_t maximum_node)const{
    const bool ansi=ports_.ansi();const auto width=std::max(12U,ports_.columns()>1?ports_.columns()-1:1U);
    const auto prefix="Node "+chat_node(p.identity.node,maximum_node);
    const auto text=chat_safe_text(prefix+": "+p.name+(p.identity==identity_?" (you)":"")+" - "+p.activity,width);
    if(!ansi)return text;
    return "\x1b[0;37m"+text.substr(0,prefix.size())+"\x1b[1;30m:\x1b[0;37m"+text.substr(prefix.size()+1);
}
std::optional<ChatParticipant> ChatCallerSession::choose(bool room_only){
    if(ports_.ansi()&&!present(menu_text_sgr(7)))return {};
    const auto callers=hub_.visible(identity_,room_only);unsigned row{};const auto page=std::max(5U,ports_.rows()>5?ports_.rows()-5:5);
    for(const auto& p:callers){if(!present("\r\n"+caller_row(p)))return {};if(++row%page==0){const auto key=line("\r\nEnter for more, Q to quit: ",1);if(!key||upper(*key)=="Q")return {};}}
    const auto value=line("\r\nNode number (Enter cancels): ",3);if(!value||value->empty())return {};unsigned n{};for(char c:*value){if(c<'0'||c>'9')return {};n=n*10+static_cast<unsigned>(c-'0');}
    for(const auto& p:callers)if(p.identity.node==n){if(p.identity==identity_){present("\r\nChoose another caller; you cannot send text or request node chat with yourself.\r\n");return {};}return p;}
    present("\r\nThat caller is not available.\r\n");return {};
}
bool ChatCallerSession::override_dnd(const ChatParticipant& p){if(!p.preferences.dnd)return true;const auto me=hub_.participant(identity_);if(!me||!me->sysop){present("\r\nThat caller has Do Not Disturb on.\r\n");return false;}const auto answer=line("\r\nThat caller has Do Not Disturb on. Override? [y/N] ",1);return answer&&upper(*answer)=="Y";}
bool ChatCallerSession::compose(bool whisper,std::optional<ChatIdentity> reply){auto target=reply?hub_.participant(*reply):choose(whisper);if(!target){if(reply)return present("\r\nThe sender disconnected.\r\n");return true;}if(!override_dnd(*target))return true;
    const auto message=line("\r\nPrivate message (Enter cancels): ");if(!message)return false;if(message->empty())return true;
    const auto r=hub_.text(identity_,target->identity,*message,whisper,target->preferences.dnd);return present("\r\n"+std::string(r==ChatResult::ok?(target->safe?"Sent.":"Queued until the caller returns to a safe screen."):chat_result_text(r))+"\r\n");}
bool ChatCallerSession::invite(const ChatParticipant& p){if(!override_dnd(p))return true;const auto result=hub_.invite(identity_,p.identity,ports_.now(),p.preferences.dnd);if(result.code!=ChatResult::ok)return present("\r\n"+std::string(chat_result_text(result.code))+"\r\n");
    if(!present("\r\nChat requested. Waiting up to 60 seconds; Q cancels.\r\n"))return false;
    for(;;){if(!ports_.refresh())return false;hub_.tick(ports_.now());bool start=false;std::string ended;for(const auto& e:hub_.poll_request(identity_,result.request)){if(e.kind==ChatEventKind::started)start=true;else if(e.kind==ChatEventKind::ended)ended=e.text;}if(start)return private_chat();if(!ended.empty())return present("\r\n"+ended+"\r\n");
        const auto key=ports_.read();if(key.outcome==TerminalOutcome::timed_out)continue;if(!completed(key.outcome))return false;if(key.byte&&(*key.byte=='Q'||*key.byte=='q')){hub_.cancel(identity_,result.request);return true;}}
}
bool ChatCallerSession::online(){private_return_to_menu_=false;for(;;){const auto callers=hub_.visible(identity_);unsigned rows{};std::uint16_t maximum_node=ports_.node_count?ports_.node_count():99;for(const auto& p:callers)maximum_node=std::max(maximum_node,p.identity.node);
    const auto height=std::max(1U,ports_.rows()>10?ports_.rows()-10:1U);const bool ansi=ports_.ansi();
    if(!present(who_heading(ansi,ports_.columns())))return false;
    for(std::size_t index=0;index<callers.size();++index){const auto& p=callers[index];
        if(!present(caller_row(p,maximum_node)+"\r\n"))return false;
        if(++rows==height&&index+1<callers.size()){const auto more=line("\r\nEnter for more, Q to quit: ",1);if(!more)return false;if(upper(*more)=="Q")return true;rows=0;if(!present(who_heading(ansi,ports_.columns())))return false;}}
    const bool disconnect=ports_.can_disconnect&&ports_.can_disconnect()&&ports_.disconnect;
    if(!present("\r\n"+who_action('1',"Send text to caller",ansi)+who_action('2',"Request Node Chat",ansi)+(disconnect?who_action('D',"Disconnect Caller",ansi):std::string{})+who_action('Q',"Quit",ansi)))return false;
    TerminalInputResult answer;
    {
        hub_.safe(identity_,false);struct Restore{ChatHub& hub;ChatIdentity id;~Restore(){hub.safe(id,true);}} restore{hub_,identity_};
        if(ports_.hotkeys&&ports_.hotkeys()){
            const auto key=ports_.terminal.read_key({"chat.who.command","\r\nCommand (Enter returns): ",disconnect?std::vector<char>{'1','2','D','Q'}:std::vector<char>{'1','2','Q'},'Q',true});
            answer={key.outcome,key.key?std::string(1,*key.key):std::string{}};
        }else answer=ports_.terminal.read_visible({"chat.who.command","\r\nCommand (Enter returns): ",1,EmptyInputPolicy::use_default});
    }
    if(answer.outcome==TerminalOutcome::overlength||answer.outcome==TerminalOutcome::invalid||answer.outcome==TerminalOutcome::cancelled){if(!present("\r\nChoose 1, 2 or Q.\r\n"))return false;continue;}
    if(!completed(answer.outcome))return false;
    if(answer.value.empty()||upper(answer.value)=="Q")return true;
    if(answer.value=="1"){if(!compose())return false;}else if(answer.value=="2"){const auto target=choose();if(target&&!invite(*target))return false;if(private_return_to_menu_)return true;}
    else if(upper(answer.value)=="D"&&disconnect){const auto target=choose();if(!target)continue;
        const auto prompt="Disconnect "+target->name+" on node "+std::to_string(target->identity.node)+"? [y/N] ";
        bool confirmed=false;
        if(ports_.hotkeys&&ports_.hotkeys()){const auto confirmation=ports_.terminal.read_key({"sysop.disconnect.confirm",prompt,{'Y','N'},'N',false,true});confirmed=confirmation.key=='Y';}
        else {const auto confirmation=ports_.terminal.read_visible({"sysop.disconnect.confirm",prompt,1,EmptyInputPolicy::use_default});confirmed=confirmation.has_value()&&upper(confirmation.value)=="Y";}
        if(confirmed){if(!ports_.can_disconnect||!ports_.can_disconnect()){if(!present("\r\nSysop access changed; no disconnect requested.\r\n"))return false;}
            else if(!present("\r\n"+ports_.disconnect(*target)+"\r\n"))return false;}
    }
}}
std::optional<bool> ChatCallerSession::consent(std::uint64_t request){
    hub_.safe(identity_,false);struct Restore{ChatHub& hub;ChatIdentity id;~Restore(){hub.safe(id,true);}} restore{hub_,identity_};
    const auto color=ports_.ansi()?menu_text_sgr(7):std::string{};
    if(!present(system_menu_text("Accept? [y/N] ",ports_.ansi())))return {};
    char answer{};
    for(;;){
        if(!ports_.refresh())return {};
        hub_.tick(ports_.now());
        if(!hub_.invite_pending(identity_,request))return false;
        const auto key=ports_.read();if(key.outcome==TerminalOutcome::timed_out)continue;
        if(!completed(key.outcome))return {};
        if(!key.byte)continue;
        const auto c=*key.byte;
        if(c=='\r'||c=='\n'){if(ports_.accepted_key)ports_.accepted_key(c=='\r');if(!present("\r\n"))return {};return answer=='Y'||answer=='y';}
        if((c=='\b'||c==127)&&answer){answer=0;if(!present("\b \b"))return {};}
        else if(!answer&&(c=='Y'||c=='y'||c=='N'||c=='n')){answer=static_cast<char>(c);if(!present(std::string(1,answer)))return {};}
    }
}
bool ChatCallerSession::acknowledge_end(std::string_view text){
    hub_.safe(identity_,false);
    struct Restore{ChatHub& hub;ChatIdentity id;~Restore(){hub.safe(id,true);}} restore{hub_,identity_};
    const auto color=ports_.ansi()?menu_text_sgr(7):std::string{};
    std::string notice=color;
    if(framed()){
        // Keep the ending inside the existing frame; do not overwrite its
        // bottom border or return to a menu before the caller acknowledges it.
        const auto message=chat_safe_text(text,150);
        for(unsigned row=0;row<2;++row){const auto offset=std::min<std::size_t>(message.size(),row*75U);
            const auto part=message.substr(offset,75);notice+=at(18+row,3)+part+std::string(75-part.size(),' ');}
        notice+=at(21,3)+std::string(75,' ')+at(22,3)+"<enter>"+std::string(68,' ')+at(22,10);
    }else notice+="\r\n"+std::string(text)+"\r\n<enter>";
    if(!present(notice))return false;
    for(;;){
        if(!ports_.refresh())return false;
        const auto key=ports_.read();if(key.outcome==TerminalOutcome::timed_out)continue;
        if(!completed(key.outcome))return false;
        if(key.byte&&(*key.byte=='\r'||*key.byte=='\n')){
            if(ports_.accepted_key)ports_.accepted_key(*key.byte=='\r');
            return present("\r\n");
        }
    }
}
bool ChatCallerSession::events(bool conversation,const std::function<void(std::string)>& notice,bool private_mode){hub_.tick(ports_.now());
    if(hub_.claim_operator(identity_,ports_.now())){if(!private_chat())return false;if(conversation&&!redraw(false))return false;}
    auto events=hub_.poll(identity_,true);for(const auto& e:events){
    if(private_mode&&e.kind==ChatEventKind::ended&&!e.request&&!hub_.peer(identity_)&&!hub_.operator_active(identity_)){
        if(!acknowledge_end(e.text))return false;
        continue;
    }
    if(e.kind==ChatEventKind::private_text){const auto color=ports_.ansi()?menu_text_sgr(7):std::string{};const auto body=ports_.ansi()?menu_text_sgr(ports_.text_color?ports_.text_color():7):std::string{};if(!present(color+"\r\nPrivate text from "+e.name+" (Node "+chat_node(e.sender.node)+"):\r\n"+body+e.text+"\r\n"))return false;const auto reply=line("Reply? [y/N] ",1);if(!reply)return false;if(upper(*reply)=="Y"&&!compose(false,e.sender))return false;plain_prompt_pending_=true;}
    else if(e.kind==ChatEventKind::invite){const auto color=ports_.ansi()?menu_text_sgr(7):std::string{};if(!present(color+"\r\n"+e.name+" (Node "+chat_node(e.sender.node)+") requests a private chat.\r\n"))return false;const auto answer=consent(e.request);if(!answer)return false;const auto r=hub_.answer(identity_,e.request,*answer,ports_.now());if(r==ChatResult::ok&&!private_chat())return false;plain_prompt_pending_=true;}
    else if(e.kind==ChatEventKind::public_text&&conversation){append(e.text);}
    else if(!e.text.empty()){if(conversation)append(e.text);else if(e.kind==ChatEventKind::notice)notice_rows(e.text,ports_.columns(),notice);else notice(e.text);}
    if(conversation&&(!private_mode||hub_.peer(identity_)||hub_.operator_active(identity_))&&!redraw(hub_.peer(identity_).has_value()||hub_.operator_active(identity_),false))return false;
}return true;}
bool ChatCallerSession::safe_events(const std::function<void(std::string)>& notice){hub_.safe(identity_,true);return events(false,notice);}
bool ChatCallerSession::help(bool private_mode){
    hub_.safe(identity_,false);struct Restore{ChatHub& hub;ChatIdentity id;~Restore(){hub.safe(id,true);}} restore{hub_,identity_};
    std::vector<std::string> entries=private_mode?std::vector<std::string>{"/Q End Chat","/? or /HELP Commands","Escape opens Help."}:
        std::vector<std::string>{"/R Change Chat Room","/L List Chat Rooms","/W Who's Online","/T Private Online Text","/WHISPER Private text to a room participant","/Q Chat Menu","G Goodbye / Logoff","/G Quick Goodbye","/? or /HELP Commands","/EMOTE HELP Perform the HELP emote","//text Send text beginning with a slash","Escape opens Help.","","EMOTES - public actions"};
    if(!private_mode)for(const auto& e:ports_.emotes())entries.push_back("/"+(e.command=="HELP"?std::string("EMOTE HELP"):e.command)+(e.targeted?" - choose a room participant":" - public action"));
    if(!private_mode&&ports_.door_commands){const auto commands=ports_.door_commands();if(!commands)return false;if(!commands->commands.empty())entries.push_back("DOORS");for(const auto& command:commands->commands)entries.push_back("/"+std::string(1,command.key)+" "+command.name);}
    const auto width=std::max(12U,ports_.columns()>1?ports_.columns()-1:1U),height=std::max(1U,ports_.rows()>7?ports_.rows()-7:1U);
    std::vector<std::string> lines;for(const auto& entry:entries){auto clean=chat_safe_text(entry,512);do{lines.push_back(clean.substr(0,width));clean.erase(0,std::min<std::size_t>(width,clean.size()));}while(!clean.empty());}
    for(std::size_t start=0;start<lines.size();start+=height){
        if(!present(heading(private_mode?"PRIVATE CHAT COMMANDS":"CHAT COMMANDS",ports_.ansi(),ports_.columns())))return false;
        const auto end=std::min(lines.size(),start+height);for(auto i=start;i<end;++i)if(!present(lines[i]+"\r\n"))return false;
        // Help is a key-driven pause, not a one-character text field. Ignore
        // stray typing/navigation; cancelling Help must not end the session.
        for(;;){
            const auto answer=ports_.terminal.read_key({"chat.help",end<lines.size()?"\r\nEnter for more, Q to return: ":"\r\nPress Enter to return: ",{'Q',' '},' ',false,true});
            if(answer.outcome==TerminalOutcome::cancelled)return ports_.refresh();
            if(answer.outcome==TerminalOutcome::invalid||answer.outcome==TerminalOutcome::overlength)continue;
            if(!completed(answer.outcome))return false;
            if(answer.key=='Q')return true;
            break;
        }
    }return true;
}
bool ChatCallerSession::private_chat(){const auto me=hub_.participant(identity_);const ChatRoomId prior_room=me?me->room:ChatRoomId{};auto history=std::move(history_);auto draft=std::move(draft_);const auto version=history_version_;history_.clear();draft_.clear();history_version_=plain_seen_=0;plain_ready_=false;auto exit=ChatExit::main_menu;const auto ok=run_conversation(true,exit);hub_.end(identity_,ports_.now());hub_.end_operator(identity_,ports_.now());if(prior_room)hub_.join(identity_,prior_room);history_=std::move(history);draft_=std::move(draft);history_version_=version;plain_seen_=version;plain_ready_=false;private_return_to_menu_=true;return ok;}
std::optional<bool> ChatCallerSession::page_sysop(){
    if(ports_.ansi()&&!present(menu_text_sgr(7)))return {};
    hub_.safe(identity_,true);const auto request=hub_.page(identity_,ports_.now());
    const auto comment=[&](std::string_view message)->std::optional<bool>{
        if(!present("\r\n"+std::string(message)+"\r\n"))return {};
        const auto answer=line("Leave a Comment to Sysop? [y/N] ",1);if(!answer)return {};return upper(*answer)=="Y";
    };
    if(request.code!=ChatResult::ok)return comment(request.code==ChatResult::busy?"You already have a pending page or chat.":"The Sysop is not available for chat.");
    try {if(ports_.page_accepted)ports_.page_accepted();}catch(...){/* audio is optional */}
    struct Cancel {ChatHub& hub;ChatIdentity id;std::uint64_t request;~Cancel(){hub.cancel_page(id,request);}} cancel{hub_,identity_,request.request};
    if(!present("\r\nSysop paged. Waiting up to 30 seconds; Q cancels.\r\n"))return {};
    for(;;){
        if(!ports_.refresh())return {};
        hub_.tick(ports_.now());
        if(hub_.claim_operator(identity_,ports_.now()))return private_chat()?std::optional<bool>(false):std::nullopt;
        if(!hub_.page_pending(identity_,request.request))return comment("The page was not answered.");
        const auto key=ports_.read();if(key.outcome==TerminalOutcome::timed_out)continue;
        if(!completed(key.outcome))return {};
        if(key.byte&&(*key.byte=='Q'||*key.byte=='q')){if(ports_.accepted_key)ports_.accepted_key(false);return false;}
    }
}
std::optional<ChatRoom> ChatCallerSession::selected_room(){
    const auto rooms=hub_.rooms(identity_);
    for(const auto& room:rooms)if(room.id==selected_room_)return room;
    const auto entry=hub_.configuration().entry;
    for(const auto& room:rooms)if(room.id==entry){selected_room_=room.id;return room;}
    if(!rooms.empty()){selected_room_=rooms.front().id;return rooms.front();}
    selected_room_=0;return {};
}
std::optional<ChatExit> ChatCallerSession::menu_action(MenuAction action){
    if(ports_.ansi()&&!present(menu_text_sgr(7)))return {};
    if(action==MenuAction::chat_text)return compose()?std::optional<ChatExit>{ChatExit::chat_menu}:std::nullopt;
    if(action!=MenuAction::chat_room&&action!=MenuAction::chat_rooms)return ChatExit::chat_menu;
    if(!ports_.refresh())return {};
    const auto current=selected_room();
    if(action==MenuAction::chat_room){
        if(current)return enter(current->id);
        return present("\r\nNo Chat Rooms are available to you.\r\n")?std::optional<ChatExit>{ChatExit::chat_menu}:std::nullopt;
    }
    hub_.safe(identity_,false);struct Restore{ChatHub& hub;ChatIdentity id;~Restore(){hub.safe(id,true);}} restore{hub_,identity_};
    const auto rooms=hub_.rooms(identity_);unsigned count{};
    const auto page=std::max(1U,ports_.rows()>5?ports_.rows()-5:1U);
    for(const auto& room:rooms){
        if(!present("\r\n"+std::to_string(room.id)+" "+room.name+(current&&current->id==room.id?" (current)":"")))return {};
        if(++count%page==0&&count<rooms.size()){const auto more=line("\r\nEnter for more, Q to return: ",1);if(!more)return {};if(upper(*more)=="Q")return ChatExit::chat_menu;}
    }
    const auto selection=ports_.terminal.read_visible({"chat.room.selection","\r\nRoom number (Enter cancels): ",5,EmptyInputPolicy::use_default,{},true});
    if(selection.outcome==TerminalOutcome::cancelled||selection.outcome==TerminalOutcome::invalid||selection.outcome==TerminalOutcome::overlength)
        return ports_.refresh()?std::optional<ChatExit>{ChatExit::chat_menu}:std::nullopt;
    if(!completed(selection.outcome))return {};
    if(selection.value.empty())return ChatExit::chat_menu;
    unsigned n{};for(char c:selection.value){if(c<'0'||c>'9')return ChatExit::chat_menu;n=n*10+static_cast<unsigned>(c-'0');}
    if(!ports_.refresh())return {};
    for(const auto& room:hub_.rooms(identity_))if(room.id==n){selected_room_=room.id;return ChatExit::chat_menu;}
    if(!present("\r\nThat Chat Room is not available. Your selection has not changed.\r\n"))return {};
    return ChatExit::chat_menu;
}
std::optional<ChatExit> ChatCallerSession::enter(ChatRoomId room){const auto joined=hub_.join(identity_,room);if(joined!=ChatResult::ok){if(!present("\r\n"+std::string(chat_result_text(joined))+"\r\n"))return {};return ChatExit::chat_menu;}selected_room_=room;history_.clear();draft_.clear();history_version_=plain_seen_=0;plain_ready_=false;auto exit=ChatExit::chat_menu;const auto ok=run_conversation(false,exit);hub_.leave(identity_);if(!ok)return {};return exit;}
bool ChatCallerSession::run_conversation(bool private_mode,ChatExit& exit){
    hub_.safe(identity_,true);if(!redraw(private_mode))return false;unsigned escape{};std::uint64_t escape_at{};bool discard_lf{};
    for(;;){if(!ports_.refresh())return false;
        if(!events(true,[](std::string){},private_mode))return false;
        if(private_mode&&!hub_.peer(identity_)&&!hub_.operator_active(identity_))return true;
        const auto key=ports_.read();if(key.outcome==TerminalOutcome::timed_out){if(escape==1&&ports_.now()-escape_at>=200){escape=0;if(!help(private_mode)||!redraw(private_mode))return false;}continue;}
        if(!completed(key.outcome))return false;
        if(!key.byte)continue;
        const auto c=*key.byte;
        if(escape){if(escape==1&&(c=='['||c=='O')){escape=2;continue;}if(escape==2){if(c>=0x40&&c<=0x7e)escape=0;continue;}escape=0;if(!help(private_mode)||!redraw(private_mode))return false;continue;}
        if(c==27){escape=1;escape_at=ports_.now();continue;}if(c=='\n'&&discard_lf){discard_lf=false;continue;}discard_lf=false;
        if(c=='\b'||c==127){if(!draft_.empty()){draft_.pop_back();if(ports_.ansi()&&ports_.rows()>=24&&ports_.columns()>=80){if(!present(at(21,3)+std::string(75,' ')+at(21,3)+(draft_.size()>75?draft_.substr(draft_.size()-75):draft_)))return false;}else if(!present("\b \b"))return false;}continue;}
        if(c!='\r'&&c!='\n'){if(c>=32&&c!=0x9b&&draft_.size()<512){draft_+=static_cast<char>(c);if(ports_.ansi()&&ports_.rows()>=24&&ports_.columns()>=80){if(!present(at(21,3)+(draft_.size()>75?draft_.substr(draft_.size()-75):draft_)))return false;}else if(!present(std::string(1,static_cast<char>(c))))return false;}continue;}
        discard_lf=c=='\r';if(ports_.accepted_key)ports_.accepted_key(discard_lf);const auto value=std::move(draft_);draft_.clear();plain_prompt_pending_=true;const auto command=upper(value);
        if(command=="/Q")return true;
        if(!private_mode&&command=="G"){exit=ChatExit::goodbye;return true;}
        if(!private_mode&&command=="/G"){exit=ChatExit::quick_goodbye;return true;}
        if(command=="/?"||command=="/HELP"){if(!help(private_mode))return false;}
        else if(!private_mode&&command=="/W"){if(!online())return false;}
        else if(!private_mode&&(command=="/T"||command=="/WHISPER")){if(!compose(command=="/WHISPER"))return false;}
        else if(!private_mode&&(command=="/L"||command=="/R")){const auto rooms=hub_.rooms(identity_);for(const auto& r:rooms)if(!present("\r\n"+std::to_string(r.id)+" "+r.name))return false;
            if(command=="/R"){const auto selection=line("\r\nRoom number (Enter cancels): ",3);if(!selection)return false;unsigned n{};bool valid=!selection->empty();for(char d:*selection){if(d<'0'||d>'9'){valid=false;break;}n=n*10+static_cast<unsigned>(d-'0');}if(valid){const auto r=hub_.join(identity_,static_cast<ChatRoomId>(n));if(r==ChatResult::ok){selected_room_=static_cast<ChatRoomId>(n);history_.clear();}else append(chat_result_text(r));}}
            else if(!present("\r\nPress Enter to return: ")||ports_.terminal.pause({"chat.rooms.return"})!=TerminalOutcome::completed)return false;}
        else if(value.size()>1&&value[0]=='/'&&value[1]=='/'){const auto result=hub_.say(identity_,value.substr(1));if(result!=ChatResult::ok)append(chat_result_text(result));}
        else if(!value.empty()&&value[0]=='/'){
            if(!private_mode&&command.size()==2&&ports_.door_commands&&ports_.door_command){
                const auto commands=ports_.door_commands();if(!commands)return false;
                const auto found=std::find_if(commands->commands.begin(),commands->commands.end(),[&](const auto& item){return item.key==command[1];});
                if(found!=commands->commands.end()){
                    const auto me=hub_.participant(identity_);if(!me)return false;const auto room=me->room;hub_.leave(identity_);hub_.safe(identity_,false);
                    const auto result=ports_.door_command({MenuId::chat,found->key,commands->revision,commands->door_revision});if(!result)return false;
                    if(hub_.join(identity_,room)!=ChatResult::ok){append("The room is no longer available.");return true;}
                    hub_.safe(identity_,true);append(*result);if(!redraw(false))return false;continue;
                }
            }
            bool found=false;if(!private_mode)for(const auto& e:ports_.emotes())if(command=="/"+e.command||(e.command=="HELP"&&command=="/EMOTE HELP")){found=true;std::optional<ChatParticipant> target;if(e.targeted){target=choose(true);if(!target)break;const auto live=hub_.participant(target->identity);const auto me=hub_.participant(identity_);if(!live||!me||live->room!=me->room)break;}
                const auto me=hub_.participant(identity_);if(!me)return false;const auto text=render_chat_emote(e,me->name,target?target->name:"");if(text)hub_.say(identity_,*text,true);break;}
            if(!found)append("Unknown command. Use /? for Commands.");
        }else if(!value.empty()){const auto r=hub_.say(identity_,value);if(r!=ChatResult::ok)append(chat_result_text(r));}
        const bool modal=command=="/?"||command=="/HELP"||(!private_mode&&(command=="/W"||command=="/T"||command=="/WHISPER"||command=="/R"||command=="/L"));
        if(!redraw(private_mode,modal))return false;
    }
}
}
