void maintenance_migration_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    for(const char* point:{"before-message-maintenance-migration","before-message-maintenance-migration-commit","after-message-maintenance-migration-commit"}){
        auto s=options(root/point);auto prior_plan=db::sfx_storage_migrations();prior_plan.resize(12);
        const auto file=s.data_directory/"SFX.DB";
        {db::SqliteStorage prior(s,policy,prior_plan);check(prior.start().succeeded() && prior.shutdown().succeeded(),"exact v12 maintenance predecessor");}
        seed(file);app::MessageActor alice{id(2),2,1,1};
        {db::SqliteStorage prior(s,policy,prior_plan);check(prior.start().succeeded(),"populated v12 maintenance predecessor opens");
            check(bool(prior.commit_message(request(prior,alice,{app::MessageKind::public_echo,{2},{},{}},7000))),"old public message retained before 8C");
            check(bool(prior.commit_message(request(prior,alice,{app::MessageKind::private_local,{},3,{}},7001))),"old private message retained before 8C");
            check(prior.shutdown().succeeded(),"v12 predecessor stopped");}
        s.bootstrap_boundary_hook=[point](std::string_view at){return at==point?app::StorageStatus{app::StorageErrorCode::io,"isolated maintenance migration fault"}:app::StorageStatus::success();};
        {db::SqliteStorage failed(s,policy);check(!failed.start(),"maintenance migration failure reported");}
        check(scalar(file,"PRAGMA user_version")== (std::string_view(point)=="after-message-maintenance-migration-commit"?13:12),"maintenance migration/ledger publication atomic");
        {internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);check(internal::validate_sfx_shipping_database(c.get(),true),"maintenance failed attempt leaves exact recognized schema");}
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==2 && scalar(file,"SELECT count(*) FROM sfx_message_commit_receipts")==2,"migration failure never purges history");
        s.bootstrap_boundary_hook={};db::SqliteStorage current(s,policy);check(current.start().succeeded(),"maintenance migration retries to current once");
        check(scalar(file,"SELECT count(*) FROM sfx_echo_retention WHERE maximum_count=1000 AND maximum_age_days IS NULL")==2,"both old starter echoes receive only missing defaults");
        check(scalar(file,"SELECT maximum_age_days FROM sfx_unread_mail_retention")==365,"missing unread policy seeded365");
        check(scalar(file,"SELECT count(*) FROM sfx_message_maintenance_runs")==0 && scalar(file,"SELECT count(*) FROM sfx_message_permanent")==0,"seeding runs no cleanup and assigns no automatic Permanent");
        check(current.shutdown().succeeded() && current.start().succeeded() && current.shutdown().succeeded(),"restart idempotent without cleanup");
        check(scalar(file,"SELECT count(*) FROM sfx_schema_migrations")==sfx::test::shipping_migration_count && scalar(file,"SELECT count(*) FROM sfx_local_messages")==2,"the full migration ledger with frozen predecessor and old bodies retained");
    }
}

