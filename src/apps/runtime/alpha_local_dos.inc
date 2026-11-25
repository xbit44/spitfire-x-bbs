// Uses the same owned job, private pipes and identity-bound file lifetime as
// native Doors. The external host owns no BBS database or admin connection.
namespace dos_protocol=core::dos_host;
std::string dos_short(const Path& path){
    std::array<wchar_t,32768> text{};const auto n=GetShortPathNameW(path.c_str(),text.data(),static_cast<DWORD>(text.size()));require(n&&n<text.size(),"door_dos_short_path_unavailable");std::string result;
    for(unsigned i=0;i<n;++i){require(text[i]>=32&&text[i]<127&&text[i]!=' ',"door_dos_path_requires_ascii_short_name");result+=static_cast<char>(text[i]);}return result;
}
bool dos_short_directory(std::string_view value) {
    if (value.size() > 64 || !dos_protocol::detail::path(value)) return false;
    value.remove_prefix(3);
    while (!value.empty()) {
        const auto slash = value.find('\\');
        const auto part = value.substr(0,slash);
        const auto dot = part.find('.');
        if (part.empty() || part.substr(0,dot).empty() || part.substr(0,dot).size() > 8 ||
            (dot != part.npos && (part.size()-dot-1 > 3 || part.find('.',dot+1)!=part.npos))) return false;
        if (slash == value.npos) break;
        value.remove_prefix(slash+1);
    }
    return true;
}
// Host standard devices are harmless local NUL handles. Do not expose the
// caller pipes to CRT/NTVDM standard-device inheritance on any Windows version.
struct DosHostDevices {
    Handle input,output,error;
    DosHostDevices(){
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        input.reset(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
        output.reset(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
        error.reset(CreateFileW(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,OPEN_EXISTING,0,nullptr));
        require(input&&output&&error,"door_dos_local_devices_unavailable");
    }
};
struct DosCommand {std::string line;Handle pin;};
DosCommand dos_command(std::string expanded,const Path& startup,bool shell){
    DosCommand result;
    require(expanded.size()<=4096&&expanded.find_first_of("\r\n\0",0,3)==expanded.npos,"door_dos_command_invalid");
    if(shell){std::array<wchar_t,32768> system{};const auto count=GetSystemDirectoryW(system.data(),static_cast<UINT>(system.size()));require(count&&count<system.size(),"door_system_directory");const auto interpreter=Path(system.data())/L"COMMAND.COM";result.pin=inst::open_object(interpreter,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);expanded=dos_short(interpreter)+" /C "+expanded;}
    else {
        auto value=wide(expanded);std::size_t at=0;while(at<value.size()&&value[at]==L' ')++at;require(at<value.size(),"door_command_empty");const bool quoted=value[at]==L'"';const auto begin=at+(quoted?1:0);auto end=value.find(quoted?L'"':L' ',begin);if(end==value.npos){require(!quoted,"door_dos_command_quotes");end=value.size();}
        Path image=value.substr(begin,end-begin);if(image.is_relative())image=startup/image;if(image.extension().empty())image+=L".exe";result.pin=inst::open_object(image,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);DWORD type{};require(GetBinaryTypeW(image.c_str(),&type)&&type==SCS_DOS_BINARY,"door_dos_program_required");
        expanded=dos_short(image)+utf8(Path(value.substr(end+(quoted?1:0))));
    }
    require(expanded.size()<=120&&std::all_of(expanded.begin(),expanded.end(),[](unsigned char c){return c>=32&&c<127;}),"door_dos_command_exceeds_120_ascii_bytes");result.line=std::move(expanded);return result;
}
struct DoorKeys {
    bool discard_lf{},escape{},escaped{};std::uint64_t escape_at{};
    // Allow a human-paced second key, including over a delayed caller link.
    // Ctrl-] twice still forwards one literal byte immediately.
    void expire(std::string& out){if(escape&&GetTickCount64()-escape_at>=2000){out+=char(29);escape=false;}}
    void append(const std::vector<std::uint8_t>& bytes,std::string& out){expire(out);for(const auto c:bytes){
        if(discard_lf){discard_lf=false;if(c==10)continue;}
        if(escape){escape=false;if(c=='d'||c=='D'){escaped=true;return;}out+=char(29);if(c==29)continue;}
        else if(c==29){escape=true;escape_at=GetTickCount64();continue;}out+=static_cast<char>(c);
    }}
};
void private_write(Pipe& pipe,const std::vector<std::uint8_t>& bytes,const app::DoorChannel& channel){
    const auto until=GetTickCount64()+5000;std::size_t at{};
    while(at<bytes.size()||!pipe.ready()){
        require(GetTickCount64()<until&&!channel.stop(),"door_private_request_cancelled_or_blocked");
        if(at<bytes.size()){auto written=pipe.write({reinterpret_cast<const char*>(bytes.data()+at),bytes.size()-at});require(written.outcome==Outcome::completed||written.outcome==Outcome::timed_out,"door_private_pipe_closed");at+=written.bytes_written;}Sleep(2);
    }
}
struct HostStatus {
    std::vector<std::uint8_t> pending;bool started{},finished{},hangup{},closed{};std::uint32_t exit_code{};std::string error;
    void poll(Pipe& pipe){
        const auto read=pipe.read();if(read.outcome==Outcome::disconnected){closed=true;return;}if(read.outcome!=Outcome::completed)return;
        require(pending.size()+read.bytes.size()<=dos_protocol::maximum_frame*dos_protocol::maximum_queued_frames,"door_host_status_bound");pending.insert(pending.end(),read.bytes.begin(),read.bytes.end());unsigned frames{};
        while(pending.size()>=4){dos_protocol::detail::Reader size_reader{pending};const auto size=size_reader.u32();require(size>=16&&size<=dos_protocol::maximum_frame,"door_host_frame_invalid");if(pending.size()<size)break;
            require(++frames<=dos_protocol::maximum_queued_frames,"door_host_status_count");std::vector<std::uint8_t> one(pending.begin(),pending.begin()+size);pending.erase(pending.begin(),pending.begin()+size);const auto frame=dos_protocol::decode_frame(one);require(frame.has_value()&&!finished,"door_host_status_order");
            if(frame->message==dos_protocol::Message::started){require(!started&&!hangup,"door_host_duplicate_start");started=true;}
            else if(frame->message==dos_protocol::Message::hangup){require(started&&!hangup,"door_host_hangup_order");hangup=true;}
            else if(frame->message==dos_protocol::Message::finished){require(started,"door_host_finished_before_start");finished=true;exit_code=frame->value;}
            else if(frame->message==dos_protocol::Message::failed){finished=true;error=frame->diagnostic;if(frame->value)error+="_win32_"+std::to_string(frame->value);}
            else require(false,"door_host_status_type");
        }
    }
};
app::DoorProgramResult run_dos_command(const Path& root,const std::wstring& owner,const app::DoorDefinition& door,const LocalDoorCaller& caller,const Path& startup,const Path& node,const Path& drop,const app::DoorDriverSettings& settings,const app::DoorChannel& channel,const std::string& command_line,const std::string& drop_path,std::vector<std::uint8_t> prefix,bool discard_lf,bool cleanup,bool* program_started=nullptr){
    app::DoorProgramResult result;Child child;DropFiles private_files;DWORD host_launch_error{};
    try{
        const auto receipt=inst::read_receipt(root,owner);require(receipt.committed&&receipt.program_count==inst::current_program_count,"door_dos_host_not_installed");
        require(receipt.door_component.registered,"door_dos_runtime_unavailable_on_this_platform");
        auto driver=inst::open_object(receipt.door_component.path,false,GENERIC_READ,FILE_SHARE_READ);
        require(inst::file_id(driver.get())==receipt.door_component.file_id&&inst::volume_serial(driver.get())==receipt.door_component.volume&&inst::file_sha256(receipt.door_component.path)==receipt.door_component.sha256,"door_dos_driver_installation_mismatch");
        auto executable=command("\""+utf8(root/L"Bin"/L"SFXDOSHost.exe")+"\"",root/L"Bin",false);
        require(inst::file_id(executable.pin.get())==receipt.program_ids.at(10)&&inst::file_sha256(executable.executable)==receipt.program_hashes.at(10),"door_dos_host_installation_mismatch");
        const auto raw=inst::unique_id();std::string session;for(auto c:raw){if(c>='A'&&c<='F')c+=L'a'-L'A';session+=static_cast<char>(c);}require(session.size()==32,"door_session_identity");
        const auto private_dir=node/wide("D"+session.substr(0,7));auto descriptor=inst::mutable_descriptor(owner);inst::create_directory(private_dir,descriptor.get());private_files.capture_directory(private_dir,true);
        Pipe input,output,error,request,control,status;input.create(owner,false);output.create(owner,true);error.create(owner,true);request.create(owner,false);control.create(owner,false);status.create(owner,true);
        HANDLE duplicated{};require(DuplicateHandle(GetCurrentProcess(),GetCurrentProcess(),GetCurrentProcess(),&duplicated,PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,TRUE,0),"door_parent_handle");Handle parent(duplicated);
        FILETIME created{},exit{},kernel{},user{};require(GetProcessTimes(GetCurrentProcess(),&created,&exit,&kernel,&user),"door_parent_identity");
        const auto handle=[](HANDLE h){return static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(h));};
        dos_protocol::Request captured;captured.parent_pid=GetCurrentProcessId();captured.parent_birth=(std::uint64_t(created.dwHighDateTime)<<32)|created.dwLowDateTime;captured.account=caller.account;captured.node=caller.node;captured.generation=caller.generation;captured.sequence=caller.sequence;
        captured.parent_process=handle(parent.get());captured.input=handle(input.child.get());captured.output=handle(output.child.get());captured.error=handle(error.child.get());captured.control=handle(control.child.get());captured.status=handle(status.child.get());
        captured.mode=cleanup||door.program.io==app::DoorIo::standard?3:door.program.io==app::DoorIo::uart_only?4:12;
        captured.can_disconnect=!cleanup&&settings.can_disconnect;captured.allow_x64=settings.allow_x64_dos;captured.root=utf8(root);captured.startup=utf8(startup);captured.node_directory=utf8(node);captured.drop_directory=utf8(drop);captured.private_directory=utf8(private_dir);captured.command=command_line;captured.session=session;captured.driver_ini=app::encode_door_driver_settings(settings);
        if (door.program.drop_file==app::DoorDropFile::doordrop_ini) captured.doordrop_path=drop_path;
        else captured.bbsdev_path=drop_path;
        private_files.create(private_dir/L"SFXRUN.ENV",dos_protocol::environment_file(captured),owner);private_files.create(private_dir/L"SFXRUN.INI",captured.driver_ini,owner);
        private_files.create(private_dir/L"SFXRUN.RET","",owner);private_files.create(private_dir/L"SFXRUN.ERR","",owner);
        const auto encoded=dos_protocol::encode(captured);require(encoded.has_value(),"door_private_request_invalid");executable.line+=L" --runtime "+std::to_wstring(handle(request.child.get()))+L" --status "+std::to_wstring(handle(status.child.get()));
        // Keep the host invisible and its standard devices separate from the
        // caller. Only the launcher creates a minimized DOS console/VDM.
        DosHostDevices devices;
        start(child,executable,startup,environment(node),devices.input.get(),devices.output.get(),devices.error.get(),{input.child.get(),output.child.get(),error.child.get(),request.child.get(),control.child.get(),status.child.get(),parent.get(),devices.input.get(),devices.output.get(),devices.error.get()},{},&host_launch_error);
        input.child.reset();output.child.reset();error.child.reset();request.child.reset();control.child.reset();status.child.reset();parent.reset();private_write(request,*encoded,channel);request.close();
        HostStatus state;std::string to_game,to_caller;DoorKeys keys{discard_lf};keys.append(prefix,to_game);const auto began=GetTickCount64();std::uint64_t blocked_input{},blocked_output{},drain{};result.end=app::DoorEnd::returned;
        for(;;){
            const auto now=GetTickCount64();state.poll(status);(void)error.read();keys.expire(to_game);
            if(state.started&&program_started)*program_started=true;
            if(!state.error.empty()){result.end=app::DoorEnd::launch_failed;result.diagnostic=state.error;break;}
            if(const auto stop=channel.stop()){result.end=*stop;break;}if(keys.escaped){result.end=app::DoorEnd::escaped;break;}
            if(cleanup&&now-began>=30000){result.end=app::DoorEnd::backpressure_timeout;break;}
            if(!state.started&&now-began>=12000){result.end=app::DoorEnd::handshake_failed;result.diagnostic="door_dos_host_startup_timeout";break;}
            if(state.hangup){result.end=settings.can_disconnect&&!cleanup?app::DoorEnd::game_disconnect:app::DoorEnd::returned;break;}
            const bool exited=WaitForSingleObject(child.process.get(),0)==WAIT_OBJECT_0;
            if(exited&&!drain)drain=now+1000;
            if(state.started&&!state.finished&&!exited&&!cleanup&&to_game.size()<4095){auto read=channel.read(4095-to_game.size());require(app::valid_interactive_stream_read_result(4095-to_game.size(),read),"door_caller_read");if(read.outcome==Outcome::completed){if(channel.keyboard)channel.keyboard();keys.append(read.bytes,to_game);}else if(read.outcome!=Outcome::timed_out){result.end=app::DoorEnd::caller_disconnected;break;}}
            if(!to_game.empty()&&state.started&&!exited){auto sent=input.write(to_game);if(sent.outcome==Outcome::completed){to_game.erase(0,sent.bytes_written);blocked_input=0;}else if(sent.outcome==Outcome::timed_out){if(!blocked_input)blocked_input=now;}else{result.end=app::DoorEnd::transport_failed;break;}}
            if(to_caller.empty()){const auto read=output.read();if(read.outcome==Outcome::completed&&!cleanup)to_caller.assign(read.bytes.begin(),read.bytes.end());}
            if(!to_caller.empty()){const auto sent=channel.write(to_caller);require(app::valid_interactive_stream_write_result(to_caller.size(),sent),"door_caller_write");if(sent.outcome==Outcome::completed){to_caller.erase(0,sent.bytes_written);blocked_output=0;}else if(sent.outcome==Outcome::timed_out){if(!blocked_output)blocked_output=now;}else{result.end=app::DoorEnd::caller_disconnected;break;}}
            if((input.pending&&now-input.pending_since>=5000&&!input.ready())||(blocked_input&&now-blocked_input>=5000)||(blocked_output&&now-blocked_output>=5000)){result.end=app::DoorEnd::backpressure_timeout;break;}
            if(exited&&to_caller.empty()&&(output.closed||now>=drain)){if(!state.finished){DWORD code{};require(GetExitCodeProcess(child.process.get(),&code),"door_dos_host_exit_unavailable");throw std::runtime_error("door_dos_host_exit_"+std::to_string(code)+"_without_status");}require(state.pending.empty(),"door_dos_host_status_incomplete");result.exit_code=state.exit_code;if(state.exit_code){result.end=app::DoorEnd::launch_failed;result.diagnostic="door_dos_game_exit_"+std::to_string(state.exit_code);}break;}
            Sleep(2);
        }
        if(WaitForSingleObject(child.process.get(),0)==WAIT_TIMEOUT){const auto frame=dos_protocol::encode_frame({dos_protocol::Message::stop,result.end==app::DoorEnd::caller_disconnected?1u:result.end==app::DoorEnd::shutdown?3u:2u,{}});if(frame){const auto sent=control.write({reinterpret_cast<const char*>(frame->data()),frame->size()});(void)sent;}
            if(result.end==app::DoorEnd::caller_disconnected){input.close();const auto until=GetTickCount64()+5300;while(WaitForSingleObject(child.process.get(),10)==WAIT_TIMEOUT&&GetTickCount64()<until){(void)output.read();(void)error.read();state.poll(status);if(const auto stop=channel.stop())if(*stop==app::DoorEnd::shutdown||*stop==app::DoorEnd::operator_terminated)break;}}
        }
        result.cleanup_complete=child.stop();if(result.diagnostic.empty())result.diagnostic=app::door_result_code(result.end);
    }catch(const std::exception& error){result.end=channel.stop().value_or(app::DoorEnd::launch_failed);result.diagnostic=host_launch_error?"door_dos_create_host_win32_"+std::to_string(host_launch_error):error.what();result.cleanup_complete=child.stop();}
    try{if(disposable_door_result(result)){if(!private_files.finish()){result.cleanup_complete=false;result.work_preserved=true;}}else if(private_files.has_owned()){result.work_preserved=true;result.retained_location="Node private folder/"+private_files.directory.filename().u8string();}}catch(...){result.cleanup_complete=false;result.work_preserved=true;}return result;
}
app::DoorProgramResult run_dos_sequence(const Path& root,const std::wstring& owner,const app::DoorDefinition& door,const LocalDoorCaller& caller,const Path& startup,const Path& node,const Path& drop,DropFiles& files,const app::DoorDriverSettings& settings,const app::DoorChannel& channel,std::vector<std::uint8_t> prefix,bool discard_lf){
    const app::DoorCommandPaths paths{dos_short(node),dos_short(drop),dos_short(startup),app::effective_door_code(door),caller.node};
    const bool use_ini=door.program.drop_file==app::DoorDropFile::doordrop_ini;
    if (use_ini) require(dos_short_directory(paths.node)&&dos_short_directory(paths.drop)&&dos_short_directory(paths.startup),"door_dos_ini_path_not_short");
    const auto expanded=app::expand_door_command(door.program.command_line,paths);require(expanded.has_value(),"door_command_substitution");const auto line=dos_command(*expanded,startup,door.program.command_shell);
    auto context=drop_context(caller,door,node,drop,startup,0);context.node_path=paths.node;context.drop_path=paths.drop;context.startup_path=paths.startup;const auto drops=app::make_door_drop_files(door.program.drop_file,context);require(drops.valid(),drops.diagnostic.c_str());
    if(!drops.files.empty())files.create(drop/L"SFXDOOR.LCK","SFX owned drop files\r\n",owner);for(const auto& image:drops.files)files.create(drop/wide(image.name),{reinterpret_cast<const char*>(image.bytes.data()),image.bytes.size()},owner);
    const auto drop_path=drops.environment.empty()?std::string{}:dos_short(drop/(use_ini?L"DOORDROP.INI":L"BBSDEV.DRP"));
    bool started=false;auto result=run_dos_command(root,owner,door,caller,startup,node,drop,settings,channel,line.line,drop_path,std::move(prefix),discard_lf,false,&started);
    if(started&&disposable_door_result(result)&&!door.program.cleanup_command.empty()){
        const auto cleanup=app::expand_door_command(door.program.cleanup_command,paths);require(cleanup.has_value(),"door_cleanup_substitution");
        app::DoorChannel silent;silent.read=[](std::size_t){return app::InteractiveStreamReadResult{Outcome::timed_out,{}};};silent.write=[](std::string_view value){return app::InteractiveStreamWriteResult{Outcome::completed,value.size()};};silent.stop=[&]()->std::optional<app::DoorEnd>{const auto stop=channel.stop();return stop&&(*stop==app::DoorEnd::shutdown||*stop==app::DoorEnd::operator_terminated||*stop==app::DoorEnd::calling_time||*stop==app::DoorEnd::inactivity)?stop:std::nullopt;};
        try{const auto cleanup_command=dos_command(*cleanup,startup,door.program.command_shell);auto cleaned=run_dos_command(root,owner,door,caller,startup,node,drop,settings,silent,cleanup_command.line,drop_path,{},false,true);result.cleanup_command_failed=cleaned.end!=app::DoorEnd::returned;result.cleanup_complete=cleaned.cleanup_complete;result.work_preserved=result.work_preserved||cleaned.work_preserved;if(!cleaned.retained_location.empty())result.retained_location=cleaned.retained_location;}catch(...){result.cleanup_command_failed=true;}
    }return result;
}
