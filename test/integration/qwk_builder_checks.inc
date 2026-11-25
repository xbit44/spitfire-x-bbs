// Production builder/archive/storage, synthetic actor/date/transfer DTO only.
// Actual ZIP contents inspected in memory; NOT NCM or installed wire evidence.
{
    namespace arc=sfx::archive;
    const app::QwkPacketMetadata meta{"IGNORED","Component BBS","Town","","Sysop","Bob","10-13-2026,14:00:00",{}};
    const arc::QwkDateFormatter date=[](std::int64_t){return arc::QwkRecordDate{"10-13-26","14:00",""};};
    const auto before_read=scalar(file,"SELECT last_read_sequence FROM sfx_message_counters");
    const auto before_progress=scalar(file,"SELECT count(*) FROM sfx_qwk_delivery_receipts");
    std::vector<arc::QwkPreparationEvent> preparation_events;
    const auto observe=[&](const arc::QwkPreparationEvent& event){preparation_events.push_back(event);return true;};
    auto made=arc::prepare_qwk_download(db,bob,id(400),meta,date,{},observe);
    if(!made.result.ok())std::cerr<<"Builder failure: "<<made.result.detail<<'\n';
    check(made.result.ok()&&!made.zip.empty()&&made.progress&&made.progress->delivered.size()==1,"actual complete prepared ZIP obeys combined count1");
    check(preparation_events.size()==2&&preparation_events.front().stage==arc::QwkPreparationStage::scanning&&preparation_events.front().echo.value==0&&preparation_events.back().stage==arc::QwkPreparationStage::packing,"actual first private queue reports scan once then packing without per-message output");
    const auto rejected_output=arc::prepare_qwk_download(db,bob,id(409),meta,date,{},[](const auto&){return false;});
    check(rejected_output.result.error==arc::Error::cancelled&&rejected_output.zip.empty()&&!rejected_output.progress,"progress output failure cancels preparation without delivery proof");
    const auto files=download_members(made.zip);check(files.count("CONTROL.DAT")&&files.count("DOOR.ID")&&files.count("MESSAGES.DAT")&&files.count("HEADERS.DAT"),"actual mandatory/full-header packet members");
    check(files.at("CONTROL.DAT").find("X-BIT")!=std::string::npos&&files.at("CONTROL.DAT").find("IGNORED")==std::string::npos,"board/routing metadata derives from current storage, not passed guessed ID");
    const auto& data=files.at("MESSAGES.DAT");check(data.size()>=384&&data[128]=='+'&&static_cast<unsigned char>(data[251])==0,"default rotation starts with already-read All-received owner mail, separate mail conference0");
    std::uint64_t expanded{};for(const auto& f:files)expanded+=f.second.size();
    check(made.progress->expanded_bytes==expanded&&made.progress->compressed_bytes==made.zip.size()&&made.progress->more&&made.notice==app::kQwkMoreNotice,"actual ZIP/complete metadata bytes and remaining-message notice");
    check(scalar(file,"SELECT last_read_sequence FROM sfx_message_counters")==before_read&&scalar(file,"SELECT count(*) FROM sfx_qwk_delivery_receipts")==before_progress,"actual preparation has no recipient Read/delivery progress effects");
    check(files.at("HEADERS.DAT").find("@sfx.invalid>")!=std::string::npos&&app::qwk_message_identifier("x-bit",1)=="<1.X-BIT@sfx.invalid>","stable full identifier convention independent of friendly numbering");
    auto cancelled=arc::prepare_qwk_download(db,bob,id(401),meta,date,[]{return true;});check(cancelled.result.error==arc::Error::cancelled&&cancelled.zip.empty()&&!cancelled.progress,"cancel before preparation has no staged packet or delivery proof");
    // Explicit synthetic completion DTO applies an already component-qualified
    // writer, not a fabricated claim that these bytes went over a real wire.
    check(db.commit_qwk_delivery(bob,*made.progress).succeeded(),"synthetic completed-transfer DTO advances actual prepared first segment");
    auto next=arc::prepare_qwk_download(db,bob,id(402),meta,date);check(next.result.ok()&&next.progress&&next.progress->delivered[0].echo.value==2,"fair saved rotation gives public echo next despite busy received mailbox");
    const auto next_files=download_members(next.zip);check(static_cast<unsigned char>(next_files.at("MESSAGES.DAT")[251])==2,"permanent public mapping in actual header, not caller menu position");
    auto prefs_now=db.qwk_preferences(bob);auto none=prefs_now.value->preferences;none.private_mail=app::QwkPrivateMode::none;
    check(db.save_qwk_preferences(bob,prefs_now.value->revision,none).succeeded(),"explicit mail None for controlled empty case");
    for(const std::int64_t echo:{1,2}){for(const auto column:{app::MessageScanColumn::unread,app::MessageScanColumn::to_you}){auto scan_now=db.message_scan_preference(bob,{echo});check(db.set_message_scan_preference(bob,{echo},scan_now.value->revision,column,false).succeeded(),"disable own shared scan columns for empty fixture");}}
    preparation_events.clear();auto empty=arc::prepare_qwk_download(db,bob,id(403),meta,date,{},observe);check(empty.result.ok()&&empty.zip.empty()&&!empty.progress&&empty.notice==app::kQwkEmptyNotice,"actual empty selection creates no control-only ZIP/progress receipt");
    check(preparation_events.empty(),"disabled areas and private None disclose no scan names or false packing stage");
    auto forged_actor=bob;forged_actor.session=alice.session;auto denied=arc::prepare_qwk_download(db,forged_actor,id(404),meta,date);check(!denied.result.ok()&&denied.zip.empty()&&denied.notice!=app::kQwkEmptyNotice,"authority failure never masquerades as empty selection");
    auto scan_before=db.message_scan_preference(bob,{2});check(db.set_message_scan_preference(bob,{2},scan_before.value->revision,app::MessageScanColumn::unread,true).succeeded(),"enable own shared scan for original-source comparison");
    app::QwkPointerChange pointer;pointer.operation=id(405);pointer.echo={2};pointer.mode=app::QwkPointerMode::caught_up;auto catchup=db.preview_qwk_public_pointers(bob,pointer);check(catchup&&db.change_qwk_public_pointers(bob,*catchup.value).succeeded(),"finite explicit boundary before long-source fixture");
    const std::string long_source=std::string(100,'x')+"\r\n";auto context=db.begin_message(alice,chat);
    auto long_post=db.commit_source_message({alice,id(406),*context.value,"Long source",{app::MessageSourceFormat::plain,1,long_source}});check(static_cast<bool>(long_post),"actual immutable uploaded plain source fixture");
    auto preferences=db.qwk_preferences(bob);auto off=preferences.value->preferences;off.word_wrap=false;check(db.save_qwk_preferences(bob,preferences.value->revision,off).succeeded(),"explicit wrap Off, not shipping-default decision");
    auto off_packet=arc::prepare_qwk_download(db,bob,id(407),meta,date);check(off_packet.result.ok()&&off_packet.progress&&off_packet.progress->delivered[0].message==long_post.value->id,"actual long original selected without canvas-width rewrite");
    const auto off_members=download_members(off_packet.zip);check(off_members.at("MESSAGES.DAT").substr(256,100)==std::string(100,'x')&&off_members.at("MESSAGES.DAT")[356]==static_cast<char>(0xe3),"wrap Off preserves original100-cell prose line with normalized QWK newline");
    preferences=db.qwk_preferences(bob);auto on=preferences.value->preferences;on.word_wrap=true;check(db.save_qwk_preferences(bob,preferences.value->revision,on).succeeded(),"explicit wrap On comparison");
    auto on_packet=arc::prepare_qwk_download(db,bob,id(408),meta,date);const auto on_members=download_members(on_packet.zip);check(on_packet.result.ok()&&on_members.at("MESSAGES.DAT")[336]==static_cast<char>(0xe3),"wrap On changes exported long prose at80 cells only");
    auto original=db.quote_source(bob,long_post.value->id);check(original&&original.value->original_source->bytes==long_source,"both export choices preserve exact original stored source");
}
