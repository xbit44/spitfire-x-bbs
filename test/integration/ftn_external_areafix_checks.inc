int external_areafix_fixture(Fixture& board){
    const auto& root=board.root;
#ifdef _WIN32
    const auto descriptor=ins::mutable_descriptor(board.receipt.owner);ins::create_directory(root/"Config",descriptor.get());check(bool(native::create_portable_board_key(root,board.receipt.owner)),"AreaFix fixture portable key");
#endif
    auto& link=board.config.links.front();link.credentials={};link.poll_minutes=0;link.areafix_enabled=true;
    const auto packet=native::protect_ftn_secret(root.u8string(),"TEST","PEER",native::FtnCredentialKind::packet,"PACKET12");
    const auto control=native::protect_ftn_secret(root.u8string(),"TEST","PEER",native::FtnCredentialKind::areafix,"AREA-KEY");check(packet&&control,"separate packet and AreaFix credentials");link.credentials.packet=*packet;link.credentials.areafix=*control;
    app::FtnMessageEcho echo;echo.network="TEST";echo.tag="SHARED.ECHO";echo.description="External AreaFix";board.config.message_echoes.push_back(echo);board.config.permissions.push_back({"TEST","PEER",echo.tag,false,true,false,true});
    board.save(2);external_start(board);const auto revision=board.config.revision;
    const auto make=[&](const char* name,std::string_view packet_password,std::string_view area_password){
        app::FtnPacket p;p.header.origin=*app::parse_ftn_address("2:100/2@test");p.header.destination=*app::parse_ftn_address("2:100/1@test");p.header.year=2026;p.header.month=11;p.header.day=10;std::copy(packet_password.begin(),packet_password.end(),p.header.password.begin());
        app::FtnPackedMessage m;m.origin_net=m.destination_net=100;m.origin_node=2;m.destination_node=1;m.date="10 Nov 26  12:00:00";m.from="Bob";m.to="AreaFix";m.attributes=1;m.subject=area_password;m.text="\1MSGID: 2:100/2@test external-area-"+std::string(name)+"\r\1CHRS: CP437 2\r+SHARED.ECHO\r%STATUS\r";p.messages.push_back(m);const auto wire=app::encode_ftn_packet(p);check(bool(wire),"AreaFix request packet encoded");external_fixture_write(root/name,*wire);
    };
    make("correct.pkt","PACKET12","AREA-KEY");make("wrong.pkt","PACKET12","BADKEY");make("missing.pkt","PACKET12","");make("badpkt.pkt","BADKEY","AREA-KEY");make("nopkt.pkt","","AREA-KEY");
    external_fixture_write(root/"READY","known P0 separate AreaFix fixture\n");
    await([&]{return external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=4")==1&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=3 AND reason='ftn_areafix_password_wrong'")==2&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=3 AND reason='ftn_packet_password_wrong'")==2;},"separate correct AreaFix applies; wrong/missing control or packet credentials hold");
    const auto saved=board.db->ftn_configuration({true,{}});check(saved&&saved.value&&saved.value->revision>revision&&app::ftn_echo_permission(*saved.value,"TEST","PEER","SHARED.ECHO",false,false),"actual AreaFix changes send permission and revision");
    await([&]{return board.engine->snapshot().active_revision==saved.value->revision&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=5")>=1;},"processor reloads and hands generated response to stock BinkD");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==0,"AreaFix handoff is never Native delivery");
    external_fixture_write(root/"RESPONSE_READY","response handed to BinkD\n");
    await([&]{return std::filesystem::exists(root/"RESPONSE_RECEIVED");},"driver observed stock response transfer");
    board.engine->stop();check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE proof=5 AND external_session_authenticated=0")==5,"all AreaFix inputs keep known P0 custody");
    check(external_fixture_count(board,"SELECT count(*) FROM pragma_foreign_key_check")==0,"AreaFix references clean");check(board.db->shutdown().succeeded(),"AreaFix store closed");external_fixture_write(root/"DONE","PASS External AreaFix\n");std::cout<<"PASS external AreaFix Runtime "<<checks<<" checks\n";return 0;
}
