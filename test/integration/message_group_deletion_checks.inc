void group_deletion_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    check(sfx::test::install_sqlite_write_fault_vfs("section8c-group-delete"),"populated group physical fault scope");
    try{
        auto settings=options(root/"section8c-group-delete");db::SqliteStorage store(settings,policy);const auto file=settings.data_directory/"SFX.DB";
        check(store.start().succeeded() && store.shutdown().succeeded(),"multi-echo group current schema");seed(file);check(store.start().succeeded(),"multi-echo group fixture actors");
        const app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1};unsigned token=46000;
        const auto catalog=[&](){return *store.message_configuration({true,{}},{},{}).value;};
        app::MessageConfigurationChange change;change.operation=id(token++);change.action=app::MessageConfigurationAction::group;change.expected_configuration_revision=catalog().revision;change.group.name="Populated removable group";change.group.order=4;
        const auto group=store.change_message_configuration({true,{}},change);check(bool(group),"new actual custom group");
        std::vector<app::LocalEchoId> echoes;
        for(unsigned i=0;i<2;++i){change={};change.operation=id(token++);change.action=app::MessageConfigurationAction::echo;change.expected_configuration_revision=catalog().revision;
            change.echo.group={group.value->entity_id};change.echo.name="Populated child "+std::to_string(i);change.echo.order=i;const auto echo=store.change_message_configuration({true,{}},change);check(bool(echo),"new populated custom child");echoes.push_back({echo.value->entity_id});
            const auto post=store.commit_message(request(store,alice,{app::MessageKind::public_echo,echoes.back(),{},{}},token++));check(bool(post),"actual group child public content");
            check(bool(store.set_message_permanent(sysop,id(token++),post.value->id,false,true)),"each included child has Permanent content");}
        const app::MessageDeletionTarget target{app::MessageDeletionKind::group,group.value->entity_id,group.value->entity_revision};
        const auto preview=store.preview_message_deletion({true,{}},target,{});check(preview && preview.value && preview.value->echo_count==2 && preview.value->message_count==2 && preview.value->permanent_count==2,"exact populated multi-echo/Permanent group preview");
        {internal::SqliteConnection corrupted(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
            // Deliberate corruption of this NEW disposable fixture only. Ordinary
            // DDL already refuses this label; never weaken production constraints.
            corrupted.exec("PRAGMA ignore_check_constraints=ON","test-only corruption seam");
            corrupted.exec("UPDATE sfx_message_groups SET name=char(1) WHERE group_id=2","controlled malformed public label fixture only");
            corrupted.exec("PRAGMA ignore_check_constraints=OFF","restore ordinary constraints on test connection");}
        check(!store.preview_message_deletion({true,{}},target,{}),"corrupt unprintable target cannot be silently substituted/truncated by final confirmation");
        const auto bad_name=settings.data_directory/"retained-damaged-public-label.sqlite";
        check(!std::filesystem::exists(bad_name),"new damaged-label evidence path only");
        {internal::SqliteConnection damaged(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
            internal::SqliteConnection retained(bad_name,SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE,std::chrono::milliseconds(100),false);
            // Raw SQLite pager copy preserves the actual invalid CHECK value and
            // current WAL pages. A production verified backup must not be made to
            // accept corruption merely to retain engineering failure evidence.
            auto* copy=sqlite3_backup_init(retained.get(),"main",damaged.get(),"main");
            check(copy!=nullptr,"raw damaged-fixture evidence copy starts");
            const auto copied=sqlite3_backup_step(copy,-1);const auto closed=sqlite3_backup_finish(copy);
            check(copied==SQLITE_DONE && closed==SQLITE_OK,"preserve exact damaged label before test-only correction");}
        {internal::SqliteConnection corrected(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);corrected.exec("UPDATE sfx_message_groups SET name='Populated removable group' WHERE group_id=2","controlled fixture correction, not product repair");}
        check(scalar(bad_name,"SELECT unicode(name) FROM sfx_message_groups WHERE group_id=2")==1,"retained damaged label remains uncorrected");
        const auto deletion=id(token++);sfx::test::inject_next_sqlite_wal_write_fault(SQLITE_FULL);
        const auto failed=store.commit_message_deletion({true,{}},deletion,*preview.value,{});
        check(!failed && sfx::test::pending_sqlite_write_fault()==SQLITE_OK && scalar(file,"SELECT count(*) FROM sfx_message_groups")==2 && scalar(file,"SELECT count(*) FROM sfx_local_echoes")==4 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==2 && scalar(file,"SELECT count(*) FROM sfx_message_purge_scope")==0,"physical group FULL never exposes partial children/config/content deletion");
        const auto interrupted=store.commit_message_deletion({true,{}},id(token++),*preview.value,[]{return true;});check(!interrupted && scalar(file,"SELECT count(*) FROM sfx_local_echoes")==4,"controlled group commit cancellation preserves full set");
        const auto done=store.commit_message_deletion({true,{}},deletion,*preview.value,{});
        check(done && done.value && done.value->echo_count==2 && done.value->message_count==2 && done.value->permanent_count==2 && scalar(file,"SELECT count(*) FROM sfx_message_groups")==1 && scalar(file,"SELECT count(*) FROM sfx_local_echoes")==2 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==0,"explicit final Yes removes all selected children/Permanent/content atomically, preserves unrelated starters");
        check(store.commit_message_deletion({true,{}},deletion,*preview.value,{}).value->replayed && scalar(file,"SELECT count(*) FROM sfx_message_area_delete_receipts")==1,"group uncertain result reconciles exact single receipt");
        check(store.shutdown().succeeded() && store.start().succeeded() && scalar(file,"SELECT count(*) FROM sfx_local_echoes")==2 && store.shutdown().succeeded(),"removed custom group stays removed after restart");
    }catch(...){sfx::test::inject_next_sqlite_write_fault(SQLITE_OK);sfx::test::uninstall_sqlite_write_fault_vfs();throw;}
    sfx::test::uninstall_sqlite_write_fault_vfs();
}
