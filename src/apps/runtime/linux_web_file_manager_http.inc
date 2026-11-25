    const auto file_owner=[&](const httplib::Request& request){return cookie_name+'|'+cookie(request,cookie_name);};
    const auto file_review=[](const std::optional<rt::web::WebFileReview>& value){
        if(!value)return Json{{"found",false}};
        return Json{{"found",true},{"handle",value->handle},{"choices",value->choices},{"submitted",value->submitted},{"known",value->known},{"status",value->known?rt::web::web_file_status(value->status):Json(nullptr)}};
    };
    api_get("/api/files/read",[&](const auto& request,auto& response){
        Json query={{"kind","records"},{"area","0"},{"search",""},{"after","0"},{"selection","0"},{"pending_after",nullptr}};
        for(const auto& p:request.params){if(!query.contains(p.first)||request.get_param_value_count(p.first)!=1)throw std::runtime_error("file_query_invalid");query[p.first]=p.second;}
        json_response(response,rt::web::web_file_snapshot(runtime.query_file_management(rt::web::web_file_query(query))));
    });
    api_get("/api/files/hatch-catalog",[&](const auto& request,auto& response){if(!request.params.empty())throw std::runtime_error("file_query_invalid");const auto value=runtime.file_hatch_choices();if(!value){json_response(response,{{"detail","FTN destinations are unavailable. No file was queued."}},409);return;}json_response(response,rt::web::web_hatch_catalog(*value));});
    api_get("/api/files/areas",[&](const auto& request,auto& response){app::LocalFileConfigurationQuery query;query.part=app::FileConfigurationPart::areas;
        for(const auto& p:request.params){if((p.first!="revision"&&p.first!="after_id")||request.get_param_value_count(p.first)!=1)throw std::runtime_error("file_query_invalid");const auto n=number(p.second);if(n>=static_cast<std::uint64_t>(INT64_MAX)||std::to_string(n)!=p.second)throw std::runtime_error("file_query_invalid");if(p.first=="revision")query.catalog_revision=static_cast<std::int64_t>(n);else query.after_id=static_cast<std::int64_t>(n);}
        const auto value=runtime.file_configuration_snapshot(query);if(value.code!=app::LocalAccountResultCode::ok){json_response(response,{{"detail","File areas changed. Reload and review the destination."}},409);return;}json_response(response,rt::web::file_catalog_page(value,query,""));
    });
    api_get("/api/files/job",[&,file_owner,file_review](const auto& request,auto& response){if(!request.params.empty())throw std::runtime_error("file_query_invalid");json_response(response,file_review(file_actions.current(file_owner(request))));});
    api_post("/api/files/prepare",[&,file_owner,file_review](const auto& request,auto& response){
        const auto value=parse(request,131072);app::Identifier128 job,operation;randombytes_buf(job.bytes.data(),job.bytes.size());randombytes_buf(operation.bytes.data(),operation.bytes.size());const auto task=rt::web::web_file_task(value,job,operation);
        // Re-read the exact catalog record for a useful stale-selection refusal.
        // Native jobs independently revalidate payload custody and revisions.
        if(task.kind==app::FileManagementTaskKind::operation){app::FileManagementReadRequest query;query.kind=app::FileManagementQueryKind::detail;query.selection=task.operation.file;const auto current=runtime.query_file_management(query);
            if(current.code!=app::LocalAccountResultCode::ok||current.records.size()!=1||current.records.front().file.revision!=task.operation.expected_revision||current.records.front().operation_pending||current.records.front().purged){json_response(response,{{"detail","The selected file changed or has unfinished work. Refresh and review it first."}},409);return;}}
        const auto result=file_actions.prepare(file_owner(request),task,value);if(!result){json_response(response,{{"detail","Review the current job or verify the selected file first. No new job was started."}},409);return;}json_response(response,file_review(result));
    });
    api_post("/api/files/job",[&,file_owner,file_review](const auto& request,auto& response){const auto value=parse(request,512);require_keys(value,{"handle","action"});json_response(response,file_review(file_actions.act(file_owner(request),value.at("handle").template get<std::string>(),value.at("action").template get<std::string>())));});
