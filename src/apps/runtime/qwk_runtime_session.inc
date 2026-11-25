// Local composition inside Context::run_menus; no persisted caller file paths.
// Packet preparation/validation is single-admitted to bound Win32 peak memory.
// Ordinary menus, text/art transfers and database writer budgets stay unchanged.
            static std::mutex qwk_work_mutex;
            std::shared_ptr<const QwkPacketPlan> qwk_active_download_plan;
            const auto qwk_request=[&]() -> std::optional<QwkTransferRequest> {
                const auto actor=message_actor();if(!actor)return {};const auto policy=owner.storage.qwk_caller_policy(*actor);if(!policy||!policy.value)return {};
                return QwkTransferRequest{*actor,policy.value->board_id,policy.value->revision};
            };
            const auto qwk_authorized=[&](const QwkTransferRequest& request){
                if(!calling_time_tick()||!refresh()||!ready)return false;
                return qwk_runtime_authorized(owner.storage,ready->durable_session_id,ready->account_id,request,qwk_active_download_plan.get());
            };
            packet_channel.authorized=qwk_authorized;
            AlphaQwkPacketTransfer packet_transfer(owner.database_path.parent_path().parent_path(),owner.installation_owner_sid,std::move(packet_channel),[&](std::string_view phase,std::uint32_t error,std::uint32_t exit,std::size_t pending){
                // Content-free failure codes only: no caller packet names,
                // paths, subjects, bodies, credentials or trusted fingerprints.
                // Observing a failure cannot change its safety/outcome policy.
                if(phase=="packet_reply_sealed"||phase=="packet_send_completed")return;
                auto context=log_context;context.diagnostic_code=std::string(phase.substr(0,48))+"_w"+std::to_string(error)+"_e"+std::to_string(exit)+"_p"+std::to_string(pending);
                owner.add_log(OperationalKind::message_area_unavailable,context,OperationalOutcome::terminal_failure);
            },[weak=std::weak_ptr<AlphaOperationalJournal>(owner.journal)](std::string resource,std::string diagnostic){if(const auto journal=weak.lock())journal->problem(AttentionKind::work_preserved,std::move(resource),std::move(diagnostic));});
            const auto qwk_transfer_notice=[](QwkTransferCode code,QwkTransferRefusal refusal,bool upload)->std::string {
                if(code==QwkTransferCode::cancelled)return "No packet was transferred. No message/read/pointer changes made.";
                if(refusal==QwkTransferRefusal::extension)return "Reply upload refused. Choose a .REP or .ZIP packet. Nothing imported.";
                if(refusal==QwkTransferRefusal::too_large)return "Reply upload refused: maximum 16 MiB (16,777,216 bytes). Nothing imported.";
                if(refusal==QwkTransferRefusal::skipped)return "Download was skipped. No read/pointer changes made.";
                if(refusal==QwkTransferRefusal::content)return "Reply packet is invalid or unsupported. Nothing imported.";
                return upload?"Reply upload unavailable or failed. No import success is claimed.":"Packet transfer unavailable or failed. No read/pointer changes made.";
            };
            const auto qwk_download=[&]() -> std::optional<std::string> {
                std::unique_lock<std::mutex> work(qwk_work_mutex,std::try_to_lock);if(!work.owns_lock())return "Offline packet preparation is busy. Please try again shortly.";
                const auto request=qwk_request();if(!request)return "Offline packet authority is unavailable.";
                const auto start=GetTickCount64();auto deadline=start+kMessageReceiveTotalMillis;
                const auto ceiling=[&]{auto value=inactivity.absolute_deadline();const auto take=[&](std::uint64_t n){if(n)value=value?std::min(value,n):n;};take(owner.drain_deadline.load());if(remaining_calling_seconds)take(GetTickCount64()+static_cast<std::uint64_t>(*remaining_calling_seconds)*1000);return value;};
                const auto cancelled=[&]{const auto live=ceiling();if(live)deadline=std::min(deadline,live);const auto now=GetTickCount64();return now<start||now>=deadline||!qwk_authorized(*request);};
                if(cancelled())return {};
                QwkPacketMetadata metadata;std::shared_ptr<TimeContext> local;{std::lock_guard<std::mutex> lock(owner.state_mutex);metadata.board_name=owner.configuration.bbs_name;local=owner.time_context;}
                metadata.sysop=owner.sysop_display_name();metadata.caller_name=own_name();const auto stamp=local&&local->local?local->local->format_utc_seconds(owner.clock.unix_seconds()):std::nullopt;
                if(!stamp||stamp->size()<19)return "BBS-local packet timestamp unavailable.";
                metadata.created=stamp->substr(5,2)+"-"+stamp->substr(8,2)+"-"+stamp->substr(0,4)+","+stamp->substr(11,8);
                const auto date=[local](std::int64_t utc)->sfx::archive::QwkRecordDate {
                    const auto value=local&&local->local?local->local->format_utc_seconds(utc):std::nullopt;if(!value||value->size()<19)throw std::runtime_error("qwk_local_date_unavailable");
                    return {value->substr(5,2)+"-"+value->substr(8,2)+"-"+value->substr(2,2),value->substr(11,5),std::to_string(utc)};
                };
                if(!present_message_text(message_terminal,"qwk.download.preparing","Preparing your QWK packet. Large selections may take a moment."))return {};
                const auto progress=[&](const sfx::archive::QwkPreparationEvent& event){return !cancelled()&&present_message_text(message_terminal,"qwk.download.progress",qwk_preparation_progress_text(event));};
                auto& packet_storage=owner.storage;const auto operation=owner.identifiers.generate();
                auto packet=prepare_qwk_with_activity([&packet_storage,actor=request->actor,operation,metadata=std::move(metadata),date](auto stop,auto observe)mutable{
                    return prepare_caller_qwk_download(packet_storage,actor,operation,std::move(metadata),date,std::move(stop),std::move(observe));
                },cancelled,progress,[&](std::string_view bytes){return write_all(modal_stream,bytes);},state->snapshot().terminal_columns);
                if(!packet.result.ok()||!packet.storage)return qwk_preparation_notice(packet);
                if(!packet.progress)return packet.notice; // exact empty notice; no helper/ZIP
                // Keep the exact prepared selection live through confirmation,
                // relay and commit. Actor/board checks alone cannot detect an
                // echo/group read gate or shared selection changed meanwhile.
                qwk_active_download_plan=std::make_shared<const QwkPacketPlan>(packet.progress->initial);
                struct ClearDownloadPlan{std::shared_ptr<const QwkPacketPlan>& plan;~ClearDownloadPlan(){plan.reset();}} clear_download_plan{qwk_active_download_plan};
                if(cancelled())return {};if(!packet_transfer.available(*request))return packet_transfer.unavailable_reason()+" No read/pointer changes made.";
                const auto confirm=message_terminal.read_key({"qwk.download.confirm","Download this QWK packet via ZMODEM? [Y/n] ",{'Y','N'},'Y'});
                if(confirm.outcome==TerminalOutcome::cancelled||confirm.key=='N')return "Download cancelled. No read/pointer changes made.";if(!confirm.has_key())return {};
                if(cancelled())return {};const auto sent=packet_transfer.send(*request,packet.zip);packet.zip={};
                if(sent.code==QwkTransferCode::disconnected)return {};if(sent.code!=QwkTransferCode::completed)return qwk_transfer_notice(sent.code,sent.refusal,false);
                if(cancelled())return "Transfer completed, but current authority/deadline changed. Delivery progress not committed.";
                auto stop=std::make_shared<std::atomic<bool>>(false);struct Revoke{std::shared_ptr<std::atomic<bool>> stop;~Revoke(){stop->store(true);}} revoke{stop};
                packet.progress->cancelled=[stop,deadline]{return stop->load()||GetTickCount64()>=deadline;};
                auto committed=owner.storage.commit_qwk_delivery(request->actor,*packet.progress);
                if(!committed){
                    // Bounded writer barrier, then immutable durable receipt.
                    // Never retry a different plan or claim rollback on doubt.
                    (void)owner.storage.checkpoint();const auto outcome=owner.storage.qwk_delivery_outcome(request->actor,*packet.progress);
                    if(outcome&&outcome.value&&*outcome.value)committed={StorageStatus::success(),**outcome.value};
                }
                if(!committed||!committed.value)return "QWK transfer completed, but delivery progress could not be confirmed. No progress success is claimed.";
                if(sent.confirm_completed)sent.confirm_completed();
                return std::string("QWK downloaded: ")+std::to_string(committed.value->delivered)+" message(s)."+(committed.value->more?"\n"+std::string(kQwkMoreNotice):"");
            };
            const auto qwk_upload=[&]() -> std::optional<std::string> {
                std::unique_lock<std::mutex> work(qwk_work_mutex,std::try_to_lock);if(!work.owns_lock())return "Offline packet validation is busy. Please try again shortly.";
                const auto request=qwk_request();if(!request)return "Offline packet authority is unavailable.";
                if(!packet_transfer.available(*request))return packet_transfer.unavailable_reason()+" Nothing imported.";
                if(!present_message_text(message_terminal,"qwk.upload.limits","Allowed: .REP or .ZIP; maximum 16 MiB compressed / 64 MiB expanded.\nThe entire reply packet must validate before anything is imported."))return {};
                const auto confirm=message_terminal.read_key({"qwk.upload.confirm","Upload and import this reply packet via ZMODEM? [Y/n] ",{'Y','N'},'Y'});
                if(confirm.outcome==TerminalOutcome::cancelled||confirm.key=='N')return "Reply upload cancelled. Nothing imported.";if(!confirm.has_key())return {};
                auto received=packet_transfer.receive(*request);if(received.code==QwkTransferCode::disconnected)return {};
                if(received.code!=QwkTransferCode::completed||!received.source)return qwk_transfer_notice(received.code,received.refusal,true);
                if(!qwk_authorized(*request))return "Reply authority changed. Nothing imported.";
                auto imported=owner.storage.import_qwk_reply(request->actor,received.source);
                bool absence_confirmed=false;
                if(!imported){const auto barrier=owner.storage.checkpoint();const auto outcome=owner.storage.qwk_import_outcome(request->actor,received.source->info());if(outcome&&outcome.value&&*outcome.value)imported={StorageStatus::success(),**outcome.value};else absence_confirmed=bool(barrier)&&outcome&&outcome.value&&!outcome.value->has_value();}
                if(!imported||!imported.value)return qwk_import_notice(imported.status,absence_confirmed);
                received.source->confirm_completed();
                if(imported.value->replayed)return std::string(kQwkReplayNotice);
                return "Reply packet imported: "+std::to_string(imported.value->messages)+" message(s), "+std::to_string(imported.value->commands)+" ADD/DROP change(s)."+(imported.value->missing_parents?"\n"+std::to_string(imported.value->missing_parents)+" reply link(s) unavailable; accepted as independently authorized new messages.":"");
            };
            QwkCallerSession offline({owner.storage,message_terminal,message_actor,[&]{return owner.identifiers.generate();},[&]() -> std::optional<MessageArea>{return messages.refresh_selection()?messages.selection():std::nullopt;},[&](std::string_view text)->std::optional<std::int64_t>{std::shared_ptr<TimeContext> local;{std::lock_guard<std::mutex> lock(owner.state_mutex);local=owner.time_context;}return local&&local->local&&local->zone?qwk_local_entry_utc(text,*local->local,*local->zone):std::nullopt;},qwk_download,qwk_upload});
