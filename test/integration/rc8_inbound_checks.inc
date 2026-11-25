    // RC8 uses actual receipt-bound files. Publication is a no-overwrite rename
    // of the same object; Inbound contains only complete original filenames.
    for(const auto* folder:{L"Work",L"FTN/Inbound",L"FTN/Inbound/TEST",L"Staging"})ins::create_directory((root/folder).make_preferred(),descriptor.get());
    auto flat=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Inbound",owner);
    auto stage=native::FilePayloadDirectory::private_store(root/L"Staging",owner);
    auto legacy=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Inbound"/L"TEST",owner);
    request.configuration_revision=c.revision;request.link="PEER";request.network="TEST";request.proof=app::FtnIngressProof::inbound_cram;request.id=id(90);request.session=id(90);request.file={"20meg.zip",10,1700000900};
    app::FtnIngressReceipt published;
    {auto receive=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);receive.append("abc");receive.checkpoint();check(flat.absent("20meg.zip"),"partial transfer is absent from visible Inbound");}
    {auto receive=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);check(receive.offset()==3,"partial resume retains exact committed offset");receive.append("defghij");receive.finish();published=receive.receipt();check(std::filesystem::exists(root/L"FTN"/L"Inbound"/L"20meg.zip"),"completed file published under received name");}
    {auto received=flat.read("20meg.zip");check(received.identity().sha256==hash("abcdefghij"),"visible ZIP contains exact original bytes");}
    request.id=id(91);request.session=id(91);
    {auto reader=flat.read("20meg.zip");const auto original=reader.identity();std::string reason;
        try{auto collision=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);}catch(const std::exception& error){reason=error.what();}
        if(reason!="ftn_inbound_name_collision")std::cerr<<"DUPLICATE_WITH_READER reason="<<reason<<'\n';
        check(reason=="ftn_inbound_name_collision","fresh same-name arrival is a collision even while original has a live reader");
        check(reader.identity().file_id==original.file_id&&reader.identity().sha256==original.sha256,"conflict preserves existing native object and every original byte");
    }
    refused([&]{auto collision=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);},"repeated same-name collision cannot overwrite completed file");
    ins::Receipt installation;installation.root=root;installation.owner=owner;installation.program_count=12;installation.ftn_layout=true;{auto h=ins::open_object(root/L"FTN",true);installation.volume=installation.ftn_volume=ins::volume_serial(h.get());installation.ftn_directory_id=ins::file_id(h.get());}
    {auto exact=native::open_ftn_received(db,installation,published,[]{return true;});check(exact.name()=="20meg.zip"&&exact.identity().sha256==published.digest,"tosser resolver opens original filename with exact receipt identity");}
    request.id=id(92);request.session=id(92);request.file={"older.zip",10,1700000902};app::FtnIngressReceipt old;
    {auto receive=native::FtnNativeReceive::open(legacy,db,request,[]{return true;});receive.append("abcdefghij");receive.finish();old=receive.receipt();}
    request.id=id(93);request.session=id(93);request.file={"unfinished.zip",10,1700000903};
    {auto receive=native::FtnNativeReceive::open(legacy,db,request,[]{return true;});receive.append("abc");receive.checkpoint();receive.append("zz");}
    // Drop directory capabilities before the migration removes the now-empty
    // legacy directory. The source files' native identity must stay unchanged.
    {auto other=std::move(legacy);}
    native::migrate_ftn_inbound(db,installation,[]{return true;});
    check(!std::filesystem::exists(root/L"FTN"/L"Inbound"/L"TEST")&&std::filesystem::exists(root/L"FTN"/L"Inbound"/L"older.zip"),"legacy ID folder removed only after bound files migrate");
    {auto exact=native::open_ftn_received(db,installation,old,[]{return true;});check(exact.identity().sha256==old.digest,"migrated payload retains original object/hash");}
    auto migrated=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Partial"/L"TEST",owner);
    {auto resume=native::FtnNativeReceive::open(migrated,db,request,[]{return true;},&flat);check(resume.offset()==3,"legacy partial tail recovery uses durable checkpoint");resume.append("defghij");resume.finish();}
    native::migrate_ftn_inbound(db,installation,[]{return true;});check(flat.names().size()==3,"repeated migration leaves exact readable flat completed inventory");
    ins::create_directory(root/L"FTN"/L"Inbound"/L"TEST",descriptor.get());
    {auto old_directory=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Inbound"/L"TEST",owner);request.id=id(94);request.session=id(94);request.file={"20meg.zip",10,1700000904};auto receive=native::FtnNativeReceive::open(old_directory,db,request,[]{return true;});receive.append("0123456789");receive.finish();old=receive.receipt();}
    native::migrate_ftn_inbound(db,installation,[]{return true;});
    {const auto collision=db.ftn_ingress_receipt(host,old.id);check(collision&&collision.value->state==app::FtnIngressState::held,"legacy duplicate is held with explicit filename-conflict reason");auto exact=native::open_ftn_received(db,installation,*collision.value,[]{return true;});check(exact.identity().sha256==hash("0123456789"),"legacy duplicate bytes remain intact in FTN Conflicts");check(flat.read("20meg.zip").identity().sha256==published.digest,"conflicting migration never overwrites first original filename");}
    request.id=id(95);request.session=id(95);request.file={"interrupted.zip",10,1700000905};app::Identifier128 interrupted;
    {auto receive=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);interrupted=receive.receipt().id;receive.append("abcdefghij");receive.checkpoint();}
    {auto source=stage.read(native::ftn_native_artifact_name(interrupted),true);source.rename_to(flat,"interrupted.zip");}
    {auto receive=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);check(receive.offset()==10&&flat.absent("interrupted.zip"),"rename-before-SQL crash resumes only the exact checkpoint object");receive.finish();}
    request.id=id(96);request.session=id(96);request.file={"crash-conflict.zip",10,1700000906};app::FtnIngressReceipt conflict_crash;
    {auto receive=native::FtnNativeReceive::open(stage,db,request,[]{return true;});receive.append("0123456789");receive.finish();conflict_crash=receive.receipt();}
    {auto conflicts=native::FilePayloadDirectory::private_store(root/L"FTN"/L"Conflicts"/L"TEST",owner);auto source=stage.read(native::ftn_native_artifact_name(conflict_crash.id),true);source.rename_to(conflicts,source.name());}
    native::migrate_ftn_inbound(db,installation,[]{return true;});
    {const auto recovered=db.ftn_ingress_receipt(host,conflict_crash.id);check(recovered&&recovered.value->state==app::FtnIngressState::held,"conflict rename-before-HOLD crash reconciles from exact bound native object");}

    // A skipped collision is not a completed transfer/ACK and does not prevent
    // a later genuine transfer after the conflicting name is no longer present.
    request.id=id(97);request.session=id(97);request.file={"retry-after-skip.zip",10,1700000907};
    {auto existing=flat.create("retry-after-skip.zip",10);append(existing,"0123456789");}
    std::string skipped_reason;try{auto ignored=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);}catch(const std::exception& error){skipped_reason=error.what();}
    check(skipped_reason=="ftn_inbound_name_collision","fresh conflicting name is not a received/ACKed transfer");
    {auto existing=flat.read("retry-after-skip.zip",true);existing.remove_exact();}
    {auto retry=native::FtnNativeReceive::open(stage,db,request,[]{return true;},&flat);check(retry.offset()==0,"retry after skipped conflict still requires all peer bytes");retry.append("abcdefghij");check(retry.finish().sha256==hash("abcdefghij"),"genuine later transfer commits exact bytes after name is free");}
