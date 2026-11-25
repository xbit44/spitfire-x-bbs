#include "sfx/application/file_presentation.hpp"
#include "sfx/application/qwk.hpp"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>

namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool value,const char* detail){++checks;if(!value)throw std::runtime_error(detail);}
struct Terminal final:app::ICallerTerminal {
    std::deque<app::TerminalInputResult> inputs;std::string output;bool fail{};unsigned displays{};
    app::TerminalOutcome set_text_style(app::TerminalTextStyle)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome clear_screen()override{return app::TerminalOutcome::completed;}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& r)override{
        check(app::valid_terminal_input_request(r),"valid native text prompt");check(r.prompt.find('\x1b')==std::string::npos,"no ANSI in inert input prompt");
        if(inputs.empty())return {app::TerminalOutcome::disconnected,{}};
        const auto v=inputs.front();inputs.pop_front();return v;
    }
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{return {};}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest&)override{return {};}
    app::TerminalOutcome present(const app::TerminalPresentationRequest&)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& r)override{
        ++displays;check(app::valid_terminal_display_presentation_request(r),"valid inert paged display program");
        for(const auto& op:r.program->operations){if(op.kind==app::TerminalDisplayOperationKind::cp437_text){check(op.bytes.find('\x1b')==std::string::npos,"no control metadata");output+=op.bytes;}else if(op.kind==app::TerminalDisplayOperationKind::paging_newline)output+='\n';}
        return fail?app::TerminalOutcome::output_failure:app::TerminalOutcome::completed;
    }
    app::TerminalOutcome pause(const app::TerminalPauseRequest&)override{return app::TerminalOutcome::completed;}
};
app::FileListPage page(unsigned count=20){app::FileListPage p;p.title="Files";p.context="Main / 1 General Files";p.columns="# Filename";p.previous=p.next=p.tag=p.sort=p.jump=true;
    for(unsigned i=1;i<=count;++i)p.rows.push_back({i,100+i,3,"file"+std::to_string(i)+".txt - @SYSOP@ inert"});
    return p;}
app::FileListPage compact_page(unsigned count=60){auto p=page(count);p.title.clear();p.context.clear();p.numbered=false;p.columns="Filename        Size     Description";
    for(auto& r:p.rows){r.filename="file"+std::to_string(r.number)+".zip";r.text=r.filename+std::string(16-r.filename.size(),' ')+" 73.4K   Inert "+sfx::application::qwk_cp437_to_utf8("\xb0\xb1\xb2\xdb")+" @SYSOP@ description";}return p;}