void maintenance_configuration_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);const auto file=s.data_directory/"SFX.DB";db::SqliteStorage store(s,policy);
    check(store.start().succeeded() && store.shutdown().succeeded(),"new retention configuration fixture");seed(file);check(store.start().succeeded(),"retention configuration fixture actors");
    app::MessageActor alice{id(2),2,1,1};
    check(bool(store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},7100))),"post before cap edit");
    check(bool(store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},7101))),"second post before cap edit");
    unsigned operation=7200;
    const auto snapshot=[&](){auto r=store.message_configuration({true,{}},app::MessageGroupId{1},{});check(bool(r) && r.value.has_value(),"authoritative retention snapshot");return *r.value;};
    const auto select=[](const app::MessageConfigurationSnapshot& snap,std::int64_t echo){for(const auto& e:snap.echoes)if(e.id.value==echo)return e;throw std::runtime_error("retention test echo missing");};
    const auto save=[&](app::LocalEchoId echo,app::MessageRetentionPolicy limits,bool expected){
        const auto before=snapshot();app::MessageConfigurationChange change;change.action=app::MessageConfigurationAction::echo;
        change.operation=id(operation++);change.expected_configuration_revision=before.revision;change.echo=select(before,echo.value);
        change.echo.retention=limits;change.retention_mask=3;const auto result=store.change_message_configuration({true,{}},change);
        check(bool(result)==expected,"finite/Unlimited policy validation through storage boundary");
        const auto after=snapshot();if(expected){const auto actual=select(after,echo.value).retention;check(actual.maximum_count==limits.maximum_count && actual.maximum_age_days==limits.maximum_age_days,"exact retention choices saved");}
        else check(after.revision==before.revision,"invalid policy cannot alter revision");
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==2,"saving policy never purges");
    };
    save({2},{1,1},true);save({2},{app::kMaximumRetainedMessageCount,app::kMaximumMessageAgeDays},true);
    save({2},{{},{}},true);save({2},{0,{}},false);save({2},{1000000000U,{}},false);save({2},{{},65536U},false);
    save({2},{1,5},true);
    std::vector<app::LocalEchoId> targets;
    for(const auto& name:{"Retention target A","Retention target B"}){
        const auto before=snapshot();app::MessageConfigurationChange c;c.operation=id(operation++);c.expected_configuration_revision=before.revision;c.action=app::MessageConfigurationAction::echo;
        c.echo.group={1};c.echo.name=name;c.echo.order=4+static_cast<std::int64_t>(targets.size());
        const auto result=store.change_message_configuration({true,{}},c);check(bool(result) && result.value.has_value(),"create echo with ready-to-use retention defaults");targets.push_back({result.value->entity_id});
        const auto created=select(snapshot(),targets.back().value);check(created.retention.maximum_count==1000U && !created.retention.maximum_age_days,"new echo count1000/ageUnlimited");
    }
    save(targets[0],{100,11},true);save(targets[1],{200,22},true);
    const auto clone=[&](std::uint8_t mask,bool stale){
        const auto before=snapshot();app::MessageConfigurationChange c;c.operation=id(operation++);c.expected_configuration_revision=before.revision;
        c.action=app::MessageConfigurationAction::clone_permissions;c.clone_source={2};c.source_revision=select(before,2).revision;c.retention_mask=mask;
        for(const auto target:targets)c.clone_targets.push_back({target,select(before,target.value).revision});
        if(stale)++c.clone_targets.back().revision;
        const auto result=store.change_message_configuration({true,{}},c);check(bool(result)!=stale,"atomic independent retention clone");
        const auto after=snapshot();if(stale){check(after.revision==before.revision,"stale clone rolls back catalog");
            for(const auto target:targets){const auto a=select(after,target.value),b=select(before,target.value);check(a.revision==b.revision && a.retention.maximum_count==b.retention.maximum_count && a.retention.maximum_age_days==b.retention.maximum_age_days,"stale clone rolls back every target policy");}}
        else for(const auto target:targets){const auto a=select(after,target.value),b=select(before,target.value),source=select(before,2);
            check(a.retention.maximum_count==((mask&1)?source.retention.maximum_count:b.retention.maximum_count) && a.retention.maximum_age_days==((mask&2)?source.retention.maximum_age_days:b.retention.maximum_age_days),"clone copies only selected Count/Age");
            check(a.permissions.access==b.permissions.access && a.permissions.reading==b.permissions.reading && a.permissions.posting==b.permissions.posting,"policy-only clone preserves permissions");}
        check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==2,"clone never purges messages");
    };
    clone(0,false);clone(1,false);clone(2,false);clone(3,true);
    for(const auto age:{std::optional<std::uint32_t>{},std::optional<std::uint32_t>{1U},std::optional<std::uint32_t>{65535U},std::optional<std::uint32_t>{365U}}){
        const auto before=snapshot();app::MessageConfigurationChange c;c.operation=id(operation++);c.expected_configuration_revision=before.revision;
        c.action=app::MessageConfigurationAction::unread_mail_retention;c.expected_unread_retention_revision=before.unread_retention_revision;c.unread_mail_age_days=age;
        check(bool(store.change_message_configuration({true,{}},c)),"global unread age finite/Unlimited choice saves without expiry");
        check(snapshot().unread_mail_age_days==age,"global unread choice exact");
        c.operation=id(operation++);c.expected_configuration_revision=snapshot().revision;
        check(!store.change_message_configuration({false,alice},c),"ordinary caller cannot change maintenance policy");
    }
    check(store.shutdown().succeeded() && store.start().succeeded(),"saved custom/Unlimited policies survive restart");
    check(select(snapshot(),2).retention.maximum_count==1U && select(snapshot(),targets[0].value).retention.maximum_age_days==5U,"retention metadata preserved on restart");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==2 && scalar(file,"SELECT count(*) FROM sfx_message_maintenance_runs")==0,"restart/settings never initiate purge");
    check(store.shutdown().succeeded(),"retention configuration fixture stopped");
}

