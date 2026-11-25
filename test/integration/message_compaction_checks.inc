void compaction_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    check(sfx::test::install_sqlite_write_fault_vfs("section8c-compaction"),"compaction private scoped physical fault VFS");
    try{
        auto settings=options(root/"section8c-compaction");db::SqliteStorage store(settings,policy);const auto file=settings.data_directory/"SFX.DB";
        check(store.start().succeeded() && store.shutdown().succeeded(),"compaction current schema fixture bootstrap");seed(file);check(store.start().succeeded(),"compaction fixture actors");
        const app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1};unsigned token=43000;
        std::vector<app::MessageId> messages;
        for(unsigned n=0;n<40;++n){auto r=request(store,alice,{app::MessageKind::public_echo,{2},{},{}},token++);r.body=std::string(12000,'x');
            const auto committed=store.commit_message(r);check(bool(committed),"compaction disposable fragmentation post");messages.push_back(committed.value->id);}
        check(bool(store.set_message_permanent(sysop,id(token++),messages.front(),false,true)),"compaction Permanent retained fixture");
        const auto mail=store.commit_message(request(store,alice,{app::MessageKind::private_local,{},3,{}},token++));check(bool(mail),"compaction private unread fixture");
        retention_policy(store,{2},{2,{}},token++);check(run_retention(store,token++).removed_public==38,"logical removal creates reusable pages without physical compaction");
        const auto temp=root/"private-temp";std::filesystem::create_directories(temp);
        check(!store.compact_offline(temp,{}).status && scalar(file,"SELECT count(*) FROM sfx_local_messages")==3,"active storage compaction refused without mutation");
        const auto next=scalar(file,"SELECT next_message_number FROM sfx_local_echoes WHERE echo_id=2"),arrival=scalar(file,"SELECT last_arrival_ordinal FROM sfx_message_counters WHERE singleton_id=1");
        check(store.shutdown().succeeded(),"compaction component fully stopped");
        const auto cancelled=store.compact_offline(temp,[]{return true;});check(!cancelled.status && !cancelled.physical_compaction_committed && scalar(file,"SELECT count(*) FROM sfx_local_messages")==3,"early cancellation commits no physical change");
        sfx::test::inject_next_sqlite_wal_write_fault(SQLITE_FULL);const auto full=store.compact_offline(temp,{});
        std::cout<<"COMPACTION WAL FULL status="<<static_cast<unsigned>(full.status.code)<<" pending="<<sfx::test::pending_sqlite_write_fault()<<" committed="<<full.physical_compaction_committed<<" logical="<<full.logical_content_verified<<'\n';
        check(!full.status && sfx::test::pending_sqlite_write_fault()==SQLITE_OK && !full.physical_compaction_committed && full.logical_content_verified,"real compaction FULL rolls back; independent logical reconciliation, no false success");
        check(std::filesystem::is_empty(temp),"failed private compaction does not leave rows in public or private temporary files");
        const auto done=store.compact_offline(temp,{});check(done.status.succeeded() && done.physical_compaction_committed && done.logical_content_verified && done.bytes_before>0 && done.bytes_after>0,"stopped transactional compaction compares every logical table/value before and after");
        check(std::filesystem::is_empty(temp) && scalar(file,"SELECT count(*) FROM sfx_accounts")==3 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==3 && scalar(file,"SELECT count(*) FROM sfx_message_permanent WHERE permanent=1")==1,"compaction retains accounts/public/private/Permanent and closes private temporary files");
        check(scalar(file,"SELECT next_message_number FROM sfx_local_echoes WHERE echo_id=2")==next && scalar(file,"SELECT last_arrival_ordinal FROM sfx_message_counters WHERE singleton_id=1")==arrival && scalar(file,"SELECT maximum_count FROM sfx_echo_retention WHERE echo_id=2")==2,"stable explicit identities/counters/policies survive physical rewrite");
        check(store.start().succeeded() && bool(store.quote_source(alice,messages.front())) && bool(store.quote_source(alice,mail.value->id)) && store.shutdown().succeeded(),"reopen current compacted store; protected and sender private body still usable");
    }catch(...){sfx::test::inject_next_sqlite_write_fault(SQLITE_OK);sfx::test::uninstall_sqlite_write_fault_vfs();throw;}
    sfx::test::uninstall_sqlite_write_fault_vfs();
}
