// Disposable, genuine storage execution; clocks and corruption are test-owned.
void retention_policy(db::SqliteStorage& store,app::LocalEchoId echo,app::MessageRetentionPolicy limits,unsigned token){
    const auto before=store.message_configuration({true,{}},app::MessageGroupId{1},{});
    check(bool(before) && before.value.has_value(),"execution policy snapshot");
    app::MessageConfigurationChange c;c.action=app::MessageConfigurationAction::echo;c.operation=id(token);
    c.expected_configuration_revision=before.value->revision;c.retention_mask=3;
    for(const auto& e:before.value->echoes)if(e.id==echo)c.echo=e;
    check(c.echo.id==echo,"execution policy stable target");c.echo.retention=limits;
    check(bool(store.change_message_configuration({true,{}},c)),"execution policy saves without purge");
}
app::MessageMaintenanceRun run_retention(db::SqliteStorage& store,unsigned token){
    const auto begun=store.begin_message_retention({true,{}},id(token));check(bool(begun),"authorized maintenance begins");
    app::MessageRetentionBatch last;std::int64_t sequence=1;
    for(;sequence<500;++sequence){
        const auto batch=store.message_retention_batch({true,{}},id(token),sequence,{});
        if(!batch)std::cerr<<"retention run="<<token<<" batch="<<sequence<<" status="<<static_cast<unsigned>(batch.status.code)<<" operation="<<batch.status.operation<<'\n';
        check(bool(batch) && batch.value.has_value(),"bounded retention batch commits");last=*batch.value;
        check(last.removed_public<=32 && last.expired_unread<=32,"actual bounded work per batch");
        const auto replay=store.message_retention_batch({true,{}},id(token),sequence,{});
        check(bool(replay) && replay.value->replayed && replay.value->run.removed_public==last.run.removed_public && replay.value->run.expired_unread==last.run.expired_unread,"last committed batch retry reconciles, never double removes");
        if(last.complete)break;
    }
    check(sequence<500,"bounded fixture retention completes");
    const auto reason=last.policy_changed?app::MessageMaintenanceReason::policy_changed:last.run.reason;
    const auto outcome=reason==app::MessageMaintenanceReason::none?app::MessageMaintenanceOutcome::success:
        last.run.removed_public || last.run.expired_unread?app::MessageMaintenanceOutcome::partial:app::MessageMaintenanceOutcome::skipped;
    const auto finished=store.finish_message_retention({true,{}},id(token),outcome,reason);
    check(bool(finished) && finished.value.has_value(),"honest retention completion");return *finished.value;
}
void retention_count_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);db::SqliteStorage store(s,policy);const auto file=s.data_directory/"SFX.DB";
    check(store.start().succeeded() && store.shutdown().succeeded(),"count fixture bootstrap");seed(file);check(store.start().succeeded(),"count fixture start");
    app::MessageActor alice{id(2),2,1,1};std::vector<app::CommitMessageRequest> requests;
    for(unsigned n=0;n<1009;++n){auto r=request(store,alice,{app::MessageKind::public_echo,{2},{},{}},20000+n);check(bool(store.commit_message(r)),"cap does not refuse posts");requests.push_back(std::move(r));}
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==1009,"default cap is retained count, not posting refusal");
    const auto done=run_retention(store,21500);check(done.removed_public==9 && done.expired_unread==0,"1009 ordinary at cap1000 removes exactly oldest nine");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==1000 && scalar(file,"SELECT min(area_number) FROM sfx_local_messages")==10 && scalar(file,"SELECT max(area_number) FROM sfx_local_messages")==1009,"survivors retain numbers");
    check(scalar(file,"SELECT count(*) FROM sfx_message_purged_receipts")==9 && scalar(file,"SELECT count(*) FROM sfx_message_delete_scope")==0,"purged reconciliation receipts, no lingering scopes");
    check(!store.commit_message(requests.front()),"old purged operation never reposts");
    check(run_retention(store,21501).removed_public==0,"next run does not remove twice");
    const auto next=store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},21502));check(bool(next) && next.value->area_number==1010 && next.value->id.value>1009,"new post never reuses stable IDs/numbers");
    const app::MessageActor sysop{id(1),1,1,1};
    for(unsigned n=10;n<15;++n)check(bool(store.set_message_permanent(sysop,id(21600+n),{n},false,true)),"explicit five Permanent at default1000 cap");
    for(unsigned n=0;n<99;++n)check(bool(store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},21700+n))),"new ordinary messages above default cap remain permitted");
    check(run_retention(store,21900).removed_public==100 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==1000,"five Permanent leave room for newest995 ordinary");
    check(scalar(file,"SELECT min(area_number) FROM sfx_local_messages WHERE message_id NOT IN(SELECT message_id FROM sfx_message_permanent WHERE permanent=1)")==115 && scalar(file,"SELECT count(*) FROM sfx_message_permanent WHERE permanent=1")==5,"default protected cap keeps exact oldest Permanent and newest995 ordinary identities");
    check(store.shutdown().succeeded(),"count fixture stopped");
}
void retention_protection_and_age_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);std::int64_t now=1791278000;s.utc_now=[&]{return now;};db::SqliteStorage store(s,policy);const auto file=s.data_directory/"SFX.DB";
    check(store.start().succeeded() && store.shutdown().succeeded(),"protected/age fixture bootstrap");seed(file);check(store.start().succeeded(),"protected/age fixture start");
    app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};unsigned operation=22000;
    now-=86401;const auto parent=store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},operation++));check(bool(parent),"old parent commits");
    ++now;const auto boundary=store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},operation++));check(bool(boundary),"exact full day commits");
    ++now;const auto reply_context=store.begin_message(alice,{app::MessageKind::public_echo,{2},alice.account,{}},parent.value->id);check(bool(reply_context),"stable reply context");
    const auto reply=store.commit_message({alice,id(operation++),*reply_context.value,"Reply","Body\n"});check(bool(reply),"reply younger than age threshold commits");
    now=1791278000;retention_policy(store,{2},{{},1},operation++);
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==3,"Save does not expire old messages");
    const auto age=run_retention(store,operation++);check(age.removed_public==2,"full-day before/at/after boundaries independent of calendar date");
    const auto surviving=store.quote_source(alice,reply.value->id);check(bool(surviving) && !surviving.value->header.parent,"parent deletion detaches but preserves ineligible reply");
    check(bool(store.open_message(bob,reply.value->id)),"surviving reply remains navigable");
    for(unsigned n=0;n<5;++n){const auto p=store.commit_message(request(store,sysop,{app::MessageKind::public_echo,{2},{},{}},operation++));check(bool(p) && bool(store.set_message_permanent(sysop,id(operation++),p.value->id,false,true)),"five explicitly Permanent posts");}
    for(unsigned n=0;n<35;++n)check(bool(store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},operation++))),"ordinary protected-overage fixture posts");
    retention_policy(store,{2},{4,1},operation++);now+=86400;
    const auto protected_run=run_retention(store,operation++);check(protected_run.removed_public==36 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==5,"count/age union removes ordinary once, protects all five");
    bool warning=false;for(const auto& e:protected_run.echoes)if(e.echo.value==2)warning=e.protected_overage==1;check(warning,"Permanent overage reported without quota refusal");
    check(bool(store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},operation++))),"posts remain permitted above protected cap");
    check(store.shutdown().succeeded(),"protection/age fixture stopped");
}
void retention_private_and_hold_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);std::int64_t now=1791278000-365LL*86400;s.utc_now=[&]{return now;};db::SqliteStorage store(s,policy);const auto file=s.data_directory/"SFX.DB";
    check(store.start().succeeded() && store.shutdown().succeeded(),"private/hold fixture bootstrap");seed(file);check(store.start().succeeded(),"private/hold fixture start");
    app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};unsigned operation=23000;
    const auto unread=store.commit_message(request(store,alice,{app::MessageKind::private_local,{},3,{}},operation++));const auto read=store.commit_message(request(store,alice,{app::MessageKind::private_local,{},3,{}},operation++));
    check(bool(unread) && bool(read) && bool(store.open_message(bob,read.value->id)),"read and unread real mailbox fixture");
    app::MessageQueryOptions sent;sent.mode=app::MessageQueryMode::sent;sent.scope=app::MessageQueryScope::all_accessible;
    const auto query=store.begin_message_query(alice,sent);check(bool(query) && bool(store.open_query_message(alice,*query.value,unread.value->id,false,false)),"sent view of unread mail");
    now=1791278000-1;check(run_retention(store,operation++).expired_unread==0,"private not expired before365 full days");++now;
    check(run_retention(store,operation++).expired_unread==1,"exact365-day unread expiry only");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==2 && scalar(file,"SELECT sum(inbox_removed) FROM sfx_message_recipient_state")==1 && scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state")==1,"expiry preserves sender/body and read mail");
    check(bool(store.quote_source(alice,unread.value->id)) && !store.quote_source(bob,unread.value->id) && bool(store.quote_source(bob,read.value->id)),"recipient inbox removal never widens sender/privacy ownership");
    const auto public_post=store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},operation++));check(bool(public_post),"genuine integrity fault target");
    {
        std::promise<void> checking,released;auto ready=checking.get_future();auto gate=released.get_future().share();std::atomic_bool first{true};
        const auto token=id(operation++);
        auto checking_job=std::async(std::launch::async,[&]{return store.check_message_integrity({true,{}},token,[&]{if(first.exchange(false)){checking.set_value();gate.wait();}return false;});});
        const bool entered=ready.wait_for(std::chrono::seconds(2))==std::future_status::ready;
        auto reader=std::async(std::launch::async,[&]{return store.quote_source(alice,public_post.value->id);});
        const bool responsive=reader.wait_for(std::chrono::milliseconds(500))==std::future_status::ready;
        released.set_value();const auto read_result=reader.get();const auto check_result=checking_job.get();
        check(entered && responsive && bool(read_result) && bool(check_result) && check_result.value->clean,"dedicated integrity snapshot does not occupy caller reader queue");
    }
    const auto clean_token=id(operation++);const auto initial=store.check_message_integrity({true,{}},clean_token,{});check(bool(initial) && initial.value->complete && initial.value->clean,"real full clean check");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);int prior{};
        check(sqlite3_db_config(c.get(),SQLITE_DBCONFIG_ENABLE_TRIGGER,0,&prior)==SQLITE_OK,"test-only connection suppresses trigger, stored schema unchanged");
        c.exec("UPDATE sfx_local_messages SET body=x'00' WHERE kind=0","genuine invalid stored message bytes, no product repair");}
    const auto corrupt=store.check_message_integrity({true,{}},id(operation++),{});check(bool(corrupt) && corrupt.value->complete && !corrupt.value->clean && corrupt.value->findings.size()==1 && corrupt.value->findings.front().scope==2,"real semantic corruption attributed to shared-store echo scope");
    check(store.shutdown().succeeded() && store.start().succeeded(),"scoped hold persists across normal restart");
    retention_policy(store,{2},{{},1},operation++);now+=86400;
    const auto blocked=run_retention(store,operation++);check(blocked.removed_public==0 && blocked.reason==app::MessageMaintenanceReason::corruption_hold && scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=0")==1,"manual policy edit/run cannot bypass scoped corruption hold");
    const auto replay=store.check_message_integrity({true,{}},clean_token,{});check(bool(replay) && replay.value->run.replayed && !replay.value->complete && !replay.value->clean,"old clean check never masquerades as current check");
    check(!store.message_integrity_holds({true,{}}).value->empty(),"old clean result leaves current hold");
    const auto cancelled=store.check_message_integrity({true,{}},id(operation++),[]{return true;});check(bool(cancelled) && !cancelled.value->complete && !cancelled.value->clean && !store.message_integrity_holds({true,{}}).value->empty(),"cancelled check cannot clear hold");
    const auto damaged=s.data_directory/"damaged-before-controlled-correction.sqlite";
    check(store.create_backup(damaged).status.succeeded() && scalar(damaged,"SELECT count(*) FROM sfx_local_messages WHERE hex(body)='00'")==1,"retain exact genuinely damaged database/hold snapshot before controlled test correction");
    // Simulate supported restoration into a NEW test copy elsewhere in installed
    // qualification. Here controlled fault correction exercises only current-check
    // state; it is explicitly not a product repair feature.
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);int prior{};check(sqlite3_db_config(c.get(),SQLITE_DBCONFIG_ENABLE_TRIGGER,0,&prior)==SQLITE_OK,"test fault correction connection");c.exec("UPDATE sfx_local_messages SET body=x'426f64790a' WHERE kind=0","controlled test correction only");}
    const auto fresh=store.check_message_integrity({true,{}},id(operation++),{});check(bool(fresh) && fresh.value->complete && fresh.value->clean && fresh.value->cleared_hold && store.message_integrity_holds({true,{}}).value->empty(),"fresh complete clean current check alone clears hold");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=0")==1,"clearing hold does not itself purge");
    check(run_retention(store,operation++).removed_public==1,"next explicit run may apply policy after current clean check");
    check(scalar(damaged,"SELECT count(*) FROM sfx_local_messages WHERE hex(body)='00'")==1 && scalar(damaged,"SELECT count(*) FROM sfx_message_integrity_holds WHERE scope=2")==1,"damaged evidence remains unchanged after good current resumption");
    check(store.shutdown().succeeded(),"private/hold fixture stopped");
}
