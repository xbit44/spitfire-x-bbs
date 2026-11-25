{
    Fixture receiver(root/L"AuthReceiver",11,12),sender(root/L"AuthSender",12,11);
    receiver.ready();sender.ready();
    app::SoundPollRequest auth_consumer{id(60011),id(60100),0,true};
    const auto initial=receiver.sounds->poll(auth_consumer,silent,GetTickCount64());auth_consumer.after=initial.through;auth_consumer.fresh_desktop_start=false;
    const auto wrong_secret=native::protect_ftn_secret(sender.root.u8string(),"TEST","PEER",native::FtnCredentialKind::session,"different-test-key");
    check(bool(wrong_secret),"wrong synthetic key protected for engine diagnostic");
    sender.config.links.front().credentials.session=*wrong_secret;sender.config.links.front().port=receiver.engine->snapshot().classic_port;sender.save(2);sender.apply();sender.ready();
    receiver.queue(990,"private bytes must remain queued");check(sender.engine->poll("TEST","PEER"),"diagnostic wrong-password loopback poll starts");
    await([&]{const auto s=receiver.engine->snapshot();return std::any_of(s.activity.begin(),s.activity.end(),[](const auto& a){return a.message.find("CRAM format valid; digest does not match")!=std::string::npos;});},"engine publishes precise digest rejection");
    const auto rejected=receiver.engine->snapshot();
    check(std::any_of(rejected.activity.begin(),rejected.activity.end(),[](const auto& a){return a.network=="TEST"&&a.link=="PEER"&&a.message.find("CRAM format valid")!=std::string::npos;}),"failed authentication retains selected candidate for friendly name resolution");
    check(std::none_of(rejected.sessions.begin(),rejected.sessions.end(),[](const auto& s){return s.transfer.peer_verified||s.transfer.handshake_complete||s.transfer.sent_files||s.transfer.received_files;}),"diagnostic candidate does not grant authentication or transfer");
    const auto rejected_sounds=receiver.sounds->poll(auth_consumer,silent,GetTickCount64());check(rejected_sounds.events.empty(),"candidate match does not emit accepted inbound sound");
    const auto queued=receiver.db->ftn_queue_page({true,{}},0);check(queued&&queued.value->outbound[0]==1&&queued.value->outbound[3]==0,"failed auth leaves private outbound custody queued");
    check(std::all_of(rejected.activity.begin(),rejected.activity.end(),[](const auto& a){return a.message.size()<=192&&a.message.find("fixture-session-key")==std::string::npos&&a.message.find("different-test-key")==std::string::npos;}),"diagnostic activity fits IPC bounds without either secret");
    for(const auto& a:rejected.activity)std::cout<<"AUTH ACTIVITY "<<a.message<<'\n';
}
