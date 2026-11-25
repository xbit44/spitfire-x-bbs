namespace file_detail {
bool purge_valid(const app::FileIntakePurge& r){return r.operation.valid()&&(!r.staging||payload_valid(*r.staging))&&(!r.placed||payload_valid(*r.placed));}
std::array<std::uint8_t,32> purge_fingerprint(const app::FileIntakePurge& r){
    Fingerprint f("explicit-held-purge");f.text(stage_key(r.operation));f.number(r.staging.has_value());if(r.staging)f.payload(*r.staging);f.number(r.placed.has_value());if(r.placed)f.payload(*r.placed);return f.value();
}
app::FileIntakePurge intake_purge(sqlite3* db,app::Identifier128 id){
    SqliteStatement q(db,"SELECT stage_bytes,stage_sha256,stage_volume,stage_file_id,placed_bytes,placed_sha256,placed_volume,placed_file_id,completed FROM sfx_file_intake_purges WHERE operation_id=?1","read exact durable explicit purge");q.bind(1,id);if(!q.row())unavailable();
    app::FileIntakePurge result;result.operation=id;
    if(!q.text(2).empty())result.staging=app::FilePayloadIdentity{static_cast<std::uint64_t>(q.int64(0)),digest(q,1),q.text(2),q.text(3)};
    if(!q.text(6).empty())result.placed=app::FilePayloadIdentity{static_cast<std::uint64_t>(q.int64(4)),digest(q,5),q.text(6),q.text(7)};
    result.completed=q.boolean(8);return result;
}
bool purge_bound(const std::optional<app::FilePayloadIdentity>& observed,const std::optional<app::FileObjectIdentity>& bound){
    return !observed||(bound&&observed->volume==bound->volume&&observed->file_id==bound->file_id);
}
}
application::StorageResult<application::FileIntakePurge> SqliteAccountRepository::file_intake_purge(const application::FileConfigurationActor& a,application::Identifier128 id){
    if(!id.valid())return read_failure<application::FileIntakePurge>(invalid("validate explicit purge identity"));
    return impl_->readers_.read<application::FileIntakePurge>("read authorized private purge intent",[a,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return file_detail::intake_purge(db,id);});
}
application::StorageStatus SqliteAccountRepository::prepare_file_intake_purge(const application::FileConfigurationActor& a,const application::FileIntakePurge& r){
    if(!file_detail::purge_valid(r)||r.completed)return invalid("validate explicit held-upload purge preview");
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate purge clock");const auto fingerprint=file_detail::purge_fingerprint(r);
    return impl_->write("journal explicitly confirmed exact held objects before removal",8192,[a,r,now,fingerprint](sqlite3* db){
        message_detail::config_authority(db,a);
        SqliteStatement old(db,"SELECT request_fingerprint FROM sfx_file_intake_purges WHERE operation_id=?1","replay exact purge intent only");old.bind(1,r.operation);if(old.row())return file_detail::digest(old,0)==fingerprint?SQLITE_OK:SQLITE_ABORT;
        const auto intake=file_detail::intake(db,r.operation);
        if(intake.state!=application::FileIntakeState::held||intake.accepted_file.value||!file_detail::purge_bound(r.staging,intake.staged_object)||!file_detail::purge_bound(r.placed,intake.placed_object))return SQLITE_ABORT;
        if(r.staging&&file_detail::payload_valid(intake.payload)&&!file_detail::same_payload(*r.staging,intake.payload))return SQLITE_ABORT;
        if(r.placed&&!application::valid_file_name(intake.filename))return SQLITE_ABORT;
        const application::FilePayloadIdentity empty{};const auto& stage=r.staging?*r.staging:empty;const auto& placed=r.placed?*r.placed:empty;
        SqliteStatement q(db,"INSERT INTO sfx_file_intake_purges VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,0,?11)","retain exact native purge preview");
        q.bind(1,r.operation);q.bind(2,fingerprint);q.bind(3,static_cast<std::int64_t>(stage.bytes));q.bind(4,stage.sha256);q.bind(5,stage.volume);q.bind(6,stage.file_id);q.bind(7,static_cast<std::int64_t>(placed.bytes));q.bind(8,placed.sha256);q.bind(9,placed.volume);q.bind(10,placed.file_id);q.bind(11,now);q.step();
        file_detail::audit(db,a,r.operation,"upload.purge.prepared",{},intake.area,now);return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::finish_file_intake_purge(const application::FileConfigurationActor& a,const application::FileIntakePurge& r){
    if(!file_detail::purge_valid(r))return invalid("validate native confirmed purge absence");
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate purge completion clock");const auto fingerprint=file_detail::purge_fingerprint(r);
    return impl_->write("commit verified exact held-upload purge without scan or transfer credit",8192,[a,r,now,fingerprint](sqlite3* db){
        message_detail::config_authority(db,a);const auto existing=file_detail::intake_purge(db,r.operation);if(file_detail::purge_fingerprint(existing)!=fingerprint)return SQLITE_ABORT;if(existing.completed)return SQLITE_OK;
        const auto intake=file_detail::intake(db,r.operation);if(intake.state!=application::FileIntakeState::held||intake.accepted_file.value)return SQLITE_ABORT;
        SqliteStatement receipt(db,"UPDATE sfx_file_intake_purges SET completed=1 WHERE operation_id=?1 AND completed=0","retain explicit purge success");receipt.bind(1,r.operation);receipt.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        SqliteStatement terminal(db,"UPDATE sfx_file_intake SET state=4,detail='Explicit Sysop purge; exact held payload absence verified' WHERE operation_id=?1 AND state=5","release rejected upload reservation without false malware label");terminal.bind(1,r.operation);terminal.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        file_detail::audit(db,a,r.operation,"upload.purge.completed",{},intake.area,now);return SQLITE_OK;
    });
}
