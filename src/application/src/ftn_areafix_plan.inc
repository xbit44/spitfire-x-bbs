FtnAreaFixPlan plan_ftn_areafix(const FtnConfiguration& configuration,const FtnPeerMatch& requester,
    const std::vector<FtnAreaFixCommand>& commands,bool password_verified,bool paused){
    FtnAreaFixPlan out;out.configuration=configuration;out.paused=paused;
    const auto* source=link(configuration,requester);const auto* net=network(configuration,requester.network);
    if(!source||!net||!net->enabled||!source->enabled||!ftn_areafix_enabled(*source)||source->credentials.areafix.empty()||!password_verified||commands.empty()||commands.size()>256){out.response="AreaFix access denied. No subscriptions changed.\r";return out;}
    out.authorized=true;out.response="AreaFix results for your connection:\r";bool truncated{};unsigned rescan_total{};
    const auto say=[&](std::string text){if(out.response.size()+text.size()+1<=60000){out.response+=text;out.response+='\r';}else if(!truncated){out.response+="Response limit reached; further list/results omitted.\r";truncated=true;}};
    const auto permitted=[&](const FtnEchoPermission& p){return !p.file_echo&&p.manage&&same(p.network,net->code)&&same(p.link,source->code);};
    for(const auto& command:commands){
        switch(command.kind){
        case FtnAreaFixKind::help:say("+TAG add; -TAG drop; %LIST; %QUERY; %UNLINKED; %STATUS; %PAUSE; %RESUME; %RESCAN TAG [1..1000].");break;
        case FtnAreaFixKind::status:say(out.paused?"EchoMail delivery is paused.":"EchoMail delivery is enabled.");break;
        case FtnAreaFixKind::pause:out.paused=true;say("EchoMail delivery paused. Subscriptions retained.");break;
        case FtnAreaFixKind::resume:out.paused=false;say("EchoMail delivery resumed.");break;
        case FtnAreaFixKind::list:case FtnAreaFixKind::linked:case FtnAreaFixKind::unlinked:
            for(const auto& p:out.configuration.permissions)if(permitted(p)&&(command.kind==FtnAreaFixKind::list||(command.kind==FtnAreaFixKind::linked?p.send:!p.send)))say(std::string(p.send?"+ ":"- ")+p.tag);
            say("End of permitted echo list.");break;
        case FtnAreaFixKind::add:case FtnAreaFixKind::remove:case FtnAreaFixKind::rescan:{
            if(!valid_ftn_echo_tag(command.tag)){say("Invalid echo tag; no change.");break;}
            auto p=std::find_if(out.configuration.permissions.begin(),out.configuration.permissions.end(),[&](const auto& item){return permitted(item)&&same(item.tag,command.tag);});
            const auto echo=std::find_if(out.configuration.message_echoes.begin(),out.configuration.message_echoes.end(),[&](const auto& item){return same(item.network,net->code)&&same(item.tag,command.tag);});
            if(p==out.configuration.permissions.end()||echo==out.configuration.message_echoes.end()||!ftn_outbound(*source)){say(command.tag+": denied or unavailable.");break;}
            if(command.kind==FtnAreaFixKind::rescan){if(!p->send||!echo->store_locally||!echo->local_echo||!command.rescan_count||command.rescan_count>1000||rescan_total+command.rescan_count>1000){say(command.tag+": rescan refused; requires a linked locally stored echo and at most 1000 messages per request.");break;}rescan_total+=command.rescan_count;out.rescans.push_back(command);say(command.tag+": rescan requested; delivery remains pending.");break;}
            const bool add=command.kind==FtnAreaFixKind::add;if(p->send!=add){p->send=add;out.changed=true;}
            say(command.tag+(add?": local subscription enabled.":": local subscription disabled."));if(!add||!source->forward_missing_echoes)break;
            const auto* upstream=link(configuration,{net->code,source->forward_via_link});
            if(!upstream||!upstream->enabled||!ftn_outbound(*upstream)||!ftn_inbound(*upstream)||same(upstream->code,source->code)||upstream->credentials.areafix.empty()){say(command.tag+": upstream request unavailable; check the separately configured upstream.");break;}
            const auto destination=parse_ftn_address(upstream->address);if(!destination||!ftn_route_link(configuration,net->code,*destination)){say(command.tag+": upstream route unavailable; no request prepared.");break;}
            std::set<std::string> visited{upper(source->code)};const auto* provider=upstream;bool cycle{};while(provider){if(!visited.insert(upper(provider->code)).second){cycle=true;break;}if(!provider->forward_missing_echoes)break;provider=link(configuration,{net->code,provider->forward_via_link});}if(cycle){say(command.tag+": upstream forwarding cycle refused.");break;}
            auto uplink=std::find_if(out.configuration.permissions.begin(),out.configuration.permissions.end(),[&](const auto& item){return !item.file_echo&&same(item.network,net->code)&&same(item.link,upstream->code)&&same(item.tag,command.tag);});
            if(uplink==out.configuration.permissions.end()||!uplink->receive){say(command.tag+": upstream receive permission is missing; no request queued.");break;}
            // Keep the owner's upstream receive permission unchanged. Durable
            // request reconciliation decides whether this already-known tag
            // needs a new request; mere permission is not remote membership.
            if(std::none_of(out.upstream.begin(),out.upstream.end(),[&](const auto& item){return same(item.link,upstream->code)&&same(item.tag,echo->tag);}))out.upstream.push_back({upstream->code,echo->tag});
            say(command.tag+": upstream request prepared; acceptance has not been observed.");break;
        }
        default:say("Unsupported AreaFix command; no change for that command.");break;
        }
    }
    return out;
}