struct Harness {
    Terminal terminal;app::MessageEditorDimensions dimensions{true,80,24};std::deque<unsigned char> bytes;std::string frames;std::vector<std::int64_t> emitted;unsigned accepted{};bool live{true},fail_write{},fail_ack{};
    app::FileListPorts ports(){app::FileListPorts p{terminal};p.dimensions=[&]{return dimensions;};p.byte=[&](){if(bytes.empty())return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::disconnected,{}};auto c=bytes.front();bytes.pop_front();return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,c};};p.write=[&](std::string_view s){frames+=s;return !fail_write;};p.live=[&]{return live;};p.accepted_key=[&](bool){++accepted;};p.emitted=[&](const auto& rows){for(const auto& row:rows)emitted.push_back(row.identity);return !fail_ack;};return p;}
    void input(std::string_view value){for(unsigned char c:value)bytes.push_back(c);}
};
}
int main(int argc,char** argv){try{
    check(argc==1||argc==2,"optional fresh capture directory only");std::filesystem::path captures;
    if(argc==2){captures=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(captures),"never replace captured evidence");std::filesystem::create_directories(captures);}
    check(app::file_terminal_text("hello\x1b[2J\x01@FILEAREA@") .find('\x1b')==std::string::npos,"metadata escapes removed");
    check(app::file_terminal_text(u8"caf\u00e9") == std::string("caf\x82"),"lossless supported CP437 glyph");
    check(app::file_terminal_text(u8"a\U0001f600z")=="a?z","unsupported codepoint explicit replacement");
    check(app::file_terminal_text("abcd",2)=="ab","bounded text");
    {const std::vector<app::MenuTextRow> rows{{app::MenuTextRole::notification,"Arrival"},{app::MenuTextRole::status,"Current area"},{app::MenuTextRole::prompt,"Command: "}};check(app::render_file_menu_rows(rows,true,{},7)==app::render_menu_rows(rows,true,{},7),"non-command notifications/status/prompt preserve original adjacent framing exactly");}
    for(const auto menu:{app::MenuId::files,app::MenuId::batch_transfer}){
        const auto rows=app::builtin_menu_rows(app::menu_snapshot(menu,{10,false,1},1),"Date","Time");
        const auto rendered=app::render_file_menu_rows(rows,true,{12,false,true},7);
        for(const auto& command:app::menu_definition(menu).commands)if(command.key!='/')check(rendered.find("[\x1b[1;37;40m"+std::string(1,command.key)+"\x1b[0;37;40m:>")!=std::string::npos,"every native File/Batch hotkey letter-only white despite saved caller text color");
        check(app::render_file_menu_rows(rows,false,{},7).find('\x1b')==std::string::npos,"all native File/Batch plain fallbacks ANSI-free");
    }
    for(const char key:std::string("IOECWNHQTPNSJ")){
        check(app::file_option(key,"Description",true)=="\x1b[0;37;40m[\x1b[1;37;40m"+std::string(1,key)+"\x1b[0;37;40m] Description","only letter emphasized for every option");
        check(app::file_option(key,"Description",false)=="["+std::string(1,key)+"] Description","all plain options ANSI-free");
    }
    {Terminal t;check(app::present_file_text(t,"file.text",std::string(81,'x'),80,24),"paged text output");check(t.output==std::string(80,'x')+"\nx\n","width bounded text wrapping");}
    for(const auto columns:{40U,80U})for(const auto ansi:{false,true}){
        Terminal t;
        const auto exact=std::string(columns-1,'n');
        const std::vector<std::string> transfer{"Upload report:",exact,"  Scanner unavailable or failed. Private copy retained."};
        check(app::present_file_transfer_text(t,"files.transfer.result",transfer,{ansi,columns,24}),"display one transfer report");
        check(t.displays==1&&t.output.find(exact+"\n  Scanner")!=std::string::npos,"column boundary adds exactly one newline");
        check(t.output.find("Private copy retained.\n")!=std::string::npos,"transfer status stays readable at both widths");
        check(t.output.find('\x1b')==std::string::npos,"transfer metadata remains inert");
    }
    check(app::file_transfer_text_handoff(false)=="\r\n","plain transfer handoff emits no escapes");
    check(app::file_transfer_text_handoff(true)=="\r\x1b[2K\x1b[?7h\x1b[0m","ANSI handoff cleans only the protocol row");
    check(!app::prepare_file_view(std::string("MZ\0binary",9),false),"content detection rejects executable bytes regardless of extension");
    check(!app::prepare_file_view(std::string(262145,'x'),false),"standalone/member viewer bound");
    check(!app::prepare_file_view("\x1b[?1049hunsafe",true),"unsupported terminal controls never passed through");
    for(bool ansi:{false,true}){
        const auto text=app::prepare_file_view("one\r\ntwo\t@SYSOP@\x1a",ansi);check(text&&app::valid_terminal_display_program(*text),"content text and DOS EOF safely paged");
        const auto art=app::prepare_file_view("\x1b[31mRED\x1b[0m",ansi);check(art&&app::valid_terminal_display_program(*art),"bounded ANSI canvas becomes safe generated rendition");
        for(const auto& op:art->operations)check(ansi||op.kind!=app::TerminalDisplayOperationKind::ansi_sequence,"ASCII artwork fallback contains no ANSI");
    }
    {Harness h;h.input("12\r");std::size_t at{};auto p=h.ports();const auto r=app::choose_file_row(p,page(),at);check(r.action==app::FileListAction::open && r.identity==112 && r.revision==3 && at==11,"multi-digit selects exact shown identity/revision");check(h.accepted==1,"accepted CR synchronized once");check(h.frames.find(app::file_option('T',"Tag",true))!=std::string::npos,"actual ANSI frame hotkey-only standard");check(std::find(h.emitted.begin(),h.emitted.end(),120)==h.emitted.end(),"unseen below viewport never acknowledged");}
    {
        Harness h;h.input("\x1b[A\x1b[B\x1b[Bt");
        std::size_t at{};auto p=h.ports();
        const auto r=app::choose_file_row(p,page(),at);
        check(r.action==app::FileListAction::tag && r.identity==103,"arrows and tag select exact record");
        const auto clear=h.frames.find("\x1b[2J");
        check(clear!=h.frames.npos && h.frames.find("\x1b[2J",clear+1)==h.frames.npos,
            "initial draw is the only clear during unchanged-boundary and highlight moves");
        check(h.emitted.size()==16,"highlight moves do not acknowledge any files a second time");
    }
    {Harness h;h.input("\x1b[F\r");std::size_t at{};auto p=h.ports();auto r=app::choose_file_row(p,page(),at);check(r.identity==120,"End selects last row and reveals it");check(std::find(h.emitted.begin(),h.emitted.end(),120)!=h.emitted.end(),"only newly visible last row acknowledged after output");}
    {Harness h;h.input("\x1b[6~");std::size_t at{};auto p=h.ports();auto r=app::choose_file_row(p,page(5),at);check(r.action==app::FileListAction::next,"PgDn advances storage page");}
    for(const auto& pair:std::vector<std::pair<std::string,app::FileListAction>>{{"q",app::FileListAction::cancel},{"s",app::FileListAction::sort},{"j",app::FileListAction::jump},{"p",app::FileListAction::previous},{"n",app::FileListAction::next}}){Harness h;h.input(pair.first);std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,page(),at).action==pair.second,"all advertised ANSI choices usable");}
    {Harness h;h.input("q");h.fail_write=true;std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,page(),at).action==app::FileListAction::failed && h.emitted.empty(),"failed output never acknowledges");}
    {Harness h;h.input("q");h.fail_ack=true;std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,page(),at).action==app::FileListAction::failed && !h.bytes.empty(),"failed acknowledgement stops before input");}
    {Harness h;h.input("t");std::size_t at{};auto p=h.ports();p.byte=[&](){h.live=false;return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,'t'};};check(app::choose_file_row(p,page(),at).action==app::FileListAction::failed,"revocation after display prevents action");}
    {Harness h;std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,page(),at).action==app::FileListAction::failed,"disconnect cannot select");}
    {Harness h;h.input("q");std::size_t at{};auto p=h.ports();auto bad=page();bad.rows[1].identity=bad.rows[0].identity;check(app::choose_file_row(p,bad,at).action==app::FileListAction::failed && h.frames.empty(),"ambiguous identity fails before display");}
    for(const bool disabled:{true,false}){Harness h;h.dimensions.ansi=disabled;h.terminal.inputs.push_back({app::TerminalOutcome::completed,"12"});std::size_t at{};auto p=h.ports();p.lightbars=!disabled;auto r=app::choose_file_row(p,page(),at);check(r.identity==112 && h.frames==(disabled?"\x1b[0;37;40m":"") && h.terminal.output.find('\x1b')==std::string::npos,"ANSI text list resets gray; ASCII numbered fallback ANSI-free");check(h.emitted.size()==20,"complete paged plain output acknowledged");}
    {Harness h;h.dimensions.ansi=false;h.terminal.inputs={{app::TerminalOutcome::completed,"T"},{app::TerminalOutcome::completed,"2"}};std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,page(),at).identity==102,"plain explicit numbered tagging");}
    {Harness h;h.dimensions.ansi=false;h.terminal.fail=true;std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,page(),at).action==app::FileListAction::failed && h.emitted.empty(),"interrupted paged plain output not advanced");}
    {Harness h;h.input("\rq");std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,page(0),at).action==app::FileListAction::cancel,"empty list Enter cannot invent a record");}
    for(const auto& item:std::vector<std::pair<std::uint64_t,std::string>>{{0,"0.0K"},{1024,"1.0K"},{46182,"45.1K"},{1887437,"1.8M"},{1932735283,"1.8G"},{1048575,"1.0M"},{9223372036854775807ULL,"8589934592.0G"}})check(app::file_size_label(item.first)==item.second,"integer one-decimal K/M/G sizes including overflow boundary");
    {Harness h;auto list=page();list.numbered=false;for(auto& r:list.rows)r.filename="file"+std::to_string(r.number)+".txt";h.input("123\x1b[Bt");std::size_t at{};auto p=h.ports();const auto chosen=app::choose_file_row(p,list,at);check(chosen.action==app::FileListAction::tag&&chosen.identity==102,"nonnumeric list ignores digit input and T tags highlighted identity");check(h.frames.find("Number:")==h.frames.npos&&h.frames.find("Enter: open")==h.frames.npos&&h.frames.find("10 file10")==h.frames.npos,"nonnumeric screen has no digit prompt or varying row prefix");check(h.frames.find("Arrows: select - PgUp/PgDn: page - Home/End: first/last row")!=h.frames.npos&&h.frames.find("] View file")!=h.frames.npos,"approved footer and CR action");}
    {Harness h;h.dimensions.ansi=false;auto list=page();list.numbered=false;for(auto& r:list.rows)r.filename="file"+std::to_string(r.number)+".txt";h.terminal.inputs={{app::TerminalOutcome::completed,"file12.txt"}};std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,list,at).identity==112&&h.terminal.output.find("12 file12")==h.terminal.output.npos,"plain no-number selection uses exact filename");}
    {Harness h;h.dimensions.ansi=false;auto list=page(1);list.numbered=false;list.rows[0].filename=u8"caf\u00e9.txt";h.terminal.inputs={{app::TerminalOutcome::completed,std::string("caf\x82.txt")}};std::size_t at{};auto p=h.ports();check(app::choose_file_row(p,list,at).identity==101,"plain CP437 filename input maps to the exact UTF8 identity");list.rows[0].filename="../unsafe";check(app::choose_file_row(p,list,at).action==app::FileListAction::failed,"nonnumeric selectors cannot carry unsafe names");}
    for(const unsigned height:{24U,25U,50U})for(const bool notice:{false,true}){
        Harness h;h.dimensions.rows=height;auto list=compact_page();if(notice)list.notice="Download queue: 1 file(s).";h.input("q");std::size_t at{};auto p=h.ports();const auto result=app::choose_file_row(p,list,at);
        const auto visible=height-5-(notice?1U:0U);check(result.action==app::FileListAction::cancel&&h.emitted.size()==visible,"compact viewport fills available height, reserves feedback only when present");
        check(h.frames.find("\x1b[1;1H\x1b[1;37;40mFilename        Size     Description")!=h.frames.npos,"compact headings bright white on first row");
        for(const unsigned row:{2U,height-2})check(h.frames.find("\x1b["+std::to_string(row)+";1H\x1b[1;30;40m"+std::string(79,'-'))!=h.frames.npos,"both continuous separators dark gray and terminal-safe");
        check(h.frames.find("\x1b[3;1H\x1b[0;30;47mfile1.zip")!=h.frames.npos,"original configurable lightbar retained on first row");
        check(h.frames.find("\x1b[4;1H\x1b[0;37;40mfile2.zip")!=h.frames.npos,"body returns to gray, independent of white headings");
        check(h.frames.find("\xb0\xb1\xb2\xdb")!=h.frames.npos&&h.frames.find("@SYSOP@")!=h.frames.npos,"description CP437 artwork and inert tokens retained");
        check(h.frames.find("\x1b["+std::to_string(height-1)+";1H\x1b[0;37;40mArrows:")!=h.frames.npos&&h.frames.find("\x1b["+std::to_string(height)+";1H"+app::file_option('T',"Tag",true))!=h.frames.npos,"two-line footer uses last two rows without a redundant blank row");
        check(h.frames.find("Updated:")==h.frames.npos&&h.frames.find("Main /")==h.frames.npos&&h.frames.find("Number:")==h.frames.npos,"removed metadata and numeric prompt stay absent");
        check(h.emitted.back()==100+visible&&std::find(h.emitted.begin(),h.emitted.end(),101+visible)==h.emitted.end(),"new viewport still acknowledges only exact visible identities");
        if(!captures.empty()){std::ofstream out(captures/("compact-"+std::to_string(height)+(notice?"-feedback":"")+".ans"),std::ios::binary);const auto end=h.frames.rfind("\x1b[0;37;40m\r\n");check(end!=h.frames.npos,"capture excludes action exit CRLF, not a rendered frame");out.write(h.frames.data(),static_cast<std::streamsize>(end));check(static_cast<bool>(out),"complete raw ANSI frame captured");}
    }
    {Harness h;h.input("\x1b[6~t");std::size_t at{};auto p=h.ports();const auto chosen=app::choose_file_row(p,compact_page(),at);check(chosen.action==app::FileListAction::tag&&chosen.identity==120&&at==19,"PgDn advances by the larger 19-file viewport and T preserves identity");}
    {Harness h;h.input("\x1b[F\x1b[H\x1b[B\r");std::size_t at{};auto p=h.ports();const auto chosen=app::choose_file_row(p,compact_page(),at);check(chosen.identity==102&&at==1,"compact End/Home/Down/Enter preserve exact file identity");}
    {Harness h;h.dimensions.ansi=false;h.terminal.inputs={{app::TerminalOutcome::completed,"file2.zip"}};std::size_t at{};auto p=h.ports();const auto chosen=app::choose_file_row(p,compact_page(2),at);check(chosen.identity==102&&h.terminal.output.rfind("Filename",0)==0&&h.terminal.output.find('\x1b')==h.terminal.output.npos,"plain compact fallback begins at columns and remains ANSI-free");}
    #include "file_detail_checks.inc"
    #include "file_logoff_countdown_checks.inc"
    std::cout<<"PASS "<<checks<<" file-presentation checks; synthetic terminal, not installed/client UI\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
