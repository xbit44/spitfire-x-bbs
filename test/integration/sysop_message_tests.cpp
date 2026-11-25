#define main retained_message_storage_main
#include "message_storage_tests.cpp"
#undef main
struct PreviewWire:app::IInteractiveSessionByteStream {
    std::string input,frame;std::size_t at{};bool sampled{};std::vector<std::string> frames;
    app::InteractiveStreamReadResult read_some(std::size_t n) override {if(!sampled){frames.push_back(frame);sampled=true;}if(at==input.size())return {app::InteractiveStreamOutcome::disconnected,{}};const auto count=std::min(n,input.size()-at);std::vector<std::uint8_t> b(input.begin()+at,input.begin()+at+count);at+=count;return {app::InteractiveStreamOutcome::completed,std::move(b)};}
    app::InteractiveStreamWriteResult write_some(std::string_view s) override{frame+=s;return {app::InteractiveStreamOutcome::completed,s.size()};}
    void feed(std::string s){input=std::move(s);at=0;sampled=false;}
};
struct PreviewTerminal:app::ICallerTerminal {
    PreviewWire wire;app::StreamCallerTerminal native{wire};std::deque<std::string> script;
    void next(){check(!script.empty(),"message preview input available");wire.feed(script.front()+"\r");script.pop_front();}
    app::TerminalOutcome set_text_style(app::TerminalTextStyle s)override{return native.set_text_style(s);}
    app::TerminalOutcome clear_screen()override{wire.frame.clear();return native.clear_screen();}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& r)override{next();return native.read_visible(r);}
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{throw std::runtime_error("no credentials in message preview");}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest& r)override{next();return native.read_key(r);}
    app::TerminalOutcome present(const app::TerminalPresentationRequest& r)override{return native.present(r);}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& r)override{return native.present_display(r);}
    app::TerminalOutcome pause(const app::TerminalPauseRequest& r)override{wire.feed("\r");return native.pause(r);}
};
int main(int argc,char** argv){try{
    check(argc==2,"new message evidence root required");const std::filesystem::path root=argv[1];check(!std::filesystem::exists(root),"retain old message evidence");
    for(const bool ansi:{false,true}){
        const auto path=root/(ansi?"ansi":"plain");sfx::test::FakeStoragePathPolicy policy;auto settings=options(path);db::SqliteStorage store(settings,policy);check(store.start().succeeded()&&store.shutdown().succeeded(),"current message fixture");seed(path/"SFX.DB");check(store.start().succeeded(),"synthetic message actors");app::MessageActor actor{id(1),1,1,1};unsigned op=40000;
        auto draft=request(store,actor,{app::MessageKind::public_echo,{1},{},{}},op++);draft.subject="Section 14A review message";draft.body="Synthetic review text. No private owner content.\n";const auto first=store.commit_message(draft);check(bool(first),"first preview message committed");draft.operation=id(op++);draft.subject="Protected review message";const auto second=store.commit_message(draft);check(bool(second),"second preview message committed");check(bool(store.set_message_permanent(actor,id(op++),second.value->id,false,true)),"protected preview message");
        unsigned frame{};const auto run=[&](std::deque<std::string> input,bool select,std::optional<std::int64_t> number){
            PreviewTerminal t;t.script=std::move(input);t.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);app::MessageQuestionTerminal questions(t,[=]{return ansi;},[&](std::string_view s){return t.wire.write_some(s).bytes_written==s.size();});
            app::MessageQueryOptions query;query.mode=app::MessageQueryMode::echo;query.echo={1};if(select){const auto area=app::select_local_message_echo({store,questions,[&]{return std::optional<app::MessageActor>{actor};}});check(area.selection.has_value(),"actual group and echo selected");query.group=area.selection->group.id;query.echo=area.selection->echo.id;}
            app::MessageReaderPorts ports{store,questions,[&]{return std::optional<app::MessageActor>{actor};},{},[](std::int64_t){return std::string("2026-11-04 14:00:00");},[]{return true;},24,true,{},[&]{return id(op++);},[=]{return app::MessageEditorDimensions{ansi,80,25};},[]{return std::uint8_t{7};},true};
            const auto result=app::read_local_messages(ports,query,select,number);check(result!=app::MessageReaderOutcome::failed,"actual review reader completes navigation");
            for(const auto& bytes:t.wire.frames){check(bytes.find("Reply")==std::string::npos&&bytes.find("] Protect")==std::string::npos,"review context exposes neither reply nor protection mutation");std::ofstream f(path/("message-"+std::to_string(++frame)+(ansi?".ans":".txt")),std::ios::binary);f<<bytes;check(bool(f),"actual message frame retained");}
        };
        run({"1","1","1","D","N","Q"},true,{});check(bool(store.quote_source(actor,first.value->id)),"No leaves selected message unchanged");
        run({"D","Y","Q"},false,1);check(!store.quote_source(actor,first.value->id),"confirmed exact message deleted");
        run({"D","Q"},false,2);check(bool(store.quote_source(actor,second.value->id)),"Permanent message retained");check(store.shutdown().succeeded(),"message evidence stopped");
    }
    std::cout<<"PASS Sysop message review: "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL Sysop message review: "<<e.what()<<'\n';return 1;}}