void maintenance_permanent_and_sent_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);db::SqliteStorage store(s,policy);const auto file=s.data_directory/"SFX.DB";
    check(store.start().succeeded() && store.shutdown().succeeded(),"Permanent fixture creates current schema");seed(file);check(store.start().succeeded(),"Permanent fixture actors start");
    app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};unsigned operation=10000;
    const auto first=store.commit_message(request(store,alice,{app::MessageKind::public_echo,{2},{},{}},operation++));check(bool(first),"ordinary public post for explicit protection");
    const auto message=first.value->id;
    check(!*store.message_permanent_state(sysop,message).value,"ordinary post is not implicitly Permanent");
    const auto sysop_post=store.commit_message(request(store,sysop,{app::MessageKind::public_echo,{1},{},{}},operation++));check(bool(sysop_post) && !*store.message_permanent_state(sysop,sysop_post.value->id).value,"Sysop authorship alone not Permanent");
    check(!store.set_message_permanent(alice,id(operation++),message,false,true) && !store.set_message_permanent(bob,id(operation++),message,false,true),"ordinary caller cannot mark public Permanent");
    const auto mark=id(operation++);const auto marked=store.set_message_permanent(sysop,mark,message,false,true);
    check(bool(marked) && marked.value && !*marked.value,"authorized explicit Permanent commit");
    check(*store.set_message_permanent(sysop,mark,message,false,true).value,"same Permanent operation reconciles without another mutation");
    check(!store.set_message_permanent(sysop,mark,message,true,false),"Permanent replay cannot substitute new action");
    check(!store.set_message_permanent(sysop,id(operation++),message,false,true),"stale flag expectation rejected");
    check(store.quote_source(alice,message).value->header.permanent,"visible public reader header carries Permanent status");
    const auto refused=store.delete_message(sysop,id(operation++),message);
    check(!refused && refused.status.code==app::StorageErrorCode::permission_denied && bool(store.quote_source(sysop,message)),"individual deletion explicitly refuses Permanent without removal");
    check(bool(store.set_message_permanent(sysop,id(operation++),message,true,false)) && !*store.message_permanent_state(sysop,message).value,"authorized flag removal");
    check(bool(store.delete_message(sysop,id(operation++),message)) && !store.quote_source(sysop,message),"ordinary individual deletion works only after unmarking");
    const auto self=store.commit_message(request(store,alice,{app::MessageKind::private_local,{},2,{}},operation++));check(bool(self),"self-addressed private mail commits");
    check(!store.set_message_permanent(sysop,id(operation++),self.value->id,false,true),"private mail has no public Permanent flag");
    app::MessageQueryOptions sent;sent.mode=app::MessageQueryMode::sent;sent.scope=app::MessageQueryScope::all_accessible;
    const auto sent_query=store.begin_message_query(alice,sent);check(bool(sent_query),"self-mail Sent query begins");
    const auto sent_open=store.open_query_message(alice,*sent_query.value,self.value->id,false,false);
    check(bool(sent_open) && !sent_open.value->header.recipient_opened,"opening Sent copy never marks recipient Read even for self-mail");
    check(scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state")==0,"Sent view leaves recipient durable state untouched");
    app::MessageQueryOptions inbox;inbox.mode=app::MessageQueryMode::inbox;inbox.scope=app::MessageQueryScope::all_accessible;
    const auto inbox_query=store.begin_message_query(alice,inbox);check(bool(inbox_query),"self-mail actual recipient Inbox query");
    const auto inbox_open=store.open_query_message(alice,*inbox_query.value,self.value->id,false,false);
    check(bool(inbox_open) && inbox_open.value->header.recipient_opened && scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state")==1,"actual Inbox opening marks self-mail Read without deleting");
    check(bool(store.quote_source(alice,self.value->id)) && !store.quote_source(bob,self.value->id),"sender/recipient privacy preserved");
    check(store.shutdown().succeeded(),"Permanent/Sent fixture stopped");
}
