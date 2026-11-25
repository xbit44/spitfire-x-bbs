std::string_view scheduled_job_id(unsigned selected){
    require(selected>=1&&selected<=3,"job_invalid");
    return selected==1?app::kBackupJobId:selected==2?app::kMessageRetentionJobId:app::kMessageIntegrityJobId;
}
namespace {
const char* job_title(unsigned selected){return selected==1?"backup":selected==2?"Message Retention":"Message Integrity";}
const char* job_result(app::ScheduledJobResult value){
    switch(value){case app::ScheduledJobResult::never:return "Never run";case app::ScheduledJobResult::running:return "Running";
    case app::ScheduledJobResult::success:return "Success";case app::ScheduledJobResult::failed:return "Failed";
    case app::ScheduledJobResult::interrupted:return "Interrupted";case app::ScheduledJobResult::partial:return "Partial";case app::ScheduledJobResult::skipped:return "Skipped";}return "Unknown";
}
Json job_fields(app::SchedulerConfiguration& value,unsigned selected,const Json* input=nullptr){
    Fields f;f.input=input;f.boolean("enabled","Enable this scheduled job",value.enabled);
    if(selected==3){unsigned weekday=value.local_weekday.value_or(0);f.integer("weekday","Day of week",weekday,0,6);value.local_weekday=static_cast<std::uint8_t>(weekday);
        f.items.back()["type"]="select";f.items.back()["choices"]=Json::array();const std::array<const char*,7> days{{"Sunday","Monday","Tuesday","Wednesday","Thursday","Friday","Saturday"}};
        for(unsigned i=0;i<days.size();++i)f.items.back()["choices"].push_back({{"value",i},{"label",days[i]}});}
    f.integer("hour","BBS local hour (0-23)",value.local_hour,0,23);f.integer("minute","Minute (0-59)",value.local_minute,0,59);
    if(selected==1){f.text("destination","Backup folder on the Linux computer",value.backup_destination,4096,"Blank uses this board's Backup/Scheduled folder. An existing custom folder must be private and owned by the BBS account. Existing archives are not moved.");
        f.integer("retained","Verified managed backups to retain",value.retained_backups,1,365,"Stop the BBS and wait for active jobs to finish before changing retention. Saving does not delete archives.");}
    f.finish();return f.items;
}
std::pair<unsigned,app::SchedulerConfiguration> job_update(const Json& request){
    require(request.is_object()&&request.size()==3&&request.contains("revision")&&request.contains("record")&&request.contains("values"),"job_request_invalid");
    const auto selected=static_cast<unsigned>(bounded_number(request.at("record"),1,3));app::SchedulerConfiguration config;
    config.job_id=scheduled_job_id(selected);config.revision=revision(request.at("revision"));
    if(selected>1){config=app::default_message_scheduler_configuration(config.job_id);config.revision=revision(request.at("revision"));}
    (void)job_fields(config,selected,&request.at("values"));require(app::valid_scheduler_configuration(config),"schedule_invalid");return {selected,config};
}
}
Json scheduled_job_page(const LinuxScheduledJobSnapshot& snapshot,unsigned selected,const app::MessageMaintenanceStatus* maintenance){
    const auto id=scheduled_job_id(selected);require(snapshot.available&&snapshot.configuration.job_id==id,"job_unavailable");
    auto config=snapshot.configuration;auto fields=job_fields(config,selected);
    if(selected==1)fields.back()["readonly"]=!snapshot.paused||snapshot.any_active;
    Json history=Json::array();for(const auto& item:snapshot.state.history)history.push_back({{"run",item.run_id},{"started",item.started_utc},{"finished",item.finished_utc},{"result",job_result(item.result)},{"detail",item.detail}});
    Json status={{"paused",snapshot.paused},{"active",snapshot.state.active},{"queued",snapshot.queued},{"active_run",snapshot.state.active_run_id},{"active_started",snapshot.state.active_started_utc},{"active_scheduled",snapshot.state.active_scheduled},{"any_active",snapshot.any_active},{"next",snapshot.state.next_run_utc},{"last",snapshot.state.last_run_utc},
        {"result",job_result(snapshot.state.last_result)},{"detail",snapshot.state.last_detail},{"diagnostic",snapshot.diagnostic},{"history",history}};
    if(maintenance){status["verification_required"]=maintenance->shared_verification_required;status["maintenance_available"]=maintenance->available;status["holds"]=Json::array();
        for(const auto& hold:maintenance->holds)status["holds"].push_back({{"scope",std::to_string(hold.scope)},{"count",hold.count}});
        if(maintenance->latest){const auto& latest=*maintenance->latest;status["totals"]={{"removed_public",latest.removed_public},{"expired_unread",latest.expired_unread},{"committed_batches",latest.committed_batches}};}}
    return {{"route","tools.backup_schedule"},{"title","Backup & Restore - Schedules"},{"category","Tools & Maintenance"},{"revision",std::to_string(config.revision)},{"record",selected},
        {"records",Json::array({{{"id",1},{"name","backup"}},{{"id",2},{"name","Message Retention"}},{{"id",3},{"name","Message Integrity"}}})},
        {"can_save",true},{"can_remove",false},{"can_run",!snapshot.any_active&&(selected==1||!snapshot.paused)},{"job_title",job_title(selected)},
        {"activation","Schedules use BBS local time. Missed runs are skipped; jobs do not overlap or retry automatically. Saving never runs a job."},
        {"fields",fields},{"active_fields",fields},{"job_status",status}};
}
BackupScheduleUpdate backup_schedule_update(const Json& request){
    const auto parsed=job_update(request);require(parsed.first==1,"backup_job_required");const auto& config=parsed.second;BackupScheduleUpdate result;
    result.request.expected_revision=config.revision;result.request.enabled=config.enabled;result.request.local_hour=config.local_hour;result.request.local_minute=config.local_minute;result.request.backup_destination=config.backup_destination;result.retained=config.retained_backups;return result;
}
app::MessageMaintenanceScheduleRequest message_schedule_update(const Json& request){
    const auto parsed=job_update(request);require(parsed.first>1,"message_job_required");const auto& config=parsed.second;app::MessageMaintenanceScheduleRequest result;
    result.kind=parsed.first==2?app::MessageMaintenanceKind::retention:app::MessageMaintenanceKind::integrity;result.expected_revision=config.revision;result.enabled=config.enabled;result.hour=config.local_hour;result.minute=config.local_minute;result.weekday=config.local_weekday;
    require(!app::encode_message_maintenance_schedule(result).empty(),"message_schedule_invalid");return result;
}

