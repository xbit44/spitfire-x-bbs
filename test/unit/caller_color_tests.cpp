#include "sfx/application/menu_session.hpp"
#include "sfx/application/file_presentation.hpp"
#include "sfx/application/caller_utilities.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/test/terminal_stream_fake.hpp"
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <fstream>
#include <filesystem>
namespace app=sfx::application;
unsigned checks{};void check(bool ok,const char* message){++checks;if(!ok)throw std::runtime_error(message);}
struct Rendered {std::string text;std::vector<unsigned> colors;};
// Independent SGR observer: assert the color of every visible character, not
// implementation-specific escape ordering or substring appearances.
Rendered observe(std::string_view bytes){Rendered r;unsigned color=7;bool bright=false;for(std::size_t i=0;i<bytes.size();){if(bytes[i]=='\x1b'){check(i+1<bytes.size()&&bytes[i+1]=='[',"bounded CSI output");const auto end=bytes.find_first_of("mHJK",i+2);check(end!=std::string_view::npos,"complete ANSI command");if(bytes[end]=='m'){std::stringstream fields(std::string(bytes.substr(i+2,end-i-2)));std::string f;while(std::getline(fields,f,';')){const auto n=f.empty()?0:std::stoi(f);if(n==0){color=7;bright=false;}else if(n==1)bright=true;else if(n==22)bright=false;else if(n>=30&&n<=37){constexpr unsigned vga[]{0,4,2,6,1,5,3,7};color=vga[n-30];}}}i=end+1;}else{r.text+=bytes[i++];r.colors.push_back(color+(bright?8:0));}}return r;}
void colored(const Rendered& r,std::string_view value,unsigned color){const auto at=r.text.find(value);check(at!=std::string::npos,"expected visible text");for(std::size_t i=0;i<value.size();++i)if(value[i]!='\r'&&value[i]!='\n')check(r.colors[at+i]==color,"every visible cell has its intended role color");}
void footer_command_colors(const Rendered& rendered,std::string_view prompt){
    const auto start=rendered.text.rfind(prompt);
    const auto divider=prompt.find(" - [");
    const auto close=prompt.find(']');
    check(start!=std::string::npos&&divider!=std::string::npos&&close!=std::string::npos,
        "complete footer command is visible");
    for(std::size_t i=0;i<prompt.size();++i){
        const auto color=i==divider+3||i==close?15U:i>=divider&&i<divider+3?8U:7U;
        check(rendered.colors[start+i]==color,"footer command follows the fixed ANSI example");
    }
}
int main(int argc,char** argv)try{
 for(bool ansi:{false,true})for(unsigned selected=0;selected<=15;++selected)for(unsigned board:{7U,14U}){
  app::MenuPreferences pref{static_cast<std::uint8_t>(selected),false,true};const auto effective=selected?selected:board;
  for(unsigned index=0;index<12;++index){const auto id=static_cast<app::MenuId>(index<3?index:index+2);const auto menu=app::menu_snapshot(id,{10,false,1},1);
   const auto rows=app::builtin_menu_rows(menu,"Clock and date","Time and context",{},80);
   const auto raw=app::render_menu_rows(rows,ansi,pref,static_cast<std::uint8_t>(board));const auto file=app::render_file_menu_rows(rows,ansi,pref,static_cast<std::uint8_t>(board));
   for(const auto& out:{raw,file}){if(!ansi){check(out.find('\x1b')==std::string::npos,"plain fallback never has ANSI");continue;}const auto r=observe(out);colored(r,"Clock and date",effective);colored(r,"Time and context",effective);footer_command_colors(r,rows.back().text);colored(r,app::menu_definition(id).title,7);}
  }
  sfx::test::DeterministicInteractiveByteStreamFake bytes;std::uint8_t preference=static_cast<std::uint8_t>(selected);app::StreamCallerTerminalOptions opts;opts.caller_text_color=&preference;opts.text_color=static_cast<std::uint8_t>(board);app::StreamCallerTerminal terminal(bytes,opts);terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
  bytes.script_non_secret_text("Q\r");check(terminal.read_key({"review.choice","Choose [Q]uit: ",{'Q'},'Q'}).has_key(),"system prompt accepts its old command");
  check(terminal.present({"review.result","Saved.\r\n"})==app::TerminalOutcome::completed,"system feedback");
  check(terminal.present({"review.value","Visible value",app::TerminalPresentationRole::caller_value})==app::TerminalOutcome::completed,"explicit inert caller value");
  check(app::present_message_text(terminal,"message.selection.feedback","Message selection feedback"),"message UI role");
  const auto r=observe(bytes.output());if(ansi){colored(r,"Choose [",7);colored(r,"]uit: ",7);colored(r,"Q",15);colored(r,"Saved.",7);colored(r,"Visible value",effective);colored(r,"Message selection feedback",7);}else check(bytes.output().find('\x1b')==std::string::npos,"system prompts and values remain plain");
  app::TerminalDisplayProgram art;art.operations={{app::TerminalDisplayOperationKind::ansi_sequence,"\x1b[0;31;40m"},{app::TerminalDisplayOperationKind::cp437_text,"Authored red artwork"}};art.output_byte_count=art.operations[0].bytes.size()+art.operations[1].bytes.size();if(ansi){check(terminal.present_display({"review.art",&art})==app::TerminalOutcome::completed,"authored program retained");colored(observe(bytes.output()),"Authored red artwork",4);}
 }
 for(bool ansi:{false,true}){
  sfx::test::DeterministicInteractiveByteStreamFake bytes;bytes.script_non_secret_text("Q");std::uint8_t color=6;app::StreamCallerTerminalOptions opts;opts.caller_text_color=&color;app::StreamCallerTerminal terminal(bytes,opts);terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
  app::CallerUtilityPorts p{terminal,5};app::AccountRecord a;a.account_id=5;a.revision=1;a.display_name="Test Caller";a.security_level=std::uint16_t{10};a.profile.real_name="Example Name";
  p.account=[&]{return std::optional<app::AccountRecord>{a};};p.save_profile=[](const auto&,const auto&)->bool{throw std::runtime_error("color review must never save data");};p.history=[](const auto&){return app::StorageResult<std::vector<app::SessionRecord>>{{},std::vector<app::SessionRecord>{}};};p.directory=[](const auto&){return app::StorageResult<std::vector<app::CallerDirectoryEntry>>{{},std::vector<app::CallerDirectoryEntry>{}};};p.utc_now=[](){return std::int64_t{100000};};p.local_time=[](auto){return std::optional<std::string>{"2026-11-03 12:00:00"};};p.ghost=[](){return std::optional<bool>{false};};
  check(app::run_caller_utility(app::MenuAction::caller_profile,p).has_value(),"real profile controller returns normally");const auto r=observe(bytes.output());if(ansi){colored(r,"Caller name:",7);colored(r,"Test Caller",6);colored(r,"Example Name",6);colored(r,"THINGS YOU CAN CHANGE",7);colored(r,"[",7);check(r.colors[r.text.find("[R]")+1]==15,"profile hotkey alone is white");colored(r,"Real Name:",7);colored(r,"Choose a listed key, [",7);}else check(bytes.output().find('\x1b')==std::string::npos,"profile ASCII fallback");
  if(argc==2){std::filesystem::create_directories(argv[1]);std::ofstream out(std::filesystem::path(argv[1])/(ansi?"profile-ansi.txt":"profile-plain.txt"),std::ios::binary);out<<bytes.output();}
 }
 check(!app::valid_terminal_presentation_request({"review.invalid","inert",static_cast<app::TerminalPresentationRole>(255)}),"invalid role rejected");
 std::cout<<"PASS "<<checks<<" caller/system color checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}

