void message_maintenance_protocol_checks(){
    for(const auto kind:{app::MessageMaintenanceKind::retention,app::MessageMaintenanceKind::integrity}){
        auto c=app::default_message_scheduler_configuration(app::message_job_id(kind));
        app::MessageMaintenanceScheduleRequest request{kind,17,c.enabled,c.local_hour,c.local_minute,c.local_weekday};
        auto bytes=app::encode_message_maintenance_schedule(request);const auto decoded=app::decode_message_maintenance_schedule(bytes);
        require(decoded && app::encode_message_maintenance_schedule(*decoded)==bytes,"IPC31 maintenance settings exact typed roundtrip");
        for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_maintenance_schedule({bytes.begin(),bytes.begin()+n}),"maintenance settings truncation rejected");
        auto extra=bytes;extra.push_back(0);require(!app::decode_message_maintenance_schedule(extra),"maintenance trailing data rejected");
        extra=bytes;extra[9]=2;require(!app::decode_message_maintenance_schedule(extra),"maintenance Boolean strict");
        request.hour=24;require(app::encode_message_maintenance_schedule(request).empty(),"maintenance hour bound");
        request.hour=0;request.expected_revision=0;require(app::encode_message_maintenance_schedule(request).empty(),"maintenance revision required");
        require(app::decode_message_maintenance_kind(app::encode_message_maintenance_kind(kind))==kind,"maintenance job selector closed enum");
    }
    require(!app::decode_message_maintenance_kind({2}) && !app::decode_message_maintenance_kind({0,0}) && !app::decode_message_maintenance_kind({}),"unknown/ambiguous maintenance job selector rejected");
    app::MessageMaintenanceStatus small;small.available=true;small.diagnostic="maintenance_ready";
    auto bytes=app::encode_message_maintenance_status(small);require(!bytes.empty() && app::decode_message_maintenance_status(bytes).has_value(),"compact status current configuration/state roundtrip");
    for(std::size_t n=0;n<bytes.size();++n)require(!app::decode_message_maintenance_status({bytes.begin(),bytes.begin()+n}),"compact maintenance status truncation rejected");
    app::MessageMaintenanceRun latest;latest.id.bytes[0]=1;latest.started_utc=1791278000;latest.finished_utc=1791278001;latest.outcome=app::MessageMaintenanceOutcome::partial;latest.reason=app::MessageMaintenanceReason::storage_failure;
    for(std::int64_t n=1;n<=999;++n)latest.echoes.push_back({{n},n,1});
    small.latest=latest;
    for(std::int64_t n=-1;n<1000;++n)small.holds.push_back({n,1});
    small.state.last_detail=std::string(2048,'%');
    for(std::size_t n=0;n<32;++n)small.state.history.push_back({std::string(32,'A'),1791278000,1791278001,app::ScheduledJobResult::partial,std::string(2048,'%')});
    bytes=app::encode_message_maintenance_status(small);const auto stress=app::decode_message_maintenance_status(bytes);
    require(!bytes.empty() && bytes.size()<=65536 && stress && stress->latest->echoes.size()==999 && stress->holds.size()==1001 && stress->state.history.size()==32 && stress->state.history.front().detail.size()==128,"maximum new status remains one unchanged bounded frame, histories presentation-only truncated");
    require(small.state.history.front().detail.size()==2048,"bounded wire reporting leaves persistent source history unchanged");
    auto trailing=bytes;trailing.push_back(0);require(!app::decode_message_maintenance_status(trailing),"large maintenance status trailing byte rejected");
    auto duplicate=small;duplicate.holds.back().scope=duplicate.holds.front().scope;require(app::encode_message_maintenance_status(duplicate).empty(),"hold identities bounded/sorted/unique");
    duplicate=small;duplicate.latest->echoes.back().echo={1};require(app::encode_message_maintenance_status(duplicate).empty(),"echo result identities cannot be substituted/duplicated");
    std::cout << "IPC31 maximum maintenance job status=" << bytes.size() << " bytes, existing payload limit=65536\n";
}
