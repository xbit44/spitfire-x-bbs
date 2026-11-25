namespace ftn_dispatch_detail {
namespace app=application;
void own(sqlite3* db,const app::Identifier128& id,std::int64_t kind,const app::Identifier128& operation){
    SqliteStatement old(db,"SELECT kind,owner_id FROM sfx_ftn_processing_owners WHERE receipt_id=?1","reconcile immutable receipt processing owner");old.bind(1,id);
    if(old.row()){if(old.int64(0)!=kind||ftn_work_detail::identifier(old,1).bytes!=operation.bytes)message_detail::conflict();return;}
    SqliteStatement add(db,"INSERT INTO sfx_ftn_processing_owners VALUES(?1,?2,?3)","claim one durable processing owner per receipt");add.bind(1,id);add.bind(2,kind);add.bind(3,operation);add.step();
}
bool message(sqlite3* db,const app::Identifier128& id,std::int64_t revision){
    const auto c=ftn_detail::config(db);if(c.revision!=revision)message_detail::conflict();
    SqliteStatement receipt(db,"SELECT network,link,wire_name,state FROM sfx_ftn_ingress WHERE receipt_id=?1","require retained message custody");receipt.bind(1,id);if(!receipt.row())message_detail::unavailable();
    if(!app::ftn_message_wire_name(receipt.text(2)))return false;
    if(receipt.int64(3)==3){
        SqliteStatement completed(db,"SELECT 1 FROM sfx_ftn_processing_owners WHERE receipt_id=?1 AND kind=0 AND owner_id=?1","completed immutable message custody is reconciliation only");completed.bind(1,id);return completed.row();
    }
    if(receipt.int64(3)!=1&&receipt.int64(3)!=2)return false;
    // A committed TIC reference remains a conflict even if the later current
    // configuration rejects that TIC. Never erase an observed ambiguity.
    SqliteStatement conflict(db,"SELECT 1 FROM sfx_ftn_ingress t JOIN sfx_ftn_tic_inspections i USING(receipt_id) WHERE t.network=?1 COLLATE NOCASE AND t.link=?2 COLLATE NOCASE AND i.payload_name=?3 COLLATE NOCASE LIMIT 1","fence exact same-custody admitted TIC reference");conflict.bind(1,receipt.text(0));conflict.bind(2,receipt.text(1));conflict.bind(3,receipt.text(2));if(conflict.row())return false;
    SqliteStatement pending(db,"SELECT 1 FROM sfx_ftn_ingress t WHERE t.network=?1 COLLATE NOCASE AND t.link=?2 COLLATE NOCASE AND t.state IN(1,2) AND lower(substr(t.wire_name,-4))='.tic' AND NOT EXISTS(SELECT 1 FROM sfx_ftn_tic_inspections i WHERE i.receipt_id=t.receipt_id AND i.configuration_revision=?3) AND NOT EXISTS(SELECT 1 FROM sfx_ftn_tic_rejections h WHERE h.ingress_id=t.receipt_id) LIMIT 1","inspect complete TICs; durable conclusive rejections stay latched across config changes");pending.bind(1,receipt.text(0));pending.bind(2,receipt.text(1));pending.bind(3,revision);if(pending.row())return false;
    own(db,id,0,id);return true;
}
void require_message(sqlite3* db,const app::Identifier128& id,std::int64_t revision){
    if(!message(db,id,revision))throw StorageFailure(app::StorageErrorCode::backpressure,"FTN receipt waiting for TIC inspection or ambiguity review",SQLITE_BUSY);
}
}
application::StorageStatus SqliteAccountRepository::index_ftn_tic(const application::MessageConfigurationActor& a,const application::Identifier128& id,std::int64_t revision,std::string_view payload,std::string_view reason){
    namespace app=application;
    if(!ftn_work_detail::owner(a)||!id.valid()||revision<1||(!payload.empty()&&!app::valid_ftn_leaf_name(payload))||!ftn_work_detail::reason(reason)||(payload.empty()&&reason.empty()))return invalid("validate current native TIC inspection");
    return impl_->write_result<bool>("retain native TIC inspection without secret text",4096,[a,id,revision,name=std::string(payload),why=std::string(reason)](sqlite3* db,std::optional<bool>& out){
        message_detail::config_authority(db,a);if(ftn_detail::config(db).revision!=revision)message_detail::conflict();
        SqliteStatement receipt(db,"SELECT 1 FROM sfx_ftn_ingress WHERE receipt_id=?1 AND state IN(1,2,3) AND lower(substr(wire_name,-4))='.tic' AND received_bytes=declared_size AND content_sha256 IS NOT NULL","require exact complete retained TIC receipt");receipt.bind(1,id);if(!receipt.row())message_detail::unavailable();
        SqliteStatement prior(db,"SELECT payload_name,reason FROM sfx_ftn_tic_inspections WHERE receipt_id=?1 AND configuration_revision=?2","reconcile native TIC inspection");prior.bind(1,id);prior.bind(2,revision);
        if(prior.row()){
            if(!prior.text(0).empty()){if(!name.empty()&&!ftn_detail::same(prior.text(0),name))message_detail::conflict();out=true;return SQLITE_OK;}
            if(name.empty()){out=true;return SQLITE_OK;}
            SqliteStatement revise(db,"UPDATE sfx_ftn_tic_inspections SET payload_name=?3,reason=?4 WHERE receipt_id=?1 AND configuration_revision=?2","retain positive admission after a formerly rejected inspection");revise.bind(1,id);revise.bind(2,revision);revise.bind(3,name);revise.bind(4,why);revise.step();
        }else{SqliteStatement add(db,"INSERT INTO sfx_ftn_tic_inspections VALUES(?1,?2,?3,?4)","record exact native TIC admission result");add.bind(1,id);add.bind(2,revision);add.bind(3,name);add.bind(4,why);add.step();}
        out=true;return SQLITE_OK;
    }).status;
}
application::StorageResult<bool> SqliteAccountRepository::claim_ftn_message(const application::MessageConfigurationActor& a,const application::Identifier128& id,std::int64_t revision){
    if(!ftn_work_detail::owner(a)||!id.valid()||revision<1)return read_failure<bool>(invalid("validate message custody processing claim"));
    return impl_->write_result<bool>("claim message receipt after current TIC inspection fence",4096,[a,id,revision](sqlite3* db,std::optional<bool>& out){message_detail::config_authority(db,a);out=ftn_dispatch_detail::message(db,id,revision);return SQLITE_OK;});
}
