void preview_notice_format_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    db::SqliteStorage store(options(root),paths);
    check(store.start().succeeded() && store.shutdown().succeeded(),"preview-format fresh fixture");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"preview-format actors ready");
    const app::MessageActor sysop{id(1),1,1,1},bob{id(3),3,1,1};
    const auto post=store.commit_message(request(store,sysop,{app::MessageKind::public_echo,{1},{},{}},45000));
    check(post.succeeded(),"preview-format one available public post");
    for(bool resumable:{false,true})for(bool ansi:{false,true}){
        if(resumable)check(store.open_message(bob,post.value->id).succeeded(),"preview-format saved position ready");
        const auto before=store.message_scan_preference(bob,{1}).value->highest_opened;
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("T\r\nN\r\nQ\r\n");
        app::StreamCallerTerminal terminal(stream);unsigned op=45001;
        app::MessageComposePorts compose{store,terminal,[&]{return std::optional<app::MessageActor>{bob};},[&]{return id(op++);},[ansi]{return app::MessageEditorDimensions{ansi,80,24};},[&]{return terminal.read_editor_byte();},[](std::string_view){return true;},{}};
        app::MessageCallerSession session({compose,[]{return std::string("Bob");},[](std::int64_t){return std::string("2026-10-08");},[]{return true;},{}});
        check(session.initialize() && session.action(app::MenuAction::message_read).has_value(),"preview-No/Q returns through unchanged session flow");
        const auto& text=stream.output();
        check(text.find(resumable?"#1.\r\nOlder revisits do not move this position back.\r\n":"No saved available message can be resumed; select a message number.\r\n")!=std::string::npos,"preview notice uses deliberate complete-word lines");
        check(text.find("Enter message # [1-1]")!=std::string::npos && (text.find("or < ENTER > = last message read")!=std::string::npos)==resumable,"number range and saved Enter choice unchanged");
        check(store.message_scan_preference(bob,{1}).value->highest_opened==before,"preview-No/Q never updates saved read pointer");
    }
    check(store.shutdown().succeeded(),"preview-format fixture stopped");
}
