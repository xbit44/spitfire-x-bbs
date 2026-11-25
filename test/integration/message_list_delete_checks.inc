void message_list_delete_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy) {
    using Mode=app::MessageQueryMode;
    using Scope=app::MessageListDeleteScope;
    auto settings=options(root);
    const auto file=settings.data_directory/"SFX.DB";
    auto predecessor=db::sfx_storage_migrations();
    predecessor.resize(30);
    const app::MessageActor sysop{id(1),1,1,1},alice{id(2),2,1,1},bob{id(3),3,1,1};
    app::MessageQueryOptions echo;
    echo.echo={2};
    app::MessageQueryOptions inbox;
    inbox.mode=Mode::inbox;
    app::MessageQueryOptions sent;
    sent.mode=Mode::sent;
    app::MessageId public_post, private_post, previously_removed;
    {
        db::SqliteStorage old(settings,policy,predecessor);
        check(old.start() && old.health().schema_version==30 && old.shutdown(),"ordinary v30 fixture builds");
        seed(file);
        check(old.start().succeeded(),"existing v30 caller sessions preserved");
        const app::MessageTarget public_to_bob{app::MessageKind::public_echo,{2},bob.account,{}};
        const app::MessageTarget private_to_bob{app::MessageKind::private_local,{},bob.account,{}};
        const auto first=old.commit_message(request(old,alice,public_to_bob,340));
        const auto second=old.commit_message(request(old,alice,private_to_bob,341));
        const auto third=old.commit_message(request(old,alice,private_to_bob,342));
        check(first && second && third,"existing public and private posts on predecessor");
        public_post=first.value->id;
        private_post=second.value->id;
        previously_removed=third.value->id;
        check(old.remove_private_inbox_item(bob,previously_removed).succeeded(),"legacy reader removal before upgrade");
        const auto context=old.begin_message_query(bob,echo);
        check(context && old.query_message_headers(bob,*context.value,{}).value->entries.size()==1,
            "the ordinary predecessor reader still uses its original table");
        check(old.shutdown().succeeded(),"predecessor fixture closes before upgrade");
    }
    const auto clean_settings=settings;
    settings.bootstrap_boundary_hook=[](std::string_view point) {
        return point=="before-message-list-deletion-migration-commit" ?
            app::StorageStatus{app::StorageErrorCode::io,"injected v31 rollback"} : app::StorageStatus::success();
    };
    {
        db::SqliteStorage stopped(settings,policy);
        const auto failed=stopped.start();
        if (failed.operation!="before-message-list-deletion-migration-commit")
            std::cerr<<"migration boundary: "<<failed.operation<<'\n';
        check(!failed && failed.operation=="before-message-list-deletion-migration-commit",
            "fault before v31 commit is surfaced");
        internal::SqliteConnection previous(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
        check(previous.scalar_int64("PRAGMA user_version","predecessor rollback boundary")==30 &&
            internal::validate_sfx_shipping_database(previous.get(),true),"v31 fault retains exact trusted v30 predecessor");
    }
    db::SqliteStorage store(clean_settings,policy);
    check(store.start() && store.health().schema_version==31,"populated v30 fixture migrates to v31");
    const auto context=[&](const app::MessageActor& caller,const app::MessageQueryOptions& options) {
        auto result=store.begin_message_query(caller,options);
        check(static_cast<bool>(result),"open exact caller query context");
        return std::move(*result.value);
    };
    const auto list=[&](const app::MessageActor& caller,const app::MessageQueryContext& query) {
        auto result=store.query_message_list_headers(caller,query,{});
        check(static_cast<bool>(result),"query authorized message list");
        return std::move(result.value->entries);
    };
    const auto sysop_public=context(sysop,echo);
    const auto bob_public=context(bob,echo);
    auto rows=list(sysop,sysop_public);
    check(rows.size()==1 && rows.front().list_delete_allowed && !rows.front().list_deleted &&
        rows.front().list_delete_scope==Scope::everyone,"sysop may delete the active selected public row");
    rows=list(bob,bob_public);
    check(rows.size()==1 && !rows.front().list_delete_allowed,"ordinary caller cannot delete a public post");
    check(!store.set_message_list_deleted(bob,bob_public,public_post,false,true,Scope::everyone),
        "direct request cannot bypass current authority");
    check(!store.set_message_list_deleted(sysop,sysop_public,private_post,false,true,Scope::everyone),
        "visible authorization does not bypass the selected list query");
    check(store.set_message_list_deleted(sysop,sysop_public,public_post,false,true,Scope::everyone).succeeded(),
        "public delete takes effect immediately");
    check(!store.set_message_list_deleted(sysop,sysop_public,public_post,false,true,Scope::everyone),
        "stale expected state cannot toggle or repeat the change");
    check(!store.set_message_list_deleted(sysop,sysop_public,public_post,true,false,Scope::inbox),
        "an expected Inbox scope cannot change a board-wide deletion");
    rows=list(sysop,sysop_public);
    check(rows.size()==1 && rows.front().identity.id==public_post && rows.front().list_deleted &&
        rows.front().list_delete_allowed,"the same row stays visible to the authorized restorer");
    check(list(bob,bob_public).empty() && store.query_message_headers(sysop,sysop_public,{}).value->entries.empty(),
        "deleted public post hidden from ordinary headers and non-list sysop traversal");
    auto search=echo;
    search.search="Body";
    check(list(bob,context(bob,search)).empty() &&
        !store.message_number_header(bob,bob_public,1),"search and direct number entry stay within active messages");
    check(!store.open_query_message(sysop,sysop_public,public_post,false,false,true) &&
        !store.open_message(bob,public_post) && !store.quote_source(sysop,public_post),
        "stale row, direct read and quote cannot reach deleted content");
    const auto arrival=store.message_arrival_eligible(bob,public_post);
    check(arrival && !*arrival.value,"arrival routing drops the deleted public message");
    check(store.message_scan_preference(bob,{2}).value->highest_opened==0,
        "delete left normal read progress untouched");
    check(store.shutdown() && store.start(),"reconnect to persisted reversible deletion");
    rows=list(sysop,context(sysop,echo));
    check(rows.size()==1 && rows.front().list_deleted,"public deletion survives a storage restart");
    check(store.set_message_list_deleted(sysop,sysop_public,public_post,true,false,Scope::everyone).succeeded(),
        "second action immediately restores the public post");
    check(!store.quote_source(bob,public_post).value->header.recipient_opened,
        "restore preserves prior recipient unread state and content access");
    check(store.set_message_permanent(sysop,id(343),public_post,false,true) &&
        !store.set_message_list_deleted(sysop,sysop_public,public_post,false,true,Scope::everyone),
        "Permanent protection remains enforced in the writer");
    rows=list(sysop,sysop_public);
    check(rows.size()==1 && rows.front().permanent && !rows.front().list_delete_allowed,"Permanent item offers no list deletion");

    const auto bob_inbox=context(bob,inbox);
    const auto alice_sent=context(alice,sent);
    rows=list(bob,bob_inbox);
    check(rows.size()==1 && rows.front().identity.id==private_post &&
        rows.front().list_delete_scope==Scope::inbox,"Inbox grants reversible removal only to this recipient");
    check(store.set_message_list_deleted(bob,bob_inbox,private_post,false,true,Scope::inbox).succeeded(),
        "recipient immediately removes own Inbox copy");
    rows=list(bob,bob_inbox);
    check(rows.size()==1 && rows.front().list_deleted && rows.front().list_delete_allowed,
        "only this reversible removal appears starred in recipient Inbox");
    check(store.query_message_headers(bob,bob_inbox,{}).value->entries.empty() &&
        !store.open_query_message(bob,bob_inbox,private_post,false,false),
        "removed Inbox copy hidden from ordinary traversal and reader");
    app::MessageQueryOptions your_scan;
    your_scan.mode=Mode::correspondence;
    your_scan.scope=app::MessageQueryScope::all_accessible;
    for (const auto direction:{app::MessageCallerDirection::to,app::MessageCallerDirection::both}) {
        your_scan.caller_direction=direction;
        const auto query=context(bob,your_scan);
        rows=list(bob,query);
        const auto found=std::find_if(rows.begin(),rows.end(),[&](const auto& row){return row.identity.id==private_post;});
        check(found!=rows.end() && found->list_deleted && found->list_delete_scope==Scope::inbox,
            "Your Scan To/Both also exposes the exact reversible Inbox star");
        check(store.set_message_list_deleted(bob,query,private_post,true,false,Scope::inbox) &&
            store.set_message_list_deleted(bob,query,private_post,false,true,Scope::inbox),
            "recipient can restore and delete from the same To/Both list context");
    }
    rows=list(alice,alice_sent);
    check(rows.size()==2 && !rows.front().list_deleted && !rows.back().list_deleted &&
        store.quote_source(alice,private_post).value->body.canonical_lf=="Body\n",
        "sender copies remain active and original private content remains exact");
    check(store.set_message_list_deleted(bob,bob_inbox,private_post,true,false,Scope::inbox).succeeded() &&
        !store.quote_source(bob,private_post).value->header.recipient_opened,
        "restore own Inbox copy without marking it read");
    check(scalar(file,"SELECT count(*) FROM sfx_message_list_deletion")==0 &&
        scalar(file,"SELECT count(*) FROM sfx_local_messages")==3 && store.shutdown().succeeded(),
        "both restoration scopes keep original bodies and the fixture shuts down");
}
