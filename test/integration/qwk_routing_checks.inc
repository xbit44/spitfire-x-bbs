// One explicit ordered configuration, real writer transactions, synthetic REP
// records. No live hub, uploaded owner packet or transport is implied.
{
    unsigned operation=20000;
    auto config=*db.ftn_configuration({true,{}}).value;
    const auto prior=config;
    config.networks.clear();config.links.clear();config.routes.clear();config.message_echoes.clear();
    app::FtnNetwork first;first.code="FIRST";first.name="First configured";first.address="1:200/1@first";first.enabled=true;first.default_link="HUB";
    auto second=first;second.code="SECOND";second.name="Second configured";second.address="2:300/1@second";second.additional_addresses={"1:105/7@second","1:105/8@second"};
    config.networks={first,second};
    for(const auto& n:config.networks){app::FtnLink peer;peer.network=n.code;peer.code="HUB";peer.name="Fixture hub";peer.address=n.code=="FIRST"?"1:200/2@first":"2:300/2@second";peer.host="127.0.0.1";peer.enabled=true;peer.credentials.session={1};config.links.push_back(peer);}
    const auto save=[&]{config.revision=db.ftn_configuration({true,{}}).value->revision;const auto result=db.change_ftn_configuration({true,{}},id(++operation).bytes,config);if(!result)std::cerr<<"Routing config: "<<result.status.operation<<'\n';check(bool(result),"save ordered synthetic routing configuration");};save();
    const auto routing_plan=db.begin_qwk_packet(bob,id(++operation));check(routing_plan&&routing_plan.value->netmail_conference,"routing fixture has reserved NetMail conference");
    const auto conference=*routing_plan.value->netmail_conference;const auto board=routing_plan.value->policy.board_id;
    const auto source=[&](std::string address){auto record=reply_record(conference,"Remote","@"+address,"Subject: Ranked "+std::to_string(++operation)+"\n\nBody\n");record.message.private_mail=true;return std::make_shared<ReplySource>(board,std::vector<app::QwkReplyRecord>{record});};
    const auto expect=[&](std::string address,std::string network,std::string aka){auto input=source(address);const auto before=scalar(file,"SELECT count(*) FROM sfx_local_messages");const auto result=db.import_qwk_reply(bob,input);if(!result)std::cerr<<result.status.operation<<'\n';check(result&&result.value->messages==1&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before+1,"ranked REP sends one message, no fanout");
        const auto sql="SELECT count(*) FROM sfx_ftn_message_targets WHERE message_id=(SELECT max(message_id) FROM sfx_local_messages) AND network='"+network+"' AND source_aka='"+aka+"'";
        check(scalar(file,sql.c_str())==1,"chosen network and exact source AKA match configured precedence");
        check(db.import_qwk_reply(bob,input).value->replayed&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before+1,"ranked logical replay adds no message");
    };
    expect("1:105/44","SECOND","1:105/7@second");
    expect("1:999/44","FIRST","1:200/1@first");
    expect("9:999/44","FIRST","1:200/1@first");
    config.networks[0].additional_addresses={"1:105/9@first"};save();
    expect("1:105/44","FIRST","1:105/9@first");
    expect("1:105/44@second","SECOND","1:105/7@second");
    std::reverse(config.networks.begin(),config.networks.end());save();
    expect("9:999/44","SECOND","2:300/1@second");
    config.networks[0].enabled=false;save();expect("1:105/44","FIRST","1:105/9@first");
    auto invalid=source("1:200/1");auto valid=source("1:105/44");auto packet=std::make_shared<ReplySource>(board,std::vector<app::QwkReplyRecord>{valid->records[0],invalid->records[0]});
    auto before=scalar(file,"SELECT count(*) FROM sfx_local_messages");check(!db.import_qwk_reply(bob,packet)&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before,"later unroutable/local destination rolls back whole packet");
    for(auto& peer:config.links)peer.enabled=false;
    for(auto& network:config.networks)network.default_link.clear();
    save();before=scalar(file,"SELECT count(*) FROM sfx_local_messages");
    check(!db.import_qwk_reply(bob,source("1:105/44"))&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before,"zero usable route refuses without publication");
    config=prior;save();
}
