namespace {
std::string detail_count(std::uint64_t value){
    auto result=std::to_string(value);
    for(auto at=static_cast<std::ptrdiff_t>(result.size())-3;at>0;at-=3)
        result.insert(static_cast<std::size_t>(at),1,',');
    return result;
}
std::vector<std::string> detail_rows(std::string_view bytes,std::size_t width){
    std::vector<std::string> result;std::string row;
    // CP437 is one terminal cell per byte. Preserve every source space and
    // newline, including artwork; only add hard wrapping at the safe margin.
    for(char c:bytes){
        if(c=='\n'){result.push_back(std::move(row));row.clear();}
        else {if(row.size()==width){result.push_back(std::move(row));row.clear();}row+=c;}
    }
    if(!row.empty() || result.empty() || (!bytes.empty()&&bytes.back()=='\n'))result.push_back(std::move(row));
    return result;
}
struct DetailProgram {
    TerminalDisplayProgram value;
    void add(TerminalDisplayOperationKind kind,std::string bytes={}){
        if(kind==TerminalDisplayOperationKind::cp437_text && bytes.empty())return;
        value.output_byte_count+=kind==TerminalDisplayOperationKind::logical_newline?2:
            kind==TerminalDisplayOperationKind::clear_screen?7:bytes.size();
        value.operations.push_back({kind,std::move(bytes)});
    }
};
}
std::optional<TerminalDisplayProgram> prepare_file_information(const FileDetailView& view,MessageEditorDimensions size){
    const auto width=std::clamp(size.columns,20U,132U)-1;
    const auto height=std::clamp(size.rows,10U,50U);
    std::vector<std::pair<char,std::string>> commands{{'T',"Tag"},{'R',"Read Text"},{'V',"View Archive"}};
    if(view.editable)commands.push_back({'E',"Edit Description"});
    commands.push_back({'Q',"Return"});
    // Keep whole options together when possible. The final row reserves colon,
    // one inert prompt space and the accepted key's echo before the margin.
    std::vector<std::vector<std::pair<char,bool>>> footer(1);std::size_t used{};
    for(const auto& command:commands){
        const auto length=command.second.size()+4;
        if(used && used+2+length>width-3){footer.emplace_back();used=0;}
        const auto cell=[&](char c,bool hotkey=false){
            if(used==width-3){footer.emplace_back();used=0;}
            footer.back().emplace_back(c,hotkey);++used;
        };
        if(used){cell(' ');cell(' ');}
        cell('[');cell(command.first,true);cell(']');cell(' ');
        for(char c:command.second)cell(c);
    }
    const auto status=detail_rows(view.downloadable?"Available for download.":"Listing only; download restricted.",width);
    // Tall screens retain the requested breathing room. Small screens spend
    // available rows on content, with at least one row per continuation.
    const auto footer_rows=static_cast<unsigned>(footer.size());
    const auto status_rows=static_cast<unsigned>(status.size());
    const auto spare=height>footer_rows+status_rows+5?3U:0U;
    const auto reserved=footer_rows+1+status_rows+spare;
    if(reserved>=height)return {};
    const auto capacity=height-reserved;
    std::vector<std::string> content;
    const auto append=[&](std::string value){auto rows=detail_rows(value,width);content.insert(content.end(),rows.begin(),rows.end());};
    const auto inert=[](std::string_view value){auto text=file_terminal_text(value);std::replace(text.begin(),text.end(),'\n',' ');return text;};
    append("File Information");append(std::string(width,'-'));
    auto filename="Filename: "+inert(view.filename);
    const auto bytes="Size: "+file_size_label(view.bytes)+" ("+detail_count(view.bytes)+" bytes)";
    const auto size_column=std::max<std::size_t>(42,filename.size()+2);
    if(size_column+bytes.size()<=width)filename+=std::string(size_column-filename.size(),' ')+bytes;
    else filename+="  "+bytes;
    append(std::move(filename));append("Area:     "+inert(view.area));
    append("Added:    "+inert(view.added)+"    Uploader: "+inert(view.uploader)+"    Downloads: "+detail_count(view.downloads));
    append("");append(file_terminal_text(view.description));
    if(!view.feedback.empty()){append("");append(file_terminal_text(view.feedback));}
    DetailProgram result;result.value.paging_prompt_color=std::uint8_t{7};
    const auto clear=[&]{
        if(size.ansi){result.add(TerminalDisplayOperationKind::clear_screen);result.add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m");}
        else for(unsigned row=0;row<height;++row)result.add(TerminalDisplayOperationKind::logical_newline);
    };
    const auto line=[&](const std::string& text){result.add(TerminalDisplayOperationKind::cp437_text,text);result.add(TerminalDisplayOperationKind::logical_newline);};
    clear();unsigned row{};
    for(const auto& text:content){
        if(row==capacity){
            // The existing forced prompt accepts Enter/Space/S. No new command
            // or bypass is introduced. It remains within the reserved rows.
            result.add(TerminalDisplayOperationKind::paging_enabled);clear();row=0;
        }
        line(text);++row;
    }
    while(row++<capacity)line("");
    for(const auto& text:status)line(text);
    for(unsigned n=0;n<spare;++n)line("");
    line(std::string(width,'-'));
    for(std::size_t i=0;i<footer.size();++i){
        std::string text;
        const auto flush=[&]{result.add(TerminalDisplayOperationKind::cp437_text,std::move(text));text.clear();};
        for(const auto& cell:footer[i]){
            if(cell.second && size.ansi){
                flush();result.add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[1;37;40m");
                result.add(TerminalDisplayOperationKind::cp437_text,std::string(1,cell.first));
                result.add(TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;37;40m");
            }else text+=cell.first;
        }
        flush();
        if(i+1<footer.size())result.add(TerminalDisplayOperationKind::logical_newline);
    }
    result.add(TerminalDisplayOperationKind::cp437_text,":");
    return valid_terminal_display_program(result.value)?std::optional<TerminalDisplayProgram>{std::move(result.value)}:std::nullopt;
}
bool present_file_information(ICallerTerminal& terminal,const FileDetailView& view,MessageEditorDimensions size){
    const auto program=prepare_file_information(view,size);
    return program && terminal.present_display({"files.info.content",&*program,{true,kTerminalMaximumPageLength}})==TerminalOutcome::completed;
}
