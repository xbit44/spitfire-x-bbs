{
    await([&]{const auto s=client.engine->snapshot();return std::any_of(s.activity.begin(),s.activity.end(),[](const auto& a){return a.message.find("Session completed")!=std::string::npos;});},"completed activity published after final session observation");
    const auto state=client.engine->snapshot();
    for(const auto& a:state.activity)std::cout<<"ACTIVITY "<<a.message<<std::endl;
    const auto has=[&](const char* text){return std::any_of(state.activity.begin(),state.activity.end(),[&](const auto& a){return a.message.find(text)!=std::string::npos;});};
    check(has("Poll requested")&&has("Resolving")&&has("Connecting")&&has("Connected")&&has("handshake accepted")&&has("Sending")&&has("acknowledged")&&has("Received")&&has("Session completed"),"real loopback session retains request, connection, handshake acceptance, transfer, acknowledgement and completion events");
    check(state.activity.size()<=32&&std::all_of(state.activity.begin(),state.activity.end(),[](const auto& a){return a.utc_seconds>1700000000&&a.message.find("fixture-session-key")==std::string::npos;}),"actual event timestamps present; credentials never enter activity");
    const auto inbound=server.engine->snapshot();
    check(std::any_of(inbound.activity.begin(),inbound.activity.end(),[](const auto& a){return !a.network.empty()&&!a.link.empty()&&a.message=="Address matched configured connection; authentication pending.";}),"inbound activity identifies configured candidate before authenticated transfer");
}
