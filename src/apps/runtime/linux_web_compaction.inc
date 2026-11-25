    api_get("/api/compaction",[&](const auto&,auto& response){
        const auto value=runtime.compaction_status();json_response(response,{{"operation",std::to_string(value.operation)},{"active",value.active},{"completed",value.completed},
            {"stage",value.stage},{"detail",value.detail},{"backup_verified",value.backup_verified},{"backup",value.backup.string()},{"temporary",value.temporary.string()},
            {"success",value.completed&&value.result.status.succeeded()&&value.result.logical_content_verified&&value.reopened},{"reopened",value.reopened},
            {"physical_commit",value.result.physical_compaction_committed},{"logical_verified",value.result.logical_content_verified},
            {"bytes_before",std::to_string(value.result.bytes_before)},{"bytes_after",std::to_string(value.result.bytes_after)},{"private_temporary_files",value.result.private_temporary_files}});
    });
    api_post("/api/compaction/start",[&](const auto& request,auto& response){require_keys(parse(request),{});const auto operation=runtime.request_compaction();
        json_response(response,{{"operation",std::to_string(operation)},{"detail",operation?"Compaction request accepted. Check its status; acceptance does not mean completion.":"Compaction refused. Stop the BBS normally and wait for other jobs. No callers were forced off."}},operation?202:409);
    });
    api_post("/api/compaction/cancel",[&](const auto& request,auto& response){const auto value=parse(request);require_keys(value,{"operation"});const auto stopped=runtime.cancel_compaction(number(value.at("operation").template get<std::string>()));
        json_response(response,{{"accepted",stopped},{"detail",stopped?"Cancellation requested. Wait for the actual result; a physical commit may already have happened.":"This operation is no longer active. Refresh its actual result."}},stopped?202:409);
    });
