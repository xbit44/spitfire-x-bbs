{
    const auto current_policy=db.qwk_caller_policy(bob);check(current_policy&&current_policy.value->board_id=="X-BIT","ordinary caller authorized policy, no Config privilege needed");
    const auto source_for=[&](std::vector<app::QwkReplyRecord> records){return std::make_shared<ReplySource>(current_policy.value->board_id,std::move(records));};
    const auto before=scalar(file,"SELECT count(*) FROM sfx_local_messages");
    auto add=reply_record(2,"SFXQWK","ADD","");add.command=app::QwkScanCommand::add;
    auto drop=reply_record(1,"SFXQWK","DROP","");drop.command=app::QwkScanCommand::drop;
    auto long_quote=reply_record(2);long_quote.message.body="xb> "+std::string(90,'x')+" conference\n\nreply\n";
    auto source=source_for({long_quote,reply_record(0,"Alice"),add,drop});
    const auto absent=db.qwk_import_outcome(bob,source->info());check(absent&&!absent.value->has_value(),"own exact REP acceptance initially absent");
    const auto imported=db.import_qwk_reply(bob,source);if(!imported)std::cerr<<"REP code="<<static_cast<unsigned>(imported.status.code)<<" operation="<<imported.status.operation<<'\n';
    check(imported&&imported.value->messages==2&&imported.value->commands==2&&imported.value->missing_parents==0&&!imported.value->replayed&&source->rewinds==3,"whole bounded source validated before atomic two messages/two commands");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages")==before+2&&scalar(file,"SELECT count(*) FROM sfx_qwk_rep_receipts")==1,"posts and accepted REP receipt agree");
    check(scalar(file,"SELECT count(*) FROM sfx_local_messages WHERE author_account_id=3 AND subject=x'52455020636f6d706f6e656e74'")==2,"stored REP author derives from authenticated Bob");
    const auto imported_quote_id=scalar(file,"SELECT max(message_id) FROM sfx_local_messages WHERE kind=0 AND author_account_id=3");
    const auto imported_quote=db.quote_source(bob,{imported_quote_id});check(imported_quote&&imported_quote.value->body.rows[1].substr(0,4)=="xb> "&&imported_quote.value->body.rows[imported_quote.value->body.rows.size()-2].empty(),"actual REP import stores marked quote continuations and paragraph break");
    auto chat_selection=db.message_scan_preference(bob,{2}),notices_selection=db.message_scan_preference(bob,{1});
    check(chat_selection&&chat_selection.value->unread&&chat_selection.value->to_you&&notices_selection&&!notices_selection.value->unread&&!notices_selection.value->to_you,"plain ADD/DROP updates both shared scan columns");
    const auto accepted=db.qwk_import_outcome(bob,source->info());check(accepted&&accepted.value->value().replayed,"own receipt durable outcome query");
    const auto repeated=db.import_qwk_reply(bob,source);check(repeated&&repeated.value->replayed&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==before+2,"logical REP replay has no repeated post/count/commands");
    check(db.set_message_scan_preference(bob,{2},chat_selection.value->revision,app::MessageScanColumn::unread,false).succeeded(),"independent newer shared selection");
    check(db.import_qwk_reply(bob,source).value->replayed&&!db.message_scan_preference(bob,{2}).value->unread,"accepted command replay never rolls back newer selection");
    const auto invariant=[&]{return scalar(file,"SELECT count(*) FROM sfx_local_messages")*100000+scalar(file,"SELECT count(*) FROM sfx_qwk_rep_receipts")*1000+scalar(file,"SELECT total_committed FROM sfx_message_counters");};
    const auto unchanged=invariant();
    {
        internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        c.exec("CREATE TRIGGER sfx_test_rep_failure BEFORE INSERT ON sfx_local_messages WHEN CAST(NEW.subject AS TEXT)='Fail second REP' BEGIN SELECT RAISE(ABORT,'owned REP writer fault'); END","bounded new-fixture SQL fault");
    }
    auto sql_failure=source_for({reply_record(2,"All Callers","First before fault"),reply_record(0,"Alice","Fail second REP"),drop});
    check(!db.import_qwk_reply(bob,sql_failure)&&invariant()==unchanged,"actual second insert SQL failure rolls back first post/counters/intents/receipt");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec("DROP TRIGGER sfx_test_rep_failure","remove exact owned fault instrumentation from successful fixture");}
    for(unsigned position=0;position<3;++position){std::vector<app::QwkReplyRecord> records{reply_record(2),reply_record(0,"Alice"),drop};records[position].message.to="Nobody Here";records[position].command.reset();
        auto invalid=source_for(std::move(records));check(!db.import_qwk_reply(bob,invalid)&&invariant()==unchanged,"invalid first/middle/end record commits nothing");}
    for(unsigned fault=1;fault<=4;++fault){auto broken=source_for({reply_record(2,"All Callers","Fault "+std::to_string(fault)),reply_record(0,"Alice")});
        if(fault==1)broken->fail_pass=3;
        if(fault==2)broken->cancel_pass=3;
        if(fault==3)broken->mutate_pass=2;
        if(fault==4)broken->identity_valid=false;
        check(!db.import_qwk_reply(bob,broken)&&invariant()==unchanged,"source read/cancel/mutation/identity failure leaves no partial business effects");}
    auto bad_author=reply_record(2);bad_author.message.from="Alice";check(!db.import_qwk_reply(bob,source_for({bad_author}))&&invariant()==unchanged,"forged author refused rather than impersonated");
    auto partial_name=reply_record(0,"Ali");check(!db.import_qwk_reply(bob,source_for({partial_name}))&&invariant()==unchanged,"truncated recipient never guesses Alice");
    auto wrong=source_for({reply_record(2)});wrong->metadata.board_id="OTHER";check(!db.import_qwk_reply(bob,wrong)&&invariant()==unchanged,"wrong internal board refused");
    auto missing=reply_record(2,"All Callers","Missing original");missing.message.reference=9999999;
    auto missing_result=db.import_qwk_reply(bob,source_for({missing}));check(missing_result&&missing_result.value->messages==1&&missing_result.value->missing_parents==1,"unavailable original posts independently authorized new message with generic notice count");
    const auto new_count=scalar(file,"SELECT count(*) FROM sfx_local_messages");
    check(db.shutdown().succeeded()&&db.start().succeeded()&&db.import_qwk_reply(bob,source).value->replayed&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==new_count,"accepted exact logical REP receipt survives actual storage restart");
    auto forged_actor=bob;forged_actor.session=alice.session;check(!db.import_qwk_reply(forged_actor,source)&&!db.qwk_import_outcome(forged_actor,source->info()),"cross-account session cannot import/query accepted REP");
}
