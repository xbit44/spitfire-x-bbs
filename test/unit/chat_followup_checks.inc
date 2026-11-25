struct FollowupStream final : IInteractiveSessionByteStream {
    std::string input,output;std::size_t position{};bool escape_idle{};std::function<void()> reading;
    std::function<void(std::string_view)> writing;
    InteractiveStreamReadResult read_some(std::size_t) override {
        if(reading)reading();
        if(position==input.size())return {InteractiveStreamOutcome::disconnected,{}};
        return {InteractiveStreamOutcome::completed,{static_cast<std::uint8_t>(input[position++])}};
    }
    InteractiveStreamWriteResult write_some(std::string_view text) override {
        output+=text;if(writing)writing(text);return {InteractiveStreamOutcome::completed,text.size()};
    }
    InteractiveStreamReadResult read_escape_tail(std::size_t maximum) override {
        return escape_idle?InteractiveStreamReadResult{InteractiveStreamOutcome::timed_out,{}}:read_some(maximum);
    }
};
struct FollowupCaller {
    ChatHub hub;ChatParticipant a=caller(1),b=caller(2);FollowupStream stream;
    StreamCallerTerminal terminal{stream};bool ansi{};std::uint8_t color{2};unsigned refreshed{};
    ChatSessionPorts ports(){
        ChatSessionPorts p{terminal};p.read=[&]{return terminal.read_editor_byte();};
        p.write=[&](std::string_view s){stream.write_some(s);return true;};
        p.now=[]{return std::uint64_t{1000};};p.refresh=[&]{++refreshed;return true;};
        p.ansi=[&]{return ansi;};p.rows=[]{return 24U;};p.columns=[]{return 80U;};
        p.node_count=[]{return std::uint16_t{10};};p.emotes=[]{return std::vector<ChatEmote>{};};
        p.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};p.text_color=[&]{return color;};return p;
    }
    FollowupCaller(){a.name="Alice";b.name="Bob";check(hub.attach(a)&&hub.attach(b),"follow-up callers attached");hub.poll(a.identity,true);hub.poll(b.identity,true);}
};
void followup_checks(){
    for(bool ansi:{false,true})for(const std::string suffix:{"-","1","+","A","anything"}){
        FollowupCaller f;f.ansi=ansi;f.stream.input="Q"+suffix+"\rQ\r";ChatCallerSession c(f.hub,f.a.identity,f.ports());
        check(c.online(),"Who malformed line recovers without disconnect");
        check(f.stream.output.find("Choose 1, 2 or Q.")!=std::string::npos,"Who overlength command reports invalid input");
    }
    for(const std::string input:{"Q","q","\r","Q-","Q1","Q+","QA"}){
        FollowupCaller f;f.stream.input=input;auto p=f.ports();p.hotkeys=[]{return true;};ChatCallerSession c(f.hub,f.a.identity,std::move(p));
        check(c.online(),"Who hotkey quit/Enter returns without waiting for a line");
        check(f.stream.position==1,"Who consumes only its hotkey, leaves later input to caller context");
    }
    for(char action:{'1','2'}){
        FollowupCaller f;f.stream.input=std::string(1,action)+"1\rQ";auto p=f.ports();p.hotkeys=[]{return true;};ChatCallerSession c(f.hub,f.a.identity,std::move(p));
        check(c.online(),"Who text/chat action works without Enter on command");
        check(f.stream.output.find("Node number")!=std::string::npos,"Who hotkey enters the requested action");
    }
    for(bool ansi:{false,true})for(unsigned columns:{20U,40U,80U,132U,200U}){
        const auto width=columns-1;
        const std::vector<std::string> messages={"Short notice",std::string(width-14,'a'),std::string(width-13,'b'),
            "another message this time its going longer to see what it looks like on the client side. this is a really long message.. here we go",
            "Repeated   spaces must stay   in order",std::string(512,'x')};
        for(const auto& message:messages){
            FollowupCaller f;f.ansi=ansi;auto ports=f.ports();ports.columns=[=]{return columns;};
            ChatCallerSession session(f.hub,f.a.identity,std::move(ports));std::vector<std::string> rows;
            check(f.hub.notice(f.a.identity,0,message,false)==ChatResult::ok,"system notice queued unchanged");
            check(session.safe_events([&](std::string row){rows.push_back(std::move(row));}),"notice delivered at safe menu boundary");
            std::string joined;for(const auto& row:rows){check(!row.empty()&&row.size()<=width,"notice leaves safe right margin");joined+=row;}
            check(joined=="Sysop notice: "+message,"wrapping preserves full prefix and every message byte");
            const auto rendered=render_menu_rows([&]{std::vector<MenuTextRow> result;for(const auto& row:rows)result.push_back({MenuTextRole::feedback,row});result.push_back({MenuTextRole::prompt,"Command: "});return result;}(),ansi,{2,false,true},7);
            check(rendered.find("Command: ")==rendered.rfind("Command: "),"wrapped notice leaves one prompt");
            check(ansi?rendered.find(menu_text_sgr(7))==0:rendered.find('\x1b')==rendered.npos,"wrapped system rows use system color and plain fallback");
            check(!ansi||(rendered.size()>=4&&rendered.substr(rendered.size()-4)=="\x1b[0m"),"notice restores ANSI state");
            check(session.safe_events([&](std::string){check(false,"notice cannot replay at next boundary");}),"wrapped delivery drains once");
        }
    }
    for(bool ansi:{false,true}){
        FollowupCaller f;f.ansi=ansi;f.stream.input="1\r1\r2\r1\r\r";ChatCallerSession c(f.hub,f.a.identity,f.ports());
        check(c.online(),"Who self-target attempts return normally");
        check(f.stream.output.find("Alice (you)")!=std::string::npos&&f.stream.output.find("Bob (you)")==std::string::npos,"only current caller marked");
        check(f.stream.output.find("cannot send text or request node chat with yourself")!=std::string::npos,"manual self-selection refused");
        check(f.stream.output.find("Private message (Enter cancels)")==std::string::npos&&f.stream.output.find("Chat requested.")==std::string::npos,"no self text composer or invitation");
        check(f.hub.text(f.a.identity,f.a.identity,"self")!=ChatResult::ok&&f.hub.invite(f.a.identity,f.a.identity,100).code!=ChatResult::ok,"hub rejects self actions independently of UI");
        if(ansi){check(f.stream.output.find('\xdb')==std::string::npos&&f.stream.output.find("\x1b[1;30m\xfe\x1b[0;37m\xfe\x1b[1;37m\xfe")!=std::string::npos,"Who uses small square shading");check(f.stream.output.find("\x1b[1;30m:\x1b[0;37m")!=std::string::npos,"Who colons dark gray");}
        else check(f.stream.output.find('\x1b')==std::string::npos,"plain Who has no escapes");
    }
    for(bool ansi:{false,true})for(std::uint8_t color:{std::uint8_t{2},std::uint8_t{7},std::uint8_t{14}}){
        FollowupCaller f;f.ansi=ansi;f.color=color;f.stream.input="\r";
        check(f.hub.text(f.b.identity,f.a.identity,"Hello Alice")==ChatResult::ok,"received text queued");
        ChatCallerSession c(f.hub,f.a.identity,f.ports());check(c.safe_events([](std::string){}),"received text returns");
        const auto expected=(ansi?menu_text_sgr(7):std::string{})+"\r\nPrivate text from Bob (Node 02):\r\n"+(ansi?menu_text_sgr(color):std::string{})+"Hello Alice\r\n";
        check(f.stream.output.find(expected)!=std::string::npos,"private heading is system-colored and message body retains recipient color");
        if(!ansi)check(f.stream.output.find('\x1b')==std::string::npos,"plain private text has no escapes");
    }
    for(bool ansi:{false,true})for(bool disconnect:{false,true}){
        FollowupCaller f;f.ansi=ansi;f.stream.input="Y\rX\r";bool ended{},waiting{},returned{};unsigned waiting_reads{};
        auto request=f.hub.invite(f.b.identity,f.a.identity,100);check(request.code==ChatResult::ok,"peer invitation queued");
        f.stream.writing=[&](std::string_view s){
            if(!ended&&s.find("Private Chat with Bob")!=s.npos){ended=true;if(disconnect)f.hub.detach(f.b.identity,900);else f.hub.end(f.b.identity,900);}
            if(s.find("Bob has left chat")!=s.npos){waiting=true;check(s.find("<enter>")!=s.npos,"notice includes Enter prompt");}
            if(waiting&&s.find("\x1b[2J")!=s.npos)check(false,"no refresh while awaiting acknowledgement");
        };
        f.stream.reading=[&]{if(waiting){++waiting_reads;check(!returned,"still inside chat until Enter");}};
        ChatCallerSession c(f.hub,f.a.identity,f.ports());std::vector<std::string> notices;
        check(c.safe_events([&](std::string s){notices.push_back(s);}),"node departure acknowledged");returned=true;
        check(ended&&waiting&&waiting_reads==2,"non-Enter does not dismiss notice; Enter does");
        if(ansi)check(f.stream.output.find("\x1b[22;3H<enter>")!=std::string::npos,"ending acknowledgement remains inside frame border");
        check(f.stream.output.find("Chat ended.")==std::string::npos,"no generic ended duplicate in chat");
        check(c.safe_events([&](std::string s){notices.push_back(s);}),"next menu boundary safe");
        check(std::none_of(notices.begin(),notices.end(),[](const auto& s){return s.find("Chat ended")!=s.npos||s.find("has left chat")!=s.npos;}),"no deferred duplicate chat-ending menu notice");
    }
    for(const auto action:{MenuAction::chat_rooms,MenuAction::chat_text}){
        FollowupCaller f;f.stream.input="\r";ChatCallerSession c(f.hub,f.a.identity,f.ports());
        check(c.menu_action(action)==ChatExit::chat_menu&&f.hub.participant(f.a.identity)->room==0,"list or cancel never joins room");
        if(action==MenuAction::chat_rooms)check(f.stream.output.find("Room number (Enter cancels):")!=std::string::npos,"room list shows selection prompt");
    }
    {FollowupCaller f;f.stream.input="/Q\r";ChatCallerSession c(f.hub,f.a.identity,f.ports());
        check(c.menu_action(MenuAction::chat_room)==ChatExit::chat_menu,"room Q returns Chat Menu");
        check(f.stream.output.find("/Q Chat Menu")!=std::string::npos&&f.hub.participant(f.a.identity)->room==0,"room help and membership match return");}
    for(bool ansi:{false,true}){
        FollowupCaller f;f.ansi=ansi;auto config=f.hub.configuration();
        config.rooms.push_back({2,"Games",true,{DoorSecurityMode::minimum,10,false}});
        config.rooms.push_back({3,"Restricted",true,{DoorSecurityMode::minimum,500,false}});
        check(f.hub.change_configuration(config,[]{return true;})==ChatResult::ok,"extra rooms configured");
        ChatCallerSession c(f.hub,f.a.identity,f.ports());
        check(c.selected_room()->name=="Lobby","initial selection is configured entry room");
        f.stream.input="2\r";check(c.menu_action(MenuAction::chat_rooms)==ChatExit::chat_menu,"L changes selection and returns to Chat Menu");
        check(c.selected_room()->name=="Games"&&f.hub.participant(f.a.identity)->room==0,"L does not join selected room");
        f.stream.input+="/Q\r";check(c.menu_action(MenuAction::chat_room)==ChatExit::chat_menu,"E directly enters and exits selected room");
        check(c.selected_room()->id==2&&f.stream.output.find("Room number (Enter cancels):")==f.stream.output.rfind("Room number (Enter cancels):"),"E has no second room chooser; selection retained");
        for(const std::string& invalid:{std::string("\r"),std::string("3\r"),std::string("65535\r"),std::string("abc\r"),std::string("123456\r")}){
            f.stream.input+=invalid;check(c.menu_action(MenuAction::chat_rooms)==ChatExit::chat_menu&&c.selected_room()->id==2,"cancel/invalid/denied selection leaves current room intact");
        }
        config=f.hub.configuration();config.rooms[1].name="Renamed Games";
        check(f.hub.change_configuration(config,[]{return true;})==ChatResult::ok&&c.selected_room()->name=="Renamed Games","footer selection uses live renamed room");
        config=f.hub.configuration();config.rooms[1].access.level=500;
        check(f.hub.change_configuration(config,[]{return true;})==ChatResult::ok&&c.selected_room()->id==1,"revoked selection falls back to eligible entry");
        config=f.hub.configuration();config.rooms[0].access.level=500;
        check(f.hub.change_configuration(config,[]{return true;})==ChatResult::ok&&!c.selected_room(),"no inaccessible room leaked as selection");
        check(c.menu_action(MenuAction::chat_room)==ChatExit::chat_menu,"E with no eligible room returns safely");
    }
    for(bool ansi:{false,true})for(std::uint8_t color:{std::uint8_t{2},std::uint8_t{7},std::uint8_t{14}}){
        FollowupCaller f;f.ansi=ansi;f.color=color;f.stream.input="\r";
        check(f.hub.invite(f.b.identity,f.a.identity,100).code==ChatResult::ok,"default-color invitation queued");
        ChatCallerSession c(f.hub,f.a.identity,f.ports());check(c.safe_events([](std::string){}),"default-color invitation declined without disconnect");
        const auto color_code=ansi?menu_text_sgr(7):std::string{};
        check(f.stream.output.find(color_code+"\r\nBob (Node 02) requests a private chat.\r\n"+system_menu_text("Accept? [y/N] ",ansi))!=std::string::npos,"incoming request and Accept use system colors");
        if(!ansi)check(f.stream.output.find('\x1b')==std::string::npos,"plain invitation contains no escapes");
    }
    for(bool ansi:{false,true})for(bool private_mode:{false,true}){
        for(const std::string& keys:{std::string("abc\r"),std::string(256,'x')+"\r",std::string("\x1b[A\x1bOD\r"),std::string("\x01\r\r"),std::string("\x1b"),std::string("Q"),std::string("\x11")}){
            FollowupCaller f;f.ansi=ansi;f.stream.escape_idle=keys=="\x1b";
            f.stream.input=(private_mode?"Y\r":"")+std::string("/?\r")+keys+"/Q\r";
            if(private_mode)check(f.hub.invite(f.b.identity,f.a.identity,100).code==ChatResult::ok,"Help private invitation queued");
            ChatCallerSession c(f.hub,f.a.identity,f.ports());
            check(private_mode?c.safe_events([](std::string){}):c.enter(1).has_value(),"Help Escape/typing/arrows/control/Q keeps caller session alive");
            check(f.stream.output.find(private_mode?"PRIVATE CHAT COMMANDS":"CHAT COMMANDS")!=std::string::npos,"Help actually opened before input regression");
            check(f.stream.position==f.stream.input.size(),"Help preserves following chat quit command");
            check(f.hub.participant(f.a.identity).has_value()&&f.hub.participant(f.b.identity).has_value(),"both callers remain attached after Help");
            if(!ansi)check(f.stream.output.find('\x1b')==std::string::npos,"plain Help never emits ANSI");
        }
        FollowupCaller closed;closed.ansi=ansi;closed.stream.input=(private_mode?"Y\r":"")+std::string("/?\r");
        if(private_mode)check(closed.hub.invite(closed.b.identity,closed.a.identity,100).code==ChatResult::ok,"Help EOF private invitation queued");
        ChatCallerSession c(closed.hub,closed.a.identity,closed.ports());
        check(private_mode?!c.safe_events([](std::string){}):!c.enter(1).has_value(),"real transport disconnect in Help remains terminal");
    }
}

