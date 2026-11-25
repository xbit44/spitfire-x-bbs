int external_packet_fixture(Fixture& board,bool retention=false){
    const auto& root=board.root;
#ifdef _WIN32
    const auto descriptor=ins::mutable_descriptor(board.receipt.owner);ins::create_directory(root/"Config",descriptor.get());check(bool(native::create_portable_board_key(root,board.receipt.owner)),"packet fixture portable key");
#endif
    board.config.settings.allow_direct_local_netmail=true;auto& link=board.config.links.front();link.credentials={};link.poll_minutes=0;
    const auto password=native::protect_ftn_secret(root.u8string(),link.network,link.code,native::FtnCredentialKind::packet,"PACKET12");check(bool(password),"separate packet credential");link.credentials.packet=*password;
    app::FtnMessageEcho echo;echo.network="TEST";echo.tag="SHARED.ECHO";echo.description="Separate packet credential";board.config.message_echoes.push_back(echo);board.config.permissions.push_back({"TEST","PEER",echo.tag,false,true,true,false});
    if(retention)board.config.networks.front().keep_processed=false;
    board.save(2);external_start(board);
    external_fixture_write(root/"correct.pkt",external_peer_packet("2:100/2@test","2:100/1@test","CORRECT","PACKET12"));
    external_fixture_write(root/"wrong.pkt",external_peer_packet("2:100/2@test","2:100/1@test","WRONG","BADKEY"));
    external_fixture_write(root/"missing.pkt",external_peer_packet("2:100/2@test","2:100/1@test","MISSING"));
    external_fixture_write(root/"READY","known P0 packet credential fixture\n");
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(90);bool done{};
    while(std::chrono::steady_clock::now()<deadline){
        if(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=4")==1&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=3 AND reason='ftn_packet_password_wrong'")==2){done=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    check(done,"correct packet imports and wrong/missing configured passwords hold");
    if(retention){
        await([&]{return external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_expiry_intents WHERE outcome=1")==1;},"existing processor expires fully processed P0 packet original under per-network policy");
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_retained_originals")==1,"wrong and missing packet credentials remain held outside expiry candidates");
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_expiry_intents e JOIN sfx_ftn_ingress i ON i.receipt_id=e.ingress_id WHERE i.state<>4 OR i.external_session_authenticated<>0")==0,"expiry respects successful P0 admission without inventing session authentication");
        check(!std::filesystem::exists(root/"FTN/Processed/TEST/correct.pkt"),"managed successful original removed after confirmed expiry");
    }
    board.engine->stop();
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_local_messages")==2,"only correct credential imports private and public messages");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE network='TEST' AND link='PEER' AND proof=5 AND external_session_authenticated=0")==3,"known unprotected sessions keep exact link custody and no invented session authentication");
    check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE network='' OR proof=0")==0,"known peer never downgrades to direct-local unknown admission");
    check(external_fixture_count(board,"SELECT count(*) FROM pragma_foreign_key_check")==0,"packet fixture references clean");check(board.db->shutdown().succeeded(),"packet fixture store closed");external_fixture_write(root/"DONE","PASS known P0 packet credentials\n");std::cout<<"PASS known P0 packet Runtime host "<<checks<<" checks\n";return 0;
}
