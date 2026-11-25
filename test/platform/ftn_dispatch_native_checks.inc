    {
        auto waiting=receive(1110,*app::encode_ftn_packet(packet(1110)),"pending.mo0");
        auto tic=receive(1111,"native inspection fixture","pending.tic");
        auto state=native::toss_ftn_native_receipt(db,c,installed,waiting,live);
        check(state.deferred&&!state.held&&!state.processed,"known pending uninspected same-custody TIC fences message claim");
        check(db.index_ftn_tic(owner,tic.id,c.revision,"other.zip",{}).succeeded(),"host-only synthetic admitted reference, not native TIC qualification");
        check(native::toss_ftn_native_receipt(db,c,installed,waiting,live).processed,"different explicit TIC payload does not claim unrelated message");
        auto ambiguous=receive(1112,*app::encode_ftn_packet(packet(1112)),"conflict.pkt");
        auto conflict=receive(1113,"native inspection fixture","conflict.tic");
        check(db.index_ftn_tic(owner,conflict.id,c.revision,"conflict.pkt",{}).succeeded(),"exact conflicting admitted reference retained");
        check(db.shutdown().succeeded()&&db.start().succeeded(),"restart retains classification/processing fence");
        state=native::toss_ftn_native_receipt(db,c,installed,ambiguous,live);check(state.deferred&&!state.held&&!state.processed&&db.ftn_toss_progress(owner,ambiguous.id).value->empty(),"explicit conflict is neither tossed nor moved to Bad after restart");
        auto later=receive(1114,"native inspection fixture","late.tic");
        const auto progress=db.ftn_toss_progress(owner,waiting.id).value->size();
        check(db.index_ftn_tic(owner,later.id,c.revision,"pending.mo0",{}).succeeded(),"late conflict can be recorded without reversing committed mail");
        check(db.ftn_ingress_receipt(owner,waiting.id).value->state==app::FtnIngressState::processed&&db.ftn_toss_progress(owner,waiting.id).value->size()==progress,"late TIC cannot create another import or reverse completed message receipt");
        auto dispatch_race_receipt=receive(1115,*app::encode_ftn_packet(packet(1115)),"race.pkt");bool injected{};
        const auto race=[&]{
            if(!injected&&db.ftn_ingress_receipt(owner,dispatch_race_receipt.id).value->state==app::FtnIngressState::processing){
                injected=true;const auto arriving=receive(1116,"native inspection fixture","race.tic");
                check(db.index_ftn_tic(owner,arriving.id,c.revision,"race.pkt",{}).succeeded(),"TIC arrives after durable message claim but before message commit");
            }return true;
        };
        state=native::toss_ftn_native_receipt(db,c,installed,dispatch_race_receipt,race);
        if(!injected||!state.deferred||state.held||state.processed||!db.ftn_toss_progress(owner,dispatch_race_receipt.id).value->empty())std::cerr<<"DISPATCH RACE injected="<<injected<<" reason="<<state.reason<<" held="<<state.held<<" processed="<<state.processed<<'\n';
        check(injected&&state.deferred&&!state.held&&!state.processed&&db.ftn_toss_progress(owner,dispatch_race_receipt.id).value->empty(),"atomic import rechecks late admitted TIC and retains both sides without importing");
    }
