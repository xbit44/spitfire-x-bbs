void reader_color_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto settings=options(root);db::SqliteStorage store(settings,policy);
    check(store.start().succeeded()&&store.shutdown().succeeded(),"reader color fixture starts");seed(root/"SFX.DB");check(store.start().succeeded(),"reader color actors ready");
    app::MessageActor sysop{id(1),1,1,1};unsigned operation=28000;
    const auto post=[&](std::string subject,std::string body,std::optional<app::MessageSource> source={}){
        auto r=request(store,sysop,{app::MessageKind::public_echo,{1},{},{}},operation++);r.subject=subject;r.body=body;r.original_source=source;const auto saved=store.commit_message(r);check(bool(saved),"reader color message committed");return saved.value->id;
    };
    const auto first=post("Color first","Base first\n");post("Color second","Base second\n");
    post("Plain artwork",{},app::MessageSource{app::MessageSourceFormat::plain,1,"Plain art |12literal\n"});
    post("ANSI artwork",{},app::MessageSource{app::MessageSourceFormat::ansi_canvas,1,"\x1b[31mAuthored red\n"});
    std::string long_body;for(unsigned i=0;i<21;++i)long_body+="Paged base "+std::to_string(i)+"\n";post("Paged",long_body);
    post("Quoted","xb> "+std::string(90,'x')+"\n");
    post("Unquoted",std::string(35,'a')+" shortword end\n");
    const auto run=[&](bool ansi,std::uint8_t color,unsigned columns,std::int64_t number,std::string_view keys,bool more=false,bool retained=false,bool legacy_pipes=false){
        sfx::test::DeterministicInteractiveByteStreamFake bytes;bytes.script_non_secret_text(keys);
        app::StreamCallerTerminalOptions terminal_options;terminal_options.caller_text_color=&color;app::StreamCallerTerminal terminal(bytes,terminal_options);
        terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        app::MessageQuestionTerminal framed(terminal,[ansi]{return ansi;},[&](std::string_view v){return bytes.write_some(v).bytes_written==v.size();});
        app::MessageReaderPorts ports{store,framed,[&]{return std::optional<app::MessageActor>{sysop};},{},[](std::int64_t){return "2026-10-29 12:00:00";},[]{return true;},10,more,{},[&]{return id(operation++);},[=]{return app::MessageEditorDimensions{ansi,columns,25};},[=]{return color;}};
        if(retained)ports.write=[&](std::string_view value){return bytes.write_some(value).bytes_written==value.size();};
        ports.legacy_pipe_colors=[legacy_pipes]{return legacy_pipes;};
        app::MessageQueryOptions query;query.mode=app::MessageQueryMode::echo;query.echo={1};
        check(app::read_local_messages(ports,query,false,number)==app::MessageReaderOutcome::cancelled,"actual reader returns after scripted navigation and paging");
        return bytes.output();
    };
    const auto verify=[&](const std::string& output,bool ansi,unsigned desired,std::string_view needle,unsigned minimum=1){
        std::string visible;std::vector<unsigned> colors;unsigned color=7,bg=0;bool bright=false;
        for(std::size_t pos=0;pos<output.size();){const auto ch=output[pos++];if(ch!='\x1b'){visible+=ch;colors.push_back(color+(bright?8:0)+(bg<<4));continue;}
            check(ansi&&pos<output.size()&&output[pos++]=='[',"plain output contains no escapes and ANSI uses CSI");const auto begin=pos;while(pos<output.size()&&(output[pos]<'@'||output[pos]>'~'))++pos;check(pos<output.size(),"complete reader CSI");const auto end=pos++;if(output[end]!='m')continue;
            const auto arguments=output.substr(begin,end-begin);std::size_t at=0;do{const auto next=arguments.find(';',at);const auto token=arguments.substr(at,next==arguments.npos?next:next-at);const auto n=token.empty()?0:std::stoi(token);constexpr unsigned map[]={0,4,2,6,1,5,3,7};if(n==0){color=7;bg=0;bright=false;}else if(n==1)bright=true;else if(n==22)bright=false;else if(n>=30&&n<=37)color=map[n-30];else if(n>=40&&n<=47)bg=map[n-40];if(next==arguments.npos)break;at=next+1;}while(at<=arguments.size());
        }
        unsigned count{};
        for(auto at=visible.find(needle);at!=visible.npos;at=visible.find(needle,at+needle.size())){
            ++count;
            if(ansi)for(std::size_t n=0;n<needle.size();++n){
                if(colors[at+n]!=desired)
                    std::cerr<<"Reader color for '"<<needle<<"' at byte "<<n<<": expected "<<desired<<", got "<<colors[at+n]<<'\n';
                check(colors[at+n]==desired,"reader visible span has the requested color");
            }
        }
        if(count<minimum)std::cerr<<"Reader span '"<<needle<<"': expected "<<minimum<<", found "<<count<<'\n';
        check(count>=minimum,"expected reader span appears across refresh boundaries");
    };
    for(const bool ansi:{false,true})for(const std::uint8_t color:{std::uint8_t{6},std::uint8_t{10}}){
        const auto navigation=run(ansi,color,80,1,"N-Q");
        verify(navigation,ansi,7,"Local / Sysop Notices",3);verify(navigation,ansi,7,"Subject: ",3);verify(navigation,ansi,color,"Color first",2);verify(navigation,ansi,color,"Color second");
        for(const auto label:{"To: ","From: ","Date: "})verify(navigation,ansi,7,label,3);
        verify(navigation,ansi,color,"All Callers",3);verify(navigation,ansi,color,"Sysop\r\n",3);verify(navigation,ansi,color,"2026-10-29 12:00:00",3);
        verify(navigation,ansi,color,"Base first",2);verify(navigation,ansi,color,"Base second");verify(navigation,ansi,7," Protect ",3);verify(navigation,ansi,15,"ENT",3);
        const auto plain=run(ansi,color,80,3,"Q");verify(plain,ansi,color,"Plain art |12literal");
        const auto authored=run(ansi,color,80,4,"Q");verify(authored,ansi,4,"Authored red");
        const auto paged=run(ansi,color,40,5,"CCCQ",true);verify(paged,ansi,color,"Paged base 0");verify(paged,ansi,color,"Paged base 10");verify(paged,ansi,color,"Paged base 20");
        const auto full_pause=run(ansi,color,80,5,"CCCQ",true);verify(full_pause,ansi,15,"Pause");verify(full_pause,ansi,15,"ENT");verify(full_pause,ansi,7," continue, ");verify(full_pause,ansi,7,"] Stop, [");verify(full_pause,ansi,color,"Paged base 20");
        const auto quoted=run(ansi,color,40,6,"Q");verify(quoted,ansi,color,"xb> ",4);
        const auto unquoted=run(ansi,color,40,7,"Q");verify(unquoted,ansi,color," shor\r\ntword end");
        const auto sample=run(ansi,color,80,1,"Q");std::ofstream transcript(root/(std::string(ansi?"ansi-":"plain-")+std::to_string(color)+".txt"),std::ios::binary);transcript<<sample;check(bool(transcript),"actual reader output retained without alteration");
    }
    const auto scrolled=run(true,6,80,5,"\x1b[B\x1b[AQ",true,true);
    const auto with_boundary=run(true,6,80,5,"\x1b[A\x1b[B\x1b[AQ",true,true);
    check(scrolled==with_boundary,"Up at the first retained row writes nothing");
    const auto first_clear=scrolled.find("\x1b[2J");
    check(first_clear!=scrolled.npos && scrolled.find("\x1b[2J",first_clear+4)==scrolled.npos,
        "retained arrow scrolling does not clear the screen after the first draw");
    verify(scrolled,true,6,"Paged base 0");
    const auto before=store.quote_source(sysop,first);check(before&&before.value->body.canonical_lf=="Base first\n","reader styling never rewrites stored body");
    for(unsigned number=8;number<=20;++number){
        const auto label=std::to_string(number);
        post("List row "+label,"Selected list body "+label+"\n");
    }
    const auto list=[&](bool ansi,bool start_at_end,std::string_view keys){
        sfx::test::DeterministicInteractiveByteStreamFake bytes;
        bytes.script_non_secret_text(keys);
        app::StreamCallerTerminal terminal(bytes,{});
        terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        const auto write=[&](std::string_view value){return bytes.write_some(value).bytes_written==value.size();};
        app::MessageQuestionTerminal framed(terminal,[ansi]{return ansi;},write);
        app::MessageReaderPorts ports{store,framed,[&]{return std::optional<app::MessageActor>{sysop};},{},
            [](std::int64_t){return "2026-11-23";},[]{return true;},25,false,{},[&]{return id(operation++);},
            [ansi]{return app::MessageEditorDimensions{ansi,80,25};}};
        ports.byte=[&]{return terminal.read_editor_byte();};
        ports.write=write;
        ports.start_at_end=start_at_end;
        ports.adjacent_next=true;
        ports.adjacent_previous=true;
        app::MessageQueryOptions query;
        query.mode=app::MessageQueryMode::echo;
        query.echo={1};
        check(app::read_local_messages(ports,query,true)==app::MessageReaderOutcome::cancelled,
            "message listing returns from scripted keys without crossing conferences");
        return bytes.output();
    };
    std::string down;
    for(unsigned i=0;i<17;++i)down+="\x1b[B";
    check(list(true,false,down+"\rQ").find("Selected list body 18")!=std::string::npos,
        "Down crosses the full page onto the first row of a partial page");
    check(list(true,true,"\x1b[A\rQ").find("Selected list body 17")!=std::string::npos,
        "Up crosses the partial page onto the last row of the previous page");
    check(list(true,true,"\x1b[B\x1b[B\x1b[B\rQ").find("Selected list body 20")!=std::string::npos,
        "Down stops at the last result even if the next conference is available");
    check(list(true,false,"\x1b[ADQ").find("1*All Callers")!=std::string::npos,
        "Up is bounded and D marks the persisted deleted row without confirmation");
    const auto restored=list(false,false,"D 1\r1\rQ");
    check(restored.find("1*All Callers")!=std::string::npos && restored.find("Base first")!=std::string::npos,
        "plain list sees the stored star and D with a row number restores the message");
    const auto catalog=store.message_configuration({true,{}},app::MessageGroupId{1},{});
    check(catalog && catalog.value && !catalog.value->echoes.empty(),"reader can locate pipe fixture echo");
    app::MessageConfigurationChange pipe_policy;
    pipe_policy.operation=id(operation++);
    pipe_policy.action=app::MessageConfigurationAction::echo;
    pipe_policy.expected_configuration_revision=catalog.value->revision;
    pipe_policy.echo=catalog.value->echoes.front();
    check(pipe_policy.echo.id.value==1,"pipe fixture policy targets its own conference");
    pipe_policy.presentation_mask=1;
    pipe_policy.echo.renegade_pipe_colors=true;
    check(bool(store.change_message_configuration({true,{}},pipe_policy)),"reader fixture enables conference pipes");
    const std::string pipe_body="|12Styled |24literal\nNext body row\n";
    const auto upload=post("Pipe upload",{},app::MessageSource{app::MessageSourceFormat::plain,1,pipe_body});
    const auto ordinary=post("Ordinary pipes",pipe_body);
    for(const auto number:{21,22}){
        const auto current=run(true,6,80,number,"Q");
        verify(current,true,12,"Styled |24literal");
        verify(current,true,6,"Next body row");
        check(current.find("\x1b[0;1;5") == current.npos,"new reader mode emits no blink from high pipe codes");
        const auto narrow=run(false,6,40,number,"Q");
        verify(narrow,false,6,"Styled |24literal");
        const auto legacy=run(true,6,80,number,"Q",false,false,true);
        verify(legacy,true,12,"Styled literal");
        verify(legacy,true,12,"Next body row");
        check(legacy.find("\x1b[0;1;5") != legacy.npos,"legacy reader mode still applies high pipe codes");
    }
    const auto stored_upload=store.quote_source(sysop,upload),stored_ordinary=store.quote_source(sysop,ordinary);
    check(stored_upload && stored_upload.value->original_source && stored_upload.value->original_source->bytes==pipe_body &&
        stored_ordinary && stored_ordinary.value->plain_rendition_source && stored_ordinary.value->plain_rendition_source->canonical_lf==pipe_body,
        "both reader modes preserve stored plain source bytes");
    check(store.shutdown().succeeded(),"reader color fixture closes");
}