namespace {
std::string event_time(unsigned minute) {
    const auto hour_text = std::to_string(minute / 60);
    const auto minute_text = std::to_string(minute % 60);
    return (hour_text.size() == 1 ? "0" : "") + hour_text + ":" +
        (minute_text.size() == 1 ? "0" : "") + minute_text;
}
std::string event_calendar_text(unsigned mask, unsigned count, const char* all) {
    if (mask == (1U << count) - 1) return all;
    std::string out;
    for (unsigned i = 0; i < count; ++i) {
        if (mask & (1U << i)) {
            if (!out.empty()) out += ", ";
            out += std::to_string(i + 1);
        }
    }
    return out;
}
unsigned event_number(std::string_view text, unsigned minimum, unsigned maximum) {
    unsigned value{};
    const auto decoded = std::from_chars(text.data(), text.data() + text.size(), value);
    require(decoded.ec == std::errc{} && decoded.ptr == text.data() + text.size() &&
        value >= minimum && value <= maximum, "event_number_invalid");
    return value;
}
unsigned event_calendar_mask(const std::string& input, unsigned count, const char* all) {
    if (input == all) return (1U << count) - 1;
    std::istringstream stream(input);
    std::string token;
    unsigned mask{};
    while (std::getline(stream, token, ',')) {
        const auto first = token.find_first_not_of(' ');
        const auto last = token.find_last_not_of(' ');
        if (first == token.npos) return 0;
        const auto value = event_number(std::string_view(token).substr(first,last - first + 1),1,count);
        mask |= 1U << (value - 1);
    }
    return mask;
}
Json external_event_fields(app::ExternalEvent& event, const Json* input = nullptr) {
    Fields f;
    f.input = input;
    f.text("event_name","Event name",event.name,80);
    f.boolean("event_enabled","Enabled",event.enabled);
    f.text("event_program","Program or script",event.program,1024,"Enter the full path on the BBS host.");
    f.text("event_directory","Working folder",event.directory,1024);
    f.text("event_arguments","Arguments (optional)",event.arguments,2048);
    auto months = event_calendar_text(event.months,12,"Any month");
    auto dates = event_calendar_text(event.dates,31,"Any date");
    auto weekdays = event_calendar_text(event.weekdays,7,"All 7 days");
    f.text("event_months","Months",months,64);
    f.text("event_dates","Days of month",dates,128);
    f.text("event_weekdays","Days of week",weekdays,32);
    unsigned timing = !event.id ? 0U : event.runs_per_day ? 2U : 1U;
    f.integer("event_timing","Timing",timing,0,2);
    f.items.back()["type"] = "select";
    f.items.back()["choices"] = Json::array({{{"value",0},{"label","Choose timing"}},
        {{"value",1},{"label","At a specific time"}},{{"value",2},{"label","Runs per day"}}});
    auto time = !event.id ? std::string{} : event.runs_per_day ? std::to_string(event.runs_per_day) : event_time(event.minute);
    f.text("event_time","Time (HH:MM) or run count",time,5);
    f.boolean("event_exclusive","Take all nodes offline for this event",event.exclusive);
    f.boolean("event_cutoff","End calls at the scheduled time",event.end_calls);
    f.items.back()["enabled_by"] = "event_exclusive";
    const auto duration = [&](const char* key, const char* label, unsigned& value, bool wait_limit) {
        const auto supplied = f.field(key,label,"optional_number",value ? Json(value) : Json(nullptr));
        f.items.back()["minimum"] = 1;
        f.items.back()["maximum"] = UINT_MAX;
        f.items.back()["allow_empty"] = !wait_limit;
        if (wait_limit) f.items.back()["enabled_by"] = "event_exclusive";
        if (supplied) value = supplied->is_null() ? 0 : bounded_number(*supplied, 1, UINT_MAX);
    };
    duration("event_wait","Wait limit for callers and Doors (minutes)",event.wait_minutes,true);
    f.boolean("event_startup","Run when the BBS starts",event.at_start);
    f.boolean("event_missed","Run missed event when the BBS starts",event.run_missed);
    duration("event_warning","Warn if still running after (minutes)",event.warn_minutes,false);
    f.finish();
    if (input) {
        event.months = event_calendar_mask(months,12,"Any month");
        event.dates = event_calendar_mask(dates,31,"Any date");
        event.weekdays = event_calendar_mask(weekdays,7,"All 7 days");
        if (timing == 1) {
            require(time.size() == 5 && time[2] == ':',"event_time_required");
            event.minute = event_number(time.substr(0,2),0,23) * 60 + event_number(time.substr(3),0,59);
        } else {
            require(timing == 2,"event_timing_required");
            event.runs_per_day = event_number(time,1,1440);
        }
        if (!event.exclusive) event.end_calls = false;
    }
    return f.items;
}
}

