    const auto observation_epoch=[](app::Identifier128 id){std::array<char,33> text{};sodium_bin2hex(text.data(),text.size(),id.bytes.data(),id.bytes.size());return std::string(text.data());};
    const auto observation_log=[](const app::AlphaOperationalLogEntry& item){return Json{{"sequence",std::to_string(item.sequence)},{"utc",std::to_string(item.utc_seconds)},{"group",app::operational_filter_group(item)},
        {"level",item.level},{"component",item.component},{"message",item.message}};};
    api_get("/api/control/observations",[&,observation_epoch,observation_log](const auto&,auto& response){
        const auto state=runtime.status();const auto stats=runtime.persisted_statistics();const auto gate=runtime.botgate_status();Json counters=Json::array(),events=Json::array(),gate_events=Json::array();
        for(std::size_t i=0;i<state.reporting.counts.size();++i)counters.push_back({{"label",app::operational_counter_label(static_cast<app::OperationalCounter>(i))},{"value",state.reporting.available?Json(std::to_string(state.reporting.counts[i])):Json(nullptr)}});
        for(const auto& entry:state.reporting.recent)events.push_back(observation_log(entry));
        for(const auto& entry:state.logs)if(app::operational_filter_group(entry)==3)gate_events.push_back(observation_log(entry));
        const auto count=[](const app::AlphaStatisticValue& value){return Json{{"available",value.available},{"today",value.available?Json(std::to_string(value.today)):Json(nullptr)},{"total",value.available?Json(std::to_string(value.total)):Json(nullptr)}};};
        std::array<std::string,7> protection{};std::array<std::string,6> rules{};
        for(std::size_t i=0;i<protection.size();++i)protection[i]=std::to_string(gate.protection_counts[i]);
        for(std::size_t i=0;i<rules.size();++i)rules[i]=std::to_string(gate.rule_counts[i]);
        json_response(response,{{"epoch",observation_epoch(state.runtime_epoch)},{"zone",state.bbs_time_zone},{"running",runtime.bbs_owned()},{"log_health",state.log_health?app::operational_log_health_text(state.log_health):"Healthy"},
            {"statistics",{{"logons",count(stats.successful_logons)},{"seconds",count(stats.caller_seconds)},{"users",count(stats.new_users)}}},{"counters",counters},{"events",events},
            {"botgate",{{"enabled",gate.enabled},{"telnet",gate.telnet},{"ssh",gate.ssh},{"counts",protection},{"started",std::to_string(gate.protection_started)},{"rules_ready",gate.rules_ready},{"rules",rules},{"rule_status",gate.rule_status},{"geo",gate.geo_summary},
                {"feed_available",gate.available},{"feed_entries",std::to_string(gate.entries)},{"feed_result",gate.result},{"last_success",std::to_string(gate.last_success)},{"updating",gate.updating},{"progress",app::render_botgate_feed_progress(gate)},{"recent",gate_events}}}});
    });
    api_get("/api/control/logs",[&,observation_epoch,observation_log](const auto& request,auto& response){
        for(const auto& parameter:request.params)if((parameter.first!="epoch"&&parameter.first!="after")||request.get_param_value_count(parameter.first)!=1)throw std::runtime_error("log_query_invalid");
        const auto state=runtime.status();const auto epoch=observation_epoch(state.runtime_epoch);const auto requested=request.get_param_value("epoch");
        if(!requested.empty()&&(requested.size()!=32||requested.find_first_not_of("0123456789abcdef")!=std::string::npos))throw std::runtime_error("log_epoch_invalid");
        const auto after_text=request.has_param("after")?request.get_param_value("after"):"0";const auto after=number(after_text);if(std::to_string(after)!=after_text)throw std::runtime_error("log_cursor_invalid");
        if(!requested.empty()&&requested!=epoch){json_response(response,{{"reset",true},{"epoch",epoch},{"detail","The Runtime changed. The next read starts its retained log history."}});return;}
        const auto page=runtime.log_page({state.runtime_epoch,after});if(!page){json_response(response,{{"detail","Live log history is unavailable. No empty or complete history is implied."}},409);return;}
        Json entries=Json::array();for(const auto& record:page->entries){const auto entry=app::decode_operational_record(record.record,record.sequence);if(!entry)throw std::runtime_error("log_record_invalid");entries.push_back(observation_log(*entry));}
        json_response(response,{{"reset",false},{"epoch",epoch},{"after",after_text},{"next",std::to_string(page->next)},{"newest",std::to_string(page->newest)},{"gap",page->gap},{"entries",entries},{"health",state.log_health?app::operational_log_health_text(state.log_health):"Healthy"}});
    });
