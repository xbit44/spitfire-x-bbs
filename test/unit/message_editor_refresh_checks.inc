// Replay emitted ANSI cells and attributes, comparing incremental output with a
// fresh complete frame after real typing/paste, deletion, scrolling and resize.
struct EditorScreenReplay {
    std::vector<std::pair<char,std::string>> cells=std::vector<std::pair<char,std::string>>(80*200,{' ',"0;37;40"});
    unsigned row{},column{};std::string color="0;37;40";
    void feed(std::string_view bytes){
        for(std::size_t i=0;i<bytes.size();){
            if(bytes[i]=='\x1b'){
                check(i+1<bytes.size()&&bytes[i+1]=='[',"known editor escape");const auto end=bytes.find_first_of("Hmh lJ",i+2);check(end!=std::string_view::npos,"bounded editor CSI");
                const auto args=std::string(bytes.substr(i+2,end-i-2));const auto op=bytes[end];
                if(op=='H'){const auto separator=args.find(';');row=std::stoul(args.substr(0,separator))-1;column=std::stoul(args.substr(separator+1))-1;}
                else if(op=='m')color=args;
                else if(op=='J'){check(args=="2","full clear mode");for(auto& cell:cells)cell={' ',color};}
                else check(op=='h'||op=='l',"known editor mode");
                i=end+1;
            }else{check(row<200&&column<80,"editor cursor in bounds");cells[row*80+column]={bytes[i++],color};column=std::min(column+1,79U);}
        }
    }
};
void editor_refresh_checks(){
    app::MessageEditorState editor(200);editor.resize(true,80,24);editor.draft().set_subject("Synthetic paste");app::MessageEditorScreen previous;EditorScreenReplay incremental;
    auto compare=[&](bool may_clear){const auto delta=editor.update_frame("Test","Fixture",previous);check(may_clear||delta.find("\x1b[2J")==std::string::npos,"editing never clears the full screen");incremental.feed(delta);EditorScreenReplay complete;complete.feed(editor.frame("Test","Fixture"));check(incremental.cells==complete.cells&&incremental.row==complete.row&&incremental.column==complete.column,"incremental editor cells, colors and cursor match full frame");};
    compare(true);check(editor.update_frame("Test","Fixture",previous).empty(),"idle editor emits no repaint");
    const std::string text="Pasted text with spaces and punctuation. ";
    for(unsigned i=0;i<2200;++i){check(editor.apply({app::MessageEditorKey::text,static_cast<unsigned char>(text[i%text.size()])})==app::MessageValidation::valid,"paste byte accepted");compare(false);}
    for(unsigned i=0;i<100;++i){editor.apply({app::MessageEditorKey::backspace});compare(false);}
    for(const auto key:{app::MessageEditorKey::home,app::MessageEditorKey::page_up,app::MessageEditorKey::erase,app::MessageEditorKey::insert_mode,app::MessageEditorKey::page_down,app::MessageEditorKey::end}){editor.apply({key});compare(false);}
    editor.resize(true,80,40);compare(true);editor.resize(true,80,24);compare(true);
    previous.clear();compare(true); // return from Help or another full-screen owner
}
