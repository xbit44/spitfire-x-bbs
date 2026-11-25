namespace message_detail {
bool valid_scan_column(app::MessageScanColumn c){return c==app::MessageScanColumn::unread || c==app::MessageScanColumn::to_you;}
app::MessageGroupScanPreview scan_preview(sqlite3* db,const app::MessageActor& caller,app::MessageGroupId group,app::MessageScanColumn column) {
    const auto a=actor(db,caller);app::MessageGroupScanPreview result;result.group=group;result.column=column;result.configuration_revision=catalog_revision(db);
    SqliteStatement g(db,"SELECT revision FROM sfx_message_groups WHERE group_id=?1 AND enabled=1 AND access_minimum<=?2","verify scan group");g.bind(1,group.value);g.bind(2,a.level);if(!g.row())unavailable();result.group_revision=g.int64(0);
    SqliteStatement q(db,"SELECT echo_id,conference_number,name,display_order,revision,access_minimum,reading_minimum,posting_minimum FROM sfx_local_echoes WHERE group_id=?1 AND enabled=1 AND access_minimum<=?2 AND reading_minimum<=?2 ORDER BY display_order,echo_id LIMIT 1000","preview exact eligible scan set");q.bind(1,group.value);q.bind(2,a.level);
    while(q.row()) {
        if(result.entries.size()==999)unavailable();
        app::LocalEcho echo{{q.int64(0)},group,static_cast<std::uint16_t>(q.int64(1)),q.text(2),q.int64(3),q.int64(4),{static_cast<std::uint16_t>(q.int64(5)),static_cast<std::uint16_t>(q.int64(6)),static_cast<std::uint16_t>(q.int64(7))},true};
        echo.conference_number=visible_conference_number(db,echo.id,a);
        auto pref=read_preference(db,caller,echo.id);const bool on=column==app::MessageScanColumn::unread?pref.unread:pref.to_you;
        result.enable=result.enable || !on;result.entries.push_back({std::move(echo),pref});
    }
    return result;
}
std::array<std::uint8_t,32> scan_fingerprint(const app::MessageActor& caller,const app::MessageGroupScanPreview& p) {
    std::string bytes="SFX/GroupScanToggle/v1";const auto n=[&](std::int64_t value){for(int shift=56;shift>=0;shift-=8)bytes.push_back(static_cast<char>(static_cast<std::uint64_t>(value)>>shift));};
    n(caller.account);n(p.group.value);n(p.configuration_revision);n(p.group_revision);n(static_cast<int>(p.column));n(p.enable);n(static_cast<std::int64_t>(p.entries.size()));
    // Progress is deliberately not a preference revision or overwrite target.
    for(const auto& e:p.entries){n(e.echo.id.value);n(e.echo.revision);n(e.preference.revision);n(e.preference.unread);n(e.preference.to_you);}
    std::array<std::uint8_t,32> digest{};
    if(sodium_init()<0 || crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(bytes.data()),bytes.size())!=0)throw StorageFailure(StorageErrorCode::internal,"fingerprint scan toggle",SQLITE_ERROR);
    return digest;
}
}
application::StorageResult<application::MessageGroupScanPreview> SqliteAccountRepository::preview_group_scan_column(
    const application::MessageActor& caller,application::MessageGroupId group,application::MessageScanColumn column) {
    if(group.value<=0 || !message_detail::valid_scan_column(column))return read_failure<application::MessageGroupScanPreview>(invalid("validate group scan preview"));
    return impl_->readers_.read<application::MessageGroupScanPreview>("preview group scan column",[caller,group,column](sqlite3* db){message_detail::ReadSnapshot snapshot(db);return message_detail::scan_preview(db,caller,group,column);});
}
application::StorageResult<bool> SqliteAccountRepository::toggle_group_scan_column(
    const application::MessageActor& caller,application::Identifier128 operation,const application::MessageGroupScanPreview& preview) {
    if(!operation.valid() || preview.group.value<=0 || preview.entries.size()>999 || preview.group_revision<=0 || preview.configuration_revision<=0 || !message_detail::valid_scan_column(preview.column))return read_failure<bool>(invalid("validate group scan toggle"));
    const auto digest=message_detail::scan_fingerprint(caller,preview);
    return impl_->write_result<bool>("apply atomic group scan column",4096+preview.entries.size()*64,[caller,operation,preview,digest](sqlite3* db,auto& output){
        message_detail::actor(db,caller);
        SqliteStatement receipt(db,"SELECT account_id,request_fingerprint,enabled FROM sfx_message_scan_receipts WHERE operation_id=?1","reconcile group scan toggle");receipt.bind(1,operation);
        if(receipt.row()){const auto stored=receipt.blob(1);if(receipt.int64(0)!=caller.account || stored.size()!=digest.size() || sodium_memcmp(stored.data(),digest.data(),digest.size())!=0)return SQLITE_ABORT;output=receipt.boolean(2);return SQLITE_OK;}
        const auto current=message_detail::scan_preview(db,caller,preview.group,preview.column);
        if(current.configuration_revision!=preview.configuration_revision || current.group_revision!=preview.group_revision || current.enable!=preview.enable || current.entries.size()!=preview.entries.size())return SQLITE_ABORT;
        for(std::size_t i=0;i<current.entries.size();++i) {
            const auto& now=current.entries[i];const auto& old=preview.entries[i];
            if(!(now.echo.id==old.echo.id) || now.echo.revision!=old.echo.revision || now.preference.revision!=old.preference.revision || now.preference.unread!=old.preference.unread || now.preference.to_you!=old.preference.to_you)return SQLITE_ABORT;
        }
        for(const auto& item:current.entries){
            message_detail::preference(db,caller,item.echo.id);
            SqliteStatement change(db,preview.column==application::MessageScanColumn::unread?"UPDATE sfx_caller_echo_state SET unread_enabled=?1,preference_revision=preference_revision+1 WHERE account_id=?2 AND echo_id=?3 AND preference_revision=?4":"UPDATE sfx_caller_echo_state SET to_you_enabled=?1,preference_revision=preference_revision+1 WHERE account_id=?2 AND echo_id=?3 AND preference_revision=?4","CAS group scan column");
            change.bind(1,preview.enable);change.bind(2,caller.account);change.bind(3,item.echo.id.value);change.bind(4,item.preference.revision);change.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        }
        SqliteStatement saved(db,"INSERT INTO sfx_message_scan_receipts VALUES(?1,?2,?3,?4)","retain group scan operation receipt");saved.bind(1,operation);saved.bind(2,caller.account);saved.bind(3,digest);saved.bind(4,preview.enable);saved.step();
        output=preview.enable;return SQLITE_OK;
    });
}
