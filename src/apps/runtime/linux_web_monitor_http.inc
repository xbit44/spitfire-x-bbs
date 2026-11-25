    api_get("/api/control/monitor",[&](const auto& request,auto& response){
        if(!request.params.empty())throw std::runtime_error("monitor_query_invalid");
        std::lock_guard<std::mutex> lock(monitor_mutex);const auto now=clock.monotonic_millis();
        if(!monitor_sample_ms||now<*monitor_sample_ms||now-*monitor_sample_ms>=1000){monitor_snapshot=monitor_rates.sample(rt::read_linux_monitor_counters(),now);monitor_sample_ms=now;}
        const auto& view=monitor_snapshot;const auto& host=view.host;const auto metrics=runtime.monitor_runtime();const auto boot=rt::linux_boot_milliseconds();Json cores=Json::array();
        for(std::size_t i=0;i<view.cores.size();++i)cores.push_back({{"name",view.cores[i]},{"percent",host.cpu_available&&i<host.per_core_cpu_percent.size()?Json(host.per_core_cpu_percent[i]):Json(nullptr)}});
        const auto bytes=[](bool available,std::uint64_t value){return available?Json(std::to_string(value)):Json(nullptr);};
        const auto state=runtime.status();const bool running=state.lifecycle.state==app::RuntimeState::online||state.lifecycle.state==app::RuntimeState::draining;
        json_response(response,{{"sample_ms",std::to_string(*monitor_sample_ms)},{"bbs_running",running},
            {"cpu",{{"present",view.cpu_present},{"percent",host.cpu_available?Json(host.total_cpu_percent):Json(nullptr)},{"cores",cores}}},
            {"memory",{{"total",bytes(host.memory_available,host.memory_total_bytes)},{"used",bytes(host.memory_available,host.memory_used_bytes)},{"available",bytes(host.memory_available,host.memory_available_bytes)}}},
            {"disks",{{"present",view.disks_present},{"devices",view.disks},{"read_rate",host.storage_activity_available?Json(host.storage_read_bytes_per_second):Json(nullptr)},{"write_rate",host.storage_activity_available?Json(host.storage_write_bytes_per_second):Json(nullptr)}}},
            {"network",{{"present",view.network_present},{"interfaces",view.interfaces},{"received",bytes(view.network_present,host.network_received_bytes)},{"sent",bytes(view.network_present,host.network_sent_bytes)},{"receive_rate",host.network_available?Json(host.network_receive_bytes_per_second):Json(nullptr)},{"send_rate",host.network_available?Json(host.network_send_bytes_per_second):Json(nullptr)}}},
            {"sqlite",{{"available",metrics.sqlite.available},{"interval_available",metrics.sqlite.interval_available},{"reads",bytes(metrics.sqlite.interval_available,metrics.sqlite.page_reads_per_second)},{"writes",bytes(metrics.sqlite.interval_available,metrics.sqlite.page_writes_per_second)},{"cache_basis_points",metrics.sqlite.cache_hit_available?Json(metrics.sqlite.cache_hit_basis_points):Json(nullptr)}}},
            {"uptime",{{"os_ms",boot?Json(std::to_string(*boot)):Json(nullptr)},{"bbs_ms",bytes(metrics.uptime_available,metrics.uptime_milliseconds)}}}});
    });
