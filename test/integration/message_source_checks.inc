void source_migration_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    for(const char* point:{"before-message-source-migration","before-message-source-migration-commit","after-message-source-migration-commit"}){
        auto s=options(root/point);auto plan=db::sfx_storage_migrations();plan.resize(11);
        {db::SqliteStorage prior(s,policy,plan);check(prior.start().succeeded() && prior.shutdown().succeeded(),"exact v11 source predecessor");}
        const auto file=s.data_directory/"SFX.DB";seed(file);const auto backup=s.data_directory/"before-sources.db";
        app::MessageActor alice{id(2),2,1,1};
        {db::SqliteStorage prior(s,policy,plan);check(prior.start().succeeded(),"populated v11 starts");
            auto legacy=request(prior,alice,{app::MessageKind::public_echo,{2},{},{}},2000);
            check(bool(prior.commit_message(legacy)),"v11 legacy body committed before upgrade");
            check(prior.create_backup(backup).status.succeeded() && prior.shutdown().succeeded(),"real v11 source recovery backup");}
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"source migration injected fault"}:app::StorageStatus::success();};
        {db::SqliteStorage failed(s,policy);check(!failed.start(),"source migration failure truthful");}
        {internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            check(internal::validate_sfx_shipping_database(c.get(),true),"source failure retains exact known manifest");
            check(c.scalar_int64("PRAGMA user_version","source atomicity")== (std::string_view(point)=="after-message-source-migration-commit"?12:11),"source migration atomic");}
        s.bootstrap_boundary_hook={};db::SqliteStorage current(s,policy);
        check(current.start().succeeded() && current.health().schema_version==sfx::test::shipping_schema_version,"source upgrade retry once to current shipping lineage");
        check(scalar(file,"SELECT count(*) FROM sfx_message_sources")==0 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==1,"legacy message retained without fake source backfill");
        check(current.shutdown().succeeded() && current.validate_backup(backup).succeeded() && current.restore_backup(backup).succeeded() && current.start().succeeded(),"v11 source backup restore remigrates");
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==1 && scalar(file,"SELECT count(*) FROM sfx_message_commit_receipts")==1 && scalar(file,"SELECT count(*) FROM sfx_schema_migrations")==sfx::test::shipping_migration_count,"legacy message/receipt and all shipping migrations retained");
        check(current.shutdown().succeeded(),"source migration fixture stopped");
    }
}
void source_storage_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);db::SqliteStorage store(s,policy);check(store.start().succeeded() && store.shutdown().succeeded(),"source store bootstrapped");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"source actors ready");
    app::MessageActor alice{id(2),2,1,1},sysop{id(1),1,1,1},bob{id(3),3,1,1};
    app::MessageTarget target{app::MessageKind::public_echo,{2},3,{}};
    auto request_context=store.begin_message(alice,target);check(bool(request_context) && !request_context.value->pipe_colors,"source policy Off by default");
    app::CommitMessageRequest r{alice,id(2100),*request_context.value,"Owned artwork",{},app::MessageSource{app::MessageSourceFormat::ansi_canvas,1,"\x1b[2J\x1b[2;3H|12X\x1b[5;34mY"}};
    const auto posted=store.commit_message(r);if(!posted)std::cerr<<posted.status.operation<<'\n';check(bool(posted),"atomic ANSI source commit");
    check(store.commit_message(r).value->id==posted.value->id && store.reconcile_message(r).value->id==posted.value->id,"source exact retry once");
    check(scalar(file,"SELECT count(*) FROM sfx_message_sources")==1 && scalar(file,"SELECT count(*) FROM sfx_message_source_write_scope")==0,"one original and no residual insert scope");
    check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==1 && scalar(file,"SELECT count(*) FROM sfx_message_arrival_intents")==1,"source counts/arrival once");
    auto changed=r;changed.original_source->bytes+='Z';check(!store.commit_message(changed) && !store.reconcile_message(changed),"same operation changed source conflict");
    changed=r;changed.body="not a source body";check(!store.commit_message(changed),"cannot smuggle source through plain body");
    changed=r;changed.operation=id(2101);changed.original_source->bytes="\x1b[201;1HX";check(!store.commit_message(changed),"storage independently enforces caller row allowance");
    changed=r;changed.operation=id(2102);changed.original_source->bytes="\x1b]0;untrusted\a";check(!store.commit_message(changed),"storage refuses forbidden original controls");
    auto opened=store.quote_source(bob,posted.value->id);check(opened && opened.value->original_source->bytes==r.original_source->bytes && opened.value->body.rows[1]=="  |12XY","exact source and separate Off visible text");
    check(!opened.value->header.recipient_opened,"source quote does not mark recipient read");
    bool update_refused=false;try{edit(file,"UPDATE sfx_message_sources SET source=x'58'");}catch(const internal::StorageFailure&){update_refused=true;}check(update_refused,"immutable original SQL update refused");
    bool delete_refused=false;try{edit(file,"DELETE FROM sfx_message_sources");}catch(const internal::StorageFailure&){delete_refused=true;}check(delete_refused,"unscoped source delete refused");
    auto snapshot=store.message_configuration({true,{}},app::MessageGroupId{1},{});check(snapshot && snapshot.value->private_presentation_revision==1 && !snapshot.value->private_pipe_colors,"singleton private policy Off/revision one");
    const auto echo=std::find_if(snapshot.value->echoes.begin(),snapshot.value->echoes.end(),[](const auto& e){return e.id.value==2;});
    app::MessageConfigurationChange policy_change;policy_change.operation=id(2110);policy_change.action=app::MessageConfigurationAction::echo;policy_change.expected_configuration_revision=snapshot.value->revision;policy_change.echo=*echo;policy_change.presentation_mask=1;policy_change.echo.renegade_pipe_colors=true;
    const auto query=store.begin_message_query(bob,{app::MessageQueryMode::echo,app::MessageQueryScope::current_echo,{2}});check(bool(query),"source query begins before policy change");
    const auto saved=store.change_message_configuration({true,{}},policy_change);check(bool(saved),"echo Save includes pipe policy atomically");
    check(store.change_message_configuration({true,{}},policy_change).value->replayed,"pipe configuration exact retry");
    auto conflicting=policy_change;conflicting.echo.renegade_pipe_colors=false;check(!store.change_message_configuration({true,{}},conflicting),"pipe value in configuration v2 fingerprint");
    changed=r;changed.operation=id(2111);check(!store.commit_message(changed),"stale source approval refuses policy change");
    check(!store.open_query_message(bob,*query.value,posted.value->id,false,false),"query snapshot invalidated on policy change");
    opened=store.quote_source(bob,posted.value->id);check(opened && opened.value->pipe_colors && opened.value->body.rows[1]=="  XY" && opened.value->original_source->bytes==r.original_source->bytes,"current On derived text, raw source unchanged");
    auto legacy=request(store,alice,target,2112);legacy.subject="Legacy literal source";legacy.body="|12Legacy literal\n";
    const auto legacy_post=store.commit_message(legacy);check(bool(legacy_post),"ordinary legacy body stored without fabricated upload source");
    auto legacy_open=store.quote_source(bob,legacy_post.value->id);
    check(legacy_open && !legacy_open.value->original_source && legacy_open.value->plain_rendition_source && legacy_open.value->plain_rendition_source->canonical_lf=="|12Legacy literal\n" && legacy_open.value->body.canonical_lf=="Legacy literal\n","legacy raw body retained separately from current On derived text");
    const auto legacy_find=store.begin_message_query(bob,{app::MessageQueryMode::echo,app::MessageQueryScope::current_echo,{2}, {}, {}, "|12Legacy"});
    check(bool(legacy_find),"legacy current-derived search context");
    const auto legacy_headers=store.query_message_headers(bob,*legacy_find.value,{},20);
    check(legacy_headers && legacy_headers.value->entries.empty(),"search does not match legacy recognized codes removed by current On policy");
    const auto private_context=store.begin_message(alice,{app::MessageKind::private_local,{},3,{}});check(bool(private_context),"private source context");
    app::CommitMessageRequest private_request{alice,id(2120),*private_context.value,"Private art",{},app::MessageSource{app::MessageSourceFormat::plain,1,"|12Private\r\n"}};
    const auto private_post=store.commit_message(private_request);check(bool(private_post) && !store.quote_source(sysop,private_post.value->id).value->pipe_colors,"private policy independent of echo");
    app::MessageConfigurationChange priv;priv.operation=id(2121);priv.action=app::MessageConfigurationAction::private_presentation;priv.expected_configuration_revision=saved.value->configuration_revision;priv.expected_private_presentation_revision=1;priv.private_pipe_colors=true;
    check(!store.change_message_configuration({false,alice},priv),"ordinary caller cannot change presentation");
    check(bool(store.change_message_configuration({true,{}},priv)),"private policy CAS audited");
    opened=store.quote_source(bob,private_post.value->id);check(opened && opened.value->body.canonical_lf=="Private\n","private On strips recognized pipe codes");
    auto bg=store.begin_message(alice,target);app::CommitSourceMessageRequest background{alice,id(2130),*bg.value,"Background-only artwork",{app::MessageSourceFormat::ansi_canvas,1,"\x1b[44m\x1b[2J"}};
    const auto background_post=store.commit_source_message(background);check(bool(background_post),"visible colored-space artwork accepted");
    check(!store.delete_message(alice,id(2131),posted.value->id),"source does not expand delete privileges");
    const auto backup=root/"source-roundtrip.db";check(store.create_backup(backup).status.succeeded() && store.shutdown().succeeded(),"source backup includes original/derived/policy");
    check(store.validate_backup(backup).succeeded() && store.restore_backup(backup).succeeded() && store.start().succeeded(),"source backup production restore");
    check(store.quote_source(bob,posted.value->id).value->original_source->bytes==r.original_source->bytes,"source bytes survive verified restore");
    check(bool(store.delete_message(sysop,id(2140),posted.value->id)) && !store.reconcile_message(r),"privileged source deletion tombstones commit, no resurrection");
    check(scalar(file,"SELECT count(*) FROM sfx_message_sources")==2 && scalar(file,"SELECT count(*) FROM sfx_message_source_write_scope")==0,"only selected source deleted, scopes empty");
    check(store.shutdown().succeeded(),"source fixture stopped");
}
void source_fault_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    check(sfx::test::install_sqlite_write_fault_vfs("section8b-source-io"),"source scoped real WAL fault VFS");
    try{
        auto settings=options(root/"section8b-source-io");db::SqliteStorage store(settings,policy);
        check(store.start().succeeded() && store.shutdown().succeeded(),"source fault fixture bootstrap");
        const auto file=settings.data_directory/"SFX.DB";seed(file);check(store.start().succeeded(),"source fault actors ready");
        const app::MessageActor alice{id(2),2,1,1};unsigned operation=2200;
        for(const auto code:{SQLITE_FULL,SQLITE_IOERR_WRITE}){
            auto r=request(store,alice,{app::MessageKind::private_local,{},3,{}},operation++);
            r.body.clear();r.original_source=app::MessageSource{app::MessageSourceFormat::ansi_canvas,1,"\x1b[31mOwned source\r\n"};
            const auto before=scalar(file,"SELECT total_committed FROM sfx_message_counters");
            sfx::test::inject_next_sqlite_write_fault(code);const auto failed=store.commit_message(r);
            check(!failed && sfx::test::pending_sqlite_write_fault()==SQLITE_OK,"source physical FULL/I/O returns no success");
            check(scalar(file,"SELECT count(*) FROM sfx_message_sources")==before && scalar(file,"SELECT count(*) FROM sfx_message_source_write_scope")==0 && scalar(file,"SELECT count(*) FROM sfx_message_commit_receipts")==before,"source/receipt/write scope atomic rollback");
            check(!store.message_commit_outcome(r).value->committed,"source failed physical write reconciles absence");
            const auto retry=store.commit_message(r);check(retry && store.commit_message(r).value->id==retry.value->id,"source fault retry exactly once");
        }
        auto r=request(store,alice,{app::MessageKind::private_local,{},3,{}},operation++);r.body.clear();r.original_source=app::MessageSource{app::MessageSourceFormat::plain,1,"Concurrent source"};
        std::vector<std::future<app::StorageResult<app::CommittedMessage>>> pending;
        for(unsigned i=0;i<16;++i)pending.push_back(std::async(std::launch::async,[&]{return store.commit_message(r);}));
        std::optional<app::MessageId> one;
        for(auto& future:pending){const auto result=future.get();check(result && (!one || result.value->id==*one),"source simultaneous retry one immutable identity");one=result.value->id;}
        check(scalar(file,"SELECT count(*) FROM sfx_message_sources")==3 && scalar(file,"SELECT count(*) FROM sfx_message_source_write_scope")==0,"source concurrent counts once/no residual scope");
        check(store.shutdown().succeeded() && store.start().succeeded() && store.reconcile_message(r).value->id==*one,"source lost-result reconciliation after durable restart");
        check(store.shutdown().succeeded(),"source fault fixture stopped");
    }catch(...){sfx::test::inject_next_sqlite_write_fault(SQLITE_OK);sfx::test::uninstall_sqlite_write_fault_vfs();throw;}
    sfx::test::uninstall_sqlite_write_fault_vfs();
}
void source_authority_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);db::SqliteStorage store(s,policy);
    check(store.start().succeeded() && store.shutdown().succeeded(),"source authority fixture bootstrap");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"source authority actors ready");
    app::MessageActor alice{id(2),2,1,1};
    const auto make=[&](app::MessageTarget target,unsigned op){auto r=request(store,alice,target,op);r.body.clear();r.original_source=app::MessageSource{app::MessageSourceFormat::plain,1,"Owned artwork source"};return r;};
    auto public_request=make({app::MessageKind::public_echo,{2},{},{}},2400);
    edit(file,"UPDATE sfx_local_echoes SET posting_minimum=999,revision=revision+1 WHERE echo_id=2");
    check(!store.commit_message(public_request) && !store.begin_message(alice,public_request.context.target),"posting permission revoked after source context denies atomic Save");
    edit(file,"UPDATE sfx_local_echoes SET posting_minimum=1,revision=revision+1 WHERE echo_id=2");
    check(!store.commit_message(public_request),"restored posting permission does not silently renew old source context");
    auto recipient=make({app::MessageKind::private_local,{},3,{}},2401);
    edit(file,"UPDATE sfx_accounts SET account_state='locked',revision=revision+1 WHERE account_id=3");
    check(!store.commit_message(recipient) && !store.begin_message(alice,recipient.context.target),"locked private recipient refuses source Save and new receive eligibility");
    edit(file,"UPDATE sfx_accounts SET account_state='active',revision=revision+1 WHERE account_id=3");
    check(!store.commit_message(recipient),"recipient reactivation still invalidates old source context");
    auto actor_request=make({app::MessageKind::public_echo,{2},{},{}},2402);
    edit(file,"UPDATE sfx_accounts SET revision=revision+1 WHERE account_id=2");
    check(!store.commit_message(actor_request) && !store.begin_message(alice,actor_request.context.target),"stale actor identity cannot start receive or publish source");
    check(scalar(file,"SELECT count(*) FROM sfx_message_sources")==0 && scalar(file,"SELECT count(*) FROM sfx_message_commit_receipts")==0 && scalar(file,"SELECT total_committed FROM sfx_message_counters")==0 && scalar(file,"SELECT count(*) FROM sfx_message_source_write_scope")==0,"all authority refusals leave no source/receipt/counter/scope writes");
    check(store.shutdown().succeeded(),"source authority fixture stopped");
}
