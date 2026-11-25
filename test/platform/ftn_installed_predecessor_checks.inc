// QA31 derived predecessor-store fixture. Original historical DB stays exact.
std::filesystem::path predecessor_archive(){
    const auto owner=win32::current_process_user_sid_string();win32::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner).succeeded(),"exclusive predecessor preflight and archive creation");
    const std::filesystem::path source=(sfx::test::fixture_checkout() / L"research/architecture/evidence/section14-windows-stage2-20261029/predecessor-intake-cr5-r1/SFX.DB").make_preferred();
    const std::string expected="7F226A48A9E8D03FF6B8F6C647779043F99EEB2284D1F0FC310678DC0F8E4DA4";
    const auto source_pin=win32::installation::open_object(source,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ);
    check(win32::installation::file_sha256(source)==expected,"exact immutable historical schema20 member");
    const auto branch=root/L"Work"/L"FTN-Predecessor";check(!std::filesystem::exists(branch),"new predecessor fixture only");
    auto security=win32::installation::mutable_descriptor(owner);
    win32::installation::create_directory(branch,security.get());
    win32::installation::create_directory(branch/L"Original",security.get());
    win32::installation::create_directory(branch/L"Preflight",security.get());
    win32::installation::create_directory(branch/L"Preflight"/L"Data",security.get());
    const auto original=branch/L"Original"/L"SFX.DB",trial=branch/L"Preflight"/L"Data"/L"SFX.DB";
    const auto bytes=win32::installation::read_file(source,4U*1024U*1024U);
    win32::installation::create_file(original,bytes,security.get());win32::installation::create_file(trial,bytes,security.get());
    win32::WindowsLocalStoragePathPolicy paths(owner);db::SqliteStorageSettings settings;settings.data_directory=trial.parent_path();
    db::SqliteStorage preflight(settings,paths);
    const auto verified=preflight.verified_ftn_serial_history(original);
    check(verified&&verified.value&&verified.value->predecessor&&!verified.value->lineage.valid(),"normal schema validator independently classifies exact genuine predecessor");
    const auto configuration=win32::read_operational_configuration_file(root);
    check(configuration.configuration.has_value(),"normal current saved configuration reads");
    check(preflight.start().succeeded(),"normal migration on separate protected preflight copy only");
    const auto policy=preflight.security_policy();check(policy&&policy.value&&policy.value->revision==configuration.configuration->security_policy_revision&&policy.value->policy_hash==configuration.configuration->security_policy_hash&&policy.value->published_revision==policy.value->revision&&policy.value->published_policy_hash==policy.value->policy_hash,"derived snapshot agrees with current saved policy; no rewriting or reconciliation bypass");
    const auto files=preflight.file_configuration({true,{}});check(files&&files.value&&files.value->areas.size()==3,"normal predecessor file catalog reads");
    for(const auto& area:files.value->areas)check(area.storage_path=="Files/General"||area.storage_path=="Files/Uploads"||area.storage_path=="Files/UploadToSysop","historical area paths stay within r4 managed defaults");
    const auto messages=preflight.message_configuration({true,{}},{},{});check(messages&&messages.value&&messages.value->system_sysop.has_value(),"normal predecessor message and System Sysop references");
    const auto sysop=preflight.find_account_by_id(*messages.value->system_sysop);check(sysop&&sysop.value&&sysop.value->state==app::AccountState::active&&sysop.value->security_level&&*sysop.value->security_level>=policy.value->sysop_threshold,"historical Sysop resolves through its own restored identity, never r4 numeric impersonation");
    check(preflight.shutdown().succeeded(),"preflight copy closes; original remains unmigrated");
    check(win32::installation::file_sha256(source)==expected&&win32::installation::file_sha256(original)==expected,"original input and archive snapshot bytes unchanged");
    const auto receipt=win32::installation::read_receipt(root,owner);win32::installation::verify_ftn_directory(receipt);
    arc::ManagedBackupRequest request;request.installation_root=root;request.destination_directory=root/L"Backup"/L"Scheduled";
    request.kind=arc::BackupKind::manual;request.ftn_layout=receipt.ftn_layout;request.source_generation=std::to_string(receipt.generation);
    const auto transaction=win32::installation::unique_id();for(const auto value:transaction){check((value>=L'0'&&value<=L'9')||(value>=L'A'&&value<=L'F'),"predecessor archive transaction ID");request.transaction_id.push_back(static_cast<char>(value));}
    Sleep(1100);request.created=arc::current_timestamp();request.database_snapshot=original;
    const auto saved=arc::create_managed_backup(request);check(saved.result.ok()&&arc::inspect_backup(saved.archive_path).result.ok(),"derived predecessor/current-Config archive passes normal full verification");
    std::cout<<"DERIVED predecessor="<<expected<<" config="<<win32::installation::file_sha256(root/L"Config"/L"SFX.conf")<<" archive="<<saved.archive_path.u8string()<<" sha256="<<win32::installation::file_sha256(saved.archive_path)<<'\n';
    return saved.archive_path;
}

