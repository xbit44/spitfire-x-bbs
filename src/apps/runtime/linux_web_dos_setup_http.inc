    const auto dos_setup_stopped=[&]{
        if(runtime.status().lifecycle.state!=app::RuntimeState::stopped||runtime.management_work_active())throw std::runtime_error("Stop the BBS and wait for current work to finish before setting up DOS Doors.");
    };
    api_get("/api/config/doors/dos/status",[&](const auto& request,auto& response){
        if(!request.params.empty())throw std::runtime_error("dos_setup_request_invalid");
        auto result=dos_setup.status();result["setup_available"]=runtime.status().lifecycle.state==app::RuntimeState::stopped&&!runtime.management_work_active();
        if(!result["setup_available"].template get<bool>()&&!dos_setup.active())result["detail"]="Close this wizard, stop the BBS and wait for current work to finish. Reopen Set up DOS doors when ready.";
        json_response(response,result);
    });
    api_post("/api/config/doors/dos/start",[&](const auto& request,auto& response){dos_setup_stopped();json_response(response,dos_setup.start(parse(request)),202);});
    api_post("/api/config/doors/dos/cancel",[&](const auto& request,auto& response){require_keys(parse(request),{});json_response(response,dos_setup.cancel());});
    api_post("/api/config/doors/dos/save",[&](const auto& request,auto& response){dos_setup_stopped();json_response(response,dos_setup.save(parse(request)));});
    api_post("/api/config/doors/dos/game",[&](const auto& request,auto& response){dos_setup_stopped();json_response(response,dos_setup.inspect_game(parse(request)));});
    api_post("/api/config/doors/dos/prepare-game",[&](const auto& request,auto& response){dos_setup_stopped();json_response(response,dos_setup.prepare_game(parse(request)));});
