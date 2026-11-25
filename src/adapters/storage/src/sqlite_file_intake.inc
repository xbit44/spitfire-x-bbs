namespace file_detail {
std::string stage_key(app::Identifier128 id) {
    constexpr char hex[]="0123456789abcdef";std::string result;
    for(auto byte:id.bytes){result+=hex[byte>>4];result+=hex[byte&15];}return result;
}
std::string detection_text(const app::FileScanResult& scan) {
    if(static_cast<unsigned>(scan.verdict)>5 || scan.detail.size()>512 || app::sanitize_file_text(scan.detail,512,false)!=scan.detail || scan.detections.size()>32)conflict();
    std::string result;
    for(const auto& name:scan.detections){
        if(name.empty() || name.size()>128 || app::sanitize_file_text(name,128,false)!=name || name.find('\n')!=std::string::npos)conflict();
        if(!result.empty())result+='\n';
        result+=name;
    }
    return result;
}
app::FileIntake intake(sqlite3* db,app::Identifier128 operation) {
    SqliteStatement q(db,"SELECT account_id,session_id,stage_key,filename,intended_area_id,area_revision,maximum_bytes,private_submission,scan_required,transfer_complete,received_bytes,content_sha256,native_volume,native_file_id,scan_verdict,detections,detail,state,created_utc,file_id,bbs_local_date,public_uploader,short_description,long_description,sysop_only,placement_area_id,placement_revision,filename_key FROM sfx_file_intake WHERE operation_id=?1","read private native intake journal");q.bind(1,operation);if(!q.row())unavailable();
    app::FileIntake r;r.operation=operation;r.account=q.int64(0);r.session=q.identifier(1);r.stage_key=q.text(2);r.filename=q.text(3);r.area={q.int64(4)};r.area_revision=q.int64(5);r.maximum_bytes=static_cast<std::uint64_t>(q.int64(6));r.private_submission=q.boolean(7);r.scan_required=q.boolean(8);r.transfer_complete=q.boolean(9);
    r.payload={static_cast<std::uint64_t>(q.int64(10)),digest(q,11),q.text(12),q.text(13)};r.scan.verdict=static_cast<app::FileScanVerdict>(q.int64(14));
    auto names=q.text(15);while(!names.empty()){const auto end=names.find('\n');r.scan.detections.push_back(names.substr(0,end));if(end==std::string::npos)break;names.erase(0,end+1);}
    r.scan.detail=q.text(16);r.state=static_cast<app::FileIntakeState>(q.int64(17));r.created_utc=q.int64(18);r.accepted_file={q.is_null(19)?0:q.int64(19)};r.bbs_local_date=q.text(20);r.public_uploader=q.text(21);r.descriptions={q.text(22),q.text(23)};r.sysop_only=q.boolean(24);
    r.placement_area={q.int64(25)};r.placement_revision=q.int64(26);
    const auto storage_key=q.text(27);if(!storage_key.empty()&&storage_key.front()==':'){
        if(!r.private_submission||storage_key!=":"+r.stage_key)corrupt();
        r.placement_name=r.stage_key;
    }
    SqliteStatement objects(db,"SELECT stage_volume,stage_file_id,placed_volume,placed_file_id,staging_clean FROM sfx_file_intake_objects WHERE operation_id=?1","read durable native receive bindings");objects.bind(1,operation);
    if(objects.row()){r.staged_object=app::FileObjectIdentity{objects.text(0),objects.text(1)};if(!objects.text(2).empty())r.placed_object=app::FileObjectIdentity{objects.text(2),objects.text(3)};r.staging_clean=objects.boolean(4);}
    if(r.stage_key!=stage_key(operation) || r.area_revision<1 || r.placement_area.value<1 || r.placement_revision<1 || r.maximum_bytes<1 || r.maximum_bytes>app::kMaximumCallerFileBytes || r.account<1 || !r.session.valid())corrupt();
    return r;
}
void intake_actor(sqlite3* db,const app::FileActor& a,const app::FileIntake& r) {
    authority(db,a);if(a.account!=r.account || a.session!=r.session)unavailable();
}
app::FileArea intake_destination(sqlite3* db,const app::FileArea& intended) {
    if(!intended.sysop_review)return intended;
    SqliteStatement q(db,"SELECT area_id FROM sfx_file_areas WHERE kind=1","resolve built-in review destination");if(!q.row())unavailable();
    return area(db,{q.int64(0)}).area;
}
bool intake_policy_current(sqlite3* db,const app::FileActor& a,const app::FileIntake& r,const app::FileArea& intended) {
    const auto actual=intake_destination(db,intended);
    return intended.revision==r.area_revision && actual.id==r.placement_area && actual.revision==r.placement_revision &&
        app::file_area_uploadable(group(db,intended.group),intended,authority(db,a),r.private_submission) &&
        actual.enabled && (actual.permissions.uploads_enabled||authority(db,a).sysop()) && group(db,actual.group).enabled;
}
app::FileArea intake_area(sqlite3* db,const app::FileActor& a,const app::FileIntake& r) {
    const auto ar=area(db,r.area).area;if(!intake_policy_current(db,a,r,ar))unavailable();return ar;
}
bool intake_acceptable(sqlite3* db,const app::FileIntake& r,const app::FileArea& ar) {
    const auto p=policy(db);const bool scan=r.scan_required || p.scan_uploads;const auto actual=area(db,r.placement_area).area;
    return r.transfer_complete && payload_valid(r.payload) && !reupload_blocked(db,r.payload) && r.payload.bytes<=r.maximum_bytes && r.payload.bytes<=app::file_upload_limit(p,ar) && r.payload.bytes<=app::file_upload_limit(p,actual) &&
        (scan?r.scan.verdict==app::FileScanVerdict::clean:(r.scan.verdict==app::FileScanVerdict::not_scanned || r.scan.verdict==app::FileScanVerdict::clean));
}
bool scan_same(const app::FileScanResult& a,const app::FileScanResult& b){return a.verdict==b.verdict && a.detections==b.detections && a.detail==b.detail;}
} // namespace file_detail

