// An independent scripted BinkP1.0 peer supplies a bounded arbitrary M_PWD,
// transfers a real encoded packet over TCP, then the native tosser imports it.
// This is real socket/custody/storage integration, not a Synchronet-client run.
{
    // A separate configured network preserves the earlier intentional TIC
    // ambiguity fixtures; they must continue fencing their original custody.
    for(auto& network:c.networks)network.enabled=false;
    auto wire_network=n;wire_network.code="WIRE";wire_network.name="Anonymous wire integration";wire_network.enabled=true;c.networks.push_back(wire_network);
    auto wire_link=l;wire_link.network="WIRE";wire_link.credentials={};c.links.push_back(wire_link);
    c.settings.allow_direct_local_netmail=true;
    check(bool(db.change_ftn_configuration(owner,id(87000).bytes,c)),"anonymous wire fixture clears only its own secrets");c=*db.ftn_configuration(owner).value;
#ifdef _WIN32
    const auto wire_owner=who;
    for(const auto* role:{L"Inbound",L"Outbound",L"Processed"})ins::create_directory(root/L"FTN"/role/L"WIRE",sd.get());
    auto wire_incoming=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Inbound"/L"WIRE",who);
    auto wire_output=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Outbound"/L"WIRE",who);
    auto nonsecure=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Nonsecure",who);
#else
    for(const auto* role:{"Inbound","Outbound","Processed"})check(::mkdir((root/"FTN"/role/"WIRE").c_str(),0700)==0,"new isolated wire custody directory");
    auto wire_incoming=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Inbound"/L"WIRE",bindings);
    auto wire_output=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Outbound"/L"WIRE",bindings);
    auto nonsecure=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Nonsecure",bindings);
#endif
    app::FtnOutboundJob private_job;private_job.id=id(87001);private_job.operation=private_job.id;
    private_job.network="WIRE";private_job.link=wire_link.code;private_job.kind=app::FtnOutboundKind::payload;
    private_job.file={"private.bin",4,1700000000};
    {auto held=wire_output.create(native::ftn_native_artifact_name(private_job.id),4);const std::array<std::uint8_t,4> bytes{{'m','a','i','l'}};
        held.append(bytes.data(),bytes.size());private_job.digest=held.identity().sha256;}
    check(db.queue_ftn_jobs(owner,c.revision,{private_job}).succeeded(),"real private outgoing artifact queued for anonymous pickup denial");
    unsigned wire_serial=87010;
    const auto wire_receive=[&](app::FtnPacket mail,std::string claims,bool expect_refusal=false){
        const auto bytes=*app::encode_ftn_packet(mail);
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
        const auto running=[&]{return std::chrono::steady_clock::now()<deadline;};
#ifdef _WIN32
        auto listener=native::FtnNativeListener::open("127.0.0.1",0,false,c.settings,wire_owner);
#else
        auto listener=native::FtnNativeListener::open("127.0.0.1",0,false,c.settings);
#endif
        native::FtnNativeSessionResult result;bool accepted{},got{},offered{},peer_completed{};
        const auto session=id(wire_serial++);
        std::thread server([&]{try{
            std::unique_ptr<native::FtnNativeChannel> channel;
            while(!channel&&running())channel=listener->accept(std::chrono::milliseconds(50),running);
            if(channel){native::FtnNativeSessionPorts ports{db,c,wire_incoming,wire_output,root.u8string(),session,{},false,running,{},{},{}};
                ports.directory=[&](std::string_view network,bool outbound){return outbound?wire_output:network.empty()?nonsecure:wire_incoming;};
                result=native::run_ftn_native_session(*channel,std::move(ports));}
        }catch(const std::exception& error){result.reason=error.what();}});
        try{
            app::FtnLink remote;remote.host="127.0.0.1";remote.port=listener->port();
#ifdef _WIN32
            auto channel=native::FtnNativeChannel::connect_ipv4({127,0,0,1},listener->port(),remote,wire_owner,running);
#else
            auto channel=native::FtnNativeChannel::connect_ipv4({127,0,0,1},listener->port(),remote,running);
#endif
            app::BinkpDecoder decoder;bool adr{};
            while(!adr&&running()){const auto read=channel->read(std::chrono::milliseconds(50),running);if(read.outcome==native::FtnReadOutcome::closed)break;
                decoder.feed(read.bytes,[&](app::BinkpFrame frame){if(frame.command&&frame.id==1)adr=true;return true;});}
            channel->write(app::binkp_command(app::BinkpCommand::nul,"VER IndependentAnonymousFixture binkp/1.0"),running);
            channel->write(app::binkp_command(app::BinkpCommand::adr,claims),running);
            channel->write(app::binkp_command(app::BinkpCommand::pwd,"CRAM-MD5-0123456789abcdef0123456789abcdef"),running);
            bool end{},remote_eob{};
            while(!end&&running()){const auto read=channel->read(std::chrono::milliseconds(50),running);if(read.outcome==native::FtnReadOutcome::closed)break;
                decoder.feed(read.bytes,[&](app::BinkpFrame frame){if(!frame.command)return true;
                    switch(static_cast<app::BinkpCommand>(frame.id)){
                    case app::BinkpCommand::ok:
                        accepted=true;channel->write(app::binkp_command(app::BinkpCommand::file,"anonymous.pkt "+std::to_string(bytes.size())+" 1700000001 0"),running);
                        channel->write(app::binkp_data(bytes),running);break;
                    case app::BinkpCommand::got:got=true;channel->write(app::binkp_command(app::BinkpCommand::eob),running);if(remote_eob)end=true;break;
                    case app::BinkpCommand::eob:remote_eob=true;if(got)end=true;break;
                    case app::BinkpCommand::file:offered=true;end=true;break;
                    case app::BinkpCommand::err:end=true;break;
                    default:break;}return true;});}
            peer_completed=got&&remote_eob;
        }catch(const std::exception&){}
        server.join();
        if(expect_refusal){
            check(!accepted&&!got&&!result.handshake_complete&&!result.peer_verified&&!result.last_ingress.valid()&&
                result.reason=="ftn_session_unknown_or_ambiguous_aka","disabled unconfigured direct-local policy refuses unknown sender before password/file custody");
            return app::FtnIngressReceipt{};
        }
        if(!result.completed)std::cerr<<"ANONYMOUS WIRE "<<result.reason<<'\n';
        check(accepted&&got&&peer_completed&&result.completed&&result.received_files==1,"actual anonymous TCP peer transfers complete packet into private native custody");
        check(!result.peer_verified&&!offered&&result.sent_files==0,"anonymous session cannot authenticate or pick up pending outgoing mail");
        const auto receipt=db.ftn_ingress_receipt(owner,result.last_ingress);
        check(receipt&&receipt.value,"real session receipt retained for native toss");return *receipt.value;
    };
    const auto deliver=[&](unsigned number,bool known){auto mail=packet(number,false);mail.header.password.fill(0);
        if(!known){mail.header.origin.node=99;mail.messages.front().origin_node=99;}
        mail.messages.front().subject="Anonymous wire NetMail "+std::to_string(number);
        const auto receipt=wire_receive(mail,known?"2:100/2@test":"2:100/99@test");
        check(receipt.proof==(known?app::FtnIngressProof::inbound_tcp:app::FtnIngressProof::nonsecure),"actual known-empty and unknown session proofs stay distinct");
        const auto before=scalar(database,"SELECT count(*) FROM sfx_local_messages");
        const auto tossed=native::toss_ftn_native_receipt(db,c,installed,receipt,live);
        if(!tossed.processed)std::cerr<<"ANONYMOUS TOSS "<<tossed.reason<<'\n';
        check(tossed.processed&&tossed.imported==1&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==before+1,"anonymous wire packet delivers exactly one resulting local private message");
        app::MessageId local_message;
        {internal::SqliteConnection connection(database,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            internal::SqliteStatement query(connection.get(),"SELECT original_message_id FROM sfx_ftn_import_receipts WHERE ingress_id=?1 AND message_index=0","read exact wire-to-local import result");
            query.bind(1,receipt.id);check(query.row(),"actual anonymous import has a durable original message identity");local_message.value=query.int64(0);}
        const auto delivered=db.open_message(alice,local_message);
        check(delivered&&delivered.value->header.subject==mail.messages.front().subject&&
            delivered.value->header.target.kind==app::MessageKind::private_local&&delivered.value->header.target.recipient==alice.account,
            "exact local recipient can open the actual imported private NetMail through managed storage");
    };
    deliver(87100,true);deliver(87101,false);
    const auto reject=[&](app::FtnPacket mail,const char* reason){mail.header.password.fill(0);
        const auto receipt=wire_receive(std::move(mail),"2:100/2@test");const auto before=scalar(database,"SELECT count(*) FROM sfx_local_messages");
        const auto tossed=native::toss_ftn_native_receipt(db,c,installed,receipt,live);
        check(tossed.held&&!tossed.imported&&scalar(database,"SELECT count(*) FROM sfx_local_messages")==before,reason);
    };
    reject(packet(87102,true),"anonymous session cannot import echo or create relay mail");
    auto relay=packet(87103,false);relay.header.destination.node=88;relay.messages.front().destination_node=88;
    reject(relay,"anonymous session cannot relay private NetMail to a nonlocal destination");
    auto wire_missing=packet(87104,false);wire_missing.messages.front().to="No Such Local Caller";
    reject(wire_missing,"anonymous packet cannot guess a missing local recipient");
    c.settings.allow_direct_local_netmail=false;
    check(bool(db.change_ftn_configuration(owner,id(87105).bytes,c)),"direct-local policy disabled in isolated fixture");c=*db.ftn_configuration(owner).value;
    // This setting applies to unconfigured systems. A deliberately configured
    // empty-secret link retains its explicit local-delivery policy.
    deliver(87105,true);
    const auto before_unknown=scalar(database,"SELECT count(*) FROM sfx_local_messages");
    wire_receive(packet(87106,false),"2:100/99@test",true);
    check(scalar(database,"SELECT count(*) FROM sfx_local_messages")==before_unknown,"refused unknown sender cannot create a local message");
    const auto retained=db.claim_ftn_jobs(owner,"WIRE",wire_link.code,id(87200),id(87201),4,false);
    check(retained&&retained.value->size()==1&&retained.value->front().id.bytes==private_job.id.bytes,
        "anonymous inbound sessions leave exact private queued artifact available only to authorized host pickup");
}
