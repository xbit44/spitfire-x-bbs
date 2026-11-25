                owner.chat_hub.safe(chat_identity,false);
DoorSessionPorts doors{owner.storage,ports,message_actor,{}};
                const auto bulletin_sources=platform::win32::door_bulletin_sources(owner.database_path.parent_path().parent_path(),owner.installation_owner_sid,[weak=std::weak_ptr<AlphaOperationalJournal>(owner.journal)](DoorBulletinId id,std::string_view diagnostic){if(const auto journal=weak.lock())journal->problem(AttentionKind::door_bulletin_unavailable,"Door Bulletin "+std::to_string(id.value),std::string(diagnostic));});
                doors.bulletin_refresh=[&](DoorBulletinId id,TrustedDisplaySelectionRequest request,std::int64_t revision){const auto actor=message_actor();if(!actor)return DoorBulletinRefreshResult{{StorageErrorCode::permission_denied,"Caller session ended"},{},{},{}};return refresh_door_bulletin(owner.storage,{false,*actor},id,std::move(request),bulletin_sources,revision);};
                DoorAutomaticEvent active_door_event{DoorAutomaticEvent::none};
                std::optional<MenuDoorSelection> active_menu_door;
                const auto authorize_door=[&](const MessageActor& actor,const DoorLaunch& selected){
                    if(active_menu_door)return owner.storage.menu_door_launch(actor,*active_menu_door);
                    return active_door_event==DoorAutomaticEvent::none?
                        owner.storage.door_launch(actor,{selected.menu.id,selected.entry.number,selected.catalog_revision}):
                        owner.storage.door_event_launch(actor,{selected.door.id,active_door_event,selected.catalog_revision});
                };
                doors.launch=[&](const DoorLaunch& selected)->std::optional<DoorEnd>{
                    owner.chat_hub.safe(chat_identity,false);
                    const auto actor=message_actor();if(!actor)return {};
                    const auto authorized=authorize_door(*actor,selected);
                    if(!authorized||!authorized.value)return door_end(RloginEnd::connection_failed);
                    const auto launch=*authorized.value;const auto& target=launch.door.rlogin;
                    const bool telgate=launch.door.kind==DoorExecutionKind::telnet;
                    auto reservation=owner.door_reservations.acquire(launch.door.id,launch.door.allow_multiple);if(!reservation)return DoorEnd::busy;
                    bool release_door_reservation=true;
                    struct ReservationRestore {DoorReservations::Lease& lease;bool& clean;~ReservationRestore(){if(clean)lease.release_after_cleanup();}} reservation_restore{*reservation,release_door_reservation};
#include "alpha_caller_local_door.inc"
                    RloginSecret credential,pepper;
                    if(!telgate&&target.prompt_credential){
                        const auto current=owner.storage.load_credential(actor->account);
                        auto secret=current&&current.value?caller_login_secret.for_account(actor->account,*current.value):std::nullopt;
                        if(!secret){caller_login_secret.clear();
                            if(!write_all(stream,(ready->graphics==GraphicsMode::ansi?menu_text_sgr(7):std::string{})+"\r\nYour login password is no longer available for this connection. Log off and sign in again.\r\n"))return {};
                            return door_end(RloginEnd::handshake_failed);}
                        credential=std::move(*secret);
                    }else if(!telgate&&!target.protected_credential.empty()){
                        auto secret=platform::win32::unprotect_rlogin_secret(owner.database_path.parent_path().parent_path(),target.protected_credential);
                        if(!secret)return door_end(RloginEnd::handshake_failed);credential=std::move(*secret);
                    }
                    if(!telgate&&!target.protected_pepper.empty()){
                        auto secret=platform::win32::unprotect_rlogin_secret(owner.database_path.parent_path().parent_path(),target.protected_pepper);
                        if(!secret)return door_end(RloginEnd::handshake_failed);pepper=std::move(*secret);
                    }
                    const auto latest_actor=message_actor();if(!latest_actor)return {};
                    const auto latest=authorize_door(*latest_actor,launch);
                    if(!latest||!latest.value)return door_end(RloginEnd::connection_failed);
                    const auto account=owner.storage.find_account_by_id(latest_actor->account);
                    if(!account||!account.value||account.value->account_id<1||account.value->account_id>UINT32_MAX||
                        account.value->created_utc_seconds<1||account.value->created_utc_seconds>UINT32_MAX)return door_end(RloginEnd::handshake_failed);
                    RloginCallerIdentity identity{account.value->display_name.value_or("Caller"),static_cast<std::uint32_t>(account.value->account_id),static_cast<std::uint32_t>(account.value->created_utc_seconds),{}};
                    if(!telgate&&target.authentication!=RloginAuthentication::traditional && target.salt.empty()){
                        const auto qwk=owner.storage.qwk_policy({true,{}});if(!qwk||!qwk.value)return door_end(RloginEnd::handshake_failed);identity.board_qwk_id=qwk.value->board_id;
                    }
                    if(!telgate&&!target.quiet){
                        if(!write_all(stream,(ready->graphics==GraphicsMode::ansi?menu_text_sgr(7):std::string{})+(active_menu_door?"\r\nRemote Door: Ctrl-] then D returns to the launching menu; Ctrl-] twice sends Ctrl-].\r\n":"\r\nRemote Door: Ctrl-] then D returns to this Door Menu; Ctrl-] twice sends Ctrl-].\r\n")))return {};
                        if(target.pause&&terminal.pause({"door.rlogin.connect"})!=TerminalOutcome::completed)return door_end(RloginEnd::gateway_escape);
                    }
                    if(!telgate&&target.banner&&!target.quiet&&!write_all(stream,(ready->graphics==GraphicsMode::ansi?menu_text_sgr(7):std::string{})+"Connecting to "+launch.door.name+" ("+target.host+":"+std::to_string(target.port)+")...\r\n"))return {};
                    if(telgate&&!write_all(stream,(ready->graphics==GraphicsMode::ansi?menu_text_sgr(7):std::string{})+"\r\nConnecting to "+launch.door.telnet.host+":"+std::to_string(launch.door.telnet.port)+"...\r\nPress Ctrl-X for the Telnet Gate menu.\r\nCtrl-X passes through in remote BINARY mode.\r\nWhile connecting, Ctrl-X then D cancels.\r\nRemote Telnet is unencrypted, including from an SSH caller.\r\n"))return {};
                    const auto sequence=state->begin_door(launch.door.id,launch.door.name,launch.door.kind);if(!sequence)return {};
                    door_active=true;
                    struct DoorStateRestore {Connection& caller;std::uint64_t sequence;~DoorStateRestore(){caller.door_active=false;caller.state->end_door(sequence);}} restore{*this,sequence};
                    const auto record=[&](const char* code,OperationalOutcome outcome){
                        auto context=log_context;context.resource_id=launch.door.id;context.resource_name=telgate?launch.door.telnet.host.substr(0,100)+":"+std::to_string(launch.door.telnet.port):launch.door.name;context.diagnostic_code=std::string(telgate?"door_telgate_":"door_")+code;
                        const auto node=state->snapshot();context.duration_seconds=node.connected_seconds;owner.add_log(OperationalKind::stage_changed,std::move(context),outcome);
                    };
                    record("entered",OperationalOutcome::normal);
                    (void)owner.storage.append_caller_activity(ready->durable_session_id,{CallerActivityKind::door_entered,owner.clock.unix_seconds(),launch.door.name},state->snapshot().hostname);
                    bool telnet_owned=false;
                    if(!ssh){telnet_owned=static_cast<AlphaTelnetStream&>(*transport).begin_gateway();if(!telnet_owned){record("transport_failed",OperationalOutcome::terminal_failure);last_menu_outcome=InteractiveStreamOutcome::disconnected;shutdown(socket.get(),SD_BOTH);return {};}}
                    struct TransportRestore {Connection& caller;bool telnet;bool restored{true};~TransportRestore(){if(telnet&&!static_cast<AlphaTelnetStream&>(*caller.transport).end_gateway()){caller.last_menu_outcome=InteractiveStreamOutcome::disconnected;shutdown(caller.socket.get(),SD_BOTH);}}} transport_restore{*this,telnet_owned};
                    auto prefix=terminal.take_binary_input_prefix();bool termination_recorded{};std::uint64_t next_authority{};
                    RloginBridgePorts gateway;
                    gateway.now=[] {return GetTickCount64();};
                    gateway.keyboard=[&]{inactivity.keyboard(GetTickCount64());if(post_login_inactivity)post_login_inactivity->keyboard(GetTickCount64());dismiss_notifications();};
                    gateway.caller_read=[&](std::size_t maximum){return ssh?static_cast<AlphaSshStream&>(*transport).read_ready(maximum):static_cast<AlphaTelnetStream&>(*transport).read_ready(maximum);};
                    gateway.caller_write=[&](std::string_view bytes){return ssh?static_cast<AlphaSshStream&>(*transport).write_ready(bytes):static_cast<AlphaTelnetStream&>(*transport).write_ready(bytes);};
                    gateway.dimensions=[&]{const auto node=state->snapshot();return std::make_pair(node.terminal_rows,node.terminal_columns);};
                    gateway.caller_discard=[&]{return ssh||static_cast<AlphaTelnetStream&>(*transport).finish_gateway_output();};
                    gateway.stop=[&]()->std::optional<RloginEnd>{
                        if(state->stopping.load()||owner.stopping.load())return owner.stopping.load()?RloginEnd::shutdown:RloginEnd::caller_disconnected;
                        if(state->door_termination_requested(sequence)){if(!termination_recorded){record("terminate_requested",OperationalOutcome::cancelled);termination_recorded=true;}return RloginEnd::operator_terminated;}
                        if(!calling_time_tick())return calling_time_expired?RloginEnd::calling_time:RloginEnd::authority_lost;
                        const auto now=GetTickCount64();
                        if(now>=next_authority){
                            const auto current=message_actor();const auto policy=owner.storage.security_policy();
                            if(!current||!policy||!policy.value)return RloginEnd::authority_lost;
                            inactivity.set_sysop_exemption(live_security_level.load()>=policy.value->sysop_threshold);next_authority=now+1000;
                        }
                        const auto idle=inactivity.poll(now);if(idle==CallerInactivity::Action::disconnect){last_menu_outcome=InteractiveStreamOutcome::timed_out;return RloginEnd::inactivity;}
                        if(idle==CallerInactivity::Action::warn){afk_notice=true;afk_music_sent=false;}
                        return {};
                    };
                    TelgateLocalPorts local;local.ansi=ready->graphics==GraphicsMode::ansi;
                    std::optional<ChatIdentity> recipient;
                    const auto local_refresh=[&]{
                        const auto actor=message_actor();if(!actor)return false;
                        auto me=owner.chat_hub.participant(chat_identity);const auto preference=owner.storage.chat_preferences(*actor);const auto policy=owner.storage.security_policy();
                        if(!me||!preference||!preference.value||!policy||!policy.value)return false;
                        me->level=live_security_level.load();me->sysop=live_sysop.load();me->sysop_threshold=policy.value->sysop_threshold;me->preferences=*preference.value;
                        return owner.chat_hub.refresh(*me);
                    };
                    local.callers=[&]{std::string out;if(!local_refresh())return std::string("Callers unavailable.");for(const auto& caller:owner.chat_hub.visible(chat_identity)){out+="Node "+chat_node(caller.identity.node)+" "+caller.name+" - "+caller.activity+"\n";if(out.size()>3800){out+="Additional callers omitted.\n";break;}}return out;};
                    local.select_recipient=[&](unsigned node){recipient.reset();if(!local_refresh())return false;for(const auto& caller:owner.chat_hub.visible(chat_identity))if(caller.identity.node==node&&caller.identity!=chat_identity){recipient=caller.identity;return true;}return false;};
                    local.private_text=[&](std::string_view text){if(!recipient||!local_refresh())return std::string("Caller unavailable.");const auto result=owner.chat_hub.text(chat_identity,*recipient,text);recipient.reset();return result==ChatResult::ok?std::string("Private text queued."):std::string(chat_result_text(result));};
                    RloginBridgeResult result;
                    try{if(telgate)result=run_alpha_telnet(launch.door.telnet,gateway,local,std::move(prefix.bytes),prefix.discard_delimiter_lf);
                        else result=run_alpha_rlogin(target,identity,std::move(credential),std::move(pepper),gateway,std::move(prefix.bytes),prefix.discard_delimiter_lf,
                        [&](bool entry){if(owner.journal)owner.journal->sound(entry?SoundEvent::rlogin_entry:SoundEvent::rlogin_exit);});}
                    catch(...){result.end=RloginEnd::transport_failed;}
                    credential.clear();pepper.clear();
                    if(state->door_termination_requested(sequence)){if(!termination_recorded)record("terminate_requested",OperationalOutcome::cancelled);result.end=RloginEnd::operator_terminated;}
                    const auto outcome=result.end==RloginEnd::remote_closed?OperationalOutcome::normal:
                        result.end==RloginEnd::gateway_escape||result.end==RloginEnd::operator_terminated?OperationalOutcome::cancelled:
                        result.end==RloginEnd::shutdown?OperationalOutcome::shutdown:result.end==RloginEnd::inactivity||result.end==RloginEnd::calling_time?OperationalOutcome::timed_out:OperationalOutcome::disconnected;
                    record(rlogin_result_code(result.end),outcome);
                    (void)owner.storage.append_caller_activity(ready->durable_session_id,{CallerActivityKind::door_returned,owner.clock.unix_seconds(),launch.door.name},state->snapshot().hostname);
                    return door_end(result.end);
                };
                doors.event_outcome=[&](DoorId id,DoorAutomaticEvent event,DoorEnd end){
                    auto context=log_context;context.resource_id=id;
                    context.diagnostic_code=std::string(event==DoorAutomaticEvent::logon?"door_logon_":"door_logoff_")+door_result_code(end);
                    owner.add_log(OperationalKind::stage_changed,std::move(context),end==DoorEnd::returned?OperationalOutcome::normal:OperationalOutcome::cancelled);
                };
                ports.doors=[&]()->std::optional<DoorMenuExit>{return run_door_session(doors);};
                primary_door_commands=[&](MenuId menu)->std::optional<MenuDoorCommands>{
                    const auto actor=message_actor();if(!actor)return {};
                    const auto result=owner.storage.menu_door_commands(*actor,menu);return result&&result.value?result.value:std::nullopt;
                };
                primary_door_launch=[&](const MenuDoorSelection& selection)->std::optional<std::string>{
                    const auto actor=message_actor();if(!actor)return {};
                    const auto authorized=owner.storage.menu_door_launch(*actor,selection);
                    if(!authorized||!authorized.value)return "The Door or its access changed. Please choose again.";
                    active_menu_door=selection;
                    struct CommandRestore {std::optional<MenuDoorSelection>& selection;~CommandRestore(){selection.reset();}} command_restore{active_menu_door};
                    const auto screen=ports.snapshot();if(!screen)return {};
                    TrustedDisplaySelectionRequest pre;pre.identity=TrustedDisplayIdentity::door_prelaunch;pre.graphics=screen->graphics;
                    pre.authenticated_security_level=screen->access.security_level;pre.context=screen->context;pre.door_asset_number=authorized.value->door.id;
                    pre.context.door_name=authorized.value->door.name;pre.context.door_number=std::to_string(authorized.value->door.id);pre.context.door_menu=authorized.value->menu.name;
                    const auto artwork=ports.artwork(pre);if(artwork.available()){
                        auto program=artwork.program->terminal_program;
                        for(auto& op:program.operations)if(op.kind==TerminalDisplayOperationKind::paging_newline)op.kind=TerminalDisplayOperationKind::logical_newline;
                        if(!ports.display(program))return {};
                    }
                    const auto result=doors.launch(*authorized.value);if(!result)return {};
                    switch(*result){case DoorEnd::caller_disconnected:case DoorEnd::game_disconnect:case DoorEnd::shutdown:case DoorEnd::authority_lost:case DoorEnd::calling_time:case DoorEnd::inactivity:return {};default:break;}
                    if(*result==DoorEnd::busy)return "This Door is busy. Try again later.";
                    if(*result==DoorEnd::launch_failed||*result==DoorEnd::unavailable)return "The Door could not be started.";
                    return "Returned from "+authorized.value->door.name+".";
                };
                ports.door_commands=primary_door_commands;ports.door_command=primary_door_launch;
                ports.door_event=[&](DoorAutomaticEvent event){
                    if(state->stopping.load()||owner.stopping.load())return false;
                    active_door_event=event;
                    struct EventRestore {DoorAutomaticEvent& event;~EventRestore(){event=DoorAutomaticEvent::none;}} event_restore{active_door_event};
                    return run_door_events(doors,event);
                };
