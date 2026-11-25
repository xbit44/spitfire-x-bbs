namespace file_detail {
bool finish_removed_location(sqlite3* db,app::FileId file,app::Identifier128 operation,bool purge){
    SqliteStatement old(db,purge?"UPDATE sfx_file_removed SET purged=1 WHERE file_id=?1 AND purged=0":"DELETE FROM sfx_file_removed WHERE file_id=?1 AND purged=0","complete existing owner removal custody");old.bind(1,file.value);old.step();if(sqlite3_changes(db)==1)return true;
    if(!ftn_publications_available(db))return false;
    SqliteStatement ftn(db,"UPDATE sfx_ftn_file_displacements SET state=?1,owner_operation=?2 WHERE file_id=?3 AND state=0","retain FTN displacement and subsequent explicit owner action");ftn.bind(1,std::int64_t{purge?2:1});ftn.bind(2,operation);ftn.bind(3,file.value);ftn.step();return sqlite3_changes(db)==1;
}
app::ManagedFile managed(sqlite3* db,app::FileId id){
    const auto native=entry(db,id);app::ManagedFile r;r.file=native.file;r.payload=native.payload;
    if(!native.storage_key.empty()&&native.storage_key.front()==':'){
        // This encoding is accepted only with an immutable accepted private
        // intake and its actual native placement binding, never from a UI name.
        SqliteStatement opaque(db,"SELECT i.stage_key,i.filename,i.received_bytes,i.content_sha256,o.placed_volume,o.placed_file_id FROM sfx_file_intake i JOIN sfx_file_intake_objects o USING(operation_id) WHERE i.file_id=?1 AND i.state=3 AND i.private_submission=1 AND i.filename_key=?2","bind opaque basename to accepted private intake");opaque.bind(1,id.value);opaque.bind(2,native.storage_key);
        if(!opaque.row())corrupt();
        const auto stage=opaque.text(0);
        if(native.file.origin!=app::FileOrigin::caller_upload||native.storage_key!=":"+stage||stage.size()!=32||!std::all_of(stage.begin(),stage.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');}))corrupt();
        r.native_basename=stage;
        if(opaque.text(1)!=r.file.name||opaque.int64(2)!=static_cast<std::int64_t>(r.payload.bytes)||digest(opaque,3)!=r.payload.sha256||
            (r.file.state!=app::FileCatalogState::removed&&(opaque.text(4)!=r.payload.volume||opaque.text(5)!=r.payload.file_id)))corrupt();
    }else if(native.storage_key!=key(r.file.name))corrupt();
    SqliteStatement intention(db,"SELECT intended_area_id FROM sfx_file_intake WHERE file_id=?1 AND state=3","retain original requested destination for Sysop review");intention.bind(1,id.value);if(intention.row())r.intended_area={intention.int64(0)};
    r.reupload_blocked=reupload_blocked(db,r.payload);
    SqliteStatement removed(db,"SELECT stage_key,purged FROM sfx_file_removed WHERE file_id=?1","locate durable removed payload");removed.bind(1,id.value);if(removed.row()){r.removed_key=removed.text(0);r.purged=removed.boolean(1);}
    if(ftn_publications_available(db)){
        SqliteStatement ftn(db,"SELECT stage_key,state FROM sfx_ftn_file_displacements WHERE file_id=?1 AND state IN(0,2) ORDER BY rowid DESC LIMIT 1","locate actual FTN displacement custody without inventing Sysop removal");ftn.bind(1,id.value);
        if(ftn.row()&&r.file.state==app::FileCatalogState::removed&&r.removed_key.empty()){r.removed_key=ftn.text(0);r.purged=ftn.int64(1)==2;}
    }
    SqliteStatement pending(db,"SELECT 1 FROM sfx_file_operations WHERE file_id=?1 AND state IN(0,2)","read native mutation reservation");pending.bind(1,id.value);r.operation_pending=pending.row()||ftn_file_pending(db,id);return r;
}
bool operation_valid(const app::FileOperationRequest& r){
    if(!r.operation.valid()||r.file.value<1||r.expected_revision<1||r.expected_revision==INT64_MAX||static_cast<unsigned>(r.action)>7||r.destination.value<0||r.expected_destination_revision<0)return false;
    if(r.action==app::FileOperationAction::metadata)return r.destination.value==0&&r.expected_destination_revision==0&&r.filename.empty()&&app::valid_file_descriptions(r.descriptions)&&r.public_uploader.size()<=64&&app::sanitize_file_text(r.public_uploader,64,false)==r.public_uploader;
    if(r.action==app::FileOperationAction::rename||r.action==app::FileOperationAction::copy||r.action==app::FileOperationAction::move||r.action==app::FileOperationAction::restore)
        return r.destination.value>0&&r.expected_destination_revision>0&&app::valid_file_name(r.filename)&&r.descriptions.short_text.empty()&&r.descriptions.long_text.empty()&&r.public_uploader.empty();
    return r.destination.value==0&&r.expected_destination_revision==0&&r.filename.empty()&&r.descriptions.short_text.empty()&&r.descriptions.long_text.empty()&&r.public_uploader.empty();
}
std::array<std::uint8_t,32> operation_fingerprint(const app::FileConfigurationActor& a,const app::FileOperationRequest& r){
    Fingerprint f("file-manager");f.actor(a);f.number(static_cast<unsigned>(r.action));f.number(r.file.value);f.number(r.expected_revision);f.number(r.destination.value);f.number(r.expected_destination_revision);f.text(r.filename);f.text(r.descriptions.short_text);f.text(r.descriptions.long_text);f.text(r.public_uploader);f.number(r.sysop_only);f.number(r.allow_duplicate_content);return f.value();
}
app::FileOperation operation(sqlite3* db,const app::FileConfigurationActor& actor,app::Identifier128 id){
    SqliteStatement q(db,"SELECT action,installation_owner,actor_account_id,file_id,source_area_id,destination_area_id,expected_revision,source_name,destination_name,destination_revision,short_description,long_description,public_uploader,sysop_only,allow_duplicate,source_bytes,source_sha256,source_volume,source_native_id,removed_key,state,result_file_id,detail FROM sfx_file_operations WHERE operation_id=?1","read exact native file operation");q.bind(1,id);if(!q.row())unavailable();
    // Any currently authorized Sysop may inspect/reconcile another operator's
    // interrupted intent. The original actor remains immutable in its receipt.
    message_detail::config_authority(db,actor);
    app::FileOperation r;auto& request=r.request;request.operation=id;request.action=static_cast<app::FileOperationAction>(q.int64(0));request.file={q.int64(3)};request.destination={q.is_null(5)?0:q.int64(5)};request.expected_revision=q.int64(6);request.filename=q.text(8);request.expected_destination_revision=q.int64(9);request.descriptions={q.text(10),q.text(11)};request.public_uploader=q.text(12);request.sysop_only=q.boolean(13);request.allow_duplicate_content=q.boolean(14);
    r.source=managed(db,request.file);r.source.file.area={q.int64(4)};r.source.file.name=q.text(7);r.source.file.revision=request.expected_revision;
    r.source.payload={static_cast<std::uint64_t>(q.int64(15)),digest(q,16),q.text(17),q.text(18)};r.source.removed_key=q.text(19);r.state=static_cast<app::FileOperationState>(q.int64(20));r.result={q.is_null(21)?0:q.int64(21)};r.detail=q.text(22);
    SqliteStatement placed(db,"SELECT placed_volume,placed_native_id FROM sfx_file_operations WHERE operation_id=?1","read durable destination native identity");placed.bind(1,id);if(placed.row()&&!placed.text(0).empty())r.placed_object=app::FileObjectIdentity{placed.text(0),placed.text(1)};return r;
}
bool destination_free(sqlite3* db,app::FileAreaId area_id,std::string_view name,app::FileId except,app::Identifier128 operation_id){
    SqliteStatement q(db,"SELECT 1 FROM sfx_file_catalog WHERE area_id=?1 AND filename_key=?2 AND catalog_state<>1 AND file_id<>?3 LIMIT 1","reserve destination without overwrite");q.bind(1,area_id.value);q.bind(2,key(std::string(name)));q.bind(3,except.value);return !q.row()&&!name_reserved(db,area_id,name,operation_id);
}
void publish_file(sqlite3* db,app::FileId id,app::FileAreaId area_id,std::int64_t now){
    SqliteStatement p(db,"INSERT INTO sfx_file_publications(file_id,area_id,published_utc) VALUES(?1,?2,?3)","publish final-area availability, retain original upload date");p.bind(1,id.value);p.bind(2,area_id.value);p.bind(3,now);p.step();const std::int64_t sequence=sqlite3_last_insert_rowid(db);
    SqliteStatement q(db,"UPDATE sfx_file_catalog SET published_utc=?1,publication_sequence=?2 WHERE file_id=?3","bind new-file scan publication");q.bind(1,now);q.bind(2,sequence);q.bind(3,id.value);q.step();
}
} // namespace file_detail

application::StorageResult<application::ManagedFile> SqliteAccountRepository::managed_file(const application::FileConfigurationActor& a,application::FileId id){
    if(id.value<1)return read_failure<application::ManagedFile>(invalid("validate managed file identity"));
    return impl_->readers_.read<application::ManagedFile>("read authorized managed file",[a,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return file_detail::managed(db,id);});
}
application::StorageResult<std::vector<application::FileNameStatus>> SqliteAccountRepository::file_name_status(const application::FileConfigurationActor& a,application::FileAreaId id,const std::vector<std::string>& names){
    using Result=std::vector<application::FileNameStatus>;
    if(id.value<1||names.size()>100||std::any_of(names.begin(),names.end(),[](const auto& n){return !application::valid_file_name(n);}))return read_failure<Result>(invalid("validate bounded exact filename lookup"));
    return impl_->readers_.read<Result>("compare native enumeration with authorized catalog",[a,id,names](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);file_detail::area(db,id);Result result;result.reserve(names.size());
        for(const auto& name:names){
            application::FileNameStatus item;item.filename=name;
            SqliteStatement q(db,"SELECT file_id FROM sfx_file_catalog WHERE area_id=?1 AND (filename_key=?2 OR (substr(filename_key,1,1)=':' AND (substr(filename_key,2)=?2 OR sfx_file_fold(filename)=?2))) AND catalog_state<>1 LIMIT 1","exact friendly or native private filename, including missing catalog entries");q.bind(1,id.value);q.bind(2,file_detail::key(name));if(q.row())item.registered={q.int64(0)};
            item.pending=file_detail::name_reserved(db,id,name);
            item.staging_only_hold=item.pending&&!file_detail::name_reserved(db,id,name,{},true);
            result.push_back(std::move(item));
        }return result;
    });
}
application::StorageResult<application::ManagedFilePage> SqliteAccountRepository::managed_file_page(const application::FileConfigurationActor& a,const application::FileManagementQuery& r,application::FileId after,std::size_t limit){
    if(r.area.value<0||after.value<0||limit<1||limit>100||static_cast<unsigned>(r.field)>3||(r.state&&static_cast<unsigned>(*r.state)>2)||r.search.size()>128||application::sanitize_file_text(r.search,128,false)!=r.search)return read_failure<application::ManagedFilePage>(invalid("validate managed file page"));
    return impl_->readers_.read<application::ManagedFilePage>("list authorized native File Manager page",[a,r,after,limit](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);application::ManagedFilePage result;
        const std::string retired=file_detail::group_deletions_available(db)?"NOT EXISTS(SELECT 1 FROM sfx_file_areas ga JOIN sfx_file_group_deletions gd USING(group_id) WHERE ga.area_id=sfx_file_catalog.area_id AND gd.completed=1) AND ":"";
        const auto managed_sql=std::string("SELECT file_id FROM sfx_file_catalog WHERE ")+retired+"file_id>?1 AND (?2=0 OR area_id=?2) AND (?3<0 OR catalog_state=?3) AND (?4='' OR (?5 IN(0,1) AND sfx_file_fold(filename) LIKE ?6 ESCAPE '\\') OR (?5 IN(0,2) AND (instr(sfx_file_fold(short_description),sfx_file_fold(?4))>0 OR instr(sfx_file_fold(long_description),sfx_file_fold(?4))>0)) OR (?5 IN(0,3) AND instr(sfx_file_fold(public_uploader),sfx_file_fold(?4))>0)) ORDER BY file_id LIMIT ?7";SqliteStatement q(db,managed_sql.c_str(),"bounded catalog and metadata search");q.bind(1,after.value);q.bind(2,r.area.value);q.bind(3,r.state?static_cast<std::int64_t>(*r.state):-1);q.bind(4,r.search);q.bind(5,static_cast<std::int64_t>(r.field));q.bind(6,file_detail::filename_pattern(r.search));q.bind(7,static_cast<std::int64_t>(limit+1));
        while(q.row()){if(result.files.size()==limit){result.next=result.files.back().file.id;break;}result.files.push_back(file_detail::managed(db,{q.int64(0)}));}return result;
    });
}
application::StorageResult<application::FileOperation> SqliteAccountRepository::prepare_file_operation(const application::FileConfigurationActor& a,const application::FileOperationRequest& r){
    if(!file_detail::operation_valid(r))return read_failure<application::FileOperation>(invalid("validate native file mutation"));
    const auto fingerprint=file_detail::operation_fingerprint(a,r);const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::FileOperation>(invalid("validate file mutation clock"));
    return impl_->write_result<application::FileOperation>("reserve File Manager intent before payload mutation",r.descriptions.long_text.size()+8192,[a,r,fingerprint,now](sqlite3* db,auto& output){
        message_detail::config_authority(db,a);
        SqliteStatement old(db,"SELECT request_fingerprint FROM sfx_file_operations WHERE operation_id=?1","reconcile exact File Manager command");old.bind(1,r.operation);if(old.row()){if(file_detail::digest(old,0)!=fingerprint)return SQLITE_ABORT;output=file_detail::operation(db,a,r.operation);return SQLITE_OK;}
        const auto source=file_detail::managed(db,r.file);using Action=application::FileOperationAction;
        if(r.action==Action::metadata&&r.public_uploader.empty()&&source.file.origin!=application::FileOrigin::ftn)return SQLITE_ABORT;
        if(source.file.revision!=r.expected_revision||source.operation_pending||source.purged)return SQLITE_ABORT;
        const bool removed=source.file.state==application::FileCatalogState::removed;
        const bool missing=source.file.state==application::FileCatalogState::missing;
        if(r.action==Action::restore){if(!removed&&!missing)return SQLITE_ABORT;if(missing&&(!(r.destination==source.file.area)||r.filename!=source.file.name))return SQLITE_ABORT;}
        else if(r.action!=Action::purge&&removed)return SQLITE_ABORT;
        if(removed&&source.removed_key.empty())return SQLITE_ABORT;
        if(r.action==Action::purge){
            // Reconcile accepted recovery storage before discarding the
            // surviving published/removal copy. Never orphan a hidden copy.
            SqliteStatement recovery(db,"SELECT 1 FROM sfx_file_intake i JOIN sfx_file_intake_objects o USING(operation_id) WHERE i.file_id=?1 AND i.state=3 AND o.staging_clean=0 LIMIT 1","require accepted staging reconciliation before library purge");recovery.bind(1,r.file.value);if(recovery.row())return SQLITE_BUSY;
        }
        if(missing&&r.action!=Action::metadata&&r.action!=Action::restore&&r.action!=Action::purge)return SQLITE_ABORT;
        SqliteStatement active(db,"SELECT 1 FROM sfx_file_transfers WHERE file_id=?1 AND state=0 LIMIT 1","protect active readers/transfers");active.bind(1,r.file.value);if(active.row())return SQLITE_BUSY;
        if(r.destination.value){
            if(file_detail::area(db,r.destination).area.revision!=r.expected_destination_revision)return SQLITE_ABORT;
            if(r.action==Action::rename&&!(r.destination==source.file.area))return SQLITE_ABORT;
            // A legacy private row's display name is NOT its physical basename.
            // Permit owner Rename to that same original display name only when
            // managed() has proved the immutable opaque intake binding. Native
            // preflight still requires a different, absent destination; every
            // reservation/revision/object/no-overwrite guard remains in force.
            const bool normalize_legacy=r.action==Action::rename&&!source.native_basename.empty();
            if((r.action==Action::rename||r.action==Action::move||r.action==Action::copy)&&r.destination==source.file.area&&file_detail::key(r.filename)==file_detail::key(source.file.name)&&!normalize_legacy)return SQLITE_ABORT;
            if(!file_detail::destination_free(db,r.destination,r.filename,r.action==Action::copy?application::FileId{}:r.file,r.operation))return SQLITE_CONSTRAINT;
        }
        // A copy is necessarily duplicate content. Require an explicit Sysop
        // warning override, but never permit it to overwrite a destination name.
        if(r.action==Action::copy&&!r.allow_duplicate_content)return SQLITE_CONSTRAINT;
        SqliteStatement q(db,"INSERT INTO sfx_file_operations VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,?19,?20,?21,?22,?23,0,NULL,'',NULL,?24,'','')","journal bounded recoverable mutation");
        q.bind(1,r.operation);q.bind(2,fingerprint);q.bind(3,static_cast<std::int64_t>(r.action));q.bind(4,a.installation_owner);if(a.caller)q.bind(5,a.caller->account);else q.bind_null(5);q.bind(6,r.file.value);q.bind(7,source.file.area.value);if(r.destination.value)q.bind(8,r.destination.value);else q.bind_null(8);q.bind(9,r.expected_revision);q.bind(10,source.file.name);q.bind(11,r.filename);q.bind(12,r.expected_destination_revision);q.bind(13,file_detail::key(r.filename));q.bind(14,r.descriptions.short_text);q.bind(15,r.descriptions.long_text);q.bind(16,r.public_uploader);q.bind(17,r.sysop_only);q.bind(18,r.allow_duplicate_content);q.bind(19,static_cast<std::int64_t>(source.payload.bytes));q.bind(20,source.payload.sha256);q.bind(21,source.payload.volume);q.bind(22,source.payload.file_id);q.bind(23,source.removed_key);q.bind(24,now);q.step();
        file_detail::audit(db,a,r.operation,"file.operation.prepared",r.file,source.file.area,now);output=file_detail::operation(db,a,r.operation);return SQLITE_OK;
    });
}
application::StorageResult<application::FileOperation> SqliteAccountRepository::commit_file_operation(const application::FileConfigurationActor& a,application::Identifier128 id,const application::FileOperationCompletion& native){
    if(!id.valid()||!file_detail::payload_valid(native.source)||(native.destination_present&&!file_detail::payload_valid(native.destination)))return read_failure<application::FileOperation>(invalid("validate trusted native file mutation outcome"));
    file_detail::Fingerprint f("file-mutation-completion");f.payload(native.source);f.number(native.source_absent);f.number(native.destination_present);if(native.destination_present)f.payload(native.destination);const auto fingerprint=f.value();
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::FileOperation>(invalid("validate mutation completion clock"));
    return impl_->write_result<application::FileOperation>("commit verified native mutation without transfer accounting",24576,[a,id,native,fingerprint,now](sqlite3* db,auto& output){
        message_detail::config_authority(db,a);const auto op=file_detail::operation(db,a,id);const auto& r=op.request;using Action=application::FileOperationAction;
        if(op.state==application::FileOperationState::completed){SqliteStatement receipt(db,"SELECT completion_fingerprint FROM sfx_file_operations WHERE operation_id=?1","replay exact completed mutation only");receipt.bind(1,id);if(!receipt.row()||file_detail::digest(receipt,0)!=fingerprint)return SQLITE_ABORT;output=op;return SQLITE_OK;}
        if(op.state==application::FileOperationState::cancelled||!file_detail::same_payload(native.source,op.source.payload))return SQLITE_ABORT;
        const auto current=file_detail::entry(db,r.file);if(current.file.revision!=r.expected_revision||!file_detail::same_payload(current.payload,op.source.payload))return SQLITE_ABORT;
        const bool missing=current.file.state==application::FileCatalogState::missing;
        const bool absent=r.action==Action::metadata?missing:(r.action!=Action::copy&&!(r.action==Action::restore&&missing));
        const bool destination=r.action!=Action::purge&&r.action!=Action::mark_missing&&!(r.action==Action::metadata&&missing);
        if(native.source_absent!=absent||native.destination_present!=destination)return SQLITE_ABORT;
        if(destination&&(native.destination.bytes!=native.source.bytes||native.destination.sha256!=native.source.sha256))return SQLITE_ABORT;
        if(destination&&!file_detail::same_payload(native.source,native.destination)&&(!op.placed_object||op.placed_object->volume!=native.destination.volume||op.placed_object->file_id!=native.destination.file_id))return SQLITE_ABORT;
        if((r.action==Action::metadata&&!missing)||(r.action==Action::restore&&missing))if(!file_detail::same_payload(native.source,native.destination))return SQLITE_ABORT;
        if(r.action==Action::copy&&native.source.volume==native.destination.volume&&native.source.file_id==native.destination.file_id)return SQLITE_ABORT;
        // Preparation validates the selected area revision. Pending intents
        // prevent physical path changes; a later label/ACL edit must not strand
        // an already performed native move. Sysop authority is rechecked above.
        if(r.destination.value&&!file_detail::destination_free(db,r.destination,r.filename,r.action==Action::copy?application::FileId{}:r.file,id))return SQLITE_ABORT;
        application::FileId result=r.file;
        if(r.action==Action::metadata){
            SqliteStatement q(db,"UPDATE sfx_file_catalog SET short_description=?1,long_description=?2,public_uploader=?3,sysop_only=?4,revision=revision+1 WHERE file_id=?5","edit descriptions/public attribution only");q.bind(1,r.descriptions.short_text);q.bind(2,r.descriptions.long_text);q.bind(3,r.public_uploader);q.bind(4,r.sysop_only);q.bind(5,r.file.value);q.step();
        }else if(r.action==Action::copy){
            SqliteStatement q(db,"INSERT INTO sfx_file_catalog SELECT NULL,?1,?2,?3,short_description,long_description,public_uploader,uploader_account_id,byte_count,content_sha256,?4,?5,original_upload_utc,?6,0,0,CASE WHEN origin=3 THEN 3 ELSE 2 END,0,sysop_only,1 FROM sfx_file_catalog WHERE file_id=?7","copy independent native object without fabricated upload credit");q.bind(1,r.destination.value);q.bind(2,r.filename);q.bind(3,file_detail::key(r.filename));q.bind(4,native.destination.volume);q.bind(5,native.destination.file_id);q.bind(6,now);q.bind(7,r.file.value);q.step();result={sqlite3_last_insert_rowid(db)};file_detail::publish_file(db,result,r.destination,now);
        }else if(r.action==Action::rename||r.action==Action::move||r.action==Action::restore){
            SqliteStatement q(db,"UPDATE sfx_file_catalog SET area_id=?1,filename=?2,filename_key=?3,native_volume=?4,native_file_id=?5,catalog_state=0,revision=revision+1 WHERE file_id=?6","commit exact destination, retain original uploader/date");q.bind(1,r.destination.value);q.bind(2,r.filename);q.bind(3,r.action==Action::restore&&missing?current.storage_key:file_detail::key(r.filename));q.bind(4,native.destination.volume);q.bind(5,native.destination.file_id);q.bind(6,r.file.value);q.step();
            if(r.action==Action::restore||!(r.destination==op.source.file.area))file_detail::publish_file(db,result,r.destination,now);
            if(r.action==Action::restore&&!missing&&!file_detail::finish_removed_location(db,r.file,id,false))return SQLITE_ABORT;
        }else if(r.action==Action::remove){
            SqliteStatement q(db,"UPDATE sfx_file_catalog SET catalog_state=1,native_volume=?1,native_file_id=?2,revision=revision+1 WHERE file_id=?3","recoverably remove catalog visibility, not payload bytes");q.bind(1,native.destination.volume);q.bind(2,native.destination.file_id);q.bind(3,r.file.value);q.step();
            SqliteStatement removed(db,"INSERT INTO sfx_file_removed VALUES(?1,?2,?3,0)","retain exact durable private removal key");removed.bind(1,r.file.value);removed.bind(2,id);removed.bind(3,file_detail::stage_key(id));removed.step();
        }else if(r.action==Action::purge){
            // A current Remove deletes directly. The same durable action also
            // completes old retained removals; immutable history remains valid.
            if(current.file.state==application::FileCatalogState::removed){
                if(!file_detail::finish_removed_location(db,r.file,id,true))return SQLITE_ABORT;
            }else{
                SqliteStatement q(db,"INSERT INTO sfx_file_removed VALUES(?1,?2,?3,1)","retain deletion receipt without a retained payload");q.bind(1,r.file.value);q.bind(2,id);q.bind(3,file_detail::stage_key(id));q.step();
            }
            SqliteStatement revision(db,"UPDATE sfx_file_catalog SET catalog_state=1,revision=revision+1 WHERE file_id=?1","remove deleted listing, preserve audit identity");revision.bind(1,r.file.value);revision.step();
        }else{
            SqliteStatement q(db,"UPDATE sfx_file_catalog SET catalog_state=2,revision=revision+1 WHERE file_id=?1","mark natively verified absent payload only");q.bind(1,r.file.value);q.step();
        }
        if(r.action==Action::copy&&current.file.origin==application::FileOrigin::ftn){
            SqliteStatement derived(db,"INSERT INTO sfx_ftn_file_lineage(file_id,receipt_operation,source_file_id,copy_operation) SELECT ?1,receipt_operation,file_id,?2 FROM sfx_ftn_file_lineage WHERE file_id=?3","retain FTN source receipt and exact authorized copy operation");derived.bind(1,result.value);derived.bind(2,id);derived.bind(3,r.file.value);derived.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        }
        SqliteStatement complete(db,"UPDATE sfx_file_operations SET state=1,result_file_id=?1,completion_fingerprint=?2,detail='Native outcome committed' WHERE operation_id=?3 AND state IN(0,2)","retain immutable operation completion");complete.bind(1,result.value);complete.bind(2,fingerprint);complete.bind(3,id);complete.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        file_detail::audit(db,a,id,"file.operation.completed",result,r.destination.value?r.destination:op.source.file.area,now);output=file_detail::operation(db,a,id);return SQLITE_OK;
    });
}
application::StorageResult<application::FileOperation> SqliteAccountRepository::file_operation(const application::FileConfigurationActor& a,application::Identifier128 id){
    if(!id.valid())return read_failure<application::FileOperation>(invalid("validate native operation identity"));
    return impl_->readers_.read<application::FileOperation>("read authorized native operation",[a,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);return file_detail::operation(db,a,id);});
}
application::StorageStatus SqliteAccountRepository::bind_file_operation_destination(const application::FileConfigurationActor& a,application::Identifier128 id,const application::FileObjectIdentity& native){
    const auto valid=[](const auto& v){return !v.empty()&&v.size()<=128&&application::sanitize_file_text(v,128,false)==v;};
    if(!id.valid()||!valid(native.volume)||!valid(native.file_id))return invalid("validate native destination identity");
    return impl_->write("bind newly created destination before writing payload",4096,[a,id,native](sqlite3* db){
        const auto op=file_detail::operation(db,a,id);using Action=application::FileOperationAction;
        if(op.state==application::FileOperationState::completed||op.state==application::FileOperationState::cancelled||op.request.action==Action::metadata||op.request.action==Action::purge||op.request.action==Action::mark_missing)return SQLITE_ABORT;
        if(op.placed_object)return op.placed_object->volume==native.volume&&op.placed_object->file_id==native.file_id?SQLITE_OK:SQLITE_ABORT;
        SqliteStatement q(db,"UPDATE sfx_file_operations SET placed_volume=?1,placed_native_id=?2 WHERE operation_id=?3","retain exact operation-owned destination");q.bind(1,native.volume);q.bind(2,native.file_id);q.bind(3,id);q.step();return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::cancel_file_operation(const application::FileConfigurationActor& a,application::Identifier128 id,const application::FileOperationCompletion& native){
    if(!id.valid()||!file_detail::payload_valid(native.source)||native.destination_present)return invalid("validate unchanged-source cancellation proof");
    file_detail::Fingerprint f("file-mutation-cancel");f.payload(native.source);f.number(native.source_absent);const auto fingerprint=f.value();
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate cancellation clock");
    return impl_->write("cancel only verified unmutated intent",4096,[a,id,native,fingerprint,now](sqlite3* db){
        const auto op=file_detail::operation(db,a,id);
        if(op.state==application::FileOperationState::cancelled){SqliteStatement q(db,"SELECT completion_fingerprint FROM sfx_file_operations WHERE operation_id=?1","exact cancellation replay");q.bind(1,id);return q.row()&&file_detail::digest(q,0)==fingerprint?SQLITE_OK:SQLITE_ABORT;}
        if(op.state==application::FileOperationState::completed||!file_detail::same_payload(op.source.payload,native.source)||native.source_absent!=(op.source.file.state==application::FileCatalogState::missing))return SQLITE_ABORT;
        const auto current=file_detail::entry(db,op.request.file);if(current.file.revision!=op.request.expected_revision||!file_detail::same_payload(current.payload,native.source))return SQLITE_ABORT;
        SqliteStatement q(db,"UPDATE sfx_file_operations SET state=3,completion_fingerprint=?1,detail='Unchanged source and absent destination verified; cancelled' WHERE operation_id=?2","release only verified unmutated reservation");q.bind(1,fingerprint);q.bind(2,id);q.step();
        file_detail::audit(db,a,id,"file.operation.cancelled",op.request.file,op.source.file.area,now);return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::hold_file_operation(const application::FileConfigurationActor& a,application::Identifier128 id,std::string_view detail){
    if(!id.valid()||detail.empty()||detail.size()>512||application::sanitize_file_text(detail,512,false)!=detail)return invalid("validate safe file-operation hold");
    return impl_->write("retain failed native mutation for explicit reconciliation",4096,[a,id,detail=std::string(detail)](sqlite3* db){message_detail::config_authority(db,a);const auto op=file_detail::operation(db,a,id);if(op.state==application::FileOperationState::completed||op.state==application::FileOperationState::cancelled)return SQLITE_ABORT;SqliteStatement q(db,"UPDATE sfx_file_operations SET state=2,detail=?1 WHERE operation_id=?2","hold without delete, overwrite or false success");q.bind(1,detail);q.bind(2,id);q.step();return SQLITE_OK;});
}
application::StorageResult<std::vector<application::FileOperation>> SqliteAccountRepository::pending_file_operations(const application::FileConfigurationActor& a,application::Identifier128 after,std::size_t limit){
    if(limit<1||limit>100)return read_failure<std::vector<application::FileOperation>>(invalid("bound pending mutation query"));
    return impl_->readers_.read<std::vector<application::FileOperation>>("read authorized unresolved file operations",[a,after,limit](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);std::vector<application::FileOperation> result;SqliteStatement q(db,"SELECT operation_id FROM sfx_file_operations WHERE state IN(0,2) AND operation_id>?1 ORDER BY operation_id LIMIT ?2","bounded operation pagination");q.bind(1,after);q.bind(2,static_cast<std::int64_t>(limit));while(q.row())result.push_back(file_detail::operation(db,a,q.identifier(0)));return result;});
}
