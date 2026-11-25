// Native composition inside Context::run_menus. Caller DTOs remain path-free.
            const auto file_installation=owner.database_path.parent_path().parent_path();
            const auto file_local_date=[&]{const auto local=message_date(owner.clock.unix_seconds());return local.size()>=10?local.substr(0,10):std::string{};};
            const auto file_live=[&]{return modal_tick()&&calling_time_tick()&&message_actor().has_value();};
            const auto file_authority=[&]{const auto policy=owner.storage.security_policy();return policy&&policy.value?FileAuthority{live_security_level.load(),policy.value->sysop_threshold}:FileAuthority{};};
            FileListPorts file_list{terminal};
            file_list.dimensions=[&]{const auto node=state->snapshot();return MessageEditorDimensions{ready->graphics==GraphicsMode::ansi,node.terminal_columns,node.terminal_rows};};
            file_list.byte=[&]{struct Restore{MenuModalStream& stream;~Restore(){stream.preserve_poll_timeouts(false);}} restore{modal_stream};modal_stream.preserve_poll_timeouts(true);return terminal.read_editor_byte();};
            file_list.write=[&](std::string_view text){return file_live()&&write_all(modal_stream,text);};
            file_list.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};file_list.live=file_live;
            file_list.lightbars_enabled=[&]{std::lock_guard<std::mutex> lock(owner.state_mutex);return owner.configuration.menus.lightbars_enabled;};
            file_list.selection_colors=[&]{std::lock_guard<std::mutex> lock(owner.state_mutex);return std::make_pair(owner.configuration.menus.selected_foreground,owner.configuration.menus.selected_background);};
            std::unique_ptr<AlphaCallerFileTransfers> native_files;
            bool file_scan_announced{},file_zmodem{};
            const auto file_verified_uploader=own_name();
            CallerFileViewerPorts viewer_ports{owner.storage,file_installation,owner.installation_owner_sid,message_actor,file_list};
            viewer_ports.authority=file_authority;
            viewer_ports.display=[&](const TerminalDisplayProgram& program,const std::function<bool()>& guard){
                struct Restore{MenuModalStream& stream;std::function<bool()> previous;~Restore(){stream.replace_guard(std::move(previous));}} restore{modal_stream,modal_stream.replace_guard(guard)};
                const auto rows=state->snapshot().terminal_rows;
                return terminal.present_display({"files.viewer.content",&program,{true,static_cast<std::uint16_t>(std::clamp<unsigned>(rows>1?rows-1:24,10,24))}});
            };
            AlphaFileViewer file_viewer(std::move(viewer_ports));
            FileSessionPorts file_ports{owner.storage,file_list};file_ports.actor=message_actor;
            file_ports.transfer_logoff=[&]{return file_transfer_logoff_countdown(file_list);};
            file_ports.malware_scanning_available=[]{return !platform::win32::windows7_no_av();};
            file_ports.authority=file_authority;
            file_ports.operation=[&]{return owner.identifiers.generate();};file_ports.date=message_date;file_ports.caller_name=own_name;
            file_ports.scan_date=[&](std::string_view text)->std::optional<std::int64_t>{if(text.size()!=10)return {};std::shared_ptr<TimeContext> local;{std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}return local&&local->local&&local->zone?qwk_local_entry_utc(std::string(text)+" 00:00",*local->local,*local->zone):std::nullopt;};
            file_ports.protocol=[&]()->std::optional<TransferProtocolPreference>{const auto saved=owner.storage.transfer_preference(ready->account_id);return saved&&saved.value&&saved.value->account_id==ready->account_id?std::optional<TransferProtocolPreference>{saved.value->preference}:std::nullopt;};
            file_ports.upload=[&](const auto& items,bool private_submission,auto protocol){file_scan_announced=false;file_zmodem=protocol==TransferProtocolPreference::zmodem;return native_files->upload(items,private_submission,protocol);};
            file_ports.download=[&](const auto& items,auto protocol){file_zmodem=protocol==TransferProtocolPreference::zmodem;return native_files->download(items,protocol);};
            file_ports.view=[&](FileId id,bool archive){return file_viewer.view(id,archive);};
            file_ports.upload_space=[&](FileAreaId id)->std::optional<std::uint64_t>{try{
                const auto actor=message_actor();if(!actor)return {};const auto visible=owner.storage.file_catalog(*actor);const auto config=owner.storage.file_configuration({true,{}});if(!visible||!visible.value||!config||!config.value)return {};
                const auto found=std::find_if(visible.value->areas.begin(),visible.value->areas.end(),[&](const auto& a){return a.id==id;});if(found==visible.value->areas.end())return {};
                auto intake=platform::win32::FilePayloadDirectory::private_store(file_installation/L"Data"/L"FileIntake",owner.installation_owner_sid);
                const auto usable=[](std::uint64_t bytes){return bytes>platform::win32::kFilePayloadFreeReserve?bytes-platform::win32::kFilePayloadFreeReserve:0;};
                const auto area_space=[&](FileAreaId target){std::vector<std::filesystem::path> others;std::string destination;for(const auto& a:config.value->areas)if(a.area.id==target)destination=a.storage_path;else others.push_back(std::filesystem::u8path(a.storage_path));
                    auto area=platform::win32::FilePayloadDirectory::area(file_installation,std::filesystem::u8path(destination),others,{L"Bin",L"Config",L"Data",L"Display",L"Backup",L"Logs",L"Work",L"Doors",L"BotGate"},owner.installation_owner_sid);return usable(area.available_bytes());};
                auto space=std::min({area_space(id),usable(intake.available_bytes()),file_upload_limit(config.value->policy,*found)});
                if(found->sysop_review){const auto review=std::find_if(config.value->areas.begin(),config.value->areas.end(),[](const auto& a){return a.area.kind==FileAreaKind::review_uploads;});if(review==config.value->areas.end())return {};space=std::min(space,area_space(review->area.id));}
                return space;
            }catch(...){return {};}};
            file_ports.selection_artwork=[&](bool groups)->std::optional<bool>{
                const auto reader=platform::win32::Win32TrustedDisplayAssetReader::open((file_installation/L"Display").wstring());if(!reader)return false;
                TrustedDisplaySelectionRequest request;request.identity=groups?TrustedDisplayIdentity::menu_file_groups:TrustedDisplayIdentity::menu_file_areas;request.graphics=ready->graphics;request.authenticated_security_level=live_security_level.load();
                TrustedDisplaySelector selector(*reader);const auto artwork=selector.select(request);if(artwork.code==TrustedDisplaySelectionCode::not_found)return {};if(!artwork.available())return false;
                return terminal.present_display({"files.selection.artwork",&artwork.program->terminal_program,{true,24}})==TerminalOutcome::completed;
            };
            FileCallerSession files(std::move(file_ports));
            file_channel.authorized=file_live;
            file_channel.release=[&,release=std::move(file_channel.release)](bool synchronized){
                release(synchronized);
                if(synchronized&&file_zmodem&&last_menu_outcome!=InteractiveStreamOutcome::disconnected){
                    try{(void)write_all(modal_stream,file_transfer_text_handoff(ready->graphics==GraphicsMode::ansi));}catch(...){}
                }
            };
            CallerFileTransferPorts transfer_ports{owner.storage,file_installation,owner.installation_owner_sid,message_actor,[&]{return owner.identifiers.generate();},file_local_date,std::move(file_channel)};
            transfer_ports.work_problem=[weak=std::weak_ptr<AlphaOperationalJournal>(owner.journal)](std::string resource,std::string diagnostic){if(const auto journal=weak.lock())journal->problem(AttentionKind::work_preserved,std::move(resource),std::move(diagnostic));};
            transfer_ports.observer=[&](std::string_view phase,std::uint32_t error,std::uint32_t exit,std::size_t pending){
                // Maintenance events are global records. Caller identity fields
                // would violate their contract and silently reject the event.
                OperationalContext context;context.resource_id=1;context.diagnostic_code=std::string(phase);
                context.resource_name="File node "+std::to_string(log_context.node)+": "+std::string(phase)+"; error="+std::to_string(error)+"; helper="+std::to_string(exit)+"; pending="+std::to_string(pending);
                context.resource_name.resize(std::min<std::size_t>(128,context.resource_name.size()));const bool recovered=phase=="file_transfer_recovered";
                owner.journal->emit({recovered?OperationalKind::file_maintenance:OperationalKind::file_maintenance_problem,recovered?OperationalOutcome::cancelled:OperationalOutcome::unavailable,std::move(context)});
            };
            transfer_ports.review=[&](const FileUploadItem& item,const platform::win32::NativeFileIntakeOutcome& result,std::uint64_t bytes){return files.review_upload(item,result.diz,bytes);};
            transfer_ports.scan_notice=[&](const FileUploadItem& item,const platform::win32::NativeFileIntakeOutcome& result){
                if(!platform::win32::clamav_available())return;
                const auto verdict=result.scan.verdict;const bool complete=verdict==FileScanVerdict::clean||verdict==FileScanVerdict::detected;
                owner.journal->clamav_health(true,complete,complete,verdict==FileScanVerdict::timeout?"clamav_scan_timeout":verdict==FileScanVerdict::incomplete?"clamav_scan_incomplete":"clamav_scan_unavailable");
                if(verdict!=FileScanVerdict::detected)return;
                AttentionRecord notice;notice.kind=AttentionKind::clamav_finding;notice.utc_seconds=owner.clock.unix_seconds();notice.node=log_context.node;notice.transport=ssh?"SSH":"Telnet";
                notice.identity=AttentionIdentity::verified;notice.account_name=file_verified_uploader;notice.filename=item.name;notice.signatures=result.scan.detections;notice.deletion_confirmed=result.deletion_confirmed;notice.deletion_journal_confirmed=result.journal_confirmed;
                owner.journal->clamav_finding(std::move(notice));
            };
            transfer_ports.scan=[&](platform::win32::FilePayloadLease& input,std::uint64_t maximum){
                namespace ins=platform::win32::installation;
                std::unique_ptr<platform::win32::ClamAvScratch> job;
                const auto cleanup=[&]()noexcept{
                    if(!job)return;
                    if(!job->cleanup())try{
                        OperationalContext context;context.resource_id=1;context.resource_name="ClamAV scratch retained: "+job->path().filename().u8string();
                        context.diagnostic_code="clamav_scratch_cleanup_incomplete";
                        owner.journal->emit({OperationalKind::file_maintenance_problem,OperationalOutcome::storage_failure,std::move(context)});
                    }catch(...){owner.journal->mark_event_failed();}
                };
                struct Cleanup{decltype(cleanup)& action;~Cleanup(){action();}} cleanup_on_exit{cleanup};
                try{
                    if(!file_live())return FileScanResult{FileScanVerdict::incomplete,{},"Scan cancelled before start"};
                    const auto receipt=ins::read_receipt(file_installation,owner.installation_owner_sid);ins::verify_structure(receipt,false);
                    platform::win32::verify_clamav_installation(file_installation,owner.installation_owner_sid,receipt.id);
                    const auto descriptor=ins::structural_descriptor(owner.installation_owner_sid);const auto engine=file_installation/L"Bin"/L"Tools"/L"ClamAV";
                    auto engine_root=ins::open_object(engine,true);ins::require(ins::exact_security(engine_root.get(),descriptor.get()),"scan_engine_role_not_protected");
                    std::vector<ins::Handle> security_pins;for(const auto& member:platform::win32::clamav_program_members()){auto pin=ins::open_object(engine/member.name,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);ins::require(ins::exact_security(pin.get(),descriptor.get()),"scan_engine_member_role_not_protected");security_pins.push_back(std::move(pin));}
                    auto definitions=platform::win32::FilePayloadDirectory::private_store(file_installation/L"Data"/L"ClamAV",owner.installation_owner_sid);
                    job=platform::win32::ClamAvScratch::create(file_installation/L"Work"/L"ClamAV",owner.installation_owner_sid);
                    auto scratch=platform::win32::FilePayloadDirectory::private_store(job->path(),owner.installation_owner_sid);
                    if(!file_scan_announced){if(!write_all(modal_stream,"\r\nScanning upload(s) for malware...\r\n\r\n"))return FileScanResult{FileScanVerdict::incomplete,{},"Scan output unavailable"};file_scan_announced=true;}
                    platform::win32::WindowsFileScanner scanner(engine,owner.installation_owner_sid);
                    // An indeterminate activity indicator, NOT a percentage or
                    // successful scan/acceptance claim. Keep it alive until
                    // the actual scanner ends, including slow engine startup.
                    bool output_ok=write_all(modal_stream,file_scan_activity(0));
                    const auto result=scanner.scan(input,definitions,scratch,maximum,
                        [&]{return !output_ok||!file_live();},
                        [&](std::uint64_t elapsed){if(output_ok)output_ok=write_all(modal_stream,"\r"+file_scan_activity(elapsed));});
                    if(output_ok)output_ok=write_all(modal_stream,"\r                        \r");
                    // Only the committed batch result may announce file added.
                    return output_ok?result.result:FileScanResult{FileScanVerdict::incomplete,{},"Scan output unavailable"};
                }catch(...){return FileScanResult{FileScanVerdict::error,{},"Scanner unavailable; held privately"};}
            };
            native_files=std::make_unique<AlphaCallerFileTransfers>(std::move(transfer_ports));
