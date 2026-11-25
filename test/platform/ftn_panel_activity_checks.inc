{
    app::FtnRuntimeResponse ready;ready.code=app::FtnConfigurationCode::ok;ready.mode=app::FtnMailerMode::native;ready.capable=true;ready.saved_revision=ready.active_revision=8;ready.diagnostic="ftn_native_active";
    app::FtnRuntimeResponse queue=ready;queue.queues.emplace();s.binkp.apply(ready);s.binkp.show_queues(queue);
    unsigned polls{};
    const auto request=[&](const app::FtnRuntimeRequest& r){auto out=ready;
        if(r.action==app::FtnRuntimeAction::connections)out.connections.push_back({"n-private-network","n-private-link","FidoNet / Synthetic Hub",true,true});
        else {check(r.action==app::FtnRuntimeAction::poll&&r.network=="n-private-network"&&r.link=="n-private-link","friendly selection preserves private routing identity");++polls;out.activity.push_back({1793440000,0,"FidoNet","Synthetic Hub","Poll requested; waiting for a connection slot."});}
        return out;
    };
    s.binkp.poll_now(request);check(polls==1&&!s.binkp.queue_view&&window_text(s.binkp.log).find(L"Poll requested")!=std::wstring::npos&&window_text(s.binkp.log).find(L"n-private")==std::wstring::npos,"one eligible hub polls directly and replaces queue view with named activity");
    ready.activity={{1793440001,1,"FidoNet","Synthetic Hub","Resolving hub address."},{1793440002,1,"FidoNet","Synthetic Hub","Connecting to 127.0.0.1:24777."},{1793440003,1,"FidoNet","Synthetic Hub","Session failed: Could not connect. Unacknowledged mail remains queued."}};
    s.binkp.apply(ready);const auto log=window_text(s.binkp.log);check(log.find(L"2026-")!=std::wstring::npos&&log.find(L"Resolving")<log.find(L"Connecting")&&log.find(L"Connecting")<log.find(L"Session failed"),"timestamped fast session phases remain visible between refreshes");
    State capture{window,g_appearance};render_png(capture,out/(theme?"panel-retro-activity.png":"panel-classic-activity.png"));
    {auto full=ready;full.activity.resize(32,full.activity.back());s.binkp.apply(full);check(SendMessageW(s.binkp.log,EM_GETFIRSTVISIBLELINE,0,0)>0,"long activity log scrolls to newest result even without editor focus");s.binkp.apply(ready);}
    s.binkp.show_queues(queue);SendMessageW(window,WM_COMMAND,BinkPConsole::Activity,0);check(!s.binkp.queue_view&&window_text(s.binkp.log).find(L"Connecting")!=std::wstring::npos,"Activity Log button returns from queues without another poll");
    s.binkp.poll_now([](const auto&){return app::FtnRuntimeResponse{};});check(window_text(s.binkp.log).find(L"Could not load FTN connections")!=std::wstring::npos,"connection query failure is visible");
    s.binkp.poll_now([&](const auto&){return ready;});check(window_text(s.binkp.log).find(L"No connections configured")!=std::wstring::npos,"empty configuration gives actionable feedback");
    s.binkp.poll_now([&](const auto&){auto r=ready;r.connections.push_back({"n-private","n-private","Disabled Hub",false,true});return r;});check(window_text(s.binkp.log).find(L"No enabled outgoing")!=std::wstring::npos,"disabled connections do not silently do nothing");
    s.binkp.poll_now([&](const auto& r){if(r.action==app::FtnRuntimeAction::connections)return request(r);return app::FtnRuntimeResponse{};});check(window_text(s.binkp.log).find(L"Poll unavailable")!=std::wstring::npos,"rejected poll does not claim success");
}
