// Exact native expiry on the actual joined engine processor. The injected
// ingress below is a synthetic host-provenance port, not a new peer claim.
{
    check(server.engine->pause_for_backup(),"retention fixture obtains actual joined engine hold");
    server.config.networks.front().keep_processed=false;server.save(61010);server.apply();
    auto retained_packet=packet;retained_packet.messages.front().text="AREA:ENGINE.TEST\r\1MSGID: 2:100/2 engine-retention-held\r\1CHRS: CP437 2\rKeep imported body after original expiry.\r";
    const auto retained_wire=app::encode_ftn_packet(retained_packet);check(bool(retained_wire),"retention hold packet encodes");
    app::FtnIngressReceipt retained;retained.id=id(61011);retained.session=id(61012);retained.identity[0]=0x61;retained.identity[1]=0x11;
    retained.network="TEST";retained.link="PEER";retained.proof=app::FtnIngressProof::inbound_cram;retained.configuration_revision=server.config.revision;retained.file={"retention.pkt",retained_wire->size(),1700000000};
    auto retention_incoming=native::FilePayloadDirectory::private_store(server.root/L"FTN"/L"Partial"/L"TEST",server.receipt.bindings);
    auto retention_completed=native::FilePayloadDirectory::private_store(server.root/L"FTN"/L"Inbound",server.receipt.bindings);
    {auto receiver=native::FtnNativeReceive::open(retention_incoming,*server.db,retained,live,&retention_completed);receiver.append(*retained_wire);receiver.finish();retained=receiver.receipt();}
    const auto tossed=native::toss_ftn_native_receipt(*server.db,server.config,server.receipt,retained,live);
    check(tossed.processed&&tossed.imported==1,"actual successful import creates newly due original while engine is held");
    auto processed_directory=native::FilePayloadDirectory::private_store(server.root/L"FTN"/L"Processed"/L"TEST",server.receipt.bindings);
    const auto name=retained.file.name;
    sleep_ms(1200);
    check(!processed_directory.absent(name)&&server.engine->snapshot().paused&&!server.engine->snapshot().processing,"Save cannot restart expiry while backup custody hold remains in force");
    server.engine->resume_after_backup();server.ready();
    // Linux first renames the original into its private expiry capsule. That
    // temporary source-name absence is not proof of deletion or SQL commit.
    await([&]{
        sfx::adapters::storage::internal::SqliteConnection observed(server.root/L"Data"/L"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
        sfx::adapters::storage::internal::SqliteStatement outcome(observed.get(),"SELECT count(*) FROM sfx_ftn_expiry_intents WHERE ingress_id=?1 AND outcome=1","wait for actual committed expiry");outcome.bind(1,retained.id);
        return outcome.row()&&outcome.int64(0)==1&&processed_directory.absent(name);
    },"existing joined engine processor commits confirmed exact expiry after hold release");
    sfx::adapters::storage::internal::SqliteConnection read(server.root/L"Data"/L"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
    sfx::adapters::storage::internal::SqliteStatement completed(read.get(),"SELECT count(*) FROM sfx_ftn_expiry_intents WHERE ingress_id=?1 AND outcome=1","real engine confirmed expiry receipt");completed.bind(1,retained.id);
    check(completed.row()&&completed.int64(0)==1,"actual processor persists one confirmed native expiry outcome");
    sfx::adapters::storage::internal::SqliteStatement imported(read.get(),"SELECT count(*) FROM sfx_ftn_import_receipts WHERE ingress_id=?1","retained imported provenance");imported.bind(1,retained.id);
    check(imported.row()&&imported.int64(0)==1,"automatic original expiry retains imported message provenance");
}
