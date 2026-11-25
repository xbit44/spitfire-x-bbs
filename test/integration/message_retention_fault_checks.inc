void retention_fault_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    check(sfx::test::install_sqlite_write_fault_vfs("section8c-retention-io"),"retention scoped physical write fault VFS");
    try{
        auto s=options(root/"section8c-retention-io");s.busy.sqlite_timeout=std::chrono::milliseconds(20);s.busy.transaction_deadline=std::chrono::milliseconds(100);s.busy.maximum_attempts=1;
        db::SqliteStorage store(s,policy);const auto file=s.data_directory/"SFX.DB";
        check(store.start().succeeded() && store.shutdown().succeeded(),"retention fault fixture bootstrap");seed(file);check(store.start().succeeded(),"retention fault fixture ready");
        app::MessageActor alice{id(2),2,1,1};unsigned operation=24000;
        for(unsigned n=0;n<100;++n)check(bool(store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},operation++))),"fault fixture posts");
        retention_policy(store,{2},{1,{}},operation++);const auto run_id=id(operation++);
        check(bool(store.begin_message_retention({true,{}},run_id)),"fault run begins");
        check(!store.begin_message_retention({true,{}},id(operation++)) && !store.check_message_integrity({true,{}},id(operation++),{}),"no overlapping manual/scheduled/check work");
        // Notices is an empty captured area, then Chat has eligible messages.
        const auto first=store.message_retention_batch({true,{}},run_id,1,{});check(bool(first) && first.value->removed_public==0,"empty area batch captured");
        {internal::SqliteConnection held(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);held.exec("BEGIN IMMEDIATE","real controlled competing writer");
            const auto busy=store.message_retention_batch({true,{}},run_id,2,{});check(!busy && busy.status.code==app::StorageErrorCode::busy,"contention fails bounded without dropping locks");held.exec("ROLLBACK","release test writer only");}
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==100,"busy does not mutate candidates");
        unsigned polls{};const auto cancelled=store.message_retention_batch({true,{}},run_id,2,[&]{return ++polls>=37;});
        check(!cancelled && scalar(file,"SELECT count(*) FROM sfx_local_messages")==100 && scalar(file,"SELECT count(*) FROM sfx_message_delete_scope")==0,"in-batch cancellation rolls back all uncommitted work/scopes");
        check(store.message_maintenance_run({true,{}},run_id).value->removed_public==0,"cancelled work is not optimistic count");
        const auto committed=store.message_retention_batch({true,{}},run_id,2,{});if(!committed)std::cerr<<"retention commit code="<<static_cast<unsigned>(committed.status.code)<<" operation="<<committed.status.operation<<"\n";check(bool(committed) && committed.value->removed_public==32,"next explicit batch commits bounded32");
        sfx::test::inject_next_sqlite_wal_write_fault(SQLITE_FULL);
        const auto full=store.message_retention_batch({true,{}},run_id,3,{});
        std::cout << "RETENTION WAL FULL status=" << static_cast<unsigned>(full.status.code) << " pending=" << sfx::test::pending_sqlite_write_fault() << " actual=" << scalar(file,"SELECT count(*) FROM sfx_local_messages") << '\n';
        check(!full && sfx::test::pending_sqlite_write_fault()==SQLITE_OK,"genuine FULL physical write produces no success");
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==68 && scalar(file,"SELECT count(*) FROM sfx_message_purged_receipts")==32 && scalar(file,"SELECT count(*) FROM sfx_message_delete_scope")==0,"physical failure rolls back current batch, preserves earlier committed32");
        const auto finish=store.finish_message_retention({true,{}},run_id,app::MessageMaintenanceOutcome::failed,app::MessageMaintenanceReason::storage_failure);
        check(bool(finish) && finish.value->outcome==app::MessageMaintenanceOutcome::partial && finish.value->removed_public==32,"partial failure honestly reports committed totals");
        const auto replay=store.begin_message_retention({true,{}},run_id);check(bool(replay) && replay.value->replayed && replay.value->outcome==app::MessageMaintenanceOutcome::partial,"old run receipt never resumes/purges again");
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==68,"failure has no immediate automatic retry");
        check(run_retention(store,operation++).removed_public==67,"next explicitly requested run reevaluates current data");
        const auto stale_id=id(operation++);check(bool(store.begin_message_retention({true,{}},stale_id)),"policy-change fixture begins");
        retention_policy(store,{2},{{},{}},operation++);
        for(std::int64_t seq=1;seq<=3;++seq)check(bool(store.message_retention_batch({true,{}},stale_id,seq,{})),"stale captured policy skips without substitution");
        const auto changed=store.finish_message_retention({true,{}},stale_id,app::MessageMaintenanceOutcome::skipped,app::MessageMaintenanceReason::policy_changed);
        check(bool(changed) && changed.value->removed_public==0 && changed.value->reason==app::MessageMaintenanceReason::policy_changed,"captured changed policy reports skipped, not success");
        const auto interrupted=id(operation++);check(bool(store.begin_message_retention({true,{}},interrupted)),"unfinished fixture run begins");
        check(store.shutdown().succeeded() && store.start().succeeded() && store.recover_interrupted_message_maintenance({true,{}}).succeeded(),"startup records interruption without executing retention");
        check(store.message_maintenance_run({true,{}},interrupted).value->outcome==app::MessageMaintenanceOutcome::interrupted && scalar(file,"SELECT count(*) FROM sfx_local_messages")==1,"restart never replays old destructive work");
        for(unsigned n=0;n<35;++n)check(run_retention(store,operation++).removed_public==0,"bounded quiet success history");
        const auto history=store.message_maintenance_history({true,{}},app::MessageMaintenanceKind::retention);
        check(bool(history) && history.value->size()==32,"retained details/history bounded32");
        check(store.begin_message_retention({true,{}},run_id).value->replayed && scalar(file,"SELECT count(*) FROM sfx_local_messages")==1,"pruned details retain minimal token reconciliation, no renewed purge");
        check(!store.begin_message_retention({false,alice},id(operation++)),"ordinary caller denied maintenance authority");
        check(store.shutdown().succeeded(),"retention fault fixture stopped");
    }catch(...){sfx::test::inject_next_sqlite_write_fault(SQLITE_OK);sfx::test::uninstall_sqlite_write_fault_vfs();throw;}
    sfx::test::uninstall_sqlite_write_fault_vfs();
}