Json external_events_page(const app::ExternalEventCatalog& catalog, std::optional<std::uint64_t> selected,
    const app::BbsLocalTimeAdapters& zone, std::int64_t now) {
    app::ExternalEvent event;
    if (!selected && !catalog.events.empty()) selected = catalog.events.front().id;
    Json records = Json::array();
    for (const auto& row : catalog.events) {
        if (selected == row.id) event = row;
        const auto next = app::external_event_slot(row, zone, now);
        std::string schedule = row.runs_per_day ? std::to_string(row.runs_per_day) + " runs per day" : event_time(row.minute);
        if (row.at_start) schedule += " and at startup";
        records.push_back({{"id",std::to_string(row.id)},{"name",row.name},{"schedule",schedule},
            {"next",next ? zone.format_utc_seconds(*next).value_or("Unavailable") : row.enabled ? "Unavailable" : "—"},
            {"enabled",row.enabled}});
    }
    const auto fields = external_event_fields(event);
    return {{"route","tools.scheduled_jobs"},{"title","Scheduled Jobs"},{"category","Tools & Maintenance"},
        {"revision",std::to_string(catalog.revision)},{"record",std::to_string(event.id)},{"records",records},
        {"fields",fields},{"active_fields",fields},{"can_save",true},{"can_remove",event.id != 0}};
}

app::ExternalEvent external_event_update(const Json& supplied) {
    app::ExternalEvent event;
    (void)external_event_fields(event,&supplied);
    require(app::valid_external_event(event) && std::filesystem::u8path(event.program).is_absolute() &&
        std::filesystem::u8path(event.directory).is_absolute(),"external_event_settings_invalid");
    return event;
}
