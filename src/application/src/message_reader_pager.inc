// Buffer one opened message as safe physical rows. Scrolling never reopens
// storage or advances read pointers. ANSI repaints use Home, not Clear Screen.
class ReaderPager {
public:
    explicit ReaderPager(const MessageReaderPorts& ports):p_(ports),nonstop_(!ports.more){
        fit(p_.dimensions?p_.dimensions():MessageEditorDimensions{});
    }
    TerminalOutcome show(const TerminalDisplayProgram& source,bool preserve_quotes=false){
        sources_.emplace_back(source,preserve_quotes);
        return buffer(source,preserve_quotes);
    }
    TerminalOutcome resize(){
        const auto d=p_.dimensions?p_.dimensions():MessageEditorDimensions{};
        if(d.columns==dimensions_.columns && d.rows==dimensions_.rows && d.ansi==dimensions_.ansi)return TerminalOutcome::completed;
        const auto previous_width=width_;
        fit(d);
        if(width_!=previous_width){
            rows_.clear();style_.clear();
            for(const auto& [source,quotes]:sources_){
                const auto out=buffer(source,quotes);
                if(out!=TerminalOutcome::completed)return out;
            }
        }
        top_=std::min(top_,rows_.empty()?0:rows_.size()-1);
        furthest_top_=top_;
        invalidate();
        return render();
    }
    void invalidate(){drawn_=false;pause_visible_=false;painted_.clear();}
    void notice(std::string text){notice_=std::move(text);}
    TerminalOutcome command_footer(std::size_t rows){
        const auto resized=resize();
        if(resized!=TerminalOutcome::completed)return resized;
        footer_rows(rows+(notice_.empty()?0U:1U));
        const auto capacity=height_-footer_rows_;
        if(!drawn_ || shown_>capacity){
            top_+=shown_>capacity?shown_-capacity:0;
            top_=std::min(top_,rows_.empty()?0:rows_.size()-1);
            const auto out=render(capacity);
            if(out!=TerminalOutcome::completed)return out;
        }
        if(ansi_){
            const auto positioned=position_cursor(shown_+1,true);
            if(positioned!=TerminalOutcome::completed)return positioned;
        }
        if(!notice_.empty()){
            auto text=std::move(notice_);
            notice_.clear();
            if(text.size()>=width_)text.resize(width_-1);
            if(!say(p_,std::move(text)))return TerminalOutcome::output_failure;
        }
        return TerminalOutcome::completed;
    }
private:
    void fit(MessageEditorDimensions d){
        dimensions_=d;ansi_=d.ansi;
        width_=std::clamp<std::size_t>(d.columns,2,4096);
        const auto height=height_=std::max<std::size_t>(3,std::min<std::size_t>(d.rows,std::clamp<std::size_t>(p_.page_length,kTerminalMinimumPageLength,kTerminalMaximumPageLength)));
        pause_prompt_="<Pause> <ENT> continue, [S] Stop, [N] nonstop, ";
        if(p_.allow_skip)pause_prompt_+="[K] Skip, ";
        pause_prompt_+="[Q] Quit ";
        if(pause_prompt_.size()>=width_){
            pause_prompt_=p_.allow_skip?"<ENT> More [S] Stop [N] All [K] Skip [Q] Quit ":"<ENT> More [S] Stop [N] All [Q] Quit ";
            if(pause_prompt_.size()>=width_)pause_prompt_=p_.allow_skip?"Enter S N K Q ":"Enter S N Q ";
        }
        const auto prompt=pause_prompt_.size()+(ansi_?2U:1U);
        limit_=height-std::min(height-1,(prompt+width_-1)/width_);
        footer_rows(footer_rows_);
    }
    TerminalOutcome buffer(const TerminalDisplayProgram& source,bool preserve_quotes=false){
        if(preserve_quotes)return quoted(source);
        TerminalDisplayProgram row;std::size_t column{};
        const auto add=[&](TerminalDisplayOperationKind kind,std::string bytes={}){
            row.output_byte_count+=kind==TerminalDisplayOperationKind::logical_newline?2:bytes.size();row.operations.push_back({kind,std::move(bytes)});
        };
        if(!style_.empty())add(TerminalDisplayOperationKind::ansi_sequence,style_);
        const auto flush=[&](){
            add(TerminalDisplayOperationKind::logical_newline);rows_.push_back(std::move(row));row={};column=0;
            if(!style_.empty())add(TerminalDisplayOperationKind::ansi_sequence,style_);
        };
        for(const auto& op:source.operations){
            if(op.kind==TerminalDisplayOperationKind::ansi_sequence){style_=op.bytes;add(op.kind,op.bytes);}
            else if(op.kind==TerminalDisplayOperationKind::logical_newline)flush();
            else if(op.kind==TerminalDisplayOperationKind::cp437_text){
                for(std::size_t at=0;at<op.bytes.size();){if(column==width_)flush();
                    const auto count=std::min(width_-column,op.bytes.size()-at);add(op.kind,op.bytes.substr(at,count));at+=count;column+=count;}
            }else return TerminalOutcome::invalid;
        }
        if(column)flush();
        return TerminalOutcome::completed;
    }
public:
    TerminalOutcome run(){
        if(nonstop_ && rows_.size()<=height_-footer_rows_)return render(height_-footer_rows_);
        if(nonstop_){if(p_.terminal.clear_screen()!=TerminalOutcome::completed)return TerminalOutcome::output_failure;const auto out=emit(0,rows_.size());top_=rows_.size()>limit_?rows_.size()-limit_:0;shown_=rows_.size()-top_;return out==TerminalOutcome::completed?footer():out;}
        const auto out=render();return out==TerminalOutcome::completed?wait():out;
    }
    void footer_rows(std::size_t rows){footer_rows_=std::clamp<std::size_t>(rows,1,height_-1);}
    TerminalOutcome wait(bool reviewing=false){
        while(reviewing || top_+limit_<rows_.size()){
            if(p_.safe_boundary&&!p_.safe_boundary())return TerminalOutcome::output_failure;
            const auto resized=resize();
            if(resized!=TerminalOutcome::completed)return resized;
            const bool retained_prompt=ansi_ && pause_visible_;
            if(ansi_ && !retained_prompt){
                const auto placed=position_cursor(limit_+1,true);
                if(placed!=TerminalOutcome::completed)return placed;
                pause_visible_=true;
            }
            std::vector<char> keys{'S','N','C','Q'};
            if(p_.allow_skip)keys.push_back('K');
            const auto key=p_.terminal.read_key({"message.reader.more",pause_prompt_,keys,'C',false,true,true,!ansi_,retained_prompt});
            if(key.navigation==TerminalNavigationKey::left||key.navigation==TerminalNavigationKey::right){message_command=key.navigation==TerminalNavigationKey::left?'-':'N';return finish_pause();}
            if(key.navigation==TerminalNavigationKey::up||key.navigation==TerminalNavigationKey::down){const auto out=scroll(key.navigation==TerminalNavigationKey::up?-1:1);if(out!=TerminalOutcome::completed)return out;reviewing=true;continue;}
            if(key.outcome==TerminalOutcome::cancelled || key.key=='Q' || key.key=='K'){
                message_command=key.key=='K'?'K':'Q';
                return finish_pause();
            }
            if(key.key=='S'){const auto out=footer();return out==TerminalOutcome::completed?TerminalOutcome::cancelled:out;}
            if(!key.has_key())return key.outcome;
            if(key.key=='N'){const auto cleared=finish_pause();if(cleared!=TerminalOutcome::completed)return cleared;nonstop_=true;const auto out=emit(std::min(top_+limit_,rows_.size()),rows_.size());top_=rows_.size()>limit_?rows_.size()-limit_:0;shown_=rows_.size()-top_;invalidate();return out==TerminalOutcome::completed?footer():out;}
            if(top_+limit_>=rows_.size())return footer();
            reviewing=false;
            top_+=limit_;const auto out=render();if(out!=TerminalOutcome::completed)return out;
        }return footer();
    }
    TerminalOutcome scroll(int direction){
        const auto resized=resize();
        if(resized!=TerminalOutcome::completed)return resized;
        const auto maximum=std::max(furthest_top_,rows_.size()>limit_?rows_.size()-limit_:0);
        const auto previous=top_;
        if(direction<0){if(top_)--top_;}else if(top_<maximum)++top_;
        if(previous==top_)return TerminalOutcome::completed;
        return render();
    }
    std::optional<char> message_command;
private:
    TerminalOutcome position_cursor(std::size_t row,bool erase_tail){
        TerminalDisplayProgram program;
        auto cursor="\x1b["+std::to_string(row)+";1H";
        program.output_byte_count=cursor.size();
        program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,std::move(cursor)});
        if(erase_tail){
            program.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,"\x1b[J"});
            program.output_byte_count+=3;
        }
        return p_.terminal.present_display({"message.reader.viewport",&program});
    }
    TerminalOutcome finish_pause(){
        if(!pause_visible_)return TerminalOutcome::completed;
        pause_visible_=false;
        return position_cursor(shown_+1,true);
    }
    TerminalOutcome footer(){
        const auto capacity=height_-footer_rows_;
        if(shown_>capacity){top_+=shown_-capacity;return render(capacity);}
        return finish_pause();
    }
    TerminalOutcome emit(std::size_t first,std::size_t last){
        for(auto i=first;i<last;++i){auto row=rows_[i];if(ansi_){row.operations.insert(row.operations.begin(),{TerminalDisplayOperationKind::ansi_sequence,"\x1b[2K"});row.operations.insert(row.operations.begin(),{TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m"});row.output_byte_count+=14;}const auto out=p_.terminal.present_display({"message.safe.body",&row,{}});if(out!=TerminalOutcome::completed)return out;}return TerminalOutcome::completed;
    }
    TerminalOutcome render(std::size_t capacity=0){
        if(!capacity)capacity=limit_;
        shown_=std::min(capacity,rows_.size()-top_);
        if(ansi_ && p_.write)return render_ansi();
        pause_visible_=false;
        furthest_top_=std::max(furthest_top_,top_);
        if(!drawn_ || !ansi_){if(p_.terminal.clear_screen()!=TerminalOutcome::completed)return TerminalOutcome::output_failure;}
        else {TerminalDisplayProgram home;home.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,"\x1b[H"});home.output_byte_count=3;if(p_.terminal.present_display({"message.reader.viewport",&home})!=TerminalOutcome::completed)return TerminalOutcome::output_failure;}
        drawn_=true;
        const auto out=emit(top_,top_+shown_);if(out!=TerminalOutcome::completed)return out;
        if(ansi_){TerminalDisplayProgram tail;tail.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m"});tail.operations.push_back({TerminalDisplayOperationKind::ansi_sequence,"\x1b[J"});tail.output_byte_count=13;return p_.terminal.present_display({"message.reader.viewport",&tail});}
        return TerminalOutcome::completed;
    }
    TerminalOutcome render_ansi(){
        const auto old_rows=painted_.size();
        std::vector<std::string> frame;
        frame.reserve(shown_);
        for(std::size_t i=0;i<shown_;++i){
            std::string line="\x1b[0;37;40m";
            const auto& row=rows_[top_+i];
            if(!valid_terminal_display_program(row))return TerminalOutcome::invalid;
            std::size_t cells{};
            for(const auto& op:row.operations){
                if(op.kind!=TerminalDisplayOperationKind::logical_newline)line+=op.bytes;
                if(op.kind==TerminalDisplayOperationKind::cp437_text)cells+=op.bytes.size();
            }
            if(cells<width_)line+="\x1b[0;37;40m\x1b[K";
            frame.push_back(std::move(line));
        }
        if(drawn_ && frame==painted_ && !pause_visible_)return TerminalOutcome::completed;
        std::string update=drawn_?"":"\x1b[0;37;40m\x1b[2J";
        for(std::size_t i=0;i<std::max(old_rows,frame.size());++i){
            if(i<old_rows && i<frame.size() && painted_[i]==frame[i])continue;
            update+="\x1b["+std::to_string(i+1)+";1H";
            update+=i<frame.size()?frame[i]:"\x1b[0;37;40m\x1b[K";
        }
        update+="\x1b["+std::to_string(shown_+1)+";1H";
        if(pause_visible_ || shown_<old_rows)update+="\x1b[J";
        if(!p_.write(update))return TerminalOutcome::output_failure;
        drawn_=true;
        pause_visible_=false;
        painted_=std::move(frame);
        furthest_top_=std::max(furthest_top_,top_);
        return TerminalOutcome::completed;
    }
    TerminalOutcome quoted(const TerminalDisplayProgram& source){
        struct Cell{char byte;std::string style;};
        std::vector<Cell> cells;std::string visible,current_style=style_;
        const auto line=[&]()->TerminalOutcome{
            auto parts=wrap_message_line(visible,width_);
            // Preserve the original physical folding of unquoted text/tables
            // and artwork. Only a recognized overflowing quote gains prefixes.
            if(std::none_of(parts.begin(),parts.end(),[](const auto& part){return part.inserted_prefix!=0;}))parts={{visible,0,visible.size(),0}};
            for(const auto& part:parts){
                TerminalDisplayProgram program;std::string emitted;
                const auto add=[&](TerminalDisplayOperationKind kind,std::string bytes={}){program.output_byte_count+=kind==TerminalDisplayOperationKind::logical_newline?2:bytes.size();program.operations.push_back({kind,std::move(bytes)});};
                std::string run;
                for(std::size_t n=0;n<part.text.size();++n){
                    const auto at=n<part.inserted_prefix?n:part.source_begin+n-part.inserted_prefix;
                    const auto& cell=cells[at];
                    if(cell.style!=emitted){if(!run.empty()){add(TerminalDisplayOperationKind::cp437_text,std::move(run));run.clear();}if(!cell.style.empty())add(TerminalDisplayOperationKind::ansi_sequence,cell.style);emitted=cell.style;}
                    run+=cell.byte;
                }
                if(!run.empty())add(TerminalDisplayOperationKind::cp437_text,std::move(run));
                if(!current_style.empty())add(TerminalDisplayOperationKind::ansi_sequence,current_style);
                add(TerminalDisplayOperationKind::logical_newline);
                const auto result=buffer(program);if(result!=TerminalOutcome::completed)return result;
            }
            cells.clear();visible.clear();return TerminalOutcome::completed;
        };
        for(const auto& op:source.operations){
            if(op.kind==TerminalDisplayOperationKind::ansi_sequence)current_style=op.bytes;
            else if(op.kind==TerminalDisplayOperationKind::cp437_text)for(const auto byte:op.bytes){cells.push_back({byte,current_style});visible+=byte;}
            else if(op.kind==TerminalDisplayOperationKind::logical_newline){const auto result=line();if(result!=TerminalOutcome::completed)return result;}
            else return TerminalOutcome::invalid;
        }
        if(!cells.empty())return line();
        return TerminalOutcome::completed;
    }
    const MessageReaderPorts& p_;std::vector<TerminalDisplayProgram> rows_;
    std::vector<std::pair<TerminalDisplayProgram,bool>> sources_;
    MessageEditorDimensions dimensions_;
    std::size_t width_{80},height_{24},limit_{23},top_{},furthest_top_{},shown_{},footer_rows_{2};
    bool nonstop_{},ansi_{},drawn_{},pause_visible_{};
    std::string pause_prompt_,notice_;
    std::vector<std::string> painted_;
    std::string style_;
};
