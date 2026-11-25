struct HeaderListState {
    std::vector<MessageQueryCursor> pages{{}};
    std::size_t page{}, selected{}, rows_per_page{};
    std::optional<MessageQueryCursor> next;
    std::vector<MessageHeader> entries;
};

std::string list_field(std::string_view value,std::size_t width){
    if(value.size()<=width)return std::string(value)+std::string(width-value.size(),' ');
    return width<=3?std::string(value.substr(0,width)):std::string(value.substr(0,width-3))+"...";
}

bool load_header_page(const MessageReaderPorts& p,const MessageQueryContext& context,HeaderListState& state){
    state.entries.clear();
    state.next=state.pages[state.page];
    while(state.next && state.entries.size()<state.rows_per_page){
        if(p.safe_boundary && !p.safe_boundary())return false;
        const auto actor=p.actor();
        if(!actor)return false;
        auto result=p.storage.query_message_list_headers(*actor,context,*state.next,state.rows_per_page-state.entries.size());
        if(!result || !result.value)return false;
        state.next=result.value->next;
        for(auto& entry:result.value->entries)state.entries.push_back(std::move(entry));
    }
    return true;
}

Chosen list_headers(const MessageReaderPorts& p,const MessageQueryContext& context,HeaderListState& state){
    const auto geometry=[&]{return p.dimensions?p.dimensions():MessageEditorDimensions{};};
    const auto page_size=[&](const MessageEditorDimensions& size){
        const auto rows=std::min<std::size_t>(size.rows>1?size.rows-1:1,p.page_length);
        return std::clamp<std::size_t>(rows>7?rows-7:1,1,18);
    };
    auto dimensions=geometry();
    const bool first_visit=!state.rows_per_page;
    if(first_visit)state.rows_per_page=page_size(dimensions);
    if(!load_header_page(p,context,state))return {true,{}};
    if(first_visit && p.start_at_end){
        while(state.next){
            state.pages.push_back(*state.next);
            ++state.page;
            if(!load_header_page(p,context,state))return {true,{}};
            if(state.entries.empty()){
                --state.page;
                state.pages.pop_back();
                if(!load_header_page(p,context,state))return {true,{}};
                state.next.reset();
                break;
            }
        }
    }
    MessageEditorKeyDecoder decoder;
    std::vector<std::string> drawn;
    std::string digits;
    std::string cursor;
    std::string notice;
    bool invalid_number{};
    const auto step=[&](int direction,bool adjacent=true)->std::optional<Chosen>{
        if(direction<0){
            if(!state.page){
                if(adjacent && p.adjacent_previous)return Chosen{false,{},false,MessageReaderOutcome::previous_conference};
                return {};
            }
            --state.page;
        }else{
            if(!state.next){
                if(adjacent && p.adjacent_next)return Chosen{false,{},false,MessageReaderOutcome::next_conference};
                return {};
            }
            if(state.page+1==state.pages.size())state.pages.push_back(*state.next);
            ++state.page;
        }
        if(!load_header_page(p,context,state))return Chosen{true,{}};
        if(direction>0 && state.entries.empty()){
            --state.page;
            state.pages.resize(state.page+1);
            if(!load_header_page(p,context,state))return Chosen{true,{}};
            state.next.reset();
            if(adjacent && p.adjacent_next)return Chosen{false,{},false,MessageReaderOutcome::next_conference};
        }
        return {};
    };
    for(;;){
        if(p.safe_boundary && !p.safe_boundary())return {true,{}};
        const auto latest=geometry();
        if(latest.columns!=dimensions.columns || latest.rows!=dimensions.rows || latest.ansi!=dimensions.ansi){
            dimensions=latest;
            drawn.clear();cursor.clear();
        }
        const auto wanted=page_size(dimensions);
        if(wanted!=state.rows_per_page){
            const auto index=state.page*state.rows_per_page+state.selected;
            state.pages={{}};state.page=0;state.rows_per_page=wanted;
            if(!load_header_page(p,context,state))return {true,{}};
            while(state.next && state.page<index/wanted){
                const auto previous=state.page;
                if(const auto result=step(1,false);result && result->failed)return *result;
                if(previous==state.page)break;
            }
            state.selected=index-state.page*wanted;
            digits.clear();invalid_number=false;
        }
        const auto columns=std::clamp<std::size_t>(dimensions.columns,2,4096)-1;
        const bool lightbar=dimensions.ansi && columns>=63 && dimensions.rows>=12 && p.byte && p.write &&
            (!p.lightbars_enabled || p.lightbars_enabled());
        if(state.entries.empty()){
            if(p.terminal.clear_screen()!=TerminalOutcome::completed)return {true,{}};
            if(!say(p,"No available messages."))return {true,{}};
            if(p.adjacent_next)return {false,{},false,MessageReaderOutcome::next_conference};
            const auto ack=p.terminal.pause({"message.headers.empty.ack"});
            return {failed_input(ack),{},ack==TerminalOutcome::cancelled};
        }
        state.selected=std::min(state.selected,state.entries.size()-1);
        const auto base=state.page*state.rows_per_page;
        const auto number_width=std::max<std::size_t>(3,std::to_string(base+state.entries.size()).size());
        const std::size_t date_width=10;
        const std::size_t name_width=columns>=63?std::min<std::size_t>(16,(columns-27)/3):8;
        const auto subject_width=std::max<std::size_t>(3,columns>number_width+2*name_width+date_width+4?
            columns-number_width-2*name_width-date_width-4:3);
        const auto row=[&](std::string no,std::string_view to,std::string_view from,std::string_view subject,std::string_view date,bool deleted=false){
            const char marker=deleted?'*':' ';
            if(columns<number_width+2*name_width+date_width+7)
                return list_field(no+marker+std::string(subject),columns);
            return list_field(std::string(number_width>no.size()?number_width-no.size():0,' ')+no+marker+
                list_field(to,name_width)+" "+list_field(from,name_width)+" "+
                list_field(subject,subject_width)+" "+list_field(date,date_width),columns);
        };
        const auto& first=state.entries.front();
        auto title=first.target.kind==MessageKind::public_echo && context.options.mode!=MessageQueryMode::correspondence?
            "Message Conference #"+std::to_string(first.conference_number)+" - "+first.group_name+": "+first.echo_name:
            "Message Headers";
        if(!p.list_summary.empty() && columns>p.list_summary.size()+3){
            const auto available=columns-p.list_summary.size()-3;
            if(title.size()>available)title=list_field(title,available);
            title+=" | "+p.list_summary;
        }
        const auto& highlighted=state.entries[state.selected];
        const bool page_has_delete=std::any_of(state.entries.begin(),state.entries.end(),[](const auto& h){return h.list_delete_allowed;});
        const std::string_view delete_label=lightbar?(highlighted.list_delete_allowed?
            (highlighted.list_deleted?"[D] Undelete, ":"[D] Delete, "):""):
            (page_has_delete?"[D] Delete/Undelete + number, ":"");
        std::string footer=std::string(lightbar?"Arrows / Enter   PgUp/PgDn   ":"")+"Number, [P] Previous, [N] Next, "+
            std::string(delete_label)+(p.allow_skip?"[K] Skip, ":"")+"[Q] Quit";
        if(footer.size()>columns){
            footer=std::string(lightbar?"Arrows/Enter PgUp/PgDn ":"")+"[P]/[N] Page "+std::string(delete_label)+
                (p.allow_skip?"[K] Skip ":"")+"[Q] Quit";
            if(footer.size()>columns && !delete_label.empty())
                footer=std::string(lightbar?"Arrows/Enter PgUp/PgDn ":"")+"[P]/[N] "+std::string(delete_label)+
                    (p.allow_skip?"[K] ":"")+"[Q] Quit";
            if(footer.size()>columns)footer=std::string("[P]/[N] ")+(p.allow_skip?"[K] ":"")+"[Q] Quit";
        }
        std::vector<std::string> lines{list_field(title,columns),row("#","To","From","Subject","Date"),std::string(columns,'-')};
        for(std::size_t i=0;i<state.entries.size();++i){
            const auto& header=state.entries[i];
            const auto date=p.date?p.date(header.identity.utc_seconds):std::to_string(header.identity.utc_seconds);
            lines.push_back(row(std::to_string(base+i+1),header.to,header.from,header.subject,date.substr(0,date_width),header.list_deleted));
        }
        std::optional<std::size_t> number;
        int page_direction{};
        bool cross_row{},toggle_delete{};
        unsigned input_row{};
        const auto leave=[&](Chosen result){
            if(input_row && !p.write("\x1b["+std::to_string(input_row)+";1H\x1b[0;37;40m\r\n"))return Chosen{true,{}};
            return result;
        };
        if(lightbar){
            lines.resize(3+state.rows_per_page,std::string(columns,' '));
            lines.push_back(std::string(columns,'-'));
            lines.push_back(list_field(footer,columns));
            auto prompt=!notice.empty()?notice+" Number: ":
                invalid_number?"Choose "+std::to_string(base+1)+"-"+std::to_string(base+state.entries.size())+". Number: ":
                "Page "+std::to_string(state.page+1)+"   Number: ";
            if(prompt.size()+digits.size()>=columns)prompt="Number: ";
            lines.push_back(list_field(prompt+digits,columns));
            input_row=static_cast<unsigned>(lines.size());
            for(std::size_t i=0;i<lines.size();++i){
                const auto color=i==2 || i==3+state.rows_per_page?menu_text_sgr(8):
                    i==3+state.selected?menu_text_sgr(0,7):menu_text_sgr(7);
                lines[i]=i+2==lines.size()?system_menu_text(lines[i],true):color+lines[i];
            }
            std::string update=drawn.empty()?"\x1b[0;37;40m\x1b[2J":"";
            for(std::size_t i=0;i<lines.size();++i)
                if(i>=drawn.size() || drawn[i]!=lines[i])update+="\x1b["+std::to_string(i+1)+";1H"+lines[i];
            const auto next_cursor="\x1b["+std::to_string(input_row)+";"+std::to_string(prompt.size()+digits.size()+1)+"H";
            if((!update.empty() || cursor!=next_cursor) && !p.write(update+"\x1b[0;37;40m"+next_cursor))return {true,{}};
            cursor=next_cursor;
            drawn=std::move(lines);
            const auto byte=p.byte();
            const auto input=byte.outcome==TerminalOutcome::timed_out?decoder.idle():
                byte.byte?decoder.feed(*byte.byte):std::optional<MessageEditorInput>{};
            if(byte.outcome!=TerminalOutcome::timed_out && !byte.byte)return {true,{}};
            if(!input)continue;
            if(input->command==MessageEditorCommand::actions)return leave({false,{},true});
            if(!input->event)continue;
            notice.clear();
            const auto key=*input->event;
            switch(key.key){
            case MessageEditorKey::up:
                if(state.selected)--state.selected;
                else if(state.page){page_direction=-1;cross_row=true;}
                break;
            case MessageEditorKey::down:
                if(state.selected+1<state.entries.size())++state.selected;
                else if(state.next){page_direction=1;cross_row=true;}
                break;
            case MessageEditorKey::page_up: page_direction=-1;break;
            case MessageEditorKey::page_down: page_direction=1;break;
            case MessageEditorKey::home: state.selected=0;break;
            case MessageEditorKey::end: state.selected=state.entries.size()-1;break;
            case MessageEditorKey::backspace: if(!digits.empty())digits.pop_back();break;
            case MessageEditorKey::enter:
                if(p.accepted_key)p.accepted_key(key.byte=='\r');
                number=digits.empty()?base+state.selected+1:positive(digits).value_or(0);
                digits.clear();
                break;
            case MessageEditorKey::text:
                if(key.byte>='0' && key.byte<='9' && digits.size()<18)digits+=static_cast<char>(key.byte);
                if(key.byte=='Q' || key.byte=='q')return leave({false,{},true});
                if(p.allow_skip && (key.byte=='K' || key.byte=='k'))return leave({false,{},false,MessageReaderOutcome::skipped});
                if(key.byte=='N' || key.byte=='n')page_direction=1;
                if(key.byte=='P' || key.byte=='p')page_direction=-1;
                if((key.byte=='D' || key.byte=='d') && digits.empty()){
                    toggle_delete=true;
                    number=base+state.selected+1;
                }
                break;
            default:break;
            }
        }else{
            drawn.clear();cursor.clear();
            if(p.terminal.clear_screen()!=TerminalOutcome::completed)return {true,{}};
            for(const auto& line:lines)if(!say(p,line))return {true,{}};
            if(invalid_number && !say(p,"Choose a listed message number."))return {true,{}};
            if(!notice.empty() && !say(p,notice))return {true,{}};
            if(page_has_delete && footer.find("[D]")==std::string::npos && !say(p,"[D] Delete/Undelete + number"))return {true,{}};
            if(!say(p,footer))return {true,{}};
            const auto choice=p.terminal.read_visible({"message.headers.choose","Page "+std::to_string(state.page+1)+"   Number: ",18,EmptyInputPolicy::use_default,{},true});
            if(quit(choice))return {false,{},true};
            if(failed_input(choice.outcome))return {true,{}};
            notice.clear();
            if(choice.value=="N" || choice.value=="n")page_direction=1;
            else if(choice.value=="P" || choice.value=="p")page_direction=-1;
            else if(p.allow_skip && (choice.value=="K" || choice.value=="k"))return {false,{},false,MessageReaderOutcome::skipped};
            else if(choice.value.size()>2 && (choice.value[0]=='D' || choice.value[0]=='d') && choice.value[1]==' '){
                toggle_delete=true;
                number=positive(std::string_view(choice.value).substr(2)).value_or(0);
            }
            else number=positive(choice.value).value_or(0);
        }
        if(page_direction){
            digits.clear();invalid_number=false;
            const auto previous=state.page;
            if(const auto result=step(page_direction,!cross_row))return leave(*result);
            if(cross_row && state.page!=previous && !state.entries.empty())
                state.selected=page_direction<0?state.entries.size()-1:0;
            continue;
        }
        if(!number)continue;
        if(*number<=base || *number-base>state.entries.size()){invalid_number=true;continue;}
        state.selected=*number-base-1;
        invalid_number=false;
        auto& header=state.entries[state.selected];
        if(toggle_delete){
            if(!header.list_delete_allowed){notice="Delete unavailable.";continue;}
            const auto actor=p.actor();
            if(!actor)return {true,{}};
            const bool deleted=!header.list_deleted;
            const auto changed=p.storage.set_message_list_deleted(*actor,context,header.identity.id,header.list_deleted,deleted,header.list_delete_scope);
            if(changed.succeeded()){
                header.list_deleted=deleted;
                notice=deleted?"Message deleted.":"Message restored.";
            }else{
                notice="Message change unconfirmed.";
                if(!load_header_page(p,context,state))return {true,{}};
            }
            continue;
        }
        if(header.list_deleted){notice="Message deleted. Use D to restore.";continue;}
        return {false,header,false,MessageReaderOutcome::completed,input_row};
    }
}
