{
    check(app::message_network_name(*server.db,alice,"TEST")=="Engine Test"&&app::message_network_name(*server.db,alice,"n-deleted-private-id")=="Unavailable network","caller network names resolve and missing entries never fall back to private keys");
    sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("Q\r\n");app::StreamCallerTerminal terminal(stream);
    app::MessageReaderPorts ports{*server.db,terminal,[&]{return std::optional<app::MessageActor>{alice};},{},[](std::int64_t){return std::string("test date");},[]{return true;},24,false};
    (void)app::read_local_messages(ports,options);
    check(stream.output().find("Network:")==std::string::npos&&stream.output().find("From address:")==std::string::npos&&stream.output().find("From: Bob (2:100/2@test)")!=std::string::npos,"compact incoming NetMail reader shows sender address inline without network routing metadata");
    sfx::test::DeterministicInteractiveByteStreamFake compose_stream;compose_stream.script_non_secret_text("Test\r\n/a\r\nY\r\n");app::StreamCallerTerminal compose_terminal(compose_stream);
    app::MessageComposePorts compose{*client.db,compose_terminal,[&]{return std::optional<app::MessageActor>{bob};},[]{return id(12000);},[]{return app::MessageEditorDimensions{false,80,24};},[&]{return compose_terminal.read_editor_byte();},[](std::string_view){return true;}};
    check(app::compose_local_message(compose,target,"Alice","Bob")==app::MessageComposeOutcome::cancelled,"synthetic outbound draft cancelled without sending");
    check(compose_stream.output().find("Network: Engine Test")!=std::string::npos&&compose_stream.output().find("Network: TEST")==std::string::npos,"actual outbound NetMail composer displays friendly network name");
    sfx::test::DeterministicInteractiveByteStreamFake selection_stream;selection_stream.script_non_secret_text("N\r\n\r\n");app::StreamCallerTerminal selection_terminal(selection_stream);
    app::MessageComposePorts chosen{*client.db,selection_terminal,compose.actor,compose.operation,compose.dimensions,[&]{return selection_terminal.read_editor_byte();},compose.write};
    app::MessageCallerSession session({chosen,[]{return std::string("Bob");},{},[]{return true;},{}});(void)session.action(app::MenuAction::message_enter);
    check(selection_stream.output().find("Network: Engine Test")!=std::string::npos&&selection_stream.output().find("(TEST)")==std::string::npos,"NetMail recipient-selection prompt hides internal code");
}
