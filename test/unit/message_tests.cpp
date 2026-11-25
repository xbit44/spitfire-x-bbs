#include "sfx/application/message.hpp"
#include "sfx/application/message_editor.hpp"
#include "sfx/application/message_arrival.hpp"
#include "sfx/application/message_presentation.hpp"
#include "sfx/application/message_storage.hpp"
#include "sfx/test/terminal_stream_fake.hpp"

#include <iostream>
#include <stdexcept>
#include <algorithm>

namespace app = sfx::application;
namespace {
unsigned checks{};
void check(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "message_reader_footer_checks.inc"
#include "message_editor_refresh_checks.inc"
}
int main(int argc,char** argv) {
    if(argc==2 && (std::string(argv[1])=="--login-summary-frame" || std::string(argv[1])=="--read-options-frame")){
        sfx::test::DeterministicInteractiveByteStreamFake output;app::StreamCallerTerminal display(output);
        app::MessageLoginScanSummary summary;summary.scanned=120;summary.unread_to_you=2;summary.read_to_you=8;summary.from_you=6;
        const bool passed=std::string(argv[1])=="--read-options-frame"?app::present_message_read_options(display,true):app::present_message_login_summary(display,summary,true);
        std::cout<<output.output();return passed?0:1;
    }
    if(argc==2 && std::string(argv[1])=="--reference-frame"){
        app::MessageEditorState reference(200);reference.resize(true,80,25);
        reference.draft().set_subject("test");reference.insert_data("This is a test message of SFX Full Screen Editor (FSE).");
        std::cout<<reference.frame("xbit","xbit");return 0;
    }
    try {
        reader_footer_checks();
        editor_refresh_checks();
        for(const bool ansi:{false,true}) {
            sfx::test::DeterministicInteractiveByteStreamFake bytes;
            app::StreamCallerTerminal raw(bytes);std::string framing;
            app::MessageQuestionTerminal messages(raw,[&]{return ansi;},[&](std::string_view text){framing+=text;return true;});
            check(app::present_message_text(messages,"qwk.notice","QWK packet settings"),"QWK notice renders through ordinary message terminal");
            check(framing==(ansi?"\x1b[0;37;40m":""),"QWK resets input-echo color to menu gray only for ANSI callers");
            check(bytes.output().find("QWK packet settings")!=std::string::npos,"QWK text remains visible");
        }
        for(const bool ansi:{false,true}){
            sfx::test::DeterministicInteractiveByteStreamFake bytes;
            app::StreamCallerTerminal raw(bytes);std::string framing;
            app::MessageQuestionTerminal messages(raw,[&]{return ansi;},[&](std::string_view text){framing+=text;return true;});
            std::string menu="QWK packet settings - ZIP / ZMODEM\n\n";
            for(const char key:std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ"))menu+="["+std::string(1,key)+"] Gray description: Gray value\n";
            menu+="A literal [X] inside a description stays gray.";
            check(app::present_message_text(messages,"qwk.options",menu),"QWK typed option list renders");
            const auto shown=bytes.output();
            check(framing.empty(),"QWK option display does not add leading blank rows");
            for(const char key:std::string("ABCDEFGHIJKLMNOPQRSTUVWXYZ")){
                const auto label="["+std::string(1,key)+"]";
                check(shown.find(ansi?"[\x1b[1;37;40m"+std::string(1,key)+"\x1b[0;37;40m] Gray description: Gray value":label+" Gray description: Gray value")!=std::string::npos,"only the hotkey character white; both brackets and description/value gray");
                check(shown.find("\x1b[1;37;40m"+label)==std::string::npos,"no option label has a bright-white opening bracket");
            }
            check(shown.find("A literal [X] inside a description stays gray.")!=std::string::npos,"no styling of inline bracketed data");
            check(ansi?shown.rfind("\x1b[0;37;40m",0)==0:shown.find('\x1b')==std::string::npos,"ANSI header gray, plain fallback escape-free");
            bytes.script_non_secret_text("q");
            const auto choice=messages.read_key({"qwk.pointer.scope","Public pointers [E G A Q]? [E]: ",{'E','G','A','Q'},'E',false,true});
            check(choice.has_key()&&choice.key=='Q'&&framing=="\r\n","one blank row between QWK options and compact question");
        }
        for(const auto maximum:{app::kMaximumRetainedMessageCount,app::kMaximumMessageAgeDays}){
            check(app::parse_message_retention_limit("1",maximum).value==1U,"retention finite minimum");
            check(app::parse_message_retention_limit(std::to_string(maximum),maximum).value==maximum,"retention finite maximum without capacity claim");
            check(!app::parse_message_retention_limit(std::to_string(static_cast<std::uint64_t>(maximum)+1),maximum).valid,"retention upper overflow refusal");
            for(const auto text:{"0","-1","+1","1.5","1x",""," 1","1 ","4294967296","999999999999999999999"})
                check(!app::parse_message_retention_limit(text,maximum).valid,"retention malformed/zero/overflow refusal");
            for(const auto text:{"Unlimited","unlimited","UNLIMITED"}){
                const auto parsed=app::parse_message_retention_limit(text,maximum);
                check(parsed.valid && !parsed.value,"explicit Unlimited distinct from invalid");
            }
        }
        check(app::valid_message_retention_policy({}),"fresh count1000 and unlimited age");
        check(app::valid_message_retention_policy({{}, {}}),"both Unlimited valid");
        check(!app::valid_message_retention_policy({0, {}}),"zero count is not Unlimited");
        check(!app::valid_message_retention_policy({{}, 65536}),"age bound validated");
        const auto arrival=std::int64_t{1700000000};
        check(!app::message_age_expired(arrival,arrival+86400-1,1),"full day before boundary preserved");
        check(app::message_age_expired(arrival,arrival+86400,1),"full day exact boundary eligible");
        check(app::message_age_expired(arrival,arrival+86400+1,1),"after boundary eligible");
        check(!app::message_age_expired(arrival,arrival-1,1),"reversed clock never expires future arrival");
        check(!app::message_age_expired(arrival,arrival+86400*365,{}),"Unlimited age disables expiry");
        for(bool ansi:{false,true}){
            sfx::test::DeterministicInteractiveByteStreamFake bytes;app::StreamCallerTerminal terminal(bytes);
            check(app::present_message_options(terminal,ansi,"message.entry.kind.options",{{'P',"Public post"},{'M',"Private local mail"},{'Q',"Cancel"}}),"E menu options render");
            auto text=bytes.output();
            check(text.find(std::string(29,'.')+" Public post")!=std::string::npos && text.find(std::string(22,'.')+" Private local mail")!=std::string::npos && text.find(std::string(34,'.')+" Cancel")!=std::string::npos,"E options dotted/aligned; approved labels only");
            if(ansi)check(text.find("\x1b[1;37;40mP\x1b[0;37;40m:>")!=std::string::npos,"E keys bright white, punctuation/text gray");
            else check(text.find('\x1b')==std::string::npos,"E plain menu escape-free");
        }
        using V = app::MessageValidation;
        auto body = app::normalize_message_body("first\r\n\r\nlast\r", 3);
        check(body && body.rows == std::vector<std::string>{"first", "", "last"}, "normalize newlines once, retain blank");
        check(body.canonical_lf == "first\n\nlast\n", "canonical LF, no phantom row");
        check(app::normalize_message_body(body.canonical_lf, 3).canonical_lf == body.canonical_lf, "normalization idempotent");
        body = app::normalize_message_body("a\n\n", 2);
        check(body && body.rows.size() == 2 && body.rows.back().empty(), "intentional trailing blank retained");
        body = app::normalize_message_body(std::string(80, 'x'), 1);
        check(body && body.rows.size() == 1, "exact 80 has no phantom line");
        body = app::normalize_message_body(std::string(81, 'x'), 2);
        check(body && body.rows[0].size() == 80 && body.rows[1] == "x", "long word split without loss");
        body = app::normalize_message_body(std::string(75, 'x') + " word next", 2);
        check(body && body.rows[0] == std::string(75, 'x') + " ", "word break preserves space");
        body = app::normalize_message_body("a\tb", 1);
        check(body && body.rows[0] == "a       b", "tabs fixed eight-cell stops");
        check(app::normalize_message_body("x", 0).validation == V::invalid_limits, "no unlimited sentinel");
        check(app::normalize_message_body("x", 65536).validation == V::invalid_limits, "line limit bounded");
        for (unsigned byte = 0; byte < 256; ++byte) {
            const auto value = std::string(1, static_cast<char>(byte));
            if ((byte < 32 && byte != '\t' && byte != '\r' && byte != '\n') || byte == 127)
                check(app::normalize_message_body(value, 10).validation == V::invalid_control, "reject unhandled controls");
            else check(static_cast<bool>(app::normalize_message_body(value, 10)), "CP437 printable byte accepted");
        }
        app::MessageDraft draft(2);
        check(draft.set_subject(std::string(70, static_cast<char>(0xdb))) == V::valid, "CP437 subject cells");
        check(draft.set_subject(std::string(71, 'a')) == V::subject_too_long && draft.subject().size() == 70, "subject refusal preserves draft");
        check(draft.replace_body("existing") == V::valid, "initial body");
        const auto original = draft.body().canonical_lf;
        check(draft.replace_body("1\n2\n3") == V::too_many_lines && draft.body().canonical_lf == original, "overlimit body atomic");
        check(draft.insert_quote(0, {"a", "b"}, {0, 1}) == V::too_many_lines && draft.body().canonical_lf == original, "oversized quote atomic");
        check(draft.insert_quote(0, {"a"}, {2}) == V::invalid_selection && draft.body().canonical_lf == original, "bad quote preserves draft");
        check(draft.insert_quote(0, {"quote"}, {0}) == V::valid && draft.body().canonical_lf == "> quote\nexisting\n", "quote insertion retains existing text");
        for(const std::string prefix:{"> "," > ","xb> ","> > ","AB> cd> "}){
            const auto raw=prefix+std::string(76,'x')+" conference next";
            const auto quoted=app::normalize_message_body(raw+"\n\nreply\n",8);
            check(quoted&&quoted.rows.size()>=4&&quoted.rows[1].substr(0,prefix.size())==prefix&&quoted.rows[quoted.rows.size()-2].empty(),"wrapped quote keeps full prefix and paragraph break");
            std::string restored;for(const auto& row:app::wrap_message_line(raw,80)){check(row.text.size()<=80,"quoted rows stay in fixed composition width");restored+=row.text.substr(row.inserted_prefix);}
            check(restored==raw,"quoted wrapping retains every authored source byte");
            check(app::normalize_message_body(quoted.canonical_lf,8).canonical_lf==quoted.canonical_lf,"quote normalization is idempotent");
            const auto cursor=app::message_body_cursor_offset(raw,8,raw.size());
            const auto mapped_body=app::normalize_message_body(raw,8);check(cursor&&*cursor+1==mapped_body.canonical_lf.size(),"cursor after long quote maps after final authored byte");
        }
        app::MessageDraft wide_quote(8);check(wide_quote.insert_quote(0,{std::string(80,'x')},{0})==V::valid&&wide_quote.body().rows.size()==2&&wide_quote.body().rows[1].substr(0,2)=="> ","quoting full-width source keeps continuation marked");
        const auto art_quote=app::wrap_message_line("> "+std::string(90,static_cast<char>(0xc4)),80);check(art_quote.size()>1&&std::all_of(art_quote.begin(),art_quote.end(),[](const auto& row){return row.inserted_prefix==0;}),"box artwork does not gain synthesized prose markers");
        const auto ranges = app::message_quote_selection("5,1,3-5,1", 5);
        check(ranges && *ranges == std::vector<std::size_t>{0,2,3,4}, "quote sorted/deduplicated source order");
        for (const char* invalid : {"", "0", "1,", "3-1", "6", "1--2", "-1", "+1", "1-999999999999999999999999"})
            check(!app::message_quote_selection(invalid, 5), "malformed quote selection rejected");
        check(app::message_quote_selection("All", 5)->size() == 5, "all quote selection");
        check(draft.save_validation() == V::valid, "save valid shared model");
        check(draft.set_subject("  ") == V::valid && draft.save_validation() == V::valid, "blank subject is valid on save");
        draft.set_subject("Subject");
        draft.replace_body(" \n ");
        check(draft.save_validation() == V::blank_body, "whitespace body rejected");
        draft.replace_body("literal /s and @SFX@ |12 [include]");
        check(draft.body().rows[0] == "literal /s and @SFX@ |12 [include]", "commands/tokens remain literal data");
        check(draft.insert_text(0, "\x1b[2J") == V::invalid_control, "escape data rejected, not executed");
        draft.replace_body("one\ntwo");
        check(draft.replace_line(2,"/S")==V::valid && draft.body().rows[1]=="/S","numbered replacement slash is data");
        check(draft.insert_line(3,"three")==V::too_many_lines && draft.body().rows.size()==2,"numbered insertion limit atomic");
        check(draft.delete_line(0)==V::invalid_selection && draft.delete_line(3)==V::invalid_selection,"invalid line deletion preserves");
        check(draft.delete_line(1)==V::valid && draft.body().rows==std::vector<std::string>{"/S"},"numbered delete");
        check(draft.insert_line(2,"last")==V::valid && draft.body().rows.back()=="last","insert count plus one");
        check(draft.replace_line(1,"bad\nrow")==V::invalid_control && draft.body().rows[0]=="/S","replacement cannot inject rows");
        using Command=app::MessageEditorCommand;
        check(app::message_line_command("/s").command==Command::save && app::message_line_command("/A").command==Command::abort,"case insensitive live shortcuts");
        check(app::message_line_command("literal /S").command==Command::none && app::message_line_command("/S extra").command==Command::none,"ordinary mentions literal");
        check(app::message_line_command("/e12").number==std::size_t{12} && app::message_line_command("/i").command==Command::insert_line && !app::message_line_command("/i").number,"line command inline or prompt number");
        check(app::message_line_command("/D0").command==Command::invalid && app::message_line_command("/E99999999999999999").command==Command::invalid,"invalid command numbers");
        app::MessageEditorState editor(4);
        check(editor.insert_data("/S\nquote @SFX@ |12")==V::valid && editor.draft().body().rows[0]=="/S","paste entry never executes shortcuts");
        const auto saved_body=editor.draft().body().canonical_lf;const auto saved_cursor=editor.cursor_offset();
        editor.resize(true,79,24);check(!editor.full_screen() && editor.draft().body().canonical_lf==saved_body && editor.cursor_offset()==saved_cursor,"small screen switch preserves draft/cursor");
        editor.resize(true,100,40);check(editor.full_screen() && editor.body_rows()==34 && editor.draft().body().canonical_lf==saved_body,"taller wider fixed wrap");
        editor.resize(false,80,24);check(editor.frame("To","From").empty(),"plain editor cannot emit ANSI frame");
        editor.resize(true,0,0);check(editor.full_screen() && editor.body_rows()==18,"unknown dimensions 80x24");
        auto frame=editor.frame("All Callers\x1b[2J","Author");check(frame.find("SFXEditor")!=std::string::npos && frame.find("ESC for Help")!=std::string::npos && frame.find("All Callers [2J")!=std::string::npos,"fixed editor safe header/footer");
        check(frame.find("\x1b[?7l")==0 && frame.rfind("\x1b[?7h")==frame.size()-5,"frame controls last-column autowrap");
        check(frame.find("\x1b[1;37;40mSubj\x1b[1;30;40m:\x1b[0;37;40m ")!=std::string::npos,"reference labels bright, colon dark gray, value gray");
        check(frame.find("\x1b[0;37;40m/S")!=std::string::npos,"body and pasted text light gray");
        check(frame.find(static_cast<char>(0xfa))!=std::string::npos && frame.find(static_cast<char>(0xfe))!=std::string::npos,"reference dotted block ruler");
        check(frame.find(" SFXEditor [INS]")!=std::string::npos && frame.find("OVR")==std::string::npos,"persistent Insert footer");
        editor.apply({app::MessageEditorKey::insert_mode});
        check(editor.frame("All Callers","Author").find(" SFXEditor [OVR]")!=std::string::npos,"mode-only change repaints footer immediately");
        editor.apply({app::MessageEditorKey::insert_mode});
        app::MessageEditorState deletion(10);deletion.insert_data("one\n\n\ntwo\n");
        deletion.apply({app::MessageEditorKey::up});deletion.apply({app::MessageEditorKey::up});deletion.apply({app::MessageEditorKey::home});
        deletion.apply({app::MessageEditorKey::erase});
        check(deletion.draft().body().canonical_lf=="one\n\ntwo\n","Delete on blank row pulls next row up once");
        deletion.apply({app::MessageEditorKey::up});deletion.apply({app::MessageEditorKey::up});deletion.apply({app::MessageEditorKey::end});deletion.apply({app::MessageEditorKey::erase});
        check(deletion.draft().body().canonical_lf=="one\ntwo\n","Delete at end removes next blank newline once");
        deletion.apply({app::MessageEditorKey::erase});
        check(deletion.draft().body().canonical_lf=="onetwo\n","Delete at end joins following text");
        editor.apply({app::MessageEditorKey::home});check(editor.cursor_column()==0,"home control");
        editor.apply({app::MessageEditorKey::end});check(editor.cursor_column()==editor.draft().body().rows[editor.cursor_row()].size(),"end control");
        app::MessageEditorState typing(2);typing.insert_data("ab");typing.apply({app::MessageEditorKey::left});typing.apply({app::MessageEditorKey::insert_mode});typing.apply({app::MessageEditorKey::text,'X'});
        check(typing.draft().body().canonical_lf=="aX\n" && !typing.inserting(),"overwrite mode");typing.apply({app::MessageEditorKey::backspace});check(typing.draft().body().canonical_lf=="a\n","backspace editing");
        typing.apply({app::MessageEditorKey::enter});typing.insert_data("second");const auto prior=typing.draft().body().canonical_lf;
        check(typing.quote({"one","two"},{0,1})==V::too_many_lines && typing.draft().body().canonical_lf==prior,"editor overlimit quote atomic");
        typing.resize(true,80,24);typing.apply({app::MessageEditorKey::page_up});typing.apply({app::MessageEditorKey::page_down});check(typing.cursor_offset()<=typing.draft().body().canonical_lf.size(),"page controls bounded");
        check(typing.insert_data("\x1b[31m")==V::invalid_control && typing.draft().body().canonical_lf==prior,"editor control-data refusal");
        sfx::test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\b!\r\nx");app::StreamCallerTerminal terminal(stream);
        auto subject=terminal.read_visible({"message.subject","Subject: ",70,app::EmptyInputPolicy::use_default,"Existing"});
        check(subject.outcome==app::TerminalOutcome::completed && subject.value=="Existin!","visible subject prefill editable");
        check(terminal.read_editor_byte().byte=='x',"editor shares prompt pending CRLF without phantom body line");
        std::string questions;
        app::MessageQuestionTerminal question_terminal(terminal,[]{return true;},[&](std::string_view text){questions+=text;return true;});
        stream.script_non_secret_text("n");
        check(question_terminal.read_key({"message.login.scan","Your Message Scan [Y/n]",{'Y','N'},'Y'}).key=='N',"decorated scan retains choice");
        check(questions.rfind("\r\n\r\n",0)==0 && questions.substr(questions.size()-2)=="\r\n" && questions.find("\x1b[0;37;40m")!=std::string::npos,"scan gray mirrored blocks and blank rows");
        check(std::count(questions.begin(),questions.end(),static_cast<char>(0xfe))==6 && questions.find(static_cast<char>(0xdb))==std::string::npos,"scan uses six small identification squares");
        sfx::test::DeterministicInteractiveByteStreamFake native_output;app::StreamCallerTerminal native_terminal(native_output);
        app::MessageLoginScanSummary summary;summary.scanned=120;summary.unread_to_you=2;summary.read_to_you=8;summary.from_you=6;
        check(app::present_message_login_summary(native_terminal,summary,true),"summary native presentation");
        check(native_output.output().find("Previously read messages to you")!=std::string::npos && std::count(native_output.output().begin(),native_output.output().end(),static_cast<char>(0xfe))==6,"summary review and small-square header");
        check(app::present_message_read_options(native_terminal,true),"native read options presentation");
        for(char key:std::string("TAONQ"))check(native_output.output().find(std::string("\x1b[1;37;40m")+key+"\x1b[0;37;40m:>")!=std::string::npos,"only actionable read letters bright white");
        check(native_output.output().find("\x1b[1;37;40mY\x1b[0;37;40m:>")==std::string::npos,"Your Message Scan is no longer nested under Read");
        native_terminal.set_text_style(app::TerminalTextStyle::ogsf_ansi);
        check(native_terminal.present({"caller.failed_login_notice","Failed login attempt - fixture\n"})==app::TerminalOutcome::completed && native_output.output().find("\x1b[0;1;37;40mFailed login attempt - fixture\r\n")!=std::string::npos,"login notice uses white safe admission-boundary equivalent");
        sfx::test::DeterministicInteractiveByteStreamFake plain_output;app::StreamCallerTerminal plain_terminal(plain_output);
        check(app::present_message_login_summary(plain_terminal,summary,false) && app::present_message_read_options(plain_terminal,false) && plain_output.output().find('\x1b')==std::string::npos,"summary and read menu plain fallback escape-free");
        summary.scanned=-1;check(!app::present_message_login_summary(plain_terminal,summary,false),"invalid counts do not publish zero summary");
        stream.script_non_secret_text("\x1b");
        stream.script_read_outcome(app::InteractiveStreamOutcome::timed_out);
        check(question_terminal.read_visible({"message.lookup","Caller: ",70,app::EmptyInputPolicy::use_default,{}}).outcome==app::TerminalOutcome::cancelled,"message question Escape cancels immediately without Enter");
        auto secret=terminal.read_secret({"credential.input","Password: ",70,app::EmptyInputPolicy::reject,"never"});check(secret.outcome==app::TerminalOutcome::invalid,"secret input refuses visible prefill");
        check(!app::valid_terminal_input_request({"message.subject","Subject: ",2,app::EmptyInputPolicy::use_default,"long"}) && !app::valid_terminal_input_request({"message.subject","Subject: ",70,app::EmptyInputPolicy::use_default,"\x1b[2J"}),"prefill bound/control validation");
        app::MessageEditorKeyDecoder keys;
        check(app::message_body_cursor_offset(std::string(60,'a')+" "+std::string(20,'b'),10,71)==std::size_t{72},"cursor follows suffix-sensitive word break");
        check(app::message_body_cursor_offset("a\tb",10,2)==std::size_t{8},"cursor follows expanded tab cells");
        check(app::message_body_cursor_offset("a\r\nb",10,3)==std::size_t{2},"cursor maps CRLF once");
        check(app::message_body_cursor_offset("a\n",10,2)==std::size_t{2},"cursor after explicit newline retains next-row insertion");
        check(keys.feed(26)->command==Command::save && keys.feed(1)->command==Command::abort && keys.feed(17)->command==Command::quote && keys.feed(19)->command==Command::subject && keys.feed(12)->command==Command::help,"live control shortcuts");
        check(!keys.feed(27) && keys.idle()->command==Command::actions,"bare escape opens action menu at idle");
        keys.feed(27);check(keys.feed('e')->event->key==app::MessageEditorKey::resume,"escape return does not move cursor");
        keys.feed(27);keys.feed('[');check(keys.feed('A')->event->key==app::MessageEditorKey::up,"CSI arrow decoded");
        keys.feed(27);keys.feed('[');keys.feed('3');check(keys.feed('~')->event->key==app::MessageEditorKey::erase,"physical CSI Delete forward erases");
        keys.feed(27);keys.feed('[');keys.feed('2');check(keys.feed('~')->event->key==app::MessageEditorKey::insert_mode,"physical CSI Insert toggles model mode");
        keys.feed(27);keys.feed('[');check(keys.feed('@')->event->key==app::MessageEditorKey::insert_mode,"SyncTERM ANSI Insert toggles mode");
        keys.feed(27);keys.feed('[');check(keys.feed('K')->event->key==app::MessageEditorKey::end,"SyncTERM ANSI End");
        keys.feed(27);keys.feed('[');check(keys.feed('V')->event->key==app::MessageEditorKey::page_up,"SyncTERM ANSI Page Up");
        keys.feed(27);keys.feed('[');check(keys.feed('U')->event->key==app::MessageEditorKey::page_down,"SyncTERM ANSI Page Down");
        check(keys.feed(127)->event->key==app::MessageEditorKey::backspace && keys.feed(8)->event->key==app::MessageEditorKey::backspace,"both Backspace encodings preserved");
        check(frame.find("\x1b[?67l")!=std::string::npos,"editor requests unambiguous SyncTERM Delete encoding");
        keys.feed(27);keys.feed('O');check(keys.feed('F')->event->key==app::MessageEditorKey::end,"SS3 End decoded");
        keys.feed(27);keys.feed('[');keys.feed('6');check(keys.feed('~')->event->key==app::MessageEditorKey::page_down,"CSI page control");
        check(keys.feed('\r')->event->key==app::MessageEditorKey::enter && !keys.feed('\n'),"decoder CRLF one live Enter");
        keys.feed(27);keys.feed(']');check(!keys.feed(26) && !keys.feed('S') && !keys.feed(7) && keys.feed('x')->event->byte=='x',"OSC contents cannot execute save or become text");
        keys.feed(27);keys.feed('[');for(unsigned n=0;n<40;++n)check(!keys.feed('1'),"overlong CSI stays bounded/drained");check(keys.feed('m')->command==Command::invalid && keys.feed('b')->event->byte=='b',"escape tail consumed before body resumes");
        app::MessageArrivalQueue arrivals(3,10);
        check(!arrivals.enqueue({{1},3,10}) && !arrivals.enqueue({{2},2,11}),"no reconnect backlog or wrong recipient notice");
        check(arrivals.enqueue({{2},3,11}) && !arrivals.enqueue({{2},3,11}),"active-session arrival deduplicated");
        check(app::message_arrival_notice("AreaFix robot")=="You have Mail [from: AreaFix robot]","sender notice exact requested appearance");
        check(app::message_arrival_notice("Bad\x1b[31m\aName").find('\x1b')==std::string::npos&&app::message_arrival_notice("Bad\aName").find('\a')==std::string::npos,"sender cannot inject color/bell controls");
        check(app::message_arrival_notice(std::string(500,'x')).size()<90,"sender notice is bounded");
        unsigned notices{};auto eligible=[](app::MessageId){return true;};auto unread=[]{return true;};auto output=[&](std::string_view line){++notices;return line=="You have a new message.";};
        check(arrivals.present(false,eligible,unread,output) && notices==0 && arrivals.pending()==1,"unsafe boundary defers pending identities");
        check(!arrivals.present(true,eligible,unread,[](std::string_view){return false;}) && arrivals.pending()==1,"output failure retains notice");
        check(arrivals.present(true,eligible,unread,output) && notices==1 && arrivals.pending()==0,"accepted safe notice then ack");
        for(std::int64_t n=12;n<200;++n)check(arrivals.enqueue({{n},3,n}),"bounded arrivals accepted/coalesced");
        check(arrivals.pending()==64 && arrivals.coalesced(),"overflow bounded at sixty-four identities");
        check(arrivals.present(true,[](app::MessageId){return false;},unread,output) && notices==2 && !arrivals.coalesced(),"current eligibility recheck and generic overflow");
        check(arrivals.present(true,eligible,unread,output) && notices==2,"repeated presentation no duplicates");
        arrivals.enqueue({{200},3,200});
        check(!arrivals.present_checked(true,[](app::MessageId)->std::optional<bool>{return {};},[]()->std::optional<bool>{return true;},output) && arrivals.pending()==1 && notices==2,"unknown eligibility preserves pending notice without output");
        app::MessageArrivalQueue second_session(3,100);check(second_session.enqueue({{201},3,201}) && arrivals.enqueue({{201},3,201}),"multiple eligible sessions each queue one notice");
        std::cout << "PASS " << checks << " shared message model/editor/arrival checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