application::StorageResult<application::FileIntake> SqliteAccountRepository::file_intake(const application::FileConfigurationActor& a,application::Identifier128 id) {
    if(!id.valid())return read_failure<application::FileIntake>(invalid("validate private intake identity"));
    return impl_->readers_.read<application::FileIntake>("read private intake for authorized reconciliation",[a,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return file_detail::intake(db,id);});
}
application::StorageStatus SqliteAccountRepository::bind_file_intake_object(const application::FileActor& a,application::Identifier128 id,bool placement,const application::FileObjectIdentity& native) {
    if(!id.valid()||native.volume.empty()||native.volume.size()>128||native.file_id.empty()||native.file_id.size()>128)return invalid("validate native intake object binding");
    return impl_->write("checkpoint native intake object before any bytes or placement",4096,[a,id,placement,native](sqlite3* db){
        const auto r=file_detail::intake(db,id);file_detail::intake_actor(db,a,r);
        const auto& old=placement?r.placed_object:r.staged_object;
        if(old)return old->volume==native.volume&&old->file_id==native.file_id?SQLITE_OK:SQLITE_ABORT;
        if(placement){
            if(r.state!=application::FileIntakeState::prepared||!r.staged_object)return SQLITE_ABORT;
            SqliteStatement q(db,"UPDATE sfx_file_intake_objects SET placed_volume=?1,placed_file_id=?2 WHERE operation_id=?3 AND placed_volume=''","checkpoint new destination before copy/rename");q.bind(1,native.volume);q.bind(2,native.file_id);q.bind(3,id);q.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        }else{
            if(r.state!=application::FileIntakeState::receiving)return SQLITE_ABORT;
            SqliteStatement q(db,"INSERT INTO sfx_file_intake_objects VALUES(?1,?2,?3,'','',0)","checkpoint fresh staging object before first write");q.bind(1,id);q.bind(2,native.volume);q.bind(3,native.file_id);q.step();
        }
        return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::finish_file_intake_cleanup(const application::FileConfigurationActor& a,application::Identifier128 id,const application::FileObjectIdentity& native) {
    if(!id.valid())return invalid("validate native intake cleanup identity");
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate intake cleanup clock");
    return impl_->write("record verified absent redundant accepted staging",4096,[a,id,native,now](sqlite3* db){
        message_detail::config_authority(db,a);const auto r=file_detail::intake(db,id);
        if(r.state!=application::FileIntakeState::accepted||!r.staged_object||!r.placed_object||r.staged_object->volume!=native.volume||r.staged_object->file_id!=native.file_id)return SQLITE_ABORT;
        if(r.staging_clean)return SQLITE_OK;
        SqliteStatement q(db,"UPDATE sfx_file_intake_objects SET staging_clean=1 WHERE operation_id=?1","retain successful redundant-staging reconciliation");q.bind(1,id);q.step();
        file_detail::audit(db,a,id,"upload.staging.cleaned",r.accepted_file,r.area,now);return SQLITE_OK;
    });
}

application::StorageResult<application::FileIntake> SqliteAccountRepository::begin_file_intake(const application::FileActor& a,const application::FileIntakeStart& r) {
    if(!r.operation.valid() || r.area.value<1 || !application::valid_iso_civil_date(r.bbs_local_date))return read_failure<application::FileIntake>(invalid("validate upload start"));
    file_detail::Fingerprint f("intake");f.number(a.account);f.text(file_detail::stage_key(a.session));f.number(r.area.value);f.number(r.private_submission);f.text(r.bbs_local_date);const auto fingerprint=f.value();const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::FileIntake>(invalid("validate upload clock"));
    return impl_->write_result<application::FileIntake>("journal upload before any incoming payload",4096,[a,r,fingerprint,now](sqlite3* db,auto& output){
        const auto auth=file_detail::authority(db,a);
        SqliteStatement old(db,"SELECT request_fingerprint FROM sfx_file_intake WHERE operation_id=?1","reconcile upload start");old.bind(1,r.operation);
        if(old.row()){if(file_detail::digest(old,0)!=fingerprint)return SQLITE_ABORT;auto record=file_detail::intake(db,r.operation);file_detail::intake_actor(db,a,record);output=std::move(record);return SQLITE_OK;}
        const auto ar=file_detail::area(db,r.area).area;
        if(!application::file_area_uploadable(file_detail::group(db,ar.group),ar,auth,r.private_submission))return SQLITE_NOTFOUND;
        const auto actual=file_detail::intake_destination(db,ar);
        if(!actual.enabled || (!actual.permissions.uploads_enabled&&!auth.sysop()) || !file_detail::group(db,actual.group).enabled)return SQLITE_NOTFOUND;
        SqliteStatement pending(db,"SELECT count(*),coalesce(sum(account_id=?1),0) FROM sfx_file_intake WHERE state IN(0,1,2,5)","bound unresolved durable intake records");pending.bind(1,a.account);if(!pending.row() || pending.int64(0)>=4096 || pending.int64(1)>=256)return SQLITE_FULL;
        SqliteStatement caller(db,"SELECT display_name FROM sfx_accounts WHERE account_id=?1","retain public uploader only");caller.bind(1,a.account);if(!caller.row())return SQLITE_NOTFOUND;
        const auto p=file_detail::policy(db);
        // The operation ID identifies PRIVATE staging/journal state only.
        // Final area payloads use the original basename. Existing ':' rows
        // remain readable/recoverable by intake()/managed() for old installs.
        SqliteStatement add(db,"INSERT INTO sfx_file_intake VALUES(?1,?2,?3,?4,?5,'',?16,?6,?7,?8,?9,?10,0,0,zeroblob(32),'','',4,'','Receive not complete',0,?11,NULL,?12,?13,'','',0,?14,?15)","retain private receive operation");
        add.bind(1,r.operation);add.bind(2,fingerprint);add.bind(3,a.account);add.bind(4,a.session);add.bind(5,file_detail::stage_key(r.operation));add.bind(6,r.area.value);add.bind(7,ar.revision);add.bind(8,static_cast<std::int64_t>(std::min(application::file_upload_limit(p,ar),application::file_upload_limit(p,actual))));add.bind(9,r.private_submission);add.bind(10,p.scan_uploads);add.bind(11,now);add.bind(12,r.bbs_local_date);add.bind(13,caller.text(0));add.bind(14,actual.id.value);add.bind(15,actual.revision);add.bind(16,std::string{});add.step();
        file_detail::audit(db,{false,a},r.operation,"upload.started",{},r.area,now);output=file_detail::intake(db,r.operation);return SQLITE_OK;
    });
}
application::StorageResult<application::FileIntake> SqliteAccountRepository::record_file_intake(const application::FileActor& a,const application::FileIntakeReceived& r) {
    if(!r.operation.valid() || !application::valid_file_name(r.filename) || !file_detail::payload_valid(r.payload))return read_failure<application::FileIntake>(invalid("validate received upload identity"));
    std::string detections;try{detections=file_detail::detection_text(r.scan);}catch(...){return read_failure<application::FileIntake>(invalid("validate bounded scan report"));}
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::FileIntake>(invalid("validate scan clock"));
    return impl_->write_result<application::FileIntake>("retain actual receive and scan outcome before acceptance",8192,[a,r,detections,now](sqlite3* db,auto& output){
        auto old=file_detail::intake(db,r.operation);file_detail::intake_actor(db,a,old);
        if(!old.staged_object||old.staged_object->volume!=r.payload.volume||old.staged_object->file_id!=r.payload.file_id)return SQLITE_ABORT;
        if(old.state!=application::FileIntakeState::receiving){
            if(old.filename!=r.filename || !file_detail::same_payload(old.payload,r.payload) || !file_detail::scan_same(old.scan,r.scan) || old.transfer_complete!=r.transfer_complete)return SQLITE_ABORT;
            output=old;return SQLITE_OK;
        }
        old.filename=r.filename;old.payload=r.payload;old.scan=r.scan;old.transfer_complete=r.transfer_complete;old.scan_required=old.scan_required||file_detail::policy(db).scan_uploads;
        const auto ar=file_detail::area(db,old.area).area;
        const bool ready=file_detail::intake_policy_current(db,a,old,ar) && file_detail::intake_acceptable(db,old,ar);
        SqliteStatement q(db,"UPDATE sfx_file_intake SET filename=?1,filename_key=?2,received_bytes=?3,content_sha256=?4,native_volume=?5,native_file_id=?6,scan_verdict=?7,detections=?8,detail=?9,transfer_complete=?10,state=?11,scan_required=?13 WHERE operation_id=?12 AND state=0","retain scan/receive result without publishing or credit");
        q.bind(1,r.filename);q.bind(2,old.placement_name.empty()?file_detail::key(r.filename):":"+old.placement_name);q.bind(3,static_cast<std::int64_t>(r.payload.bytes));q.bind(4,r.payload.sha256);q.bind(5,r.payload.volume);q.bind(6,r.payload.file_id);q.bind(7,static_cast<std::int64_t>(r.scan.verdict));q.bind(8,detections);q.bind(9,r.scan.detail);q.bind(10,r.transfer_complete);q.bind(11,static_cast<std::int64_t>(ready?1:5));q.bind(12,r.operation);q.bind(13,old.scan_required);q.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        file_detail::audit(db,{false,a},r.operation,ready?"upload.ready":"upload.held",{},old.area,now);output=file_detail::intake(db,r.operation);return SQLITE_OK;
    });
}
application::StorageResult<application::FileIntake> SqliteAccountRepository::prepare_file_intake(const application::FileActor& a,application::Identifier128 operation,const application::FileDescriptions& descriptions,bool sysop_only) {
    if(!operation.valid() || !application::valid_file_descriptions(descriptions))return read_failure<application::FileIntake>(invalid("validate upload metadata"));
    return impl_->write_result<application::FileIntake>("reserve accepted filename and metadata before native placement",descriptions.long_text.size()+4096,[a,operation,descriptions,sysop_only](sqlite3* db,auto& output){
        const auto r=file_detail::intake(db,operation);file_detail::intake_actor(db,a,r);const auto ar=file_detail::intake_area(db,a,r);
        if(!file_detail::intake_acceptable(db,r,ar))return SQLITE_ABORT;
        if(r.state==application::FileIntakeState::prepared){if(r.descriptions.short_text!=descriptions.short_text || r.descriptions.long_text!=descriptions.long_text || r.sysop_only!=sysop_only)return SQLITE_ABORT;output=r;return SQLITE_OK;}
        if(r.state!=application::FileIntakeState::ready)return SQLITE_ABORT;
        const auto& native_name=application::file_intake_payload_name(r);
        if(file_detail::name_reserved(db,r.placement_area,native_name,operation,true))return SQLITE_ABORT;
        SqliteStatement exists(db,"SELECT 1 FROM sfx_file_catalog WHERE area_id=?1 AND (filename_key=?2 OR filename_key=':'||?2) AND catalog_state<>1","refuse existing physical catalog filename");exists.bind(1,r.placement_area.value);exists.bind(2,file_detail::key(native_name));if(exists.row())return SQLITE_CONSTRAINT;
        SqliteStatement q(db,"UPDATE sfx_file_intake SET state=2,short_description=?1,long_description=?2,sysop_only=?3 WHERE operation_id=?4 AND state=1","reserve exact placement intent");q.bind(1,descriptions.short_text);q.bind(2,descriptions.long_text);q.bind(3,sysop_only);q.bind(4,operation);q.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        output=file_detail::intake(db,operation);return SQLITE_OK;
    });
}
application::StorageResult<application::FileEntry> SqliteAccountRepository::commit_file_intake(const application::FileActor& a,application::Identifier128 operation,const application::FilePayloadIdentity& placed) {
    if(!operation.valid() || !file_detail::payload_valid(placed))return read_failure<application::FileEntry>(invalid("validate placed upload identity"));
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::FileEntry>(invalid("validate upload commit clock"));
    return impl_->write_result<application::FileEntry>("commit validated upload publication and accounting once",24576,[a,operation,placed,now](sqlite3* db,auto& output){
        const auto r=file_detail::intake(db,operation);file_detail::intake_actor(db,a,r);
        if(r.state==application::FileIntakeState::accepted){const auto old=file_detail::entry(db,r.accepted_file);if(!file_detail::same_payload(old.payload,placed))return SQLITE_ABORT;output=old.file;return SQLITE_OK;}
        if(r.state!=application::FileIntakeState::prepared || !r.placed_object || r.placed_object->volume!=placed.volume || r.placed_object->file_id!=placed.file_id || placed.bytes!=r.payload.bytes || placed.sha256!=r.payload.sha256)return SQLITE_ABORT;
        const auto ar=file_detail::intake_area(db,a,r);if(!file_detail::intake_acceptable(db,r,ar) || !application::valid_file_descriptions(r.descriptions))return SQLITE_ABORT;
        SqliteStatement add(db,"INSERT INTO sfx_file_catalog(area_id,filename,filename_key,short_description,long_description,public_uploader,uploader_account_id,byte_count,content_sha256,native_volume,native_file_id,original_upload_utc,published_utc,publication_sequence,download_count,origin,catalog_state,sysop_only,revision) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,0,0,0,0,?14,1)","publish exact placed caller payload");
        add.bind(1,r.placement_area.value);add.bind(2,r.filename);add.bind(3,r.placement_name.empty()?file_detail::key(r.filename):":"+r.placement_name);add.bind(4,r.descriptions.short_text);add.bind(5,r.descriptions.long_text);add.bind(6,r.public_uploader);add.bind(7,a.account);add.bind(8,static_cast<std::int64_t>(placed.bytes));add.bind(9,placed.sha256);add.bind(10,placed.volume);add.bind(11,placed.file_id);add.bind(12,r.created_utc);add.bind(13,now);add.bind(14,r.sysop_only);add.step();const application::FileId id{sqlite3_last_insert_rowid(db)};
        SqliteStatement publication(db,"INSERT INTO sfx_file_publications(file_id,area_id,published_utc) VALUES(?1,?2,?3)","record upload availability");publication.bind(1,id.value);publication.bind(2,r.placement_area.value);publication.bind(3,now);publication.step();const std::int64_t sequence=sqlite3_last_insert_rowid(db);
        SqliteStatement seq(db,"UPDATE sfx_file_catalog SET publication_sequence=?1 WHERE file_id=?2","bind upload publication");seq.bind(1,sequence);seq.bind(2,id.value);seq.step();
        const auto stats=file_detail::statistics(db,a,file_detail::authority(db,a),r.bbs_local_date);const bool ratio=ar.ratio_uploads && !stats.ratio_exempt;
        SqliteStatement count(db,"INSERT INTO sfx_file_statistics VALUES(?1,1,0,?2,0,?3,0) ON CONFLICT(account_id) DO UPDATE SET upload_count=upload_count+1,upload_bytes=upload_bytes+excluded.upload_bytes,eligible_uploads=eligible_uploads+excluded.eligible_uploads","credit only committed caller upload");count.bind(1,a.account);count.bind(2,static_cast<std::int64_t>(placed.bytes));count.bind(3,static_cast<std::int64_t>(ratio?1:0));count.step();
        file_detail::Fingerprint fingerprint("accepted-upload");fingerprint.number(a.account);fingerprint.number(id.value);fingerprint.payload(placed);
        SqliteStatement transfer(db,"INSERT INTO sfx_file_transfers VALUES(?1,?2,?3,?4,1,?5,?6,1,1,?7,?8,?9,?10,?11,?12)","retain successful upload receipt");transfer.bind(1,operation);transfer.bind(2,fingerprint.value());transfer.bind(3,a.account);transfer.bind(4,a.session);transfer.bind(5,id.value);transfer.bind(6,r.area.value);transfer.bind(7,static_cast<std::int64_t>(placed.bytes));transfer.bind(8,placed.sha256);transfer.bind(9,ratio);transfer.bind(10,r.bbs_local_date);transfer.bind(11,r.created_utc);transfer.bind(12,now);transfer.step();
        SqliteStatement finish(db,"UPDATE sfx_file_intake SET state=3,file_id=?1 WHERE operation_id=?2 AND state=2","commit immutable upload completion");finish.bind(1,id.value);finish.bind(2,operation);finish.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        file_detail::audit(db,{false,a},operation,"upload.accepted",id,r.area,now);output=file_detail::entry(db,id).file;return SQLITE_OK;
    });
}
application::StorageResult<std::vector<application::FileIntake>> SqliteAccountRepository::pending_file_intake(const application::FileConfigurationActor& a,std::size_t limit,application::Identifier128 after) {
    if(limit<1 || limit>256)return read_failure<std::vector<application::FileIntake>>(invalid("bound private intake page"));
    return impl_->readers_.read<std::vector<application::FileIntake>>("read Sysop-only pending intake",[a,limit,after](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);std::vector<application::FileIntake> result;
        SqliteStatement q(db,"SELECT operation_id FROM sfx_file_intake i WHERE (state IN(0,1,2,5) OR (state=3 AND EXISTS(SELECT 1 FROM sfx_file_intake_objects o WHERE o.operation_id=i.operation_id AND o.staging_clean=0))) AND operation_id>?2 ORDER BY operation_id LIMIT ?1","bounded pending intake including redundant accepted staging");q.bind(1,static_cast<std::int64_t>(limit));q.bind(2,after);while(q.row())result.push_back(file_detail::intake(db,q.identifier(0)));return result;
    });
}
application::StorageStatus SqliteAccountRepository::resolve_file_intake(const application::FileConfigurationActor& a,application::Identifier128 operation,const application::FilePayloadIdentity& payload,bool deleted,std::string_view detail) {
    if(!operation.valid() || detail.size()>512 || application::sanitize_file_text(detail,512,false)!=detail || (deleted && !file_detail::payload_valid(payload)))return invalid("validate native intake resolution");
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate intake resolution clock");
    return impl_->write("record actual exact-staging deletion or private hold",4096,[a,operation,payload,deleted,detail=std::string(detail),now](sqlite3* db){
        message_detail::config_authority(db,a);const auto r=file_detail::intake(db,operation);
        if(r.state==application::FileIntakeState::deleted)return deleted&&file_detail::same_payload(r.payload,payload)?SQLITE_OK:SQLITE_ABORT;
        if(r.state==application::FileIntakeState::accepted)return SQLITE_ABORT;
        if(deleted && (r.state!=application::FileIntakeState::held || !r.scan_required || r.scan.verdict!=application::FileScanVerdict::detected || r.scan.detections.empty() || !file_detail::same_payload(r.payload,payload)))return SQLITE_ABORT;
        SqliteStatement q(db,"UPDATE sfx_file_intake SET state=?1,detail=?2 WHERE operation_id=?3","retain truthful native resolution");q.bind(1,static_cast<std::int64_t>(deleted?4:5));q.bind(2,detail);q.bind(3,operation);q.step();
        file_detail::audit(db,a,operation,deleted?"upload.detected.deleted":"upload.held",{},r.area,now,detail);return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::recover_file_intake(const application::FileConfigurationActor& a) {
    return impl_->write("hold interrupted uploads without publication or credit",4096,[a](sqlite3* db){
        message_detail::config_authority(db,a);
        SqliteStatement q(db,"UPDATE sfx_file_intake SET state=5,detail='Interrupted upload; reconcile private staging and intended destination' WHERE state IN(0,1,2) AND session_id IN(SELECT session_id FROM sfx_account_sessions WHERE completed_utc_seconds IS NOT NULL)","private durable recovery, no filesystem deletion");q.step();return SQLITE_OK;
    });
}
#include "sqlite_file_intake_purge.inc"
