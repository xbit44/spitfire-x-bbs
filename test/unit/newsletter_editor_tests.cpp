#include "sfx/application/newsletter_editor.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <deque>
namespace app=sfx::application;
namespace {unsigned checks{};void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}}
#include "newsletter_session_checks.inc"
int main(int argc,char** argv){try{
    check(argc==2,"source fixture argument");std::ifstream file(argv[1],std::ios::binary);const std::string original((std::istreambuf_iterator<char>(file)),{});
    check(original.size()==6698,"exact synthetic source fixture length");app::NewsletterDocument doc;check(doc.load(original)&&doc.bytes()==original,"whole document no-change bytes exact");
    app::MessageEditorScreen screen;check(doc.update_frame(80,25,screen).find("\x1b[2J")!=std::string::npos,"initial frame clears once");
    doc.apply({app::MessageEditorKey::right});const auto cursor_update=doc.update_frame(80,25,screen);
    check(cursor_update.find("\x1b[2J")==std::string::npos&&cursor_update.size()<100,"cursor movement uses bounded update without full-screen flash");
    check(doc.update_frame(80,25,screen).empty(),"unchanged frame writes nothing");
    for(unsigned n=0;n<17;++n)doc.apply({app::MessageEditorKey::down});
    check(doc.frame(80,25).find("SFXEditor")!=std::string::npos&&doc.bytes()==original,"render and movement never normalize bytes");
    check(doc.replace_line(1,"Edited header"),"bounded line edit accepted");const auto line_end=original.find('\r');
    check(doc.bytes()=="Edited header"+original.substr(line_end),"bounded edit preserves every unaffected byte including trailing spaces and final state");
    check(doc.load("A\tB\r\nC\nD\rE  "),"mixed separators accepted");doc.apply({app::MessageEditorKey::end});doc.apply({app::MessageEditorKey::text,'!'});
    check(doc.bytes()=="A\tB!\r\nC\nD\rE  ","edit retains tab and all mixed separators");
    doc.apply({app::MessageEditorKey::right});doc.apply({app::MessageEditorKey::backspace});check(doc.bytes()=="A\tB!C\nD\rE  ","backspace removes CRLF as one line break");
    check(doc.load("a\r\nb")&&doc.insert_line(2,"new\tline"),"insert existing-line position");check(doc.bytes()=="a\r\nnew\tline\r\nb","insert leaves original final newline absent");
    check(doc.delete_line(2)&&doc.bytes()=="a\r\nb","delete restores exact original bytes");
    check(doc.load("")&&doc.apply({app::MessageEditorKey::text,0xdb})&&doc.bytes()==std::string("\xdb",1),"new file supports CP437 extended glyph");
    const auto kept=doc.bytes();check(!doc.load("\x1b[31mANSI")&&doc.bytes()==kept,"ANSI content refused without draft mutation");
    check(!doc.load(std::string(app::NewsletterDocument::maximum_bytes+1,'x'))&&doc.bytes()==kept,"oversized content refused unchanged");
    check(doc.load("\tX\r\n  Y"),"tab movement fixture");doc.apply({app::MessageEditorKey::right});doc.apply({app::MessageEditorKey::text,'!'});check(doc.bytes()=="\t!X\r\n  Y","cursor offset preserves literal tab");
    check(doc.load("last  ")&&doc.apply({app::MessageEditorKey::end})&&doc.apply({app::MessageEditorKey::enter})&&doc.bytes()=="last  \r\n","explicit Enter alone adds new final newline");
    session_checks(original);
    std::cout<<"PASS newsletter document and session "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL newsletter document: "<<e.what()<<'\n';return 1;}}
