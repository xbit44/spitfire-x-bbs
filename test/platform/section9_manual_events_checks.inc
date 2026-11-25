// Installed native owner-IPC qualification. Not a Config/Control Panel GUI claim.
void manual_events_checks(const ins::Path& root){
    const auto owner=ins::operator_sid();const auto receipt=ins::read_receipt(root,owner);
    const auto initial=win::inspect_clamav_status(root,owner,receipt.id);
    check(initial.verified&&initial.definitions_verified,"real verified engine/definitions before Events qualification");
    const auto policy=win::offline_file_configuration_snapshot(root,owner,{});
    check(policy.code==app::LocalAccountResultCode::ok,"capture original scanning choice without mutation");
    Runtime runtime(root);win::WindowsSecureIdentifierGenerator ids;
    const auto status=app::read_status_snapshot(0,[&](const app::AlphaStatusRequest& query)->std::optional<std::vector<std::uint8_t>>{return runtime.exchange(app::LocalControlMessageType::status_request,app::LocalControlMessageType::status_response,app::encode_status_request(query));});
    check(status.has_value(),"actual installed status before Events");
    const auto page=[&](std::uint64_t after){const auto result=app::decode_log_page(runtime.exchange(app::LocalControlMessageType::log_page_request,app::LocalControlMessageType::log_page_response,app::encode_log_cursor({status->runtime_epoch,after})));check(result.has_value(),"bounded installed Events page");return *result;};
    const auto before=page(0).newest;
    const auto send=[&](app::ManualClamAvNotice notice,bool accepted){
        const auto result=app::decode_control_response(runtime.exchange(app::LocalControlMessageType::clamav_manual_notice_request,app::LocalControlMessageType::clamav_manual_notice_response,app::encode_manual_clamav_notice(notice)));
        check(result&&((result->code==app::LocalControlResponseCode::ok)==accepted),accepted?"authenticated manual activity accepted":"unverified/changed manual success refused");
    };
    const auto now=std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const auto cancelled=ids.generate();
    send({cancelled,app::ManualClamAvPhase::started,0,{}},true);
    send({cancelled,app::ManualClamAvPhase::started,0,{}},true);
    send({cancelled,app::ManualClamAvPhase::verified,1,"clamav_definitions_verified"},false);
    send({cancelled,app::ManualClamAvPhase::cancelled,now,"clamav_test_cancelled"},true);
    send({cancelled,app::ManualClamAvPhase::cancelled,now,"clamav_test_cancelled"},true);
    send({cancelled,app::ManualClamAvPhase::failed,now,"clamav_test_failure"},false);
    const auto failed=ids.generate();send({failed,app::ManualClamAvPhase::started,0,{}},true);send({failed,app::ManualClamAvPhase::failed,now,"clamav_test_failure"},true);
    std::string actual_admission_error;
    {auto scratch_job=win::ClamAvScratch::create(root/L"Work"/L"ClamAV",owner,true);
        {const auto scratch=win::FilePayloadDirectory::private_store(scratch_job->path(),owner);win::ClamAvResourceCeilings ceiling;ceiling.free_commit=0;
            try{(void)win::ClamAvResourceLease::acquire(scratch,owner,1024,{}, {},25,ceiling,win::ClamAvResourceKind::definition_update);}
            catch(const std::exception& e){actual_admission_error=e.what();}}
        check(actual_admission_error=="scan_admission_commit_headroom"&&scratch_job->cleanup(),"actual native admission refusal obtained without launching vendor or pressure");}
    for(const auto& diagnostic:std::vector<std::string>{"scan_admission_memory_headroom",actual_admission_error,"file_receive_reservation_failed"}){
        const auto refused=ids.generate();send({refused,app::ManualClamAvPhase::started,0,{}},true);
        send({refused,app::ManualClamAvPhase::failed,now,diagnostic},true);
        send({refused,app::ManualClamAvPhase::failed,now,diagnostic},true);
    }
    const auto operation=ids.generate();send({operation,app::ManualClamAvPhase::started,0,{}},true);
    const auto result=win::update_clamav_definitions(root,owner,receipt.id,now,{},{},180000,win::ClamAvUpdateMode::manual);
    std::cout<<"ACTUAL MANUAL "<<result.diagnostic<<std::endl;
    check(result.attempted&&result.process_reaped&&result.cleanup_complete&&(result.succeeded||result.diagnostic=="clamav_update_vendor_cooldown"),"real supervised vendor check while installed Runtime runs, or respected cooldown");
    send({operation,result.succeeded?app::ManualClamAvPhase::verified:app::ManualClamAvPhase::failed,now,result.diagnostic},true);
    const auto events=page(before);unsigned starts{},ends{},problems{};
    for(const auto& encoded:events.entries){const auto entry=app::decode_operational_record(encoded.record,encoded.sequence);check(entry.has_value(),"actual Events record decodes");
        if(!entry->event)continue;const auto& event=*entry->event;const auto& name=event.context.resource_name;
        if(name=="Manual ClamAV definition update started")++starts;
        else if(name.rfind("Manual ClamAV definition update ",0)==0||name.rfind("Manual ClamAV: ",0)==0){++ends;if(event.kind==app::OperationalKind::file_maintenance_problem)++problems;}
    }
    check(starts==6&&ends==6&&problems==(result.succeeded?4U:5U),"Events contain exact start/outcome pairs incl admission failures, idempotent retries and no forged success");
    runtime.stop();const auto after=win::offline_file_configuration_snapshot(root,owner,{});
    check(after.code==app::LocalAccountResultCode::ok&&after.policy.scan_uploads==policy.policy.scan_uploads&&after.policy.revision==policy.policy.revision,"Events/manual update leave original scanning choice and revision exact");
}
