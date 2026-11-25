application::StorageResult<application::FileDownloadStart> SqliteAccountRepository::reserve_file_download(const application::FileActor& a,application::Identifier128 operation,application::FileId id,std::string_view date) {
    if(!operation.valid() || id.value<1 || !application::valid_iso_civil_date(date))return read_failure<application::FileDownloadStart>(invalid("validate file download reservation"));
    file_detail::Fingerprint f("download");f.number(a.account);f.number(id.value);f.text(date);const auto fingerprint=f.value();const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::FileDownloadStart>(invalid("validate download clock"));
    return impl_->write_result<application::FileDownloadStart>("reserve one authorized file allowance",4096,[a,operation,id,date=std::string(date),fingerprint,now](sqlite3* db,auto& output){
        const auto auth=file_detail::authority(db,a);
        SqliteStatement old(db,"SELECT request_fingerprint,account_id,session_id,direction,file_id,state,byte_count,content_sha256,file_revision FROM sfx_file_transfers WHERE operation_id=?1","reconcile download reservation");old.bind(1,operation);
        if(old.row()){
            if(file_detail::digest(old,0)!=fingerprint || old.int64(1)!=a.account || old.int64(3)!=0 || old.int64(4)!=id.value || old.int64(5)==2 ||
                (old.int64(5)==0 && old.identifier(2)!=a.session))return SQLITE_ABORT;
            const auto native=file_detail::visible_entry(db,id,auth,true);
            if(native.payload.bytes!=static_cast<std::uint64_t>(old.int64(6)) || native.payload.sha256!=file_detail::digest(old,7) || native.file.revision!=old.int64(8))return SQLITE_ABORT;
            output=application::FileDownloadStart{application::FileDownloadAllowance::allowed,application::FileDownloadReservation{operation,native.file,native.payload,date,true,old.int64(5)==1}};return SQLITE_OK;
        }
        const auto native=file_detail::visible_entry(db,id,auth,true);const auto area=file_detail::area(db,native.file.area).area;
        const auto stats=file_detail::statistics(db,a,auth,date);
        const auto allowance=application::file_download_allowance(stats.rule,stats.counts,area.ratio_downloads,stats.daily_exempt,stats.ratio_exempt);
        if(allowance!=application::FileDownloadAllowance::allowed){output=application::FileDownloadStart{allowance,{}};return SQLITE_OK;}
        const bool eligible=area.ratio_downloads && !stats.ratio_exempt;
        SqliteStatement reserve(db,"INSERT INTO sfx_file_transfers VALUES(?1,?2,?3,?4,0,?5,?6,?7,0,?8,?9,?10,?11,?12,NULL)","reserve without incrementing success counters");
        reserve.bind(1,operation);reserve.bind(2,fingerprint);reserve.bind(3,a.account);reserve.bind(4,a.session);reserve.bind(5,id.value);reserve.bind(6,area.id.value);reserve.bind(7,native.file.revision);reserve.bind(8,static_cast<std::int64_t>(native.payload.bytes));reserve.bind(9,native.payload.sha256);reserve.bind(10,eligible);reserve.bind(11,date);reserve.bind(12,now);reserve.step();
        output=application::FileDownloadStart{allowance,application::FileDownloadReservation{operation,native.file,native.payload,date,false,false}};return SQLITE_OK;
    });
}
application::StorageResult<application::FileTransferOutcome> SqliteAccountRepository::finish_file_download(const application::FileActor& a,application::Identifier128 operation,const application::FilePayloadIdentity& payload,bool success) {
    if(!operation.valid() || (success && !file_detail::payload_valid(payload)))return read_failure<application::FileTransferOutcome>(invalid("validate native download completion"));
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::FileTransferOutcome>(invalid("validate download completion clock"));
    return impl_->write_result<application::FileTransferOutcome>("commit exact download outcome once",4096,[a,operation,payload,success,now](sqlite3* db,auto& output){
        const auto auth=file_detail::authority(db,a);
        SqliteStatement transfer(db,"SELECT account_id,session_id,direction,file_id,area_id,file_revision,state,byte_count,content_sha256,ratio_participating FROM sfx_file_transfers WHERE operation_id=?1","verify owned transfer completion");transfer.bind(1,operation);if(!transfer.row() || transfer.int64(0)!=a.account || transfer.int64(2)!=0)return SQLITE_NOTFOUND;
        if(transfer.int64(6)!=0){
            const bool completed=transfer.int64(6)==1;
            if(completed!=success || (success && (payload.bytes!=static_cast<std::uint64_t>(transfer.int64(7)) || payload.sha256!=file_detail::digest(transfer,8))))return SQLITE_ABORT;
            output=application::FileTransferOutcome{completed,true};return SQLITE_OK;
        }
        if(transfer.identifier(1)!=a.session)return SQLITE_NOTFOUND;
        const application::FileId id{transfer.int64(3)};const application::FileAreaId area{transfer.int64(4)};
        if(success){
            const auto native=file_detail::visible_entry(db,id,auth,true);
            if(native.file.revision!=transfer.int64(5) || !(native.file.area==area) || !file_detail::same_payload(native.payload,payload) ||
                payload.bytes!=static_cast<std::uint64_t>(transfer.int64(7)) || payload.sha256!=file_detail::digest(transfer,8))return SQLITE_ABORT;
            SqliteStatement stats(db,"INSERT INTO sfx_file_statistics VALUES(?1,0,1,0,?2,0,?3) ON CONFLICT(account_id) DO UPDATE SET download_count=download_count+1,download_bytes=download_bytes+excluded.download_bytes,eligible_downloads=eligible_downloads+excluded.eligible_downloads","success-only caller download statistics");stats.bind(1,a.account);stats.bind(2,static_cast<std::int64_t>(payload.bytes));stats.bind(3,static_cast<std::int64_t>(transfer.boolean(9)?1:0));stats.step();
            SqliteStatement popularity(db,"UPDATE sfx_file_catalog SET download_count=download_count+1 WHERE file_id=?1","success-only file popularity");popularity.bind(1,id.value);popularity.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        }
        SqliteStatement finish(db,"UPDATE sfx_file_transfers SET state=?1,finished_utc=?2 WHERE operation_id=?3 AND state=0","commit terminal transfer receipt");finish.bind(1,static_cast<std::int64_t>(success?1:2));finish.bind(2,now);finish.bind(3,operation);finish.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        file_detail::audit(db,{false,a},operation,success?"download.completed":"download.failed",id,area,now);output=application::FileTransferOutcome{success,false};return SQLITE_OK;
    });
}
application::StorageStatus SqliteAccountRepository::recover_file_transfers(const application::FileConfigurationActor& a) {
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return invalid("validate file recovery clock");
    return impl_->write("release only interrupted file reservations",4096,[a,now](sqlite3* db){
        message_detail::config_authority(db,a);
        // Runtime invokes after ordinary session recovery. Never cancel a live
        // session's reservation or manufacture a successful transfer on restart.
        SqliteStatement q(db,"UPDATE sfx_file_transfers SET state=2,finished_utc=?1 WHERE state=0 AND session_id IN(SELECT session_id FROM sfx_account_sessions WHERE completed_utc_seconds IS NOT NULL)","cancel dead-session transfers without accounting");q.bind(1,now);q.step();return SQLITE_OK;
    });
}
