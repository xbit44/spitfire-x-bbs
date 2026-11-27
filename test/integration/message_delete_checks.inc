void individual_migration_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    for(const char* point:{"before-message-delete-migration","before-message-delete-migration-commit","after-message-delete-migration-commit"}){
        auto s=options(root/point);auto prior_plan=db::sfx_storage_migrations();prior_plan.resize(10);
        {db::SqliteStorage prior(s,policy,prior_plan);check(prior.start().succeeded() && prior.shutdown().succeeded(),"exact v10 delete migration predecessor");}
        seed(s.data_directory/"SFX.DB");const auto backup=s.data_directory/"prior-v10.db";
        {db::SqliteStorage prior(s,policy,prior_plan);check(prior.start().succeeded() && prior.create_backup(backup).status.succeeded() && prior.shutdown().succeeded(),"populated v10 actual recovery backup");}
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"individual migration fault"}:app::StorageStatus::success();};
        {db::SqliteStorage failed(s,policy);check(!failed.start(),"individual migration failure truthful");}
        {internal::SqliteConnection c(s.data_directory/"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            check(internal::validate_sfx_shipping_database(c.get(),true),"migration fault exact recognized lineage");
            check(c.scalar_int64("PRAGMA user_version","atomic deletion migration")== (std::string_view(point)=="after-message-delete-migration-commit"?11:10),"individual migration atomic");}
        s.bootstrap_boundary_hook={};db::SqliteStorage retry(s,policy);
        check(retry.start().succeeded() && retry.health().schema_version==sfx::test::shipping_schema_version && retry.shutdown().succeeded(),"individual migration retry once to current shipping lineage");
        check(retry.validate_backup(backup).succeeded() && retry.restore_backup(backup).succeeded() && retry.start().succeeded(),"v10 production restore and remigration");
        check(scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_accounts")==3 && scalar(s.data_directory/"SFX.DB","SELECT count(*) FROM sfx_schema_migrations")==sfx::test::shipping_migration_count,"all accounts and the complete shipping ledger retained");
        check(retry.shutdown().succeeded(),"migration recovery stopped");
    }
}
void individual_delete_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);db::SqliteStorage store(s,policy);check(store.start().succeeded() && store.shutdown().succeeded(),"individual deletion fixture bootstrapped");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"individual deletion actors ready");
    app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};unsigned op=1300;
    const app::MessageTarget public_target{app::MessageKind::public_echo,{2},{},{}};
    auto parent=request(store,alice,public_target,op++);const auto parent_id=store.commit_message(parent).value->id;
    auto reply_target=public_target;reply_target.recipient=alice.account;
    const auto reply_context=store.begin_message(bob,reply_target,parent_id);check(static_cast<bool>(reply_context),"authorized reply context");
    app::CommitMessageRequest child_request{bob,id(op++),*reply_context.value,"Child","Reply body\n"};
    auto child=store.commit_message(child_request);check(static_cast<bool>(child),"reply child fixture");
    auto mail=request(store,alice,{app::MessageKind::private_local,{},3,{}},op++);const auto mail_id=store.commit_message(mail).value->id;
    check(store.message_delete_authority(sysop).value.value() && !store.message_delete_authority(alice).value.value(),"999 delete authority independent of ownership");
    check(!store.delete_message(alice,id(op++),parent_id) && !store.delete_message(bob,id(op++),mail_id),"ordinary authors and private recipients cannot permanent-delete");
    auto forged=sysop;forged.session=alice.session;check(!store.delete_message(forged,id(op++),parent_id),"cross-account session deletion refused");
    forged=sysop;++forged.account_revision;check(!store.delete_message(forged,id(op++),parent_id),"stale actor deletion refused");
    check(!store.delete_message(sysop,{},parent_id) && !store.delete_message(sysop,id(op++),{999999}),"invalid operation and nonexistent target refused");
    bool protected_row=false;try{edit(file,"DELETE FROM sfx_local_messages");}catch(const internal::StorageFailure&){protected_row=true;}check(protected_row,"raw unscoped deletion remains blocked");
    const auto backup=root/"before-individual-delete.db";check(store.create_backup(backup).status.succeeded(),"whole deletion recovery backup");
    const auto count=scalar(file,"SELECT count(*) FROM sfx_local_messages");const auto total=scalar(file,"SELECT total_committed FROM sfx_message_counters");
    const auto operation=id(op++);
    edit(file,"CREATE TRIGGER individual_delete_fault BEFORE INSERT ON sfx_message_delete_receipts BEGIN SELECT RAISE(ABORT,'late individual delete failure'); END");
    check(!store.delete_message(sysop,operation,parent_id) && scalar(file,"SELECT count(*) FROM sfx_local_messages")==count && scalar(file,"SELECT count(*) FROM sfx_message_delete_scope")==0 && scalar(file,"SELECT count(*) FROM sfx_message_purged_receipts")==0,"late deletion fault rolls back content, tombstone and scope");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE parent_message_id IS NOT NULL")==1,"rollback retains child parent link");
    edit(file,"DROP TRIGGER individual_delete_fault");
    auto removed=store.delete_message(sysop,operation,parent_id);check(removed && !*removed.value,"confirmed public deletion commits once");
    check(store.delete_message(sysop,operation,parent_id).value.value() && !store.delete_message(sysop,operation,mail_id),"same operation reconciles; redirected target refused");
    check(!store.open_message(alice,parent_id) && !store.open_message(sysop,parent_id) && !store.reconcile_message(parent) && !store.commit_message(parent),"deleted parent cannot read or resurrect on old commit retry");
    check(store.quote_source(bob,child.value->id).succeeded() && scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE parent_message_id IS NOT NULL")==0 && scalar(file,"SELECT total_committed FROM sfx_message_counters")==total,"only parent detached; historical counters unchanged");
    check(store.delete_message(sysop,id(op++),mail_id).succeeded() && !store.quote_source(alice,mail_id) && !store.quote_source(bob,mail_id),"Sysop deletes unrelated private message and both copies");
    const auto derived="SELECT (SELECT count(*) FROM sfx_message_arrival_intents WHERE message_id="+std::to_string(mail_id.value)+")+(SELECT count(*) FROM sfx_message_recipient_state WHERE message_id="+std::to_string(mail_id.value)+")";
    check(scalar(file,derived.c_str())==0 && scalar(file,"SELECT count(*) FROM sfx_message_delete_receipts")==2,"private target derived rows removed; unrelated reply rows retained; two content-free receipts");
    edit(file,"UPDATE sfx_security_policy SET sysop_threshold=10,revision=revision+1,published_revision=revision+1");alice.policy_revision=2;
    check(store.message_delete_authority(alice).value.value() && store.delete_message(alice,id(op++),child.value->id).succeeded(),"configured lower-level Sysop qualifies without level999");
    edit(file,"UPDATE sfx_security_policy SET sysop_threshold=999,revision=revision+1,published_revision=revision+1");alice.policy_revision=3;
    check(!store.message_delete_authority(alice).value.value() && !store.delete_message(alice,id(op++),child.value->id),"privilege loss rechecked before replay or deletion");
    check(store.shutdown().succeeded() && store.restore_backup(backup).succeeded() && store.start().succeeded(),"restore pre-delete production backup");
    alice.policy_revision=1;
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==count && scalar(file,"SELECT count(*) FROM sfx_message_delete_receipts")==0 && store.reconcile_message(parent).value->id==parent_id,"recovery restores all messages and original receipts");
    const auto run=[&](app::MessageActor who,std::string_view keys,std::optional<std::int64_t> number,bool inbox=false){
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(keys);app::StreamCallerTerminal terminal(stream);
        app::MessageQueryOptions query;query.mode=inbox?app::MessageQueryMode::inbox:app::MessageQueryMode::echo;if(!inbox)query.echo={2};
        app::MessageReaderPorts p{store,terminal,[who]{return std::optional<app::MessageActor>{who};},{},[](std::int64_t){return "test date";},[]{return true;},24,false,{},[&]{return id(op++);}};
        const auto result=app::read_local_messages(p,query,false,number);return std::pair{result,stream.output()};
    };
    const auto ordinary_permanent=run(bob,"PQ",1);
    check(ordinary_permanent.first==app::MessageReaderOutcome::cancelled&&ordinary_permanent.second.find("[P]")==std::string::npos&&ordinary_permanent.second.find("Mark this message")==std::string::npos&&!store.message_permanent_state(sysop,parent_id).value.value(),"ordinary caller neither sees nor activates an unlisted Permanent key");
    check(!store.set_message_permanent(bob,id(op++),parent_id,false,true),"storage independently rejects ordinary Permanent use");
    const auto marked=run(sysop,"P\r\nY\r\nQ\r\n",1);
    check(marked.first==app::MessageReaderOutcome::cancelled&&marked.second.find("[P] Protect [Q] Quit:")!=std::string::npos&&marked.second.find("Message marked Permanent.")!=std::string::npos&&store.message_permanent_state(sysop,parent_id).value.value(),"authorized Sysop marking retains label placement and current-state command");
    const auto unmarked=run(sysop,"P\r\nY\r\nQ\r\n",1);
    check(unmarked.first==app::MessageReaderOutcome::cancelled&&unmarked.second.find("Remove Permanent status")!=std::string::npos&&!store.message_permanent_state(sysop,parent_id).value.value(),"authorized Sysop removes Permanent through unchanged confirmation");
    auto cancelled=run(sysop,"D\r\n\r\nQ\r\n",1);
    check(cancelled.first==app::MessageReaderOutcome::cancelled && cancelled.second.find("for everyone [y/N]")!=std::string::npos && store.open_message(sysop,parent_id).succeeded(),"reader permanent default No preserves message");
    auto help=run(bob,"?Q",1);
    check(help.first==app::MessageReaderOutcome::cancelled && help.second.find("[?] Help")==std::string::npos && help.second.find("MESSAGE READER HELP")==std::string::npos && help.second.find("[D] Delete")==std::string::npos,"native reader has no Help and no ordinary public-delete command");
    auto inbox=run(bob,"D\r\n\r\n",{},true);
    check(inbox.first==app::MessageReaderOutcome::completed && store.quote_source(alice,mail_id).value->header.inbox_removed,"ordinary reader removal still preserves sender copy");
    auto accepted=run(sysop,"D\r\nY\r\nQ\r\n",1);
    check(accepted.first==app::MessageReaderOutcome::cancelled && accepted.second.find("Message permanently deleted.")!=std::string::npos && !store.quote_source(sysop,parent_id),"reader explicit Yes dispatches permanent deletion then retains reader context");
    edit(file,"UPDATE sfx_accounts SET security_level=999,revision=revision+1 WHERE account_id=3");bob.account_revision=2;
    check(store.message_delete_authority(bob).value.value() && scalar(file,"SELECT account_id FROM sfx_system_sysop")==1,"non-designated level999 independently authorized");
    auto extra=store.commit_message(request(store,alice,public_target,op++));
    check(extra && store.delete_message(bob,id(op++),extra.value->id).succeeded(),"999 deletes another author's public post without System Sysop designation");
    auto loss=store.commit_message(request(store,alice,{app::MessageKind::private_local,{},3,{}},op++));check(static_cast<bool>(loss),"privilege loss private fixture");
    sfx::test::DeterministicInteractiveByteStreamFake loss_stream;loss_stream.script_non_secret_text("D\r\nY\r\nQ\r\n");app::StreamCallerTerminal loss_terminal(loss_stream);bool revoked=false;
    app::MessageReaderPorts loss_ports{store,loss_terminal,[&]{
        if(!revoked && loss_stream.output().find("for everyone [y/N]")!=std::string::npos){edit(file,"UPDATE sfx_accounts SET security_level=10,revision=revision+1 WHERE account_id=3");bob.account_revision=3;revoked=true;}
        return std::optional<app::MessageActor>{bob};},{},{},[]{return true;},24,false,{},[&]{return id(op++);}};
    app::MessageQueryOptions loss_query;loss_query.mode=app::MessageQueryMode::inbox;
    check(app::read_local_messages(loss_ports,loss_query)==app::MessageReaderOutcome::failed && revoked,"privilege loss during permanent confirmation fails closed");
    check(store.quote_source(alice,loss.value->id).succeeded() && !store.quote_source(alice,loss.value->id).value->header.inbox_removed && loss_stream.output().find("Sender copy retained")==std::string::npos,"permission loss never falls back to ordinary inbox removal");
    check(store.shutdown().succeeded(),"individual deletion fixture stopped");
}
void individual_write_fault_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    check(sfx::test::install_sqlite_write_fault_vfs("section8a-delete-io"),"scoped deletion WAL VFS installed");
    try{
        auto s=options(root/"section8a-delete-io");db::SqliteStorage store(s,policy);check(store.start().succeeded() && store.shutdown().succeeded(),"deletion WAL fixture bootstrapped");
        const auto file=s.data_directory/"SFX.DB";seed(file);check(store.start().succeeded(),"deletion WAL actors ready");
        const app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1};unsigned op=1450;
        for(const int code:{SQLITE_FULL,SQLITE_IOERR_WRITE}){
            auto draft=request(store,alice,{app::MessageKind::private_local,{},3,{}},op++);const auto message=store.commit_message(draft).value->id;const auto operation=id(op++);
            const auto before=scalar(file,"SELECT count(*) FROM sfx_message_delete_receipts");
            sfx::test::inject_next_sqlite_write_fault(code);const auto failed=store.delete_message(sysop,operation,message);
            check(!failed && failed.status.code==(code==SQLITE_FULL?app::StorageErrorCode::full:app::StorageErrorCode::io) && sfx::test::pending_sqlite_write_fault()==SQLITE_OK,"deletion physical WAL failure is truthful");
            check(store.quote_source(alice,message).succeeded() && scalar(file,"SELECT count(*) FROM sfx_message_delete_receipts")==before && scalar(file,"SELECT count(*) FROM sfx_message_delete_scope")==0,"failed WAL deletion publishes no audit/content removal");
            check(store.delete_message(sysop,operation,message).succeeded() && store.delete_message(sysop,operation,message).value.value() && !store.commit_message(draft),"same operation WAL retry deletes once and blocks resurrection");
        }
        check(store.shutdown().succeeded(),"deletion WAL fixture stopped");
    }catch(...){sfx::test::uninstall_sqlite_write_fault_vfs();throw;}
    sfx::test::uninstall_sqlite_write_fault_vfs();
}
