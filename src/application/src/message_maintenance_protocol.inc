namespace {
bool valid_maintenance_report(const MessageMaintenanceRun& run){
    if(!run.id.valid() || run.kind>MessageMaintenanceKind::integrity || run.started_utc<=0 || run.finished_utc<0 ||
        (run.finished_utc && run.finished_utc<run.started_utc) || run.outcome>MessageMaintenanceOutcome::interrupted ||
        run.reason>MessageMaintenanceReason::runtime_restart || run.removed_public<0 || run.expired_unread<0 || run.committed_batches<0 || run.echoes.size()>999)return false;
    std::int64_t last{};for(const auto& e:run.echoes){if(e.echo.value<=last || e.removed<0 || e.protected_overage<0)return false;last=e.echo.value;}return true;
}
void write_maintenance_report(Writer& w,const MessageMaintenanceRun& run){
    w.raw(run.id.bytes.data(),16);w.u8(static_cast<std::uint8_t>(run.kind));w.u64(run.started_utc);w.u64(run.finished_utc);
    w.u8(static_cast<std::uint8_t>(run.outcome));w.u8(static_cast<std::uint8_t>(run.reason));
    w.u64(run.removed_public);w.u64(run.expired_unread);w.u64(run.committed_batches);w.u16(static_cast<std::uint16_t>(run.echoes.size()));
    for(const auto& e:run.echoes){w.u64(e.echo.value);w.u64(e.removed);w.u64(e.protected_overage);}
}
bool read_maintenance_report(Reader& r,MessageMaintenanceRun& run){
    std::uint8_t kind{},outcome{},reason{};std::uint16_t count{};
    if(!r.raw(run.id.bytes.data(),16) || !r.u8(kind) || !nonnegative64(r,run.started_utc) || !nonnegative64(r,run.finished_utc) ||
        !r.u8(outcome) || !r.u8(reason) || !nonnegative64(r,run.removed_public) || !nonnegative64(r,run.expired_unread) ||
        !nonnegative64(r,run.committed_batches) || !r.u16(count) || count>999)return false;
    run.kind=static_cast<MessageMaintenanceKind>(kind);run.outcome=static_cast<MessageMaintenanceOutcome>(outcome);run.reason=static_cast<MessageMaintenanceReason>(reason);
    for(std::uint16_t i=0;i<count;++i){MessageMaintenanceEchoResult e;if(!nonnegative64(r,e.echo.value) || !nonnegative64(r,e.removed) || !nonnegative64(r,e.protected_overage))return false;run.echoes.push_back(e);}
    return valid_maintenance_report(run);
}
}
std::vector<std::uint8_t> encode_message_maintenance_kind(MessageMaintenanceKind kind){
    if(kind>MessageMaintenanceKind::integrity)return {};
    return {static_cast<std::uint8_t>(kind)};
}
std::optional<MessageMaintenanceKind> decode_message_maintenance_kind(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()!=1 || bytes.front()>1)return {};
    return static_cast<MessageMaintenanceKind>(bytes.front());
}
std::vector<std::uint8_t> encode_message_maintenance_schedule(const MessageMaintenanceScheduleRequest& request){
    if(request.kind>MessageMaintenanceKind::integrity || !request.expected_revision)return {};
    auto c=default_message_scheduler_configuration(message_job_id(request.kind));c.enabled=request.enabled;
    c.local_hour=request.hour;c.local_minute=request.minute;c.local_weekday=request.weekday;
    if(!valid_scheduler_configuration(c))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(request.kind));w.u64(request.expected_revision);w.u8(request.enabled?1:0);
    w.u8(request.hour);w.u8(request.minute);w.u8(request.weekday?*request.weekday:255);return w.take();
}
std::optional<MessageMaintenanceScheduleRequest> decode_message_maintenance_schedule(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);MessageMaintenanceScheduleRequest request;std::uint8_t kind{},enabled{},day{};
    if(!r.u8(kind) || kind>1 || !r.u64(request.expected_revision) || !r.u8(enabled) || enabled>1 ||
        !r.u8(request.hour) || !r.u8(request.minute) || !r.u8(day) || !r.complete())return {};
    request.kind=static_cast<MessageMaintenanceKind>(kind);request.enabled=enabled!=0;if(day!=255)request.weekday=day;
    if(encode_message_maintenance_schedule(request).empty())return {};
    return request;
}
std::vector<std::uint8_t> encode_message_maintenance_status(const MessageMaintenanceStatus& status){
    if(status.kind>MessageMaintenanceKind::integrity || status.configuration.format_version!=3 || status.state.format_version!=2 ||
        status.configuration.job_id!=message_job_id(status.kind) || status.state.job_id!=status.configuration.job_id ||
        !valid_scheduler_configuration(status.configuration) || !valid_scheduler_state(status.state) || status.diagnostic.size()>128 ||
        !std::all_of(status.diagnostic.begin(),status.diagnostic.end(),[](unsigned char ch){return ch>=32 && ch<=126;}) ||
        status.holds.size()>1001 || (status.latest && (!valid_maintenance_report(*status.latest) || status.latest->kind!=status.kind)))return {};
    // One job per bounded frame. The persisted state retains its own full detail;
    // presentation truncates history only, never expands the existing buffer.
    auto presentation=status.state;for(auto& item:presentation.history)if(item.detail.size()>128)item.detail.resize(128);
    const auto config=serialize_scheduler_configuration(status.configuration),state=serialize_scheduler_state(presentation);
    Writer w;w.u8(static_cast<std::uint8_t>(status.kind));w.u8(status.available?1:0);w.u8(status.shared_verification_required?1:0);
    if(!w.text(status.diagnostic,128) || !w.text(config,1024) || !w.text(state,24000))return {};
    w.u8(status.latest?1:0);if(status.latest)write_maintenance_report(w,*status.latest);
    w.u16(static_cast<std::uint16_t>(status.holds.size()));std::int64_t previous=-2;
    for(const auto& hold:status.holds){if(hold.scope< -1 || hold.scope<=previous || hold.count<1)return {};previous=hold.scope;w.u64(static_cast<std::uint64_t>(hold.scope));w.u64(hold.count);}
    auto bytes=w.take();return bytes.size()<=kLocalControlMaximumPayloadBytes?bytes:std::vector<std::uint8_t>{};
}
std::optional<MessageMaintenanceStatus> decode_message_maintenance_status(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);MessageMaintenanceStatus status;std::uint8_t kind{},available{},guard{},latest{};std::string configuration,state;
    if(!r.u8(kind) || kind>1 || !r.u8(available) || available>1 || !r.u8(guard) || guard>1 || !r.text(status.diagnostic,128) || !r.text(configuration,1024) ||
        !r.text(state,24000) || !r.u8(latest) || latest>1)return {};
    status.kind=static_cast<MessageMaintenanceKind>(kind);status.available=available!=0;
    status.shared_verification_required=guard!=0;
    const auto c=parse_scheduler_configuration(configuration);const auto s=parse_scheduler_state(state);if(!c || !s)return {};status.configuration=*c;status.state=*s;
    if(latest){MessageMaintenanceRun run;if(!read_maintenance_report(r,run))return {};status.latest=std::move(run);}
    std::uint16_t count{};if(!r.u16(count) || count>1001)return {};
    for(std::uint16_t i=0;i<count;++i){std::uint64_t scope{};MessageIntegrityFinding hold;
        if(!r.u64(scope) || (scope!=UINT64_MAX && scope>INT64_MAX) || !nonnegative64(r,hold.count))return {};
        hold.scope=scope==UINT64_MAX?-1:static_cast<std::int64_t>(scope);status.holds.push_back(hold);}
    if(!r.complete() || encode_message_maintenance_status(status).empty())return {};
    return status;
}
