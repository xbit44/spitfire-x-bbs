void area_deletion_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    check(sfx::test::install_sqlite_write_fault_vfs("section8c-area-delete"),"whole deletion physical fault scope");
    try{
        auto settings=options(root/"section8c-area-delete");db::SqliteStorage store(settings,policy);const auto file=settings.data_directory/"SFX.DB";
        check(store.start().succeeded() && store.shutdown().succeeded(),"area deletion new candidate schema");seed(file);check(store.start().succeeded(),"area deletion fixture actors");
        const app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};unsigned token=40000;
        const auto snapshot=[&](){return *store.message_configuration({true,{}},{},{}).value;};
        const auto chat=[&](){return *store.message_configuration({true,{}},app::MessageGroupId{1},{}).value;};
        const auto edit_echo=[&](app::LocalEchoId echo){for(const auto& e:chat().echoes)if(e.id==echo)return e;throw std::runtime_error("deletion echo missing");};
        const auto post=[&](app::LocalEchoId echo){const auto m=store.commit_message(request(store,alice,{app::MessageKind::public_echo,echo,{},{}},token++));check(bool(m),"selected area post");return m.value->id;};
        const auto protected_message=post({2});post({2});
        check(bool(store.set_message_permanent(sysop,id(token++),protected_message,false,true)),"selected Permanent before whole deletion");
        const auto mail=store.commit_message(request(store,alice,{app::MessageKind::private_local,{},3,{}},token++));check(bool(mail),"unrelated private mail fixture");
        app::MessageConfigurationChange c;c.operation=id(token++);c.action=app::MessageConfigurationAction::group;c.expected_configuration_revision=snapshot().revision;c.group.name="Preserved group";c.group.order=3;
        const auto group=store.change_message_configuration({true,{}},c);check(bool(group),"unrelated group creation");
        c={};c.operation=id(token++);c.action=app::MessageConfigurationAction::echo;c.expected_configuration_revision=snapshot().revision;c.echo.group={group.value->entity_id};c.echo.name="Preserved echo";c.echo.order=0;
        const auto echo=store.change_message_configuration({true,{}},c);check(bool(echo),"unrelated echo creation");const auto survivor=post({echo.value->entity_id});
        const app::MessageDeletionTarget selected{app::MessageDeletionKind::echo,2,edit_echo({2}).revision};
        const auto preview=store.preview_message_deletion({true,{}},selected,{});check(bool(preview) && preview.value->name=="Public Chat" && preview.value->echo_count==1 && preview.value->message_count==2 && preview.value->permanent_count==1,"exact full public preview includes Permanent count");
        check(scalar(file,"SELECT count(*) FROM sfx_message_area_delete_receipts")==0 && scalar(file,"SELECT count(*) FROM sfx_local_messages")==4,"preview/No/Cancel never configuration-only deletion");
        check(!store.preview_message_deletion({false,alice},selected,{}) && !store.commit_message_deletion({false,sysop},id(token++),*preview.value,{}),"only trusted offline owner can use destructive component, not caller privilege bit");
        auto forged=*preview.value;forged.permanent_count=0;check(!store.commit_message_deletion({true,{}},id(token++),forged,{}),"confirmation counts cannot be forged with old content hash");
        post({2});check(!store.commit_message_deletion({true,{}},id(token++),*preview.value,{}),"new content invalidates stale preview without target substitution");
        const auto fresh=store.preview_message_deletion({true,{}},selected,{});check(bool(fresh) && fresh.value->message_count==3,"current full content set preview");
        const auto deletion=id(token++);sfx::test::inject_next_sqlite_wal_write_fault(SQLITE_FULL);
        const auto full=store.commit_message_deletion({true,{}},deletion,*fresh.value,{});check(!full && sfx::test::pending_sqlite_write_fault()==SQLITE_OK,"whole deletion actual physical FULL no success");
        check(scalar(file,"SELECT count(*) FROM sfx_local_echoes WHERE echo_id=2")==1 && scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE echo_id=2")==3 && scalar(file,"SELECT count(*) FROM sfx_message_area_delete_receipts")==0 && scalar(file,"SELECT count(*) FROM sfx_message_purge_scope")==0,"whole deletion FULL rolls back content/config/receipts/scopes together");
        const auto cancelled=store.commit_message_deletion({true,{}},id(token++),*fresh.value,[]{return true;});check(!cancelled && scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE echo_id=2")==3,"cancelled confirmation execution leaves target intact");
        const auto done=store.commit_message_deletion({true,{}},deletion,*fresh.value,{});check(bool(done) && !done.value->replayed && done.value->message_count==3 && done.value->permanent_count==1,"explicit whole-echo exception deletes included Permanent with exact counts");
        const auto repeated=store.commit_message_deletion({true,{}},deletion,*fresh.value,{});check(bool(repeated) && repeated.value->replayed,"same uncertain commit reconciles after target is gone");
        check(!store.commit_message_deletion({true,{}},deletion,forged,{}),"same operation cannot substitute another confirmation");
        check(bool(store.quote_source(alice,mail.value->id)) && bool(store.quote_source(bob,mail.value->id)) && bool(store.quote_source(alice,survivor)),"unrelated private and public retained");
        const auto groups=snapshot();app::MessageGroup local;for(const auto& g:groups.groups)if(g.id.value==1)local=g;
        const auto group_preview=store.preview_message_deletion({true,{}},{app::MessageDeletionKind::group,1,local.revision},{});
        check(bool(group_preview) && group_preview.value->echo_count==1,"remaining starter group preview");
        check(bool(store.commit_message_deletion({true,{}},id(token++),*group_preview.value,{})),"Sysop may remove starter Local and last starter echo");
        auto all=snapshot();check(all.groups.size()==1 && all.groups.front().id.value==group.value->entity_id,"starter not privileged or automatically recreated");
        const auto final_preview=store.preview_message_deletion({true,{}},{app::MessageDeletionKind::group,all.groups.front().id.value,all.groups.front().revision},{});
        check(bool(final_preview) && final_preview.value->message_count==1 && bool(store.commit_message_deletion({true,{}},id(token++),*final_preview.value,{})),"populated final group/echo/content removed in one operation");
        check(scalar(file,"SELECT count(*) FROM sfx_message_groups")==0 && scalar(file,"SELECT count(*) FROM sfx_local_echoes")==0 && scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE kind=1")==1,"no public areas safe; private body untouched");
        check(store.shutdown().succeeded() && store.start().succeeded() && store.message_groups(alice).value->empty() && bool(store.quote_source(alice,mail.value->id)),"restart does not reseed deliberate final-group deletion; private mail works");
        c={};c.operation=id(token++);c.action=app::MessageConfigurationAction::group;c.expected_configuration_revision=snapshot().revision;c.group.name="New group";c.group.order=0;
        const auto newly=store.change_message_configuration({true,{}},c);check(bool(newly) && newly.value->entity_id>group.value->entity_id,"new group gets new stable identity, not inherited source links");
        check(store.shutdown().succeeded(),"whole area fixture stopped");
    }catch(...){sfx::test::inject_next_sqlite_write_fault(SQLITE_OK);sfx::test::uninstall_sqlite_write_fault_vfs();throw;}
    sfx::test::uninstall_sqlite_write_fault_vfs();
}
