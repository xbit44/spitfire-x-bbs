// Real authoritative SQL guard over a successfully imported exact native
// original. Dependency rows below are synthetic, transaction-scoped fixtures;
// no deletion is invoked under them, no trigger/FK or production rule bypass.
{
    const auto guarded_original=produce(80);
    probe=native::FtnProcessedExpiry::inspect(installation,guarded_original,cancellation);
    const auto guarded_intent=prepare(guarded_original);probe.reset();
    check(bool(db.shutdown()),"stop writer before transaction-scoped guard fixtures");
    {
        internal::SqliteConnection guard_db(database,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        const auto hex=[](const app::Identifier128& value){return "X'"+native::ftn_native_artifact_name(value).substr(0,32)+"'";};
        const auto where=" WHERE ingress_id="+hex(guarded_original.ingress);
        const auto operation_sql=hex(id(71000)),other_sql=hex(id(71001));
        const auto check_guard=[&](const std::string& sql,const char* reason){
            guard_db.exec("BEGIN IMMEDIATE","begin synthetic dependency guard case");
            guard_db.exec(sql,"seed bounded synthetic dependency");
            check(!internal::validate_ftn_retention_expiry(guard_db.get(),guarded_intent,now),reason);
            guard_db.exec("ROLLBACK","leave successful original and provenance exact");
            check(internal::validate_ftn_retention_expiry(guard_db.get(),guarded_intent,now),"rolled-back synthetic dependency leaves original baseline eligible");
        };
        check(internal::validate_ftn_retention_expiry(guard_db.get(),guarded_intent,now),"complete native imported original passes baseline authoritative guard");
        check_guard("INSERT INTO sfx_ftn_reprocess_requests SELECT 1,"+operation_sql+",ingress_id,NULL,configuration_revision,volume,native_id,byte_count,sha256,zeroblob(32),successful_utc,0,0,'' FROM sfx_ftn_retained_originals"+where,"pending exact reprocess preserves original");
        check_guard("INSERT INTO sfx_ftn_tic_rejections SELECT 1,ingress_id,"+operation_sql+",configuration_revision,volume,native_id,byte_count,sha256,'test_guard',successful_utc,'','','' FROM sfx_ftn_retained_originals"+where+"; INSERT INTO sfx_ftn_tic_retries SELECT 1,"+other_sql+",ingress_id,NULL,configuration_revision,zeroblob(32),1,successful_utc,0,0,'' FROM sfx_ftn_retained_originals"+where,"pending exact TIC review retry preserves original");
        check_guard("INSERT INTO sfx_ftn_held_messages SELECT "+operation_sql+",ingress_id,99,configuration_revision,zeroblob(32),zeroblob(32),volume,native_id,byte_count,sha256,network,'2:100/1@test','2:100/2@test','2:100/1@test','Remote','Unresolved','','test_guard',1 FROM sfx_ftn_retained_originals"+where,"unresolved held message without toss progress preserves original");
        check_guard("INSERT INTO sfx_ftn_ingress SELECT "+other_sql+",zeroblob(32),network,link,proof,outbound,session_id,configuration_revision,'pending.tic',declared_size,wire_time,received_bytes,content_sha256,2,'',created_utc,updated_utc,external_session_authenticated FROM sfx_ftn_ingress WHERE receipt_id="+hex(guarded_original.ingress)+"; INSERT INTO sfx_ftn_tic_inspections SELECT "+other_sql+",configuration_revision,wire_name,'' FROM sfx_ftn_ingress WHERE receipt_id="+hex(guarded_original.ingress),"pending same-link TIC name dependency preserves successful original");
        check_guard("INSERT INTO sfx_ftn_outbound SELECT "+other_sql+",operation_id,0,'TEST','PEER',0,0,'pending.pkt',1,0,zeroblob(32),0,NULL,NULL,0,'',"+std::to_string(now)+","+std::to_string(now)+" FROM sfx_ftn_toss_progress"+where+" LIMIT 1","pending original forwarding job preserves source");
        check_guard("UPDATE sfx_ftn_serial_allocator SET recovery_required=1 WHERE singleton_id=1","serial recovery hold prohibits expiry");
        check_guard("UPDATE sfx_ftn_retention_clock SET high_water="+std::to_string(now+1),"clock rollback below persisted high water prohibits expiry");
        const auto external_job="INSERT INTO sfx_ftn_outbound SELECT "+other_sql+",operation_id,0,'TEST','PEER',0,0,'external.pkt',1,0,zeroblob(32),5,"+operation_sql+","+operation_sql+",0,'',"+std::to_string(now)+","+std::to_string(now)+" FROM sfx_ftn_toss_progress"+where+" LIMIT 1";
        const auto publication=[&](unsigned state){return "; INSERT INTO sfx_ftn_external_publications VALUES("+other_sql+","+operation_sql+",'FTN/External/test/Outbound/00640002.bsy','FTN/External/test/Outbound/00640002.flo','FTN/External/test/Copies/guard/external.pkt',"+std::to_string(state)+","+std::to_string(now)+")";};
        check_guard(external_job,"bare state5 cannot release original without durable publication");
        check_guard(external_job+publication(0),"uncommitted external publication preserves original");
        check_guard(external_job+publication(2),"uncertain external publication preserves original");
        guard_db.exec("BEGIN IMMEDIATE","bounded committed handoff guard case");guard_db.exec(external_job+publication(1),"synthetic exact committed external publication");
        check(internal::validate_ftn_retention_expiry(guard_db.get(),guarded_intent,now),"durable committed state5 handoff releases managed source dependency without inventing ACK");guard_db.exec("ROLLBACK","preserve original fixture");
        auto forged=guarded_intent;forged.dispatch=id(71002);
        check(!internal::validate_ftn_retention_expiry(guard_db.get(),forged,now),"different dispatch cannot use committed intent");
        forged=guarded_intent;forged.original.native.sha256[0]^=1;
        check(!internal::validate_ftn_retention_expiry(guard_db.get(),forged,now),"different full native identity cannot use committed intent");
        check(!internal::validate_ftn_retention_expiry(guard_db.get(),guarded_intent,now-1),"future successful time cannot become due under clock rollback");
    }
    check(bool(db.start())&&!processed.absent(processed_name(guarded_original)),"native original remains untouched after all guard cases");
}