void held_originators(){
    const auto owner=win32::current_process_user_sid_string();win32::OfflineInstallationGuard guard;
    check(guard.acquire(root,owner).succeeded(),"exclusive held-originator qualification");
    win32::WindowsLocalStoragePathPolicy paths(owner);db::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
    db::SqliteStorage storage(settings,paths);check(storage.start().succeeded(),"actual held restored store opens");
    const auto account=storage.find_account_by_name("SSH Fixture");const auto policy=storage.security_policy();
    check(account&&account.value&&policy&&policy.value,"restored established-history own account and policy");
    const auto before=storage.ftn_serial_state({true,{}});check(before&&before.value&&before.value->recovery_required,"actual restored history is held");
    app::StartSessionRequest start;start.session_id=identifier();start.correlation_id=identifier();start.account_id=account.value->account_id;start.node_id=1;start.started_utc_seconds=now();start.journal_transport="Telnet";start.journal_source_ip="127.0.0.1";start.journal_ansi=0;
    check(storage.record_session_started(start).succeeded(),"controlled held-row session through supported host API");
    app::MessageActor actor{start.session_id,account.value->account_id,account.value->revision,policy.value->revision};
    app::MessageTarget remote;remote.kind=app::MessageKind::private_ftn;remote.remote=app::MessageRemoteTarget{"own-network","42:100/1@own-network","42:100/2@own-network","Synthetic Remote"};
    const auto context=storage.begin_message(actor,remote,{});check(context&&context.value,"otherwise valid connected FTN draft remains available");
    const app::CommitMessageRequest request{actor,identifier(),*context.value,"Held FTN post","Must remain unpublished.\n"};
    const auto refused=storage.commit_message(request);check(!refused&&!refused.value,"history hold refuses new FTN publication");
    const auto known=storage.reconcile_message(request);check(!known&&!known.value&&known.status.code==app::StorageErrorCode::not_found,"refused held post has no committed receipt");
    const auto after=storage.ftn_serial_state({true,{}});check(after&&after.value&&after.value->high_water==before.value->high_water&&after.value->lineage==before.value->lineage&&after.value->recovery_required,"refusal preserves exact high-water and monotonic hold");
    app::MessageTarget local;local.kind=app::MessageKind::private_local;local.recipient=account.value->account_id;
    const auto local_context=storage.begin_message(actor,local,{});check(local_context&&local_context.value,"unrelated local mail remains available");
    check(static_cast<bool>(storage.commit_message({actor,identifier(),*local_context.value,"Local while FTN held","Local BBS still works.\n"})),"unrelated local mail commits while FTN originator is held");
    app::FinalizeSessionRequest finish;finish.session_id=start.session_id;finish.completed_utc_seconds=now();finish.audit_event_id=identifier();check(storage.finalize_session(finish).succeeded(),"controlled hold-row session finalized");
    check(storage.shutdown().succeeded(),"held store closes without self-clear");
}
