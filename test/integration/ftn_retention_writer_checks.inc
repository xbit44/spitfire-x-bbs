// Actual coordinator ordering/failure cases using the same private native
// originals and committed intents. SQL gates are test-only; no arbitrary native
// callback/factory is exposed by the product.
{
    namespace internal=sfx::adapters::storage::internal;
    using namespace std::chrono_literals;
    for(unsigned scenario=0;scenario<7;++scenario){
        original=produce(20+scenario);
        probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
        intent=prepare(original);removal=probe->seal(intent);probe.reset();
        check(bool(db.shutdown()),"repository stops before single coordinator fixture");
        struct Fault {unsigned commits{};bool deny_rollback{};};
        Fault fault{0,scenario==3};
        internal::SqlitePagerTelemetry telemetry;
        internal::SqliteWriterCoordinator writer;
        auto ws=s;ws.queue.dequeue_batch_window=0ms;ws.busy.sqlite_timeout=10ms;
        ws.busy.transaction_deadline=100ms;ws.busy.maximum_attempts=3;
        check(bool(writer.start(database,ws,storage::sfx_storage_migrations(),telemetry)),"actual dedicated writer starts against committed intent");
        if(scenario==4){
            internal::SqliteConnection holder(database,SQLITE_OPEN_READWRITE,10ms,false);
            holder.exec("BEGIN IMMEDIATE","hold real SQLite writer lock");
            const auto blocked=writer.submit_ftn_retention_expiry(removal).get();
            check(blocked.code==app::StorageErrorCode::busy&&writer.health().busy_retries>0&&removal->result().state==app::FtnExpiryNativeState::not_invoked,"bounded BEGIN retries never invoke native mutation while SQLite busy");
            holder.exec("ROLLBACK","release test lock");
            check(bool(writer.submit_ftn_retention_expiry(removal).get())&&removal->result().state==app::FtnExpiryNativeState::closed,"explicit dispatch after released BEGIN lock invokes once");
        }else{
            std::promise<void> entered,release;
            auto released=release.get_future().share();
            auto gate=writer.submit({"test-only bounded SQL gate",1,internal::WriteDurability::durable,
                [&](sqlite3* connection){
                    if(scenario==2||scenario==3)sqlite3_set_authorizer(connection,
                        [](void* raw,int action,const char* first,const char*,const char*,const char*)->int{
                            auto& f=*static_cast<Fault*>(raw);
                            if(action==SQLITE_TRANSACTION&&first){
                                if(std::string_view(first)=="COMMIT"&&++f.commits==2)return SQLITE_DENY;
                                if(std::string_view(first)=="ROLLBACK"&&f.deny_rollback)return SQLITE_DENY;
                            }return SQLITE_OK;
                        },&fault);
                    entered.set_value();released.wait();return SQLITE_OK;
                }});
            entered.get_future().get();
            std::future<app::StorageStatus> prior;
            const auto select=" WHERE ingress_id=X'"+native::ftn_native_artifact_name(original.ingress).substr(0,32)+"'";
            if(scenario==0||scenario==1){
                prior=writer.submit({"test-only prior deadline transaction",1024,internal::WriteDurability::durable,
                    [&,select](sqlite3* connection){
                        const auto sql="UPDATE sfx_ftn_retained_originals SET deadline=0,protection_revision=protection_revision+1"+select;
                        const int rc=sqlite3_exec(connection,sql.c_str(),nullptr,nullptr,nullptr);
                        return rc!=SQLITE_OK?rc:scenario==0?SQLITE_CONSTRAINT:SQLITE_OK;
                    }});
            }
            auto expiry=writer.submit_ftn_retention_expiry(removal);
            std::weak_ptr<app::FtnRetentionRemoval> queued_lifetime=removal;
            std::atomic_bool later_called{},later_closed{};
            auto later=writer.submit({"test-only later policy boundary",1,internal::WriteDurability::durable,
                [&](sqlite3* connection){
                    later_called=true;const auto owned=queued_lifetime.lock();later_closed=owned&&owned->result().state==app::FtnExpiryNativeState::closed;
                    return sqlite3_exec(connection,"UPDATE sfx_ftn_retention_clock SET high_water=high_water",nullptr,nullptr,nullptr);
                }});
            if(scenario==5)*cancellation=true;
            std::future<app::StorageStatus> drained;
            if(scenario==6){removal.reset();check(!queued_lifetime.expired(),"queued item owns sealed native lease after caller releases its last reference");drained=std::async(std::launch::async,[&]{return writer.shutdown();});}
            check(expiry.wait_for(0ms)==std::future_status::timeout&&later.wait_for(0ms)==std::future_status::timeout,"queued expiry and later write cannot pass unfinished earlier group");
            release.set_value();check(bool(gate.get()),"earlier real transaction commits before barrier");
            if(prior.valid())check(bool(prior.get())==(scenario==1),"prior deadline transaction settles as commit or rollback");
            const auto result=expiry.get();const auto later_result=later.get();
            if(scenario==0)check(bool(result)&&later_result&&later_called&&later_closed,"earlier rolled-back lengthening does not leak into expiry; later write observes checked native close");
            if(scenario==1)check(!result&&later_result&&later_called&&!later_closed&&removal->result().state==app::FtnExpiryNativeState::not_invoked,"earlier committed lengthening prevents native expiry");
            if(scenario==2)check(!result&&result.operation=="ftn_retention_native_outcome_uncertain"&&later_result&&later_called&&later_closed,"failed native completion COMMIT rolls back SQL without replay and later write runs only after ended transaction");
            if(scenario==3)check(!result&&result.operation=="ftn_retention_rollback_unresolved"&&!later_result&&!later_called&&removal->result().state==app::FtnExpiryNativeState::closed&&writer.health().state==app::StorageRuntimeState::recovery_required,"failed COMMIT plus failed ROLLBACK quarantines actual connection and refuses queued later write");
            if(scenario==5)check(!result&&later_result&&later_called&&!later_closed&&removal->result().state==app::FtnExpiryNativeState::not_invoked,"queued cancellation before dispatch retains original without native mutation");
            if(scenario==6)check(result&&later_result&&later_called&&drained.get()&&queued_lifetime.expired(),"joined shutdown drains exact queue-owned dispatch and releases native capability without detached lifetime");
            // Fault state must outlive the writer connection using its authorizer.
            check(bool(writer.shutdown())!=(scenario==3),"shutdown drains or preserves exact quarantine failure");
        }
        if(scenario==4)check(bool(writer.shutdown()),"busy case writer drains");
        removal.reset();*cancellation=false;
        check(bool(db.start()),"actual repository reopens after writer fixture");
        pending=db.pending_ftn_expiry(owner,original.ingress);
        if(scenario==2||scenario==3){
            check(pending&&pending.value->has_value(),"durable prior intent survives failed completion transaction and connection teardown");
            probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
            check(probe->observation()==native::FtnProcessedExpiry::Observation::missing&&bool(db.reconcile_ftn_expiry(owner,**pending.value,app::FtnExpiryOutcome::absent_uncertain)),"restart records absent-after-uncertain without replay or confirmed-deletion invention");probe.reset();
        }else if(scenario==1||scenario==5){
            check(!processed.absent(processed_name(original)),"blocked or cancelled queued dispatch preserves exact original");
        }
    }
#ifdef SFX_FTN_EXPIRY_CLOSE_QUALIFICATION
    original=produce(40);probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    intent=prepare(original);native::qualification_expiry_protect_next_close();
    removal=probe->seal(intent);probe.reset();
    check(bool(db.shutdown()),"repository stops for real native close-error qualification");
    internal::SqlitePagerTelemetry telemetry;
    internal::SqliteWriterCoordinator writer;
    auto ws=s;ws.queue.dequeue_batch_window=0ms;
    check(bool(writer.start(database,ws,storage::sfx_storage_migrations(),telemetry)),"native close-error writer starts");
    std::promise<void> entered,release;auto released=release.get_future().share();
    auto gate=writer.submit({"test-only close-error queue gate",1,internal::WriteDurability::durable,
        [&](sqlite3*){entered.set_value();released.wait();return SQLITE_OK;}});
    entered.get_future().get();
    auto expiry=writer.submit_ftn_retention_expiry(removal);
    std::atomic_bool later_called{};
    auto later=writer.submit({"must not run with unresolved delete-on-close",1,internal::WriteDurability::durable,
        [&](sqlite3*){later_called=true;return SQLITE_OK;}});
    release.set_value();check(bool(gate.get()),"close-error prior transaction settles");
    const auto failed=expiry.get();const auto refused=later.get();
    check(!failed&&failed.operation=="ftn_retention_native_close_unconfirmed"&&removal->result().state==app::FtnExpiryNativeState::close_unconfirmed&&removal->result().error!=0,"actual protected native handle makes CloseHandle fail, preserving actual error and uncertain outcome");
    check(!refused&&!later_called&&writer.health().state==app::StorageRuntimeState::recovery_required,"unconfirmed close fails later queued write without executing it");
    check(!writer.shutdown()&&writer.health().state==app::StorageRuntimeState::recovery_required,"coordinated teardown closes retained marked handle but preserves recovery-required result");
    removal.reset();check(processed.absent(processed_name(original)),"native object removed only after retained handle is closed during teardown");
    check(bool(db.start()),"repository reopens after isolated close-error teardown");
    pending=db.pending_ftn_expiry(owner,original.ingress);
    check(pending&&pending.value->has_value(),"close uncertainty retains earlier durable intent");
    probe=native::FtnProcessedExpiry::inspect(installation,original,cancellation);
    check(probe->observation()==native::FtnProcessedExpiry::Observation::missing&&bool(db.reconcile_ftn_expiry(owner,**pending.value,app::FtnExpiryOutcome::absent_uncertain)),"teardown disappearance stays uncertain, never invented confirmed deletion");probe.reset();
#endif
}
