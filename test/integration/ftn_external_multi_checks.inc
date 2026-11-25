std::string external_peer_packet(std::string_view origin,std::string_view destination,std::string label,std::string_view password={}){
    app::FtnPacket packet;packet.header.origin=*app::parse_ftn_address(origin);packet.header.destination=*app::parse_ftn_address(destination);packet.header.year=2026;packet.header.month=11;packet.header.day=10;
    check(password.size()<=8,"packet fixture password bound");std::copy(password.begin(),password.end(),packet.header.password.begin());
    app::FtnPackedMessage message;message.origin_net=packet.header.origin.net;message.destination_net=packet.header.destination.net;message.origin_node=packet.header.origin.node;message.destination_node=packet.header.destination.node;
    message.date="10 Nov 26  12:00:00";message.from="Shared Session Fixture";message.to="Alice";message.attributes=1;message.subject=label+" incoming NetMail";
    message.text="\1MSGID: "+std::string(origin)+" "+label+"-private-1\r\1CHRS: CP437 2\rShared incoming private body\r";packet.messages.push_back(message);
    message.to="All";message.attributes=0;message.subject=label+" incoming EchoMail";message.text="AREA:SHARED.ECHO\r\1MSGID: "+std::string(origin)+" "+label+"-echo-1\r\1CHRS: CP437 2\rShared incoming public body\r";packet.messages.push_back(message);
    const auto wire=app::encode_ftn_packet(packet);check(bool(wire),"shared stock packet encoded");return *wire;
}
int external_multi_fixture(Fixture& board,bool alias_negative,bool many=false){
    const auto& root=board.root;board.config.links.front().credentials={};board.config.links.front().poll_minutes=0;
    if(alias_negative)board.config.links.front().address="2:100/99@test";
    else{
        board.config.links.front().additional_addresses={"2:100/22@test"};
        app::FtnNetwork network;network.code="OTHER";network.name="Second shared network";network.address="3:200/1@other";network.enabled=true;board.config.networks.push_back(network);
        auto link=board.config.links.front();link.network="OTHER";link.address="3:200/2@other";link.additional_addresses.clear();board.config.links.push_back(link);
    }
    for(const auto& network:board.config.networks){app::FtnMessageEcho echo;echo.network=network.code;echo.tag="SHARED.ECHO";echo.description="Same echo tag, distinct networks";board.config.message_echoes.push_back(echo);board.config.permissions.push_back({network.code,"PEER",echo.tag,false,true,true,false});}
    if(many)for(unsigned i=0;i<62;++i){app::FtnNetwork network;network.code="IDLE"+std::to_string(i);network.name="Idle timing network "+std::to_string(i);network.address=std::to_string(100+i)+":100/1@idle"+std::to_string(i);network.enabled=true;board.config.networks.push_back(network);}
    board.save(2);external_start(board);
    if(alias_negative)external_fixture_write(root/"peer-input.pkt",external_peer_packet("2:100/99@test","2:100/1@test","UNTRUSTED"));
    else{
        unsigned operation=920;const app::MessageActor alice{id(2),2,1,1};
        for(const auto& network:board.config.networks){
            const auto link=std::find_if(board.config.links.begin(),board.config.links.end(),[&](const auto& item){return item.network==network.code;});
            if(link==board.config.links.end())continue;
            if(many){external_fixture_write(root/(network.code+"-input.pkt"),external_peer_packet(link->address,network.address,network.code));continue;}
            app::MessageTarget target;target.kind=app::MessageKind::private_ftn;target.remote=app::MessageRemoteTarget{network.code,network.address,link->address,"Bob"};auto compose=board.db->begin_message(alice,target,{});check(bool(compose),"shared-network private compose");
            check(bool(board.db->commit_message({alice,id(operation++),*compose.value,network.code+" outgoing NetMail","Shared session outgoing private body\n"})),"shared-network private commit");
            const auto echo=std::find_if(board.config.message_echoes.begin(),board.config.message_echoes.end(),[&](const auto& item){return item.network==network.code;});target={};target.kind=app::MessageKind::public_echo;target.echo={echo->local_echo};compose=board.db->begin_message(alice,target,{});check(bool(compose),"shared-network echo compose");
            check(bool(board.db->commit_message({alice,id(operation++),*compose.value,network.code+" outgoing EchoMail","Shared session outgoing public body\n"})),"shared-network echo commit");
            external_fixture_write(root/(network.code+"-input.pkt"),external_peer_packet(link->address,network.address,network.code));
        }
    }
    external_fixture_write(root/"READY","shared session fixture ready\n");
    const std::string expected=alias_negative?"ftn_external_file_peer_or_destination_unmatched":"ftn_external_bundle_mixed_routes_retained";
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);bool observed{};
    while(std::chrono::steady_clock::now()<deadline){
        const auto state=board.engine->snapshot();
        if(!state.processing_error.empty()&&std::any_of(state.activity.begin(),state.activity.end(),[&](const auto& activity){return activity.message.find(expected)!=std::string::npos;})&&(alias_negative||external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE state=4")==2)){observed=true;break;}
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    check(observed,"unsupported/untrusted file retained with explicit Runtime diagnostic");board.engine->stop();
    if(alias_negative){
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress")==0,"P=1 for another alias cannot create privileged SFX custody");
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_local_messages")==0,"unprotected secondary alias imports neither NetMail nor EchoMail");
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound")==0,"unprotected secondary alias creates no outgoing work");
    }else{
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE network='TEST' AND link='PEER' AND proof=5 AND external_session_authenticated=1 AND state=4")==1,"first network bundle has exact authenticated custody");
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress WHERE network='OTHER' AND link='PEER' AND proof=5 AND external_session_authenticated=1 AND state=4")==1,"second network packet has distinct authenticated custody");
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_local_messages WHERE CAST(subject AS TEXT) LIKE '% incoming %'")==4,"same-tag networks each import private and public mail");
        check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_ingress")==2,"mixed-route bundle has no partial custody/import");
        if(many)check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound")==0,"idle timing isolates shared receive without outgoing scanner work");
        else check(external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=5")>=4&&external_fixture_count(board,"SELECT count(*) FROM sfx_ftn_outbound WHERE state=3")==0,"four actual scanner results handed off without false remote delivery");
    }
    check(external_fixture_count(board,"SELECT count(*) FROM pragma_foreign_key_check")==0,"shared fixture references clean");check(board.db->shutdown().succeeded(),"shared fixture store closed");
    external_fixture_write(root/"DONE","PASS shared stock fixture\n");std::cout<<"PASS shared External Runtime host "<<checks<<" checks\n";return 0;
}
