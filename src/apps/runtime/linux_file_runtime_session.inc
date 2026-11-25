// Authenticated native File session composition. Physical paths and raw
// descriptors stay here; shared caller DTOs contain logical IDs only.
const auto file_bindings=store.file_bindings();if(!file_bindings)return false;
const auto file_local_date=[&]{const auto text=message_date(clock.unix_seconds());return text.size()>=10?text.substr(0,10):std::string{};};
const auto file_live=[&]{return caller.tick()&&message_actor().has_value();};
const auto file_authority=[&]{const auto policy=database().security_policy();const auto account=database().find_account_by_id(ready.account_id);
    return policy&&policy.value&&account&&account.value&&account.value->security_level?app::FileAuthority{*account.value->security_level,policy.value->sysop_threshold}:app::FileAuthority{};};
app::FileListPorts file_list{terminal};
file_list.dimensions=[&]{const auto node=assignment->snapshot();return app::MessageEditorDimensions{ready.graphics==app::GraphicsMode::ansi,node.terminal_columns,node.terminal_rows};};
file_list.byte=[&]{struct Restore{MenuModalStream& value;~Restore(){value.preserve_poll_timeouts(false);}}restore{guarded};guarded.preserve_poll_timeouts(true);return terminal.read_editor_byte();};
file_list.write=[&](std::string_view text){return file_live()&&write_all(guarded,text);};
file_list.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};file_list.live=file_live;
file_list.lightbars_enabled=[&]{return config.menus.lightbars_enabled;};
file_list.selection_colors=[&]{return std::make_pair(config.menus.selected_foreground,config.menus.selected_background);};
LinuxCallerFileViewerPorts viewer_ports{database(),root,file_bindings,message_actor,file_list,{},{}};
viewer_ports.authority=file_authority;
viewer_ports.display=[&](const app::TerminalDisplayProgram& program,const std::function<bool()>& guard){
    struct Restore{MenuModalStream& stream;std::function<bool()> previous;~Restore(){stream.replace_guard(std::move(previous));}}restore{guarded,guarded.replace_guard(guard)};
    const auto rows=assignment->snapshot().terminal_rows;
    return terminal.present_display({"files.viewer.content",&program,{true,static_cast<std::uint16_t>(std::clamp<unsigned>(rows>1?rows-1:24,10,24))}});
};
LinuxFileViewer file_viewer(std::move(viewer_ports));
std::unique_ptr<LinuxCallerFileTransfers> native_files;
bool file_scan_announced{},file_zmodem{};
app::FileSessionPorts file_ports{database(),file_list};file_ports.actor=message_actor;file_ports.authority=file_authority;
file_ports.transfer_logoff=[&]{return app::file_transfer_logoff_countdown(file_list);};
file_ports.operation=[&]{return store.identifiers().generate();};file_ports.date=message_date;file_ports.caller_name=own_name;
file_ports.scan_date=[&](std::string_view text)->std::optional<std::int64_t>{return text.size()==10?qwk_local_entry_utc(std::string(text)+" 00:00",*local,*zone):std::nullopt;};
file_ports.protocol=[&]()->std::optional<app::TransferProtocolPreference>{const auto saved=database().transfer_preference(ready.account_id);return saved&&saved.value&&saved.value->account_id==ready.account_id?std::optional<app::TransferProtocolPreference>{saved.value->preference}:std::nullopt;};
file_ports.upload=[&](const auto& items,bool private_submission,auto protocol){file_scan_announced=false;file_zmodem=protocol==app::TransferProtocolPreference::zmodem;return native_files->upload(items,private_submission,protocol);};
file_ports.download=[&](const auto& items,auto protocol){file_zmodem=protocol==app::TransferProtocolPreference::zmodem;return native_files->download(items,protocol);};
file_ports.view=[&](app::FileId id,bool archive){return file_viewer.view(id,archive);};
file_ports.upload_space=[&](app::FileAreaId id)->std::optional<std::uint64_t>{try{
    const auto actor=message_actor();if(!actor)return {};
    const auto visible=database().file_catalog(*actor);const auto configuration=database().file_configuration({true,{}});
    if(!visible||!visible.value||!configuration||!configuration.value)return {};
    const auto found=std::find_if(visible.value->areas.begin(),visible.value->areas.end(),[&](const auto& a){return a.id==id;});if(found==visible.value->areas.end())return {};
    auto intake=host::FilePayloadDirectory::private_store(root/"data/fileintake",file_bindings);
    const auto usable=[](std::uint64_t bytes){return bytes>host::kFilePayloadFreeReserve?bytes-host::kFilePayloadFreeReserve:0;};
    const auto area_space=[&](app::FileAreaId target){std::vector<std::filesystem::path> others;std::string destination;
        for(const auto& a:configuration.value->areas)if(a.area.id==target)destination=a.storage_path;else others.push_back(std::filesystem::u8path(a.storage_path));
        auto area=host::FilePayloadDirectory::area(root,std::filesystem::u8path(destination),others,{"bin","config","data","display","backup","logs","work","doors","botgate"},file_bindings);return usable(area.available_bytes());};
    auto space=std::min({area_space(id),usable(intake.available_bytes()),app::file_upload_limit(configuration.value->policy,*found)});
    if(found->sysop_review){const auto review=std::find_if(configuration.value->areas.begin(),configuration.value->areas.end(),[](const auto& a){return a.area.kind==app::FileAreaKind::review_uploads;});if(review==configuration.value->areas.end())return {};space=std::min(space,area_space(review->area.id));}
    return space;
}catch(...){return {};}};
file_ports.selection_artwork=[&](bool groups)->std::optional<bool>{
    app::TrustedDisplaySelectionRequest request;request.identity=groups?app::TrustedDisplayIdentity::menu_file_groups:app::TrustedDisplayIdentity::menu_file_areas;request.graphics=ready.graphics;request.authenticated_security_level=file_authority().level;
    const auto artwork=displays.select(request);if(artwork.code==app::TrustedDisplaySelectionCode::not_found)return {};if(!artwork.available())return false;
    return terminal.present_display({"files.selection.artwork",&artwork.program->terminal_program,{true,24}})==app::TerminalOutcome::completed;
};
app::FileCallerSession files(std::move(file_ports));
app::StreamCallerTerminal::BinaryInputPrefix file_prefix;bool file_binary{};std::uint64_t file_began{};
LinuxFileTransferChannel file_channel;file_channel.now=[&]{return clock.monotonic_millis();};file_channel.authorized=file_live;
file_channel.absolute_deadline=[&]{
    auto deadline=caller.inactivity?caller.inactivity->absolute_deadline():0;
    const auto take=[&](std::uint64_t value){if(value)deadline=deadline?std::min(deadline,value):value;};
    if(caller.remaining)take(clock.monotonic_millis()+static_cast<std::uint64_t>(std::max<std::int64_t>(0,*caller.remaining))*1000);
    if(file_binary){take(file_began+app::kMessageReceiveTotalMillis);if(ssh)static_cast<AlphaSshStream&>(stream).message_binary_deadline(deadline);}
    return deadline;
};
file_channel.acquire=[&]{
    if(file_binary||!file_live())return false;
    file_binary=true;file_began=clock.monotonic_millis();chat.safe(chat_identity,false);
    const bool acquired=ssh?static_cast<AlphaSshStream&>(stream).message_binary_mode(true):static_cast<AlphaTelnetStream&>(stream).message_binary_mode(true);
    if(!acquired){file_binary=false;assignment->cancel();return false;}
    file_prefix=terminal.take_binary_input_prefix();return true;
};
file_channel.release=[&](bool synchronized){
    const bool restored=ssh?static_cast<AlphaSshStream&>(stream).message_binary_mode(false):static_cast<AlphaTelnetStream&>(stream).message_binary_mode(false);
    file_prefix={};file_binary=false;
    if(!synchronized||!restored)assignment->cancel();
    else if(file_zmodem)(void)write_all(guarded,app::file_transfer_text_handoff(ready.graphics==app::GraphicsMode::ansi));
};
file_channel.read=[&](std::size_t maximum){
    if(!file_binary)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::failed,{}};
    if(!file_prefix.bytes.empty()){
        const auto count=std::min(maximum,file_prefix.bytes.size());std::vector<std::uint8_t> bytes(file_prefix.bytes.begin(),file_prefix.bytes.begin()+count);
        file_prefix.bytes.erase(file_prefix.bytes.begin(),file_prefix.bytes.begin()+count);return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(bytes)};
    }
    auto result=ssh?static_cast<AlphaSshStream&>(stream).read_ready(maximum):static_cast<AlphaTelnetStream&>(stream).read_ready(maximum);
    if(result.outcome==app::InteractiveStreamOutcome::completed&&file_prefix.discard_delimiter_lf&&!result.bytes.empty()){
        file_prefix.discard_delimiter_lf=false;if(result.bytes.front()=='\n')result.bytes.erase(result.bytes.begin());if(result.bytes.empty())result.outcome=app::InteractiveStreamOutcome::timed_out;
    }
    return result;
};
file_channel.write=[&](std::string_view bytes){return !file_binary?app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::failed,0}:
    ssh?static_cast<AlphaSshStream&>(stream).write_ready(bytes):stream.write_some(bytes);};
