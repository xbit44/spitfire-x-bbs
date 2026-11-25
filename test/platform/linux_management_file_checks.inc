void file_checks(rt::LinuxRuntimeHost& runtime,const std::filesystem::path& root,bool online,host::LinuxSecureIdentifierGenerator& identifiers){
    using Type=app::LocalControlMessageType;
    const auto policy=app::decode_file_configuration_snapshot(exchange(runtime,Type::file_configuration_query_request,app::encode_file_configuration_query({}),Type::file_configuration_query_response));
    check(policy&&policy->code==app::LocalAccountResultCode::ok,"typed native File Policy snapshot");
    app::FileConfigurationChange edit;edit.operation=identifiers.generate();edit.expected_catalog_revision=policy->policy.catalog_revision;edit.policy=policy->policy;edit.policy.lightbar_default=!edit.policy.lightbar_default;
    const auto saved=app::decode_file_configuration_result(exchange(runtime,Type::file_configuration_change_request,app::encode_file_configuration_change(edit),Type::file_configuration_change_response));
    check(saved&&saved->code==app::LocalAccountResultCode::ok&&runtime.bbs_owned()==online,"typed File Policy save keeps BBS state");
    check(runtime.change_file_configuration(edit).result.replayed,"exact File Policy replay");edit.operation=identifiers.generate();check(runtime.change_file_configuration(edit).code==app::LocalAccountResultCode::conflict,"stale File Policy refusal");
    app::LocalFileConfigurationQuery area_query;area_query.part=app::FileConfigurationPart::areas;area_query.group={1};const auto areas=runtime.file_configuration_snapshot(area_query);
    check(areas.code==app::LocalAccountResultCode::ok&&!areas.areas.empty(),"native File Areas available");const auto area=areas.areas.front();
    const auto folder=root/std::filesystem::u8path(area.storage_path);const std::string name=online?"live-raw.txt":"stopped-raw.txt";const auto path=folder/name;check(!std::filesystem::exists(path),"owned external-drop test filename fresh");
    {const auto prior=umask(0022);{std::ofstream output(path,std::ios::binary);output<<"Owner supplied text\r\nLiteral @BBS@ text.\r\n";}umask(prior);}
    const auto permissions=[&]{struct stat info{};check(stat(path.c_str(),&info)==0,"raw drop permission observation");return info.st_mode&07777;};
    check(permissions()==0644,"normal externally copied Sysop file starts 0644");
    const auto finish=[&](app::FileManagementTask task){
        const auto started=app::decode_file_management_status(exchange(runtime,Type::file_management_start_request,app::encode_file_management_task(task),Type::file_management_start_response));
        check(started&&started->code==app::LocalAccountResultCode::ok,"typed native file job starts");auto state=*started;
        for(unsigned attempt=0;state.state==app::FileManagementJobState::running&&attempt<2000;++attempt){std::this_thread::sleep_for(std::chrono::milliseconds(5));state=runtime.file_management_status(task.job);}
        const auto terminal=app::decode_file_management_status(exchange(runtime,Type::file_management_status_request,app::encode_file_management_job(task.job),Type::file_management_status_response));
        check(terminal&&terminal->state!=app::FileManagementJobState::running,"typed file job finishes with bounded response");return *terminal;
    };
    app::FileManagementTask task;task.job=identifiers.generate();task.kind=app::FileManagementTaskKind::preview_import;task.choice.operation=identifiers.generate();task.choice.area=area.area.id;task.choice.source_path=path.u8string();
    auto preview=finish(task);check(preview.state==app::FileManagementJobState::completed&&preview.preview&&preview.preview->mode==app::FileImportMode::in_place&&!preview.journal_confirmed,"actual host previews externally dropped file read-only in place");
    check(permissions()==0644,"typed Runtime preview preserves original permissions");
    if(online){
        check(preview.duplicate_contents>0,"second actual source shows duplicate content warning");
        app::FileManagementTask refused;refused.job=identifiers.generate();refused.kind=app::FileManagementTaskKind::import_file;refused.import=*preview.preview;
        const auto blocked=finish(refused);check(blocked.state==app::FileManagementJobState::refused&&!blocked.journal_confirmed&&std::filesystem::exists(path),"duplicate content refusal preserves unregistered source before journal");
        check(permissions()==0644,"duplicate refusal preserves original permissions");
        task.job=identifiers.generate();task.choice.allow_duplicate_content=true;preview=finish(task);
        check(preview.preview&&preview.preview->registration.allow_duplicate_content,"new explicit duplicate confirmation retains reviewed source");
    }
    task={};task.job=identifiers.generate();task.kind=app::FileManagementTaskKind::import_file;task.import=*preview.preview;const auto imported=finish(task);
    check(imported.state==app::FileManagementJobState::completed&&imported.journal_confirmed&&imported.result.value>0,"actual host confirms raw drop and commits shared catalog");
    check(permissions()==0600,"typed Runtime confirmation establishes private custody");
    app::FileManagementReadRequest read;read.kind=app::FileManagementQueryKind::detail;read.selection=imported.result;
    const auto selected=app::decode_file_management_read_snapshot(exchange(runtime,Type::file_management_query_request,app::encode_file_management_read_request(read),Type::file_management_query_response));
    check(selected&&selected->code==app::LocalAccountResultCode::ok&&selected->records.size()==1&&selected->records.front().file.name==name,"typed owner detail shows committed file");
    task={};task.job=identifiers.generate();task.kind=app::FileManagementTaskKind::open_view;task.view_file=imported.result;const auto viewed=finish(task);
    check(viewed.state==app::FileManagementJobState::completed&&viewed.view&&!viewed.view->cells.empty(),"actual host read-only text viewport");
    task={};task.job=identifiers.generate();task.kind=app::FileManagementTaskKind::close_view;task.target=viewed.view->cursor;check(finish(task).state==app::FileManagementJobState::completed,"actual host closes held preview");
    task={};task.job=identifiers.generate();task.operation.operation=identifiers.generate();task.operation.action=app::FileOperationAction::rename;task.operation.file=imported.result;task.operation.expected_revision=selected->records.front().file.revision;task.operation.destination=area.area.id;task.operation.expected_destination_revision=area.area.revision;task.operation.filename="renamed-"+name;
    const auto moved=finish(task);check(moved.state==app::FileManagementJobState::completed&&!std::filesystem::exists(path)&&std::filesystem::exists(folder/task.operation.filename),"typed actual rename changes exact native file and catalog");
    const auto cancelled=app::decode_file_management_status(exchange(runtime,Type::file_management_cancel_request,app::encode_file_management_job(task.job),Type::file_management_cancel_response));
    check(cancelled&&cancelled->state==app::FileManagementJobState::completed&&cancelled->journal_confirmed,"late cancel cannot reverse or duplicate completed operation");
    check(runtime.bbs_owned()==online,"all typed file work preserves explicit BBS state");
}
