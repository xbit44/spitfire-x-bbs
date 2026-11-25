int external_unknown_fixture(Fixture& board){
    const auto& root=board.root;
    board.config.settings.allow_direct_local_netmail=false;
    board.config.links.front().credentials={};
    app::FtnMessageEcho echo;echo.network="TEST";echo.tag="EXTERNAL.TEST";echo.description="Unknown caller cannot use this echo";
    board.config.message_echoes.push_back(echo);board.save(2);external_start(board);
    app::FtnPacket packet;packet.header.origin=*app::parse_ftn_address("2:100/99@test");packet.header.destination=*app::parse_ftn_address("2:100/1@test");packet.header.year=2026;packet.header.month=11;packet.header.day=10;
    app::FtnPackedMessage message;message.origin_net=message.destination_net=100;message.origin_node=99;message.destination_node=1;message.date="10 Nov 26  12:00:00";message.from="Unknown Fixture";message.to="Alice";message.attributes=1;message.subject="Unknown direct local NetMail";message.text="\1INTL 2:100/1 2:100/99\r\1MSGID: 2:100/99 direct-unknown-1\r\1CHRS: CP437 2\rDirect local body\r";packet.messages.push_back(message);
    message.attributes=0;message.to="All";message.subject="Unknown forbidden EchoMail";message.text="AREA:EXTERNAL.TEST\r\1MSGID: 2:100/99 forbidden-echo-1\r\1CHRS: CP437 2\rNever import this echo\r";packet.messages.push_back(message);
    const auto wire=app::encode_ftn_packet(packet);check(bool(wire),"unknown peer packet encoded");external_fixture_write(root/"peer-input.pkt",*wire);external_fixture_write(root/"READY","unknown caller policy Off\n");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);
    while(!std::filesystem::exists(root/"ENABLE")&&std::chrono::steady_clock::now()<deadline)std::this_thread::sleep_for(std::chrono::milliseconds(40));
    check(std::filesystem::exists(root/"ENABLE"),"driver observed retained unknown record before policy change");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress")==0,"direct-local Off did not create weaker custody");
    board.engine->stop();board.engine.reset();board.config.settings.allow_direct_local_netmail=true;board.save(3);external_start(board);
    await([&]{return external_fixture_count(board,"SELECT count(*) FROM sfx_local_messages WHERE CAST(subject AS TEXT)='Unknown direct local NetMail'")==1;},"real unprotected unknown stock caller imports local NetMail when enabled");
    await([&]{return external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=3")==1;},"forbidden EchoMail retains original as held");
    board.engine->stop();
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE proof=0 AND external_session_authenticated=0 AND network='' AND link=''")==1,"unknown receipt has only existing unscoped nonsecure provenance");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_local_messages WHERE CAST(subject AS TEXT)='Unknown forbidden EchoMail'")==0,"unknown caller cannot import EchoMail");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound")==0,"unknown packet creates no forwarding or external handoff");
    check(external_fixture_count(board,"SELECT count(*) FROM pragma_foreign_key_check")==0,"unknown fixture references clean");
    check(board.db->shutdown().succeeded(),"unknown fixture store closed");external_fixture_write(root/"DONE","PASS unknown stock receive\n");std::cout<<"PASS unknown External Runtime host "<<checks<<" checks\n";return 0;
}
