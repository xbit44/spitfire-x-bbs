// Exercise the exact bounded function used by the joined Runtime processor.
{
    const auto protected_original=produce(60);
    c=*db.ftn_configuration(owner).value;c.networks.front().keep_processed=true;c.networks.front().retention_days=0;
    const auto protection_token=id(operation++).bytes;
    check(bool(db.preview_ftn_retention(owner,protection_token,c))&&bool(db.change_ftn_configuration(owner,protection_token,c)),"real Save protects existing originals Forever before processor page");
    c=*db.ftn_configuration(owner).value;c.networks.front().keep_processed=false;
    check(bool(db.change_ftn_configuration(owner,id(operation++).bytes,c)),"Off affects only later successful originals in processor fixture");
    const auto due_original=produce(61),pending_original=produce(62),changed_original=produce(63);
    probe=native::FtnProcessedExpiry::inspect(installation,pending_original,cancellation);const auto prior=prepare(pending_original);probe.reset();
    probe=native::FtnProcessedExpiry::inspect(installation,changed_original,cancellation);const auto replaced_intent=prepare(changed_original);probe.reset();
    const auto changed_name=processed_name(changed_original);
    std::vector<std::uint8_t> bytes;
    {auto original_file=processed.read(changed_name,true);bytes=original_file.read(0,65536);original_file.rename_to(bad,changed_name);}
    app::FilePayloadIdentity new_identity;
    {auto replacement=processed.create(changed_name,bytes.size());replacement.append(bytes.data(),bytes.size());new_identity=replacement.identity();}
    app::Identifier128 after;
    *cancellation=true;const auto cancelled_page=native::process_ftn_retention_page(db,installation,cancellation,after);
    check(cancelled_page.examined==0&&!after.valid()&&!processed.absent(processed_name(due_original)),"cancelled processor does not enumerate or mutate custody");*cancellation=false;
    {
        auto reader=processed.read(processed_name(due_original));
        for(unsigned i=0;i<8;++i){const auto page=native::process_ftn_retention_page(db,installation,cancellation,after);check(page.examined<=4,"actual processor page stays within four recorded originals");}
        check(reader.identity().file_id==due_original.native.file_id,"active reader keeps due original held across processor retries");
    }
    for(unsigned i=0;i<8&&!processed.absent(processed_name(due_original));++i)(void)native::process_ftn_retention_page(db,installation,cancellation,after);
    check(processed.absent(processed_name(due_original)),"bounded processor expires exact due original after reader releases");
    check(!processed.absent(processed_name(protected_original)),"future-only Off never removes previously protected Forever original");
    check(processed.absent(processed_name(pending_original)),"exact present old intent reconciles before fresh controlled dispatch");
    check(processed.read(changed_name).identity().file_id==new_identity.file_id&&bad.read(changed_name).identity().file_id==changed_original.native.file_id,"processor never adopts changed same-name object or touches displaced original");
    internal::SqliteConnection reader(database,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);
    internal::SqliteStatement status(reader.get(),"SELECT outcome FROM sfx_ftn_expiry_intents WHERE operation_id=?1","read exact prior processor reconciliation outcome");status.bind(1,prior.operation);
    check(status.row()&&status.int64(0)==static_cast<int>(app::FtnExpiryOutcome::present_reconciled),"processor records old present dispatch as reconciliation, not replay");
    internal::SqliteStatement changed(reader.get(),"SELECT outcome FROM sfx_ftn_expiry_intents WHERE operation_id=?1","read exact changed-object hold");changed.bind(1,replaced_intent.operation);
    check(changed.row()&&changed.int64(0)==static_cast<int>(app::FtnExpiryOutcome::changed_held),"processor records replaced object hold without fabricated success");
}
