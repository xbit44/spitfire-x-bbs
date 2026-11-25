// Native host composition of accepted QWK policy. Packet work is single-admitted.
app::StreamCallerTerminal::BinaryInputPrefix packet_prefix;bool packet_binary{};std::uint64_t packet_began{};
app::DecodedQwkTransferChannel packet_channel;packet_channel.now=[&]{return clock.monotonic_millis();};
packet_channel.absolute_deadline=[&]{
    auto deadline=caller.inactivity?caller.inactivity->absolute_deadline():0;
    const auto take=[&](std::uint64_t value){if(value)deadline=deadline?std::min(deadline,value):value;};
    if(caller.remaining)take(clock.monotonic_millis()+static_cast<std::uint64_t>(std::max<std::int64_t>(0,*caller.remaining))*1000);
    if(packet_binary){take(packet_began+app::kMessageReceiveTotalMillis);if(ssh)static_cast<AlphaSshStream&>(stream).message_binary_deadline(deadline);}
    return deadline;
};
packet_channel.acquire=[&]{
    if(packet_binary||!caller.tick()||!message_actor())return false;
    packet_binary=true;packet_began=clock.monotonic_millis();chat.safe(chat_identity,false);
    const bool acquired=ssh?static_cast<AlphaSshStream&>(stream).message_binary_mode(true):static_cast<AlphaTelnetStream&>(stream).message_binary_mode(true);
    if(!acquired){packet_binary=false;assignment->cancel();return false;}
    packet_prefix=terminal.take_binary_input_prefix();return true;
};
packet_channel.release=[&](bool synchronized){
    const bool restored=ssh?static_cast<AlphaSshStream&>(stream).message_binary_mode(false):static_cast<AlphaTelnetStream&>(stream).message_binary_mode(false);
    packet_prefix={};packet_binary=false;if(!synchronized||!restored)assignment->cancel();
};
packet_channel.read=[&](std::size_t maximum){
    if(!packet_binary)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::failed,{}};
    if(!packet_prefix.bytes.empty()){
        const auto count=std::min(maximum,packet_prefix.bytes.size());std::vector<std::uint8_t> bytes(packet_prefix.bytes.begin(),packet_prefix.bytes.begin()+count);
        packet_prefix.bytes.erase(packet_prefix.bytes.begin(),packet_prefix.bytes.begin()+count);return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(bytes)};
    }
    auto result=ssh?static_cast<AlphaSshStream&>(stream).read_ready(maximum):static_cast<AlphaTelnetStream&>(stream).read_ready(maximum);
    if(result.outcome==app::InteractiveStreamOutcome::completed&&packet_prefix.discard_delimiter_lf&&!result.bytes.empty()){
        packet_prefix.discard_delimiter_lf=false;if(result.bytes.front()=='\n')result.bytes.erase(result.bytes.begin());if(result.bytes.empty())result.outcome=app::InteractiveStreamOutcome::timed_out;
    }
    return result;
};
packet_channel.write=[&](std::string_view bytes){return !packet_binary?app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::failed,0}:
    ssh?static_cast<AlphaSshStream&>(stream).write_ready(bytes):stream.write_some(bytes);};
            static std::mutex qwk_work_mutex;
            std::shared_ptr<const app::QwkPacketPlan> qwk_active_download_plan;
            const auto qwk_request=[&]() -> std::optional<app::QwkTransferRequest> {
                const auto actor=message_actor();if(!actor)return {};const auto policy=database().qwk_caller_policy(*actor);if(!policy||!policy.value)return {};
                return app::QwkTransferRequest{*actor,policy.value->board_id,policy.value->revision};
            };
            const auto qwk_authorized=[&](const app::QwkTransferRequest& request){
                if(!caller.tick()||!caller.refresh_access())return false;
                return qwk_runtime_authorized(database(),ready.durable_session_id,ready.account_id,request,qwk_active_download_plan.get());
            };
            packet_channel.authorized=qwk_authorized;
            app::OperationalContext qwk_log_context;qwk_log_context.epoch=epoch;qwk_log_context.node=assignment->identity().node;
            qwk_log_context.generation=assignment->identity().generation;qwk_log_context.attempt=assignment->identity().generation;
            qwk_log_context.source_ip=assignment->snapshot().source_ip;qwk_log_context.transport=ssh?app::OperationalTransport::ssh:app::OperationalTransport::telnet;
            qwk_log_context.account_id=ready.account_id;qwk_log_context.caller=own_name();qwk_log_context.durable_session_id=ready.durable_session_id;qwk_log_context.correlation_id=ready.correlation_id;
            LinuxQwkPacketTransfer packet_transfer(root,std::move(packet_channel),
                [weak=std::weak_ptr<AlphaOperationalJournal>(journal),qwk_log_context](std::string_view phase){if(const auto value=weak.lock()){auto context=qwk_log_context;context.diagnostic_code=std::string(phase.substr(0,64));value->emit({app::OperationalKind::message_area_unavailable,app::OperationalOutcome::terminal_failure,context});}},
                [weak=std::weak_ptr<AlphaOperationalJournal>(journal)](std::string resource,std::string diagnostic){if(const auto value=weak.lock())value->problem(app::AttentionKind::work_preserved,std::move(resource),std::move(diagnostic));});
            const auto qwk_transfer_notice=[](app::QwkTransferCode code,app::QwkTransferRefusal refusal,bool upload)->std::string {
                if(code==app::QwkTransferCode::cancelled)return "No packet was transferred. No message/read/pointer changes made.";
                if(refusal==app::QwkTransferRefusal::extension)return "Reply upload refused. Choose a .REP or .ZIP packet. Nothing imported.";
                if(refusal==app::QwkTransferRefusal::too_large)return "Reply upload refused: maximum 16 MiB (16,777,216 bytes). Nothing imported.";
                if(refusal==app::QwkTransferRefusal::skipped)return "Download was skipped. No read/pointer changes made.";
                if(refusal==app::QwkTransferRefusal::content)return "Reply packet is invalid or unsupported. Nothing imported.";
                return upload?"Reply upload unavailable or failed. No import success is claimed.":"Packet transfer unavailable or failed. No read/pointer changes made.";
            };
            const auto qwk_download=[&]() -> std::optional<std::string> {
                std::unique_lock<std::mutex> work(qwk_work_mutex,std::try_to_lock);if(!work.owns_lock())return "Offline packet preparation is busy. Please try again shortly.";
                const auto request=qwk_request();if(!request)return "Offline packet authority is unavailable.";
                const auto start=clock.monotonic_millis();auto deadline=start+app::kMessageReceiveTotalMillis;
                const auto ceiling=[&]{auto value=caller.inactivity?caller.inactivity->absolute_deadline():0;const auto take=[&](std::uint64_t n){if(n)value=value?std::min(value,n):n;};if(caller.remaining)take(clock.monotonic_millis()+static_cast<std::uint64_t>(std::max<std::int64_t>(0,*caller.remaining))*1000);return value;};
                const auto cancelled=[&]{const auto live=ceiling();if(live)deadline=std::min(deadline,live);const auto now=clock.monotonic_millis();return now<start||now>=deadline||!qwk_authorized(*request);};
                if(cancelled())return {};
                app::QwkPacketMetadata metadata;metadata.board_name=config.bbs_name;metadata.sysop=sysop_name;metadata.caller_name=own_name();
                const auto stamp=local->format_utc_seconds(clock.unix_seconds());
                if(!stamp||stamp->size()<19)return "BBS-local packet timestamp unavailable.";
                metadata.created=stamp->substr(5,2)+"-"+stamp->substr(8,2)+"-"+stamp->substr(0,4)+","+stamp->substr(11,8);
                const auto date=[settings=caller.settings](std::int64_t utc)->sfx::archive::QwkRecordDate {
                    const auto value=settings->local->format_utc_seconds(utc);if(!value||value->size()<19)throw std::runtime_error("qwk_local_date_unavailable");
                    return {value->substr(5,2)+"-"+value->substr(8,2)+"-"+value->substr(2,2),value->substr(11,5),std::to_string(utc)};
                };
                if(!app::present_message_text(message_terminal,"qwk.download.preparing","Preparing your QWK packet. Large selections may take a moment."))return {};
                const auto progress=[&](const sfx::archive::QwkPreparationEvent& event){return !cancelled()&&app::present_message_text(message_terminal,"qwk.download.progress",qwk_preparation_progress_text(event));};
                auto& packet_storage=database();const auto operation=store.identifiers().generate();
                auto packet=prepare_qwk_with_activity([&packet_storage,actor=request->actor,operation,metadata=std::move(metadata),date](auto stop,auto observe)mutable{
                    return prepare_caller_qwk_download(packet_storage,actor,operation,std::move(metadata),date,std::move(stop),std::move(observe));
                },cancelled,progress,[&](std::string_view bytes){return write_all(guarded,bytes);},assignment->snapshot().terminal_columns);
                if(!packet.result.ok()||!packet.storage)return qwk_preparation_notice(packet);
                if(!packet.progress)return packet.notice; // exact empty notice; no helper/ZIP
                // Keep the exact prepared selection live through confirmation,
                // relay and commit. Actor/board checks alone cannot detect an
                // echo/group read gate or shared selection changed meanwhile.
                qwk_active_download_plan=std::make_shared<const app::QwkPacketPlan>(packet.progress->initial);
                struct ClearDownloadPlan{std::shared_ptr<const app::QwkPacketPlan>& plan;~ClearDownloadPlan(){plan.reset();}} clear_download_plan{qwk_active_download_plan};
                if(cancelled())return {};
                if(!packet_transfer.available(*request))return packet_transfer.unavailable_reason()+" No read/pointer changes made.";
                const auto confirm=message_terminal.read_key({"qwk.download.confirm","Download this QWK packet via ZMODEM? [Y/n] ",{'Y','N'},'Y'});
                if(confirm.outcome==app::TerminalOutcome::cancelled||confirm.key=='N')return "Download cancelled. No read/pointer changes made.";
                if(!confirm.has_key())return {};
                if(cancelled())return {};
                const auto sent=packet_transfer.send(*request,packet.zip);packet.zip={};
                if(sent.code==app::QwkTransferCode::disconnected)return {};
                if(sent.code!=app::QwkTransferCode::completed)return qwk_transfer_notice(sent.code,sent.refusal,false);
                if(cancelled())return "Transfer completed, but current authority/deadline changed. Delivery progress not committed.";
                auto stop=std::make_shared<std::atomic<bool>>(false);struct Revoke{std::shared_ptr<std::atomic<bool>> stop;~Revoke(){stop->store(true);}} revoke{stop};
                const auto monotonic_now=[](){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
                const auto commit_now=clock.monotonic_millis();if(commit_now>=deadline)return "Transfer completed, but its deadline expired. Delivery progress not committed.";
                const auto commit_deadline=monotonic_now()+deadline-commit_now;
                packet.progress->cancelled=[stop,commit_deadline,monotonic_now]{return stop->load()||monotonic_now()>=commit_deadline;};
                auto committed=database().commit_qwk_delivery(request->actor,*packet.progress);
                if(!committed){
                    // Bounded writer barrier, then immutable durable receipt.
                    // Never retry a different plan or claim rollback on doubt.
                    (void)database().checkpoint();const auto outcome=database().qwk_delivery_outcome(request->actor,*packet.progress);
                    if(outcome&&outcome.value&&*outcome.value)committed={app::StorageStatus::success(),**outcome.value};
                }
                if(!committed||!committed.value)return "QWK transfer completed, but delivery progress could not be confirmed. No progress success is claimed.";
                if(sent.confirm_completed)sent.confirm_completed();
                return std::string("QWK downloaded: ")+std::to_string(committed.value->delivered)+" message(s)."+(committed.value->more?"\n"+std::string(app::kQwkMoreNotice):"");
            };
            const auto qwk_upload=[&]() -> std::optional<std::string> {
                std::unique_lock<std::mutex> work(qwk_work_mutex,std::try_to_lock);if(!work.owns_lock())return "Offline packet validation is busy. Please try again shortly.";
                const auto request=qwk_request();if(!request)return "Offline packet authority is unavailable.";
                if(!packet_transfer.available(*request))return packet_transfer.unavailable_reason()+" Nothing imported.";
                if(!app::present_message_text(message_terminal,"qwk.upload.limits","Allowed: .REP or .ZIP; maximum 16 MiB compressed / 64 MiB expanded.\nThe entire reply packet must validate before anything is imported."))return {};
                const auto confirm=message_terminal.read_key({"qwk.upload.confirm","Upload and import this reply packet via ZMODEM? [Y/n] ",{'Y','N'},'Y'});
                if(confirm.outcome==app::TerminalOutcome::cancelled||confirm.key=='N')return "Reply upload cancelled. Nothing imported.";
                if(!confirm.has_key())return {};
                auto received=packet_transfer.receive(*request);if(received.code==app::QwkTransferCode::disconnected)return {};
                if(received.code!=app::QwkTransferCode::completed||!received.source)return qwk_transfer_notice(received.code,received.refusal,true);
                if(!qwk_authorized(*request))return "Reply authority changed. Nothing imported.";
                auto imported=database().import_qwk_reply(request->actor,received.source);
                bool absence_confirmed=false;
                if(!imported){const auto barrier=database().checkpoint();const auto outcome=database().qwk_import_outcome(request->actor,received.source->info());if(outcome&&outcome.value&&*outcome.value)imported={app::StorageStatus::success(),**outcome.value};else absence_confirmed=bool(barrier)&&outcome&&outcome.value&&!outcome.value->has_value();}
                if(!imported||!imported.value)return qwk_import_notice(imported.status,absence_confirmed);
                received.source->confirm_completed();
                if(imported.value->replayed)return std::string(app::kQwkReplayNotice);
                return "Reply packet imported: "+std::to_string(imported.value->messages)+" message(s), "+std::to_string(imported.value->commands)+" ADD/DROP change(s)."+(imported.value->missing_parents?"\n"+std::to_string(imported.value->missing_parents)+" reply link(s) unavailable; accepted as independently authorized new messages.":"");
            };
            app::QwkCallerSession offline({database(),message_terminal,message_actor,[&]{return store.identifiers().generate();},[&]() -> std::optional<app::MessageArea>{return messages.refresh_selection()?messages.selection():std::nullopt;},[&](std::string_view text)->std::optional<std::int64_t>{return qwk_local_entry_utc(text,*local,*zone);},qwk_download,qwk_upload});
