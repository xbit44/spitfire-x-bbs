namespace file_detail {
std::string filename_pattern(std::string_view query) {
    const auto folded=key(std::string(query));query=folded;
    const bool wildcard=query.find_first_of("*?")!=std::string_view::npos;
    std::string result;if(!wildcard)result+='%';
    for(const char c:query){
        if(c=='*')result+='%';else if(c=='?')result+='_';
        else{if(c=='%' || c=='_' || c=='\\')result+='\\';result+=c;}
    }
    if(!wildcard)result+='%';
    return result;
}
} // namespace file_detail
application::StorageResult<application::FilePage> SqliteAccountRepository::file_page(const application::FileActor& a,const application::FileQuery& r,const application::FileQueryCursor& cursor,std::size_t limit) {
    if(limit<1 || limit>application::kFilePageMaximum || static_cast<unsigned>(r.scope)>2 || static_cast<unsigned>(r.sort)>4 ||
        (r.scope==application::FileSearchScope::area && r.area.value<1) || (r.scope==application::FileSearchScope::group && r.group.value<1) ||
        cursor.file.value<0 || cursor.filename_key.size()>application::kFileNameKeyBytes || !application::valid_file_utf8(cursor.filename_key) || r.publication_fence<0 || r.after_publication<0 ||
        (r.new_only && !application::valid_utc_seconds(r.since_utc)) || ((r.unseen_only||r.selected_only)&&!r.new_only) || (r.search && static_cast<unsigned>(*r.search)>1))
        return read_failure<application::FilePage>(invalid("validate bounded file query"));
    std::vector<std::string> terms;
    if(r.search){
        if(*r.search==application::FileSearchKind::descriptions){const auto parsed=application::file_description_terms(r.query);if(!parsed)return read_failure<application::FilePage>(invalid("validate description search"));terms=*parsed;}
        else if(r.query.empty() || r.query.size()>application::kFileNameBytes || application::sanitize_file_text(r.query,application::kFileNameBytes,false)!=r.query)return read_failure<application::FilePage>(invalid("validate filename search"));
    }
    return impl_->readers_.read<application::FilePage>("list authorized file page",[a,r,cursor,limit,terms](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto auth=file_detail::authority(db,a);
        if(r.scope==application::FileSearchScope::area){const auto area=file_detail::area(db,r.area).area;if(!application::file_area_visible(file_detail::group(db,area.group),area,auth))file_detail::unavailable();}
        if(r.scope==application::FileSearchScope::group){const auto g=file_detail::group(db,r.group);if(!g.enabled || auth.level<g.access)file_detail::unavailable();}
        application::FilePage result;const auto latest=file_detail::publication_fence(db);result.publication_fence=r.publication_fence?r.publication_fence:latest;if(result.publication_fence>latest)file_detail::conflict();
        const char* sort="f.file_id*0";
        switch(r.sort){case application::FileSort::filename:break;case application::FileSort::newest:sort="-f.published_utc";break;case application::FileSort::oldest:sort="f.published_utc";break;case application::FileSort::size:sort="f.byte_count";break;case application::FileSort::downloads:sort="-f.download_count";break;}
        // Ordinary callers cannot see private records and retain their indexed
        // key path. Sysop searches/cursors use the original friendly name.
        const std::string name_key=auth.sysop()?"(CASE WHEN substr(f.filename_key,1,1)=':' THEN sfx_file_fold(f.filename) ELSE f.filename_key END)":"f.filename_key";
        std::string sql="SELECT f.file_id,"+std::string(sort)+","+name_key+" FROM sfx_file_catalog f JOIN sfx_file_areas a USING(area_id) JOIN sfx_file_groups g USING(group_id) WHERE f.catalog_state=0 AND g.enabled=1 AND a.enabled=1 AND g.access_minimum<=?1 AND a.access_minimum<=?1 AND (a.kind<>2 OR ?2=1) AND (f.sysop_only=0 OR ?2=1) AND (?3=0 OR (a.download_minimum<=?1 AND (a.downloads_sysop_only=0 OR ?2=1))) AND (?4<>0 OR a.area_id=?5) AND (?4<>1 OR a.group_id=?6) AND f.publication_sequence<=?7 AND (?8=0 OR (f.publication_sequence>?9 AND f.published_utc>=?10)) AND (?11=0 OR ("+std::string(sort)+","+name_key+",f.file_id)>(?12,?13,?11))";
        sql+=" AND NOT EXISTS(SELECT 1 FROM sfx_file_operations o WHERE o.file_id=f.file_id AND o.state IN(0,2))";
        sql+=" AND (?101=0 OR NOT EXISTS(SELECT 1 FROM sfx_file_scan_seen s WHERE s.account_id=?100 AND s.file_id=f.file_id AND s.publication_sequence=f.publication_sequence)) AND (?102=0 OR NOT EXISTS(SELECT 1 FROM sfx_file_scan_areas s WHERE s.account_id=?100 AND s.area_id=f.area_id AND s.selected=0))";
        if(r.search){if(*r.search==application::FileSearchKind::filename)sql+=" AND "+name_key+" LIKE ?14 ESCAPE '\\'";
            else{sql+=" AND (";for(std::size_t i=0;i<terms.size();++i){if(i)sql+=" OR ";const auto parameter="?"+std::to_string(14+i);sql+="instr(sfx_file_fold(f.short_description),sfx_file_fold("+parameter+"))>0 OR instr(sfx_file_fold(f.long_description),sfx_file_fold("+parameter+"))>0";}sql+=')';}}
        const auto filters=sql.substr(sql.find(" FROM "));
        const auto bind=[&](SqliteStatement& q,bool summary){
            q.bind(1,auth.level);q.bind(2,auth.sysop());q.bind(3,r.downloadable_only);q.bind(4,static_cast<std::int64_t>(r.scope));q.bind(5,r.area.value);q.bind(6,r.group.value);q.bind(7,result.publication_fence);q.bind(8,r.new_only);q.bind(9,r.after_publication);q.bind(10,r.since_utc);q.bind(11,summary?std::int64_t{}:cursor.file.value);q.bind(12,cursor.sort_value);q.bind(13,cursor.filename_key);
            if(r.search){if(*r.search==application::FileSearchKind::filename)q.bind(14,file_detail::filename_pattern(r.query));else for(std::size_t i=0;i<terms.size();++i)q.bind(static_cast<int>(14+i),terms[i]);}
            q.bind(100,a.account);q.bind(101,r.unseen_only);q.bind(102,r.selected_only);
        };
        // Same read snapshot and identical visibility/search filters as rows.
        // Disable only the page cursor, so totals cannot reveal private files.
        const auto aggregate="SELECT count(*),coalesce(sum(f.byte_count),0),coalesce(max(f.published_utc),0)"+filters;
        SqliteStatement summary(db,aggregate.c_str(),"authorized complete scope totals");bind(summary,true);
        if(!summary.row()||summary.int64(0)<0||summary.int64(1)<0)file_detail::corrupt();
        result.total_files=static_cast<std::uint64_t>(summary.int64(0));result.total_bytes=static_cast<std::uint64_t>(summary.int64(1));result.last_updated_utc=summary.int64(2);
        sql+=" ORDER BY "+std::string(sort)+","+name_key+",f.file_id LIMIT "+std::to_string(limit+1);
        SqliteStatement q(db,sql.c_str(),"bounded permission-filtered file listing");bind(q,false);
        std::optional<application::FileQueryCursor> last;
        while(q.row()){
            if(result.entries.size()==limit){result.next=last;break;}
            auto file=file_detail::visible_entry(db,{q.int64(0)},auth,r.downloadable_only).file;
            SqliteStatement unseen(db,"SELECT ?1>=coalesce((SELECT scan_since_utc FROM sfx_file_caller_state WHERE account_id=?2),(SELECT created_utc_seconds FROM sfx_accounts WHERE account_id=?2)) AND NOT EXISTS(SELECT 1 FROM sfx_file_scan_seen WHERE account_id=?2 AND file_id=?3 AND publication_sequence=?4)","caller new marker from exact publication");
            unseen.bind(1,file.published_utc);unseen.bind(2,a.account);unseen.bind(3,file.id.value);unseen.bind(4,file.publication_sequence);if(!unseen.row())file_detail::corrupt();file.new_to_caller=unseen.boolean(0);
            last=application::FileQueryCursor{q.text(2),q.int64(1),file.id};file.descriptions.long_text.clear();result.entries.push_back(std::move(file));
        }
        return result;
    });
}
application::StorageResult<application::FileChangeOutcome> SqliteAccountRepository::edit_own_file_description(const application::FileActor& a,application::Identifier128 operation,application::FileId id,std::int64_t revision,const application::FileDescriptions& descriptions) {
    if(!operation.valid() || id.value<1 || revision<1 || revision==INT64_MAX || !application::valid_file_descriptions(descriptions))return read_failure<application::FileChangeOutcome>(invalid("validate own file description edit"));
    file_detail::Fingerprint f("own-description");f.number(a.account);f.number(id.value);f.number(revision);f.text(descriptions.short_text);f.text(descriptions.long_text);const auto fingerprint=f.value();const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::FileChangeOutcome>(invalid("validate file description clock"));
    return impl_->write_result<application::FileChangeOutcome>("edit authorized own description only",descriptions.long_text.size()+2048,[a,operation,id,revision,descriptions,fingerprint,now](sqlite3* db,auto& output){
        const auto auth=file_detail::authority(db,a);const application::FileConfigurationActor actor{false,a};
        if(auto old=file_detail::receipt(db,actor,operation,fingerprint)){output=*old;return SQLITE_OK;}
        const auto file=file_detail::visible_entry(db,id,auth).file;const auto area=file_detail::area(db,file.area).area;
        if(!application::file_description_editable(file_detail::group(db,area.group),area,file,auth,a.account))return SQLITE_NOTFOUND;
        if(file.revision!=revision)return SQLITE_ABORT;
        SqliteStatement reading(db,"SELECT 1 FROM sfx_file_transfers WHERE file_id=?1 AND state=0 LIMIT 1","avoid description revision changes during active transfer");reading.bind(1,id.value);if(reading.row())return SQLITE_BUSY;
        SqliteStatement q(db,"UPDATE sfx_file_catalog SET short_description=?1,long_description=?2,revision=revision+1 WHERE file_id=?3 AND revision=?4","change text only, not file identity/attribution/path");q.bind(1,descriptions.short_text);q.bind(2,descriptions.long_text);q.bind(3,id.value);q.bind(4,revision);q.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        const application::FileChangeOutcome result{revision+1,id.value,false};file_detail::save_receipt(db,actor,operation,fingerprint,result,now);file_detail::audit(db,actor,operation,"description.changed",id,file.area,now);output=result;return SQLITE_OK;
    });
}
namespace file_detail {
template<class Output> int register_native(sqlite3* db,const app::FileConfigurationActor& a,const app::FileRegistration& r,const std::array<std::uint8_t,32>& fingerprint,std::int64_t now,Output& output,bool authorized_import=false){
        message_detail::config_authority(db,a);
        SqliteStatement importing(db,"SELECT 1 FROM sfx_file_imports WHERE operation_id=?1","native import requires its own completion proof");importing.bind(1,r.operation);if(importing.row()&&!authorized_import)return SQLITE_ABORT;
        if(auto prior=file_detail::receipt(db,a,r.operation,fingerprint)){output=file_detail::entry(db,{prior->identity}).file;return SQLITE_OK;}
        const auto configured_area=file_detail::area(db,r.area).area;
        // Prepared imports freeze physical path changes. Current Sysop authority
        // still applies, but a harmless area label/ACL revision cannot strand
        // bytes already copied/moved by that durable intent.
        if(!authorized_import&&configured_area.revision!=r.expected_area_revision)return SQLITE_ABORT;
        if(name_reserved(db,r.area,r.filename,r.operation))return SQLITE_ABORT;
        SqliteStatement duplicate(db,"SELECT 1 FROM sfx_file_catalog WHERE content_sha256=?1 AND byte_count=?2 AND catalog_state<>1 LIMIT 1","warn about content duplicates");duplicate.bind(1,r.payload.sha256);duplicate.bind(2,static_cast<std::int64_t>(r.payload.bytes));if(duplicate.row() && !r.allow_duplicate_content)return SQLITE_CONSTRAINT;
        SqliteStatement collision(db,"SELECT 1 FROM sfx_file_catalog WHERE area_id=?1 AND filename_key=?2 AND catalog_state<>1","never overwrite a filename collision");collision.bind(1,r.area.value);collision.bind(2,file_detail::key(r.filename));if(collision.row())return SQLITE_ABORT;
        SqliteStatement q(db,"INSERT INTO sfx_file_catalog(area_id,filename,filename_key,short_description,long_description,public_uploader,uploader_account_id,byte_count,content_sha256,native_volume,native_file_id,original_upload_utc,published_utc,publication_sequence,download_count,origin,catalog_state,sysop_only,revision) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,0,0,?14,0,?15,1)","register exact in-place/imported payload");
        q.bind(1,r.area.value);q.bind(2,r.filename);q.bind(3,file_detail::key(r.filename));q.bind(4,r.descriptions.short_text);q.bind(5,r.descriptions.long_text);q.bind(6,r.public_uploader);if(r.uploader)q.bind(7,*r.uploader);else q.bind_null(7);q.bind(8,static_cast<std::int64_t>(r.payload.bytes));q.bind(9,r.payload.sha256);q.bind(10,r.payload.volume);q.bind(11,r.payload.file_id);q.bind(12,r.original_upload_utc);q.bind(13,now);q.bind(14,static_cast<std::int64_t>(r.origin));q.bind(15,r.sysop_only);q.step();
        const application::FileId id{sqlite3_last_insert_rowid(db)};
        SqliteStatement publication(db,"INSERT INTO sfx_file_publications(file_id,area_id,published_utc) VALUES(?1,?2,?3)","record native public availability time");publication.bind(1,id.value);publication.bind(2,r.area.value);publication.bind(3,now);publication.step();const std::int64_t sequence=sqlite3_last_insert_rowid(db);
        SqliteStatement sequence_save(db,"UPDATE sfx_file_catalog SET publication_sequence=?1 WHERE file_id=?2","bind file publication sequence");sequence_save.bind(1,sequence);sequence_save.bind(2,id.value);sequence_save.step();
        file_detail::save_receipt(db,a,r.operation,fingerprint,{1,id.value,false},now);file_detail::audit(db,a,r.operation,"file.registered",id,r.area,now,r.allow_duplicate_content?"duplicate override allowed":"");output=file_detail::entry(db,id).file;return SQLITE_OK;
}
} // namespace file_detail
application::StorageResult<application::FileEntry> SqliteAccountRepository::register_file(const application::FileConfigurationActor& a,const application::FileRegistration& r) {
    if(!file_detail::registration_valid(r))return read_failure<application::FileEntry>(invalid("validate native file registration"));
    file_detail::Fingerprint f("register");f.actor(a);file_detail::registration_fields(f,r);const auto fingerprint=f.value();const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::FileEntry>(invalid("validate file registration clock"));
    return impl_->write_result<application::FileEntry>("register native file without caller accounting",r.descriptions.long_text.size()+4096,[a,r,fingerprint,now](sqlite3* db,auto& output){return file_detail::register_native(db,a,r,fingerprint,now,output);});
}
application::StorageResult<application::FileEntry> SqliteAccountRepository::commit_file_import(const application::FileConfigurationActor& a,application::Identifier128 id,const application::FileOperationCompletion& native){
    if(!id.valid()||!file_detail::payload_valid(native.source)||!native.destination_present||!file_detail::payload_valid(native.destination))return read_failure<application::FileEntry>(invalid("validate trusted native import completion"));
    const auto fingerprint=file_detail::import_completion(native,"import-completion");const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::FileEntry>(invalid("validate import completion clock"));
    return impl_->write_result<application::FileEntry>("commit native import and its catalog record atomically",32768,[a,id,native,fingerprint,now](sqlite3* db,auto& output){
        const auto op=file_detail::import_record(db,a,id);
        if(op.state==application::FileOperationState::completed){SqliteStatement receipt(db,"SELECT completion_fingerprint FROM sfx_file_imports WHERE operation_id=?1","exact terminal import proof");receipt.bind(1,id);if(!receipt.row()||file_detail::digest(receipt,0)!=fingerprint)return SQLITE_ABORT;output=file_detail::entry(db,op.result).file;return SQLITE_OK;}
        if(op.state==application::FileOperationState::cancelled||!file_detail::same_payload(native.source,op.request.registration.payload)||native.source_absent!=(op.request.mode==application::FileImportMode::move)||native.destination.bytes!=native.source.bytes||native.destination.sha256!=native.source.sha256)return SQLITE_ABORT;
        const bool same_object=native.source.volume==native.destination.volume&&native.source.file_id==native.destination.file_id;
        if((op.request.mode==application::FileImportMode::in_place&&!same_object)||(op.request.mode==application::FileImportMode::copy&&same_object))return SQLITE_ABORT;
        if(!same_object&&(!op.placed_object||op.placed_object->volume!=native.destination.volume||op.placed_object->file_id!=native.destination.file_id))return SQLITE_ABORT;
        auto registration=op.request.registration;registration.payload=native.destination;
        file_detail::Fingerprint registered("register");registered.actor(a);file_detail::registration_fields(registered,registration);
        const auto result=file_detail::register_native(db,a,registration,registered.value(),now,output,true);if(result!=SQLITE_OK)return result;
        SqliteStatement q(db,"UPDATE sfx_file_imports SET state=1,result_file_id=?1,completion_fingerprint=?2,detail='Native import committed' WHERE operation_id=?3 AND state IN(0,2)","bind accepted import exactly once without transfer credit");q.bind(1,output->id.value);q.bind(2,fingerprint);q.bind(3,id);q.step();return sqlite3_changes(db)==1?SQLITE_OK:SQLITE_ABORT;
    });
}
