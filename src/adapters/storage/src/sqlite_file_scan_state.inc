namespace file_detail {
app::FileScanArea scan_area(sqlite3* db,app::AccountId account,app::FileAreaId id){
    SqliteStatement q(db,"SELECT selected,revision FROM sfx_file_scan_areas WHERE account_id=?1 AND area_id=?2","read caller-owned scan selection");q.bind(1,account);q.bind(2,id.value);
    app::FileScanArea result;result.area=id;if(q.row()){result.selected=q.boolean(0);result.revision=q.int64(1);}return result;
}
void visible_scan_area(sqlite3* db,app::FileAreaId id,app::FileAuthority auth){
    const auto ar=area(db,id).area;if(!app::file_area_visible(group(db,ar.group),ar,auth))unavailable();
}
} // namespace file_detail

application::StorageResult<std::vector<application::FileScanArea>> SqliteAccountRepository::file_scan_areas(const application::FileActor& a){
    return impl_->readers_.read<std::vector<application::FileScanArea>>("list only accessible scan selections",[a](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto auth=file_detail::authority(db,a);std::vector<application::FileScanArea> result;
        SqliteStatement q(db,"SELECT a.area_id FROM sfx_file_areas a JOIN sfx_file_groups g USING(group_id) WHERE a.enabled=1 AND g.enabled=1 AND a.access_minimum<=?1 AND g.access_minimum<=?1 AND (a.kind<>2 OR ?2=1) ORDER BY g.display_order,g.group_id,a.display_order,a.area_id LIMIT 1000","bounded authorized scan areas");q.bind(1,auth.level);q.bind(2,auth.sysop());
        while(q.row()){if(result.size()>=999)file_detail::corrupt();result.push_back(file_detail::scan_area(db,a.account,{q.int64(0)}));}return result;
    });
}
application::StorageResult<application::FileScanArea> SqliteAccountRepository::save_file_scan_area(const application::FileActor& a,const application::FileScanArea& r){
    if(r.area.value<1||r.revision<1||r.revision==INT64_MAX)return read_failure<application::FileScanArea>(invalid("validate scan selection"));
    return impl_->write_result<application::FileScanArea>("save one authorized scan selection",1024,[a,r](sqlite3* db,auto& output){
        file_detail::visible_scan_area(db,r.area,file_detail::authority(db,a));const auto prior=file_detail::scan_area(db,a.account,r.area);if(prior.revision!=r.revision)return SQLITE_ABORT;
        SqliteStatement q(db,"INSERT INTO sfx_file_scan_areas VALUES(?1,?2,?3,?4) ON CONFLICT(account_id,area_id) DO UPDATE SET selected=excluded.selected,revision=excluded.revision","CAS caller scan preference only");q.bind(1,a.account);q.bind(2,r.area.value);q.bind(3,r.selected);q.bind(4,r.revision+1);q.step();output=file_detail::scan_area(db,a.account,r.area);return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::acknowledge_file_scan(const application::FileActor& a,const std::vector<application::FileScanSeen>& rows){
    if(rows.empty()||rows.size()>application::kFilePageMaximum)return invalid("bound emitted scan entries");
    std::vector<std::int64_t> ids;for(const auto& row:rows){if(row.file.value<1||row.publication_sequence<1||std::find(ids.begin(),ids.end(),row.file.value)!=ids.end())return invalid("validate unique emitted scan entries");ids.push_back(row.file.value);}
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate file scan clock");
    return impl_->write("acknowledge exactly emitted file publications",rows.size()*256+1024,[a,rows,now](sqlite3* db){
        const auto auth=file_detail::authority(db,a);
        for(const auto& row:rows){
            const auto current=file_detail::visible_entry(db,row.file,auth).file;if(current.publication_sequence!=row.publication_sequence)return SQLITE_ABORT;
            SqliteStatement q(db,"INSERT INTO sfx_file_scan_seen VALUES(?1,?2,?3,?4) ON CONFLICT(account_id,file_id) DO UPDATE SET publication_sequence=excluded.publication_sequence,seen_utc=excluded.seen_utc WHERE publication_sequence<>excluded.publication_sequence","one latest seen publication per caller and file");q.bind(1,a.account);q.bind(2,row.file.value);q.bind(3,row.publication_sequence);q.bind(4,now);q.step();
        }return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::complete_file_scan(const application::FileActor& a,const std::vector<application::FileAreaId>& areas,std::int64_t fence,std::int64_t since){
    if(areas.empty()||areas.size()>999||fence<0||!application::valid_utc_seconds(since))return invalid("validate completed scan scope");
    std::vector<std::int64_t> ids;for(const auto area:areas){if(area.value<1||std::find(ids.begin(),ids.end(),area.value)!=ids.end())return invalid("validate unique scan areas");ids.push_back(area.value);}
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate scan completion clock");
    return impl_->write("record completed scope without skipping other areas",2048,[a,areas,fence,since,now](sqlite3* db){
        const auto auth=file_detail::authority(db,a);if(fence>file_detail::publication_fence(db))return SQLITE_ABORT;
        for(const auto area:areas){
            file_detail::visible_scan_area(db,area,auth);
            // Pending native operations remain unacknowledged: they must not
            // disappear behind a completed fence while temporarily hidden.
            SqliteStatement unseen(db,"SELECT 1 FROM sfx_file_catalog f WHERE area_id=?1 AND catalog_state=0 AND (sysop_only=0 OR ?2=1) AND published_utc>=?3 AND publication_sequence<=?4 AND NOT EXISTS(SELECT 1 FROM sfx_file_scan_seen s WHERE s.account_id=?5 AND s.file_id=f.file_id AND s.publication_sequence=f.publication_sequence) LIMIT 1","refuse completion with unseen publications");unseen.bind(1,area.value);unseen.bind(2,auth.sysop());unseen.bind(3,since);unseen.bind(4,fence);unseen.bind(5,a.account);if(unseen.row())return SQLITE_ABORT;
        }
        const auto prior=file_detail::caller_state(db,a.account);if(prior.revision==INT64_MAX)return SQLITE_FULL;
        SqliteStatement q(db,"INSERT INTO sfx_file_caller_state VALUES(?1,?2,?3,?4,?5,?6,?7,?8) ON CONFLICT(account_id) DO UPDATE SET scan_completed_utc=excluded.scan_completed_utc,revision=excluded.revision","completion date is display metadata, never a global exclusion cutoff");q.bind(1,a.account);q.bind(2,prior.since_utc);q.bind(3,prior.last_seen_publication);if(prior.current_area.value)q.bind(4,prior.current_area.value);else q.bind_null(4);q.bind(5,prior.lightbar);q.bind(6,prior.new_file_scan);q.bind(7,prior.revision+1);q.bind(8,now);q.step();return SQLITE_OK;
    });
}
application::StorageResult<application::FileChangeOutcome> SqliteAccountRepository::set_file_reupload_block(const application::FileConfigurationActor& a,application::Identifier128 operation,application::FileId id,std::int64_t revision,bool blocked){
    if(!operation.valid()||id.value<1||revision<1)return read_failure<application::FileChangeOutcome>(invalid("validate explicit re-upload policy action"));
    file_detail::Fingerprint f("reupload-policy");f.actor(a);f.number(id.value);f.number(revision);f.number(blocked);const auto fingerprint=f.value();
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::FileChangeOutcome>(invalid("validate re-upload policy clock"));
    return impl_->write_result<application::FileChangeOutcome>("change explicit content block without changing payload",4096,[a,operation,id,revision,blocked,fingerprint,now](sqlite3* db,auto& output){
        message_detail::config_authority(db,a);if(auto prior=file_detail::receipt(db,a,operation,fingerprint)){output=*prior;return SQLITE_OK;}
        const auto source=file_detail::entry(db,id);if(source.file.revision!=revision)return SQLITE_ABORT;
        SqliteStatement prior(db,"SELECT revision FROM sfx_file_reupload_blocks WHERE content_sha256=?1 AND byte_count=?2","retain independent content policy revision");prior.bind(1,source.payload.sha256);prior.bind(2,static_cast<std::int64_t>(source.payload.bytes));const auto previous=prior.row()?prior.int64(0):0;if(previous==INT64_MAX)return SQLITE_FULL;
        SqliteStatement q(db,"INSERT INTO sfx_file_reupload_blocks VALUES(?1,?2,?3,?4,?5,?6) ON CONFLICT(content_sha256,byte_count) DO UPDATE SET blocked=excluded.blocked,revision=excluded.revision,source_file_id=excluded.source_file_id,changed_utc=excluded.changed_utc","explicit Sysop block/unblock, no removal side effects");q.bind(1,source.payload.sha256);q.bind(2,static_cast<std::int64_t>(source.payload.bytes));q.bind(3,blocked);q.bind(4,previous+1);q.bind(5,id.value);q.bind(6,now);q.step();
        const application::FileChangeOutcome result{previous+1,id.value,false};file_detail::save_receipt(db,a,operation,fingerprint,result,now);file_detail::audit(db,a,operation,blocked?"file.reupload.blocked":"file.reupload.unblocked",id,source.file.area,now);output=result;return SQLITE_OK;
    });
}
