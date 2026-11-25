            if(launch.door.kind!=app::DoorExecutionKind::local){
                const bool telgate=launch.door.kind==app::DoorExecutionKind::telnet;
                auto reservation=reservations.acquire(launch.door.id,launch.door.allow_multiple);if(!reservation)return app::DoorEnd::busy;
                struct Release{app::DoorReservations::Lease& lease;~Release(){lease.release_after_cleanup();}}release{*reservation};
                const auto& target=launch.door.rlogin;app::RloginSecret credential,pepper;
                const auto open_secret=[&](const std::vector<std::uint8_t>& encrypted)->std::optional<app::RloginSecret>{
                    const auto keys=host::load_portable_board_key(root);if(!keys)return {};
                    return platform::portable::open_portable_credential(keys.value->key,keys.value->board,{'S','F','X','R'},"RLOGIN",encrypted);
                };
                if(!telgate&&target.prompt_credential){
                    const auto current=database().load_credential(actor->account);
                    auto secret=current&&current.value?caller.login_secret.for_account(actor->account,*current.value):std::nullopt;
                    if(!secret){caller.login_secret.clear();if(!write_all(guarded,"\r\nYour login password is no longer available for this connection. Log off and sign in again.\r\n"))return {};return app::DoorEnd::launch_failed;}
                    credential=std::move(*secret);
                }else if(!telgate&&!target.protected_credential.empty()){
                    auto secret=open_secret(target.protected_credential);if(!secret)return app::DoorEnd::launch_failed;credential=std::move(*secret);
                }
                if(!telgate&&!target.protected_pepper.empty()){
                    auto secret=open_secret(target.protected_pepper);if(!secret)return app::DoorEnd::launch_failed;pepper=std::move(*secret);
                }
                const auto account=database().find_account_by_id(actor->account);
                if(!account||!account.value||account.value->account_id<1||account.value->account_id>UINT32_MAX||
                   account.value->created_utc_seconds<1||account.value->created_utc_seconds>UINT32_MAX)return app::DoorEnd::launch_failed;
                app::RloginCallerIdentity identity{account.value->display_name.value_or("Caller"),static_cast<std::uint32_t>(account.value->account_id),static_cast<std::uint32_t>(account.value->created_utc_seconds),{}};
                if(!telgate&&target.authentication!=app::RloginAuthentication::traditional&&target.salt.empty()){
                    const auto qwk=database().qwk_policy({true,{}});if(!qwk||!qwk.value)return app::DoorEnd::launch_failed;identity.board_qwk_id=qwk.value->board_id;
                }
                if(!telgate&&!target.quiet){
                    if(!write_all(guarded,"\r\nRemote Door: Ctrl-] then D returns to this menu; Ctrl-] twice sends Ctrl-].\r\n"))return {};
                    if(target.pause&&terminal.pause({"door.rlogin.connect"})!=app::TerminalOutcome::completed)return app::DoorEnd::escaped;
                }
                if(!telgate&&target.banner&&!target.quiet&&!write_all(guarded,"Connecting to "+launch.door.name+" ("+target.host+":"+std::to_string(target.port)+")...\r\n"))return {};
                if(telgate&&!write_all(guarded,"\r\nConnecting to "+launch.door.telnet.host+":"+std::to_string(launch.door.telnet.port)+"...\r\nPress Ctrl-X for the Telnet Gate menu.\r\nCtrl-X passes through in remote BINARY mode.\r\nWhile connecting, Ctrl-X then D cancels.\r\nRemote Telnet is unencrypted, including from an SSH caller.\r\n"))return {};
                const auto latest_actor=message_actor();if(!latest_actor)return {};
                const auto latest=authorize_door(*latest_actor,launch);if(!latest||!latest.value)return app::DoorEnd::authority_lost;
                const auto sequence=assignment->begin_door(launch.door.id,launch.door.name,launch.door.kind);if(!sequence)return {};
                auto* telnet=ssh?nullptr:&static_cast<AlphaTelnetStream&>(stream);
                if(telnet&&!telnet->begin_gateway()){assignment->end_door(sequence);return {};}
                struct Restore{AlphaTelnetStream* stream;std::shared_ptr<app::CallerAssignment> state;std::uint64_t sequence;
                    ~Restore(){if(stream&&!stream->end_gateway())state->cleanup_health->failed=true;state->end_door(sequence);}}restore{telnet,assignment,sequence};
                const auto record=[&](std::string code,app::DoorEnd end){record_door(launch.door.id,launch.door.name,std::string(telgate?"door_telgate_":"door_")+code,end);};
                record("entered",app::DoorEnd::returned);durable->activity(ready.durable_session_id,{app::CallerActivityKind::door_entered,clock.unix_seconds(),launch.door.name},{});
                app::RloginBridgePorts gateway;
                gateway.now=[&]{return clock.monotonic_millis();};gateway.keyboard=[&]{caller.keyboard();};
                gateway.caller_read=[&](auto maximum){return ssh?static_cast<AlphaSshStream&>(stream).read_ready(maximum):telnet->read_ready(maximum);};
                gateway.caller_write=[&](auto bytes){return ssh?static_cast<AlphaSshStream&>(stream).write_ready(bytes):telnet->write_ready(bytes);};
                gateway.caller_discard=[&]{return !telnet||telnet->finish_gateway_output();};
                gateway.dimensions=[&]{const auto node=assignment->snapshot();return std::make_pair(node.terminal_rows,node.terminal_columns);};
                std::uint64_t next_authority{};
                gateway.stop=[&]()->std::optional<app::RloginEnd>{
                    if(assignment->stopping)return app::RloginEnd::shutdown;
                    if(assignment->door_termination_requested(sequence))return app::RloginEnd::operator_terminated;
                    if(!caller.tick())return caller.timed_out?app::RloginEnd::calling_time:app::RloginEnd::authority_lost;
                    const auto now=clock.monotonic_millis();if(now>=next_authority){if(!message_actor())return app::RloginEnd::authority_lost;next_authority=now+1000;}
                    return {};
                };
                app::TelgateLocalPorts local_ports;const auto current=ports.snapshot();if(!current)return {};local_ports.ansi=current->graphics==app::GraphicsMode::ansi;
                std::optional<app::ChatIdentity> recipient;
                local_ports.callers=[&]{std::string out;if(!chat_refresh())return std::string("Callers unavailable.");for(const auto& person:chat.visible(chat_identity)){out+="Node "+std::to_string(person.identity.node)+" "+person.name+" - "+person.activity+"\n";if(out.size()>3800){out+="Additional callers omitted.\n";break;}}return out;};
                local_ports.select_recipient=[&](unsigned node){recipient.reset();if(!chat_refresh())return false;for(const auto& person:chat.visible(chat_identity))if(person.identity.node==node&&person.identity!=chat_identity){recipient=person.identity;return true;}return false;};
                local_ports.private_text=[&](std::string_view text){if(!recipient||!chat_refresh())return std::string("Caller unavailable.");const auto result=chat.text(chat_identity,*recipient,text);recipient.reset();return result==app::ChatResult::ok?std::string("Private text queued."):std::string(app::chat_result_text(result));};
                auto prefix=terminal.take_binary_input_prefix();app::RloginBridgeResult result;
                try{
                    if(telgate)result=run_linux_telnet(root,launch.door.telnet,gateway,local_ports,std::move(prefix.bytes),prefix.discard_delimiter_lf);
                    else result=run_linux_rlogin(root,target,identity,std::move(credential),std::move(pepper),gateway,std::move(prefix.bytes),prefix.discard_delimiter_lf,
                        [&](bool entry){journal->sound(entry?app::SoundEvent::rlogin_entry:app::SoundEvent::rlogin_exit);});
                }catch(...){result.end=app::RloginEnd::transport_failed;}
                credential.clear();pepper.clear();
                if(assignment->door_termination_requested(sequence))result.end=app::RloginEnd::operator_terminated;
                const auto end=app::door_end(result.end);record(app::rlogin_result_code(result.end),end);
                durable->activity(ready.durable_session_id,{app::CallerActivityKind::door_returned,clock.unix_seconds(),launch.door.name},{});
                return end;
            }
