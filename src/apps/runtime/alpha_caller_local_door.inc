                    if(launch.door.kind==DoorExecutionKind::local){
                        const auto account=owner.storage.find_account_by_id(actor->account);
                        if(!account||!account.value||account.value->account_id<1||account.value->account_id>UINT32_MAX)return DoorEnd::launch_failed;
                        if(!calling_time_tick())return DoorEnd::calling_time;
                        const auto sequence=state->begin_door(launch.door.id,launch.door.name,DoorExecutionKind::local);if(!sequence)return {};
                        door_active=true;
                        struct LocalStateRestore {Connection& caller;std::uint64_t sequence;~LocalStateRestore(){caller.door_active=false;caller.state->end_door(sequence);}} restore{*this,sequence};
                        const auto record=[&](const char* code,OperationalOutcome outcome){auto context=log_context;context.resource_id=launch.door.id;context.resource_name=launch.door.name;context.diagnostic_code=std::string("door_")+code;owner.add_log(OperationalKind::stage_changed,std::move(context),outcome);};
                        record("entered",OperationalOutcome::normal);
                        (void)owner.storage.append_caller_activity(ready->durable_session_id,{CallerActivityKind::door_entered,owner.clock.unix_seconds(),launch.door.name},state->snapshot().hostname);
                        bool telnet_owned=false;
                        if(!ssh){telnet_owned=static_cast<AlphaTelnetStream&>(*transport).begin_gateway();if(!telnet_owned){last_menu_outcome=InteractiveStreamOutcome::disconnected;return {};}}
                        struct LocalTransportRestore {Connection& caller;bool telnet;~LocalTransportRestore(){if(telnet&&!static_cast<AlphaTelnetStream&>(*caller.transport).end_gateway()){caller.last_menu_outcome=InteractiveStreamOutcome::disconnected;shutdown(caller.socket.get(),SD_BOTH);}}} transport_restore{*this,telnet_owned};
                        auto prefix=terminal.take_binary_input_prefix();std::uint64_t next_authority{};
                        DoorChannel channel;
                        channel.keyboard=[&]{inactivity.keyboard(GetTickCount64());if(post_login_inactivity)post_login_inactivity->keyboard(GetTickCount64());dismiss_notifications();};
                        channel.read=[&](std::size_t maximum){return ssh?static_cast<AlphaSshStream&>(*transport).read_ready(maximum):static_cast<AlphaTelnetStream&>(*transport).read_ready(maximum);};
                        channel.write=[&](std::string_view bytes){return ssh?static_cast<AlphaSshStream&>(*transport).write_ready(bytes):static_cast<AlphaTelnetStream&>(*transport).write_ready(bytes);};
                        channel.stop=[&]()->std::optional<DoorEnd>{
                            if(state->stopping.load()||owner.stopping.load())return owner.stopping.load()?DoorEnd::shutdown:DoorEnd::caller_disconnected;
                            if(state->door_termination_requested(sequence))return DoorEnd::operator_terminated;
                            if(!calling_time_tick())return calling_time_expired?DoorEnd::calling_time:DoorEnd::authority_lost;
                            const auto now=GetTickCount64();if(now>=next_authority){const auto current=message_actor();const auto policy=owner.storage.security_policy();if(!current||!policy||!policy.value)return DoorEnd::authority_lost;inactivity.set_sysop_exemption(live_security_level.load()>=policy.value->sysop_threshold);next_authority=now+1000;}
                            const auto idle=inactivity.poll(now);if(idle==CallerInactivity::Action::disconnect){last_menu_outcome=InteractiveStreamOutcome::timed_out;return DoorEnd::inactivity;}
                            if(idle==CallerInactivity::Action::warn){afk_notice=true;afk_music_sent=false;}return {};
                        };
                        LocalDoorCaller caller;caller.account=static_cast<std::uint32_t>(actor->account);caller.alias=account.value->display_name.value_or("Caller");caller.node=state->identity().node;caller.security_level=live_security_level.load();
                        if(const auto remaining=effective_seconds())caller.remaining_seconds=static_cast<std::uint64_t>(std::max<std::int64_t>(0,*remaining));
                        caller.generation=state->identity().generation;caller.sequence=sequence;caller.captured_utc_seconds=owner.clock.unix_seconds();
                        auto& drop_context=caller.drop_context;drop_context.elapsed_seconds=static_cast<std::uint32_t>(std::min<std::uint64_t>((owner.clock.monotonic_millis()-menu_started)/1000,UINT32_MAX));drop_context.sysop=owner.sysop_display_name();drop_context.sysop_access=live_sysop.load();
                        drop_context.user_key="sfx-account-"+std::to_string(caller.account)+"-"+std::to_string(account.value->created_utc_seconds);
                        const bool use_ini=launch.door.program.drop_file==DoorDropFile::doordrop_ini;
                        if (use_ini) {
                            const auto key=owner.storage.door_user_key(actor->account);
                            if (!key || !key.value) return DoorEnd::launch_failed;
                            drop_context.user_key=*key.value;
                            drop_context.software_version=SFX_PRODUCT_VERSION_SHORT_STRING;
                            if (calling_time_snapshot_utc) caller.captured_utc_seconds=calling_time_snapshot_utc;
                        }
                        {std::lock_guard<std::mutex> lock(owner.state_mutex);caller.board_name=owner.configuration.bbs_name;drop_context.nodes=owner.configuration.logical_nodes;
                            if(!owner.time_context||!owner.time_context->local)return DoorEnd::launch_failed;
                            const auto now=owner.time_context->local->format_utc_seconds(caller.captured_utc_seconds),logon=owner.time_context->local->format_utc_seconds(caller.captured_utc_seconds-drop_context.elapsed_seconds);
                            if(!now||!logon||now->size()<19||logon->size()<19)return DoorEnd::launch_failed;
                            if (use_ini) {
                                const auto local=owner.time_context->local->project_utc_seconds(caller.captured_utc_seconds);
                                if (!local) return DoorEnd::launch_failed;
                                const auto magnitude=std::abs(local->offset_minutes);
                                char offset[16]{};
                                std::snprintf(offset,sizeof(offset),"%c%02d:%02d",local->offset_minutes<0?'-':'+',magnitude/60,magnitude%60);
                                drop_context.file_time=*now;
                                drop_context.file_time[10]='T';
                                drop_context.file_time+=offset;
                            }
                            const auto date=[](const std::string& iso){return iso.substr(5,2)+"/"+iso.substr(8,2)+"/"+iso.substr(2,2);};
                            const auto seconds=[](const std::string& iso){return static_cast<std::uint32_t>(std::stoi(iso.substr(11,2))*3600+std::stoi(iso.substr(14,2))*60+std::stoi(iso.substr(17,2)));};
                            drop_context.date=date(*now);drop_context.clock=now->substr(11,5);drop_context.logon_date=date(*logon);drop_context.logon_clock=logon->substr(11,5);drop_context.seconds_today=seconds(*now);drop_context.logon_seconds_today=seconds(*logon);
                        }
                        const auto node_snapshot=state->snapshot();drop_context.columns=node_snapshot.terminal_columns;drop_context.rows=node_snapshot.terminal_rows;
                        const auto qwk=owner.storage.qwk_policy({true,{}});if(!qwk||!qwk.value)return DoorEnd::launch_failed;drop_context.board_code=qwk.value->board_id;
                        const auto current_menu=ports.snapshot();caller.ansi=current_menu&&current_menu->graphics==GraphicsMode::ansi;
                        release_door_reservation=false;
                        auto result=run_alpha_local_door(owner.database_path.parent_path().parent_path(),owner.installation_owner_sid,launch.door,caller,channel,std::move(prefix.bytes),prefix.discard_delimiter_lf);
                        release_door_reservation=result.cleanup_complete;
                        if(result.work_preserved||!result.cleanup_complete||result.cleanup_command_failed)
                            owner.journal->problem(AttentionKind::work_preserved,"Door "+std::to_string(launch.door.id)+" / Node "+std::to_string(caller.node)+(result.retained_location.empty()?" / Node or startup files":" / "+result.retained_location),"door_work_incomplete_preserved");
                        // Persist bounded machine diagnostics in the existing sysop log.
                        // Never emit commands, environment/session data or raw exceptions.
                        if(result.diagnostic.rfind("door_dos_",0)==0&&result.diagnostic.size()<=96&&
                           std::all_of(result.diagnostic.begin(),result.diagnostic.end(),[](unsigned char c){return (c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_';})){
                            auto context=log_context;context.resource_id=launch.door.id;context.resource_name=launch.door.name;context.diagnostic_code=result.diagnostic;
                            owner.add_log(OperationalKind::stage_changed,std::move(context),OperationalOutcome::unavailable);
                        }

                        if(!result.cleanup_complete){state->cleanup_health->failed=true;last_menu_outcome=InteractiveStreamOutcome::disconnected;record("cleanup_failed",OperationalOutcome::terminal_failure);return {};}
                        record(door_result_code(result.end),result.end==DoorEnd::returned?OperationalOutcome::normal:OperationalOutcome::cancelled);
                        if(result.cleanup_command_failed)record("cleanup_command_failed",OperationalOutcome::terminal_failure);
                        (void)owner.storage.append_caller_activity(ready->durable_session_id,{CallerActivityKind::door_returned,owner.clock.unix_seconds(),launch.door.name},state->snapshot().hostname);
                        if(result.end==DoorEnd::game_disconnect){state->stopping=true;shutdown(socket.get(),SD_BOTH);}
                        return result.end;
                    }