file_channel.begin_recovery=[&]{return file_binary&&file_live()&&clock.monotonic_millis()>=file_began&&clock.monotonic_millis()-file_began<app::kMessageReceiveTotalMillis;};
LinuxCallerFileTransferPorts transfer_ports{database(),root,file_bindings,message_actor,[&]{return store.identifiers().generate();},file_local_date,std::move(file_channel),{},{},{},{},{}};
transfer_ports.review=[&](const app::FileUploadItem& item,const host::NativeFileIntakeOutcome& outcome,std::uint64_t bytes){return files.review_upload(item,outcome.diz,bytes);};
transfer_ports.observer=[&](std::string_view phase,std::uint32_t,std::uint32_t,std::size_t){journal->problem(app::AttentionKind::work_preserved,"file transfer",std::string(phase));};
transfer_ports.work_problem=[&](std::string resource,std::string diagnostic){journal->problem(app::AttentionKind::work_preserved,std::move(resource),std::move(diagnostic));};
transfer_ports.scan_notice=[&](const app::FileUploadItem& item,const host::NativeFileIntakeOutcome& result){
    const auto verdict=result.scan.verdict;const bool complete=verdict==app::FileScanVerdict::clean||verdict==app::FileScanVerdict::detected;
    journal->clamav_health(true,complete,complete,verdict==app::FileScanVerdict::timeout?"clamav_scan_timeout":verdict==app::FileScanVerdict::incomplete?"clamav_scan_incomplete":"clamav_scan_unavailable");
    if(verdict!=app::FileScanVerdict::detected)return;
    app::AttentionRecord notice;notice.kind=app::AttentionKind::clamav_finding;notice.utc_seconds=clock.unix_seconds();notice.node=assignment->identity().node;
    notice.transport=ssh?"SSH":"Telnet";notice.identity=app::AttentionIdentity::verified;notice.account_name=own_name();notice.filename=item.name;
    notice.signatures=result.scan.detections;notice.deletion_confirmed=result.deletion_confirmed;notice.deletion_journal_confirmed=result.journal_confirmed;
    journal->clamav_finding(std::move(notice));
};
transfer_ports.scan=[&](host::FilePayloadLease& input,std::uint64_t maximum){
    if(!file_live())return app::FileScanResult{app::FileScanVerdict::incomplete,{},"Scan cancelled before start"};
    if(!file_scan_announced){if(!write_all(guarded,"\r\nScanning upload(s) for malware...\r\n\r\n"))return app::FileScanResult{app::FileScanVerdict::incomplete,{},"Scan output unavailable"};file_scan_announced=true;}
    bool output_ok=write_all(guarded,app::file_scan_activity(0));host::LinuxFileScanner scanner(root);
    const auto result=scanner.scan(input,maximum,[&]{return !output_ok||!file_live();},
        [&](std::uint64_t elapsed){if(output_ok)output_ok=write_all(guarded,"\r"+app::file_scan_activity(elapsed));});
    if(output_ok)output_ok=write_all(guarded,"\r                        \r");
    return output_ok?result.result:app::FileScanResult{app::FileScanVerdict::incomplete,{},"Scan output unavailable"};
};
native_files=std::make_unique<LinuxCallerFileTransfers>(std::move(transfer_ports));
