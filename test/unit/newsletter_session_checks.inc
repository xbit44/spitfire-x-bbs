namespace {
struct NewsletterTerminal final:app::ICallerTerminal {
    std::deque<std::string> inputs;std::deque<char> keys;unsigned pauses{},lists{},clears{};bool cancel_list{};
    app::TerminalOutcome set_text_style(app::TerminalTextStyle)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome clear_screen()override{++clears;return app::TerminalOutcome::completed;}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest&)override{
        if(inputs.empty())throw std::runtime_error("newsletter input script exhausted");
        auto text=inputs.front();inputs.pop_front();return {text.empty()?app::TerminalOutcome::empty:app::TerminalOutcome::completed,std::move(text)};
    }
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{throw std::runtime_error("newsletter must not ask for secrets");}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest&)override{
        if(keys.empty())throw std::runtime_error("newsletter key script exhausted");
        const auto key=keys.front();keys.pop_front();return {app::TerminalOutcome::completed,key};
    }
    app::TerminalOutcome present(const app::TerminalPresentationRequest&)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& request)override{
        if(request.identity=="newsletter.list"){++lists;if(cancel_list)return app::TerminalOutcome::cancelled;}
        return app::TerminalOutcome::completed;
    }
    app::TerminalOutcome pause(const app::TerminalPauseRequest&)override{++pauses;return app::TerminalOutcome::completed;}
};
struct NewsletterSession {
    NewsletterTerminal terminal;app::NewsletterEditorPorts ports{terminal,{},{},{},{},{},{},{},{},{}};
    bool authorized{true},ansi{},fail_first{},blocked{};unsigned loads{},saves{};
    std::string original,saved,wire,keys;std::size_t next{};
    explicit NewsletterSession(std::string content):original(std::move(content)){
        ports.authorized=[&]{return authorized;};ports.ansi=[&]{return ansi;};ports.columns=[]{return 80U;};ports.rows=[]{return 25U;};
        ports.write=[&](std::string_view bytes){wire+=bytes;return true;};
        ports.byte=[&]()->app::StreamCallerTerminal::EditorByteResult{
            if(next>=keys.size())return {app::TerminalOutcome::disconnected,{}};
            const auto byte=static_cast<std::uint8_t>(keys[next++]);
            if(byte==0)return {app::TerminalOutcome::timed_out,{}};
            return {app::TerminalOutcome::completed,byte};
        };
        ports.load=[&]{++loads;return app::NewsletterSnapshot{!blocked,true,original,"original","ANSI protected"};};
        ports.save=[&](const app::NewsletterSnapshot& before,std::string_view bytes){
            check(before.bytes==original&&before.identity=="original","Save retains original conflict snapshot");
            ++saves;saved=bytes;return app::NewsletterSaveResult{!(fail_first&&saves==1),"Save result"};
        };
    }
    bool run(){return app::edit_sysop_newsletter(ports);}
};
void session_checks(const std::string& original){
    {NewsletterSession s(original);s.terminal.inputs={"/S"};check(s.run()&&s.saves==1&&s.saved==original&&s.terminal.pauses==1,"plain unchanged Save passes exact original and displays result");}
    {NewsletterSession s(original);s.terminal.inputs={"/E1","Edited","/A"};s.terminal.keys={'Y'};check(s.run()&&s.saves==0,"plain edit then Cancel never writes");}
    {NewsletterSession s(original);s.terminal.inputs={"/E1","Edited","/S","/S"};s.fail_first=true;check(s.run()&&s.saves==2&&s.saved=="Edited"+original.substr(original.find('\r')),"failed Save retains entire edited draft for retry");}
    {NewsletterSession s(original);s.terminal.inputs={"/L","/S"};s.terminal.cancel_list=true;check(s.run()&&s.saves==1&&s.terminal.lists==1,"cancelled listing returns to editor without disconnect");}
    {NewsletterSession s(original);s.ansi=true;s.keys="!\x1a";check(s.run()&&s.saved=="!"+original&&s.saves==1,"ANSI Ctrl-Z saves only typed insertion with all original bytes");}
    {NewsletterSession s(original);s.ansi=true;s.keys="!\x01\x1a";s.terminal.keys={'N'};check(s.run()&&s.saved=="!"+original,"declining discard keeps edited draft");}
    {NewsletterSession s(original);s.ansi=true;s.keys="!\x01";s.terminal.keys={'Y'};check(s.run()&&s.saves==0,"ANSI confirmed Cancel never writes");}
    {NewsletterSession s(original);s.ansi=true;s.keys=std::string("\x1b\0",2);s.terminal.keys={'S'};check(s.run()&&s.saved==original&&s.terminal.clears>=2,"bare Escape idle opens actions and Save preserves bytes");}
    {NewsletterSession s(original);s.ansi=true;s.keys="!";check(!s.run()&&s.saves==0,"disconnect leaves draft unpublished");}
    {NewsletterSession s(original);s.authorized=false;check(!s.run()&&s.loads==0&&s.saves==0,"unauthorized entry reads and writes nothing");}
    {NewsletterSession s(original);s.blocked=true;check(s.run()&&s.saves==0&&s.terminal.pauses==1,"protected ANSI produces readable result without editing");}
}
}
