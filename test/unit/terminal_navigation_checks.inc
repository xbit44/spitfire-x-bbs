// Ordinary prompts share the production terminal, not editor/selector decoding.
void prompt_navigation_checks() {
    for (bool fragmented : {false, true}) {
        for (bool secret : {false, true}) {
            test::DeterministicInteractiveByteStreamFake stream;
            const std::string typed="before\x1b[A\x1b[B\x1b[C\x1b[D\x1bOA\x1bOB\x1bOC\x1bOD\x1b[1;5Dafter\r\n";
            if(fragmented) for(char byte:typed) stream.script_non_secret_text(std::string(1,byte));
            else stream.script_non_secret_text(typed);
            app::StreamCallerTerminal terminal(stream);
            const app::TerminalInputRequest request{"navigation.line","Filename: ",40,app::EmptyInputPolicy::use_default,{},true};
            const auto result=secret?terminal.read_secret(request):terminal.read_visible(request);
            require(result.outcome==app::TerminalOutcome::completed && result.value=="beforeafter","arrows preserve ordinary/secret input across every byte boundary");
            require(stream.output().find('\x1b')==std::string::npos,"input controls never echoed");
            if(secret)require(stream.output().find("before")==std::string::npos,"secret bytes remain hidden");
        }
        test::DeterministicInteractiveByteStreamFake stream;
        const std::string typed="\x1b[A\x1bOB\x1b[1;2C\x1b[DNY\r\n";
        if(fragmented)for(char byte:typed)stream.script_non_secret_text(std::string(1,byte));
        else stream.script_non_secret_text(typed);
        app::StreamCallerTerminal terminal(stream);
        require(terminal.read_key({"navigation.key","Choose: ",{'A','B','C','D','N','Y'},'Y',false,true}).key=='N',"arrows cannot select allowed A/B/C/D or cancel key prompt");
        require(terminal.read_key({"navigation.next","Next: ",{'Y'},'Y'}).key=='Y',"valid key immediately after arrows is not swallowed");
    }
    {
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\x1b[A\x1bOD\r\nnext\r");
        app::StreamCallerTerminal terminal(stream);
        require(terminal.pause({"navigation.pause"})==app::TerminalOutcome::completed,"pause ignores arrows and waits for actual Enter");
        require(terminal.read_visible(line_request("navigation.after",20)).value=="next","pause preserves next logical input and CRLF");
    }
    for(auto outcome:{app::InteractiveStreamOutcome::timed_out,app::InteractiveStreamOutcome::cancelled,app::InteractiveStreamOutcome::disconnected,app::InteractiveStreamOutcome::shutdown_requested,app::InteractiveStreamOutcome::failed}){
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("private\x1b");stream.script_read_outcome(outcome);
        app::StreamCallerTerminal terminal(stream);
        auto result=terminal.read_visible({"navigation.escape","Value: ",30,app::EmptyInputPolicy::use_default,{},true});
        const auto expected=outcome==app::InteractiveStreamOutcome::timed_out?app::TerminalOutcome::cancelled:
            outcome==app::InteractiveStreamOutcome::failed?app::TerminalOutcome::terminal_failure:
            outcome==app::InteractiveStreamOutcome::disconnected?app::TerminalOutcome::disconnected:
            outcome==app::InteractiveStreamOutcome::shutdown_requested?app::TerminalOutcome::shutdown_requested:app::TerminalOutcome::cancelled;
        require(result.outcome==expected && result.value.empty(),"bare Escape cancels without Enter; real stop outcomes are not hidden");
    }
    {
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\x1b[\r");app::StreamCallerTerminal terminal(stream);
        require(terminal.pause({"navigation.partial"})==app::TerminalOutcome::completed,"Enter after incomplete CSI remains usable");
    }
    {
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\x1b[A");app::StreamCallerTerminal terminal(stream);
        require(terminal.read_editor_byte().byte==27 && terminal.read_editor_byte().byte=='[' && terminal.read_editor_byte().byte=='A',"raw editor/selector path retains every navigation byte");
    }
    {
        test::DeterministicInteractiveByteStreamFake stream;
        stream.script_non_secret_text("\x1b[DZ\r\n**\x18" "B00");app::StreamCallerTerminal terminal(stream);
        require(terminal.read_key({"navigation.protocol","Protocol: ",{'Z'},'Z',false,true}).key=='Z',"protocol choice ignores arrows");
        const auto prefix=terminal.take_binary_input_prefix();
        require(std::string(prefix.bytes.begin(),prefix.bytes.end())=="**\x18" "B00","protocol prefix remains byte-exact; no global binary filtering");
    }
}
