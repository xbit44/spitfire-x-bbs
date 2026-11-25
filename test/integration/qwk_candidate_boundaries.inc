// Actual SQLite selection compared with ordinary single-queue reads.
void bounded_queue_checks(storage::SqliteStorage& db,const app::MessageActor& reader,const app::MessageActor& author,const std::filesystem::path& file){
    const auto initial=db.begin_qwk_packet(reader,id(150));check(bool(initial),"bounded checks capture current plan");
    const auto scan=[&](app::LocalEchoId echo,bool enabled){for(const auto column:{app::MessageScanColumn::unread,app::MessageScanColumn::to_you}){const auto before=db.message_scan_preference(reader,echo);check(bool(before)&&bool(db.set_message_scan_preference(reader,echo,before.value->revision,column,enabled)),"ordinary synthetic scan change");}};
    for(const auto& echo:initial.value->catalog)scan(echo.echo,false);
    const auto single=db.begin_qwk_packet(reader,id(151));check(single&&single.value->queues.size()==1,"one-queue production fixture");
    const auto one=db.select_qwk_candidate(reader,*single.value,0,1);check(one&&!one.value->selected&&one.value->scanned==1&&one.value->queue==0,"single exhausted private queue stops after one turn");
    for(const auto& echo:initial.value->catalog)scan(echo.echo,true);
    app::LocalEchoId last_echo;
    for(unsigned n=0;n<6;++n){
        const app::MessageConfigurationActor owner{true,{}};const auto config=db.message_configuration(owner,{},{});check(bool(config),"fresh ordinary synthetic catalog");
        app::MessageConfigurationChange change;change.operation=id(200+n);change.action=app::MessageConfigurationAction::echo;change.expected_configuration_revision=config.value->revision;change.echo.group={1};change.echo.name="Bounded area "+std::to_string(n);change.echo.order=n+2;
        const auto added=db.change_message_configuration(owner,change);check(bool(added),"ordinary bounded-area creation");last_echo={added.value->entity_id};
        if(n<4)continue;
        if(n==5){const app::MessageTarget target{app::MessageKind::public_echo,last_echo,{},{}};const auto context=db.begin_message(author,target);check(bool(context)&&bool(db.commit_message({author,id(220),*context.value,"Ninth queue","Synthetic final-block message\n"})),"ordinary ninth-queue eligible message");}
        const auto p=db.begin_qwk_packet(reader,id(210+n));const std::size_t count=n==4?8:9;check(p&&p.value->queues.size()==count,"actual eight/nine queue fixture");
        for(std::size_t start=0;start<count;++start){
            std::optional<app::MessageId> expected;std::size_t visited{},expected_queue=start;
            for(;visited<count;){expected_queue=(start+visited)%count;const auto next=db.next_qwk_message(reader,*p.value,expected_queue);check(bool(next),"ordinary sequential comparison authority");++visited;if(next.value->has_value()){expected=next.value->value().header.identity.id;break;}}
            for(bool full:{false,true}){
                std::size_t done{},at=start;std::optional<app::MessageId> actual;
                while(done<count){
                    const auto limit=std::min(app::kQwkSelectionQueuesMaximum,count-done);
                    if(full){const auto result=db.select_qwk_message(reader,*p.value,at,limit);check(result&&result.value->scanned>0&&result.value->scanned<=limit,"bounded fresh body block");done+=result.value->scanned;at=result.value->queue;if(result.value->selected)actual=result.value->selected->header.identity.id;}
                    else {const auto result=db.select_qwk_candidate(reader,*p.value,at,limit);check(result&&result.value->scanned>0&&result.value->scanned<=limit,"bounded metadata block");done+=result.value->scanned;at=result.value->queue;if(result.value->selected)actual=result.value->selected->message;}
                    if(actual||done==count)break;
                    at=(at+1)%count;
                }
                check(actual==expected&&done==visited&&at==expected_queue,"exact fair selection/wrap/final partial block without revisit");
            }
        }
    }
    const auto plan=db.begin_qwk_packet(reader,id(230));const auto block=db.select_qwk_candidate(reader,*plan.value,0,8);check(block&&!block.value->selected&&block.value->scanned==8,"first block stops before ninth eligible queue");
    // Expire a real monotonic deadline after the first bounded traversal,
    // before the ninth queue can allocate references or encode a message.
    auto deadline=std::chrono::steady_clock::time_point::max();unsigned observed{};
    const auto refs_before=scalar(file,"SELECT count(*) FROM sfx_qwk_message_reference"),continuation_before=scalar(file,"SELECT count(*) FROM sfx_qwk_continuation");
    app::QwkPacketMetadata meta;meta.board_name="Bounded deadline";meta.sysop="Synthetic";meta.caller_name="Bob";meta.created="11-12-2026,12:00:00";
    const auto expired=sfx::archive::prepare_qwk_download(db,reader,id(234),meta,[](std::int64_t){return sfx::archive::QwkRecordDate{"11-12-26","12:00","1794484800"};},[&]{return std::chrono::steady_clock::now()>=deadline;},[&](const auto& event){if(event.stage==sfx::archive::QwkPreparationStage::scanning&&++observed==8)deadline=std::chrono::steady_clock::now()-std::chrono::milliseconds(1);return true;});
    check(observed==8&&expired.result.error==sfx::archive::Error::cancelled&&!expired.progress&&expired.zip.empty()&&scalar(file,"SELECT count(*) FROM sfx_qwk_message_reference")==refs_before&&scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==continuation_before,"deadline between queue blocks cancels with no reference/delivery progress");
    check(!db.select_qwk_candidate(author,*plan.value,0,8)&&!db.select_qwk_message(reader,*plan.value,0,0)&&!db.select_qwk_candidate(reader,*plan.value,0,9),"bounded read rejects cross-owner and invalid work limits");
    scan(last_echo,false);check(!db.select_qwk_message(reader,*plan.value,8,1),"selection change between blocks refuses stale plan");scan(last_echo,true);
    const auto access_plan=db.begin_qwk_packet(reader,id(231));
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec(("UPDATE sfx_local_echoes SET reading_minimum=999,revision=revision+1 WHERE echo_id="+std::to_string(last_echo.value)).c_str(),"change only synthetic area access between blocks");}
    check(!db.select_qwk_message(reader,*access_plan.value,8,1),"fresh block refuses revoked area access");
    // Restore only the synthetic access fault, then delete the prefetched
    // message through normal Sysop storage authority. The next fresh read must
    // see its absence rather than exporting the previously selected identity.
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);c.exec(("UPDATE sfx_local_echoes SET reading_minimum=1,revision=revision+1 WHERE echo_id="+std::to_string(last_echo.value)).c_str(),"restore synthetic access for deletion check");}
    const auto deletion_plan=db.begin_qwk_packet(reader,id(232));const auto prefetched=db.select_qwk_candidate(reader,*deletion_plan.value,8,1);check(prefetched&&prefetched.value->selected,"deletion test captures metadata only");
    const app::MessageActor sysop{id(1),1,1,1};check(bool(db.delete_message(sysop,id(233),prefetched.value->selected->message)),"ordinary Sysop deletes only synthetic prefetched message");
    const auto gone=db.select_qwk_message(reader,*deletion_plan.value,8,1);check(gone&&!gone.value->selected,"fresh read honors deletion between prefetch and encoding");
}
