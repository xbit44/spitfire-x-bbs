namespace file_detail {
namespace app = application;
[[noreturn]] void unavailable() { throw StorageFailure(StorageErrorCode::not_found,"file unavailable",SQLITE_NOTFOUND); }
[[noreturn]] void conflict() { throw StorageFailure(StorageErrorCode::conflict,"refresh file context",SQLITE_ABORT); }
[[noreturn]] void corrupt() { throw StorageFailure(StorageErrorCode::corrupt,"invalid durable file state",SQLITE_CORRUPT); }
app::FileAuthority authority(sqlite3* db,const app::FileActor& actor) {
    const auto current=message_detail::actor(db,actor);
    return {static_cast<std::uint16_t>(current.level),static_cast<std::uint16_t>(current.threshold)};
}
std::string key(std::string value) {
    const auto folded=app::file_case_key(value);if(!folded)conflict();return *folded;
}
bool group_deletions_available(sqlite3* db){SqliteStatement q(db,"SELECT 1 FROM sqlite_schema WHERE type='table' AND name='sfx_file_group_deletions'","detect retained Group deletion capability");return q.row();}
bool ftn_publications_available(sqlite3* db){
    // Exact predecessor qualification still uses this repository at v21.
    SqliteStatement q(db,"SELECT 1 FROM sqlite_schema WHERE type='table' AND name='sfx_ftn_file_publications'","detect current FTN publication capability");return q.row();
}
bool ftn_file_pending(sqlite3* db,app::FileId file){
    if(!ftn_publications_available(db))return false;
    SqliteStatement q(db,"SELECT 1 FROM sfx_ftn_file_publications WHERE displaced_file=?1 AND phase<>6 LIMIT 1","retain FTN displacement reservation");q.bind(1,file.value);return q.row();
}
bool ftn_area_pending(sqlite3* db,app::FileAreaId area){
    if(!ftn_publications_available(db))return false;
    SqliteStatement q(db,"SELECT 1 FROM sfx_ftn_file_publications WHERE area_id=?1 AND phase<>6 LIMIT 1","retain FTN publication path binding");q.bind(1,area.value);return q.row();
}
bool name_reserved(sqlite3* db,app::FileAreaId area_id,std::string_view name,app::Identifier128 except={},bool upload_retry=false){
    if(ftn_publications_available(db)){SqliteStatement ftn(db,"SELECT 1 FROM sfx_ftn_file_publications WHERE area_id=?1 AND (filename_key=?2 OR stage_name=?2 COLLATE NOCASE) AND phase<>6 AND operation_id<>?3 LIMIT 1","reserve exact friendly and staged FTN names");ftn.bind(1,area_id.value);ftn.bind(2,key(std::string(name)));ftn.bind(3,except);if(ftn.row())return true;}
    // A failed receive owns only its private staging object, not the final
    // friendly filename for a NEW verified upload. Keep the broader hold for
    // Sysop import/management, which must not adopt unaccepted receive data.
    // Prepared intents and held ACTUAL placements always block all paths.
    SqliteStatement q(db,"SELECT 1 FROM sfx_file_intake i WHERE placement_area_id=?1 AND (filename_key=?2 OR filename_key=':'||?2) AND (state=2 OR (state=5 AND (?4=0 OR EXISTS(SELECT 1 FROM sfx_file_intake_objects o WHERE o.operation_id=i.operation_id AND o.placed_volume<>'')))) AND operation_id<>?3 UNION ALL SELECT 1 FROM sfx_file_operations WHERE destination_area_id=?1 AND destination_key=?2 AND state IN(0,2) AND operation_id<>?3 UNION ALL SELECT 1 FROM sfx_file_imports WHERE area_id=?1 AND filename_key=?2 AND state IN(0,2) AND operation_id<>?3 LIMIT 1","preserve held import protection; only uploads may retry an unplaced receive");q.bind(1,area_id.value);q.bind(2,key(std::string(name)));q.bind(3,except);q.bind(4,upload_retry);return q.row();
}
std::array<std::uint8_t,32> digest(SqliteStatement& q,int index) {
    const auto bytes=q.blob(index);if(bytes.size()!=32)corrupt();
    std::array<std::uint8_t,32> result{};std::copy(bytes.begin(),bytes.end(),result.begin());return result;
}
struct Fingerprint {
    std::string bytes;
    explicit Fingerprint(std::string_view kind):bytes("SFX/File/v1/") { text(kind); }
    void number(std::int64_t n) { for(int shift=56;shift>=0;shift-=8)bytes+=static_cast<char>(static_cast<std::uint64_t>(n)>>shift); }
    void text(std::string_view t) { number(static_cast<std::int64_t>(t.size()));bytes.append(t); }
    void actor(const app::FileConfigurationActor& a) { number(a.installation_owner);number(a.caller?a.caller->account:0); }
    void counts(const app::FileCountRule& r) { number(r.daily_downloads.value_or(0));number(r.ratio?r.ratio->downloads:0);number(r.ratio?r.ratio->uploads:0); }
    void payload(const app::FilePayloadIdentity& p) { number(static_cast<std::int64_t>(p.bytes));text({reinterpret_cast<const char*>(p.sha256.data()),p.sha256.size()});text(p.volume);text(p.file_id); }
    std::array<std::uint8_t,32> value() const { return message_detail::source_digest(bytes); }
};
app::FileCountRule counts(SqliteStatement& q,int first) {
    app::FileCountRule result;
    if(!q.is_null(first))result.daily_downloads=static_cast<std::uint32_t>(q.int64(first));
    if(!q.is_null(first+1))result.ratio=app::FileRatio{static_cast<std::uint32_t>(q.int64(first+1)),static_cast<std::uint32_t>(q.int64(first+2))};
    if(q.is_null(first+1)!=q.is_null(first+2) || !app::valid_file_count_rule(result))corrupt();
    return result;
}
void bind_counts(SqliteStatement& q,int first,const app::FileCountRule& r) {
    if(r.daily_downloads)q.bind(first,static_cast<std::int64_t>(*r.daily_downloads));else q.bind_null(first);
    if(r.ratio){q.bind(first+1,static_cast<std::int64_t>(r.ratio->downloads));q.bind(first+2,static_cast<std::int64_t>(r.ratio->uploads));}
    else{q.bind_null(first+1);q.bind_null(first+2);}
}
app::FilePolicy policy(sqlite3* db) {
    SqliteStatement q(db,"SELECT revision,catalog_revision,maximum_upload_bytes,daily_downloads,ratio_downloads,ratio_uploads,lightbar_default,scan_uploads FROM sfx_file_policy WHERE singleton_id=1","read file policy");
    if(!q.row())corrupt();
    app::FilePolicy p;
    p.revision=q.int64(0);p.catalog_revision=q.int64(1);p.maximum_upload_bytes=static_cast<std::uint64_t>(q.int64(2));p.counts=counts(q,3);p.lightbar_default=q.boolean(6);p.scan_uploads=q.boolean(7);
    if(!app::valid_file_policy(p))corrupt();
    return p;
}
app::FileGroup group(sqlite3* db,app::FileGroupId id) {
    SqliteStatement q(db,"SELECT group_id,name,display_order,revision,access_minimum,enabled FROM sfx_file_groups WHERE group_id=?1","read file group");q.bind(1,id.value);if(!q.row())unavailable();
    app::FileGroup g{{q.int64(0)},q.text(1),q.int64(2),q.int64(3),static_cast<std::uint16_t>(q.int64(4)),q.boolean(5)};
    if(group_deletions_available(db)){SqliteStatement deletion(db,"SELECT operation_id,delete_disk_files,completed FROM sfx_file_group_deletions WHERE group_id=?1","read retained File Group deletion");deletion.bind(1,id.value);
    if(deletion.row()){if(deletion.boolean(2))unavailable();g.deletion_operation=deletion.identifier(0);g.delete_disk_files=deletion.boolean(1);}}
    if(!app::valid_file_group(g))corrupt();
    return g;
}
app::FileAreaConfiguration area(sqlite3* db,app::FileAreaId id) {
    SqliteStatement q(db,"SELECT a.area_id,a.group_id,a.area_number,a.name,a.description,a.display_order,a.kind,a.access_minimum,a.upload_minimum,a.download_minimum,a.uploads_enabled,a.downloads_sysop_only,a.enabled,a.own_description_edit,a.backup_contents,a.ratio_uploads,a.ratio_downloads,a.maximum_upload_bytes,a.revision,p.storage_path,a.sysop_review FROM sfx_file_areas a JOIN sfx_file_area_paths p USING(area_id) WHERE a.area_id=?1","read native file area");
    q.bind(1,id.value);if(!q.row())unavailable();app::FileAreaConfiguration result;auto& a=result.area;
    a.id={q.int64(0)};a.group={q.int64(1)};a.number=static_cast<std::uint16_t>(q.int64(2));a.name=q.text(3);a.description=q.text(4);a.order=q.int64(5);a.kind=static_cast<app::FileAreaKind>(q.int64(6));
    a.permissions={static_cast<std::uint16_t>(q.int64(7)),static_cast<std::uint16_t>(q.int64(8)),static_cast<std::uint16_t>(q.int64(9)),q.boolean(10),q.boolean(11)};
    a.enabled=q.boolean(12);a.own_description_edit=q.boolean(13);a.backup_contents=q.boolean(14);a.ratio_uploads=q.boolean(15);a.ratio_downloads=q.boolean(16);
    if(!q.is_null(17))a.maximum_upload_bytes=static_cast<std::uint64_t>(q.int64(17));
    a.revision=q.int64(18);result.storage_path=q.text(19);a.sysop_review=q.boolean(20);
    if(!app::valid_file_area(a) || result.storage_path.empty() || result.storage_path.size()>2048)corrupt();
    return result;
}
struct NativeEntry { app::FileEntry file;app::FilePayloadIdentity payload;std::string storage_key; };
NativeEntry entry(sqlite3* db,app::FileId id) {
    SqliteStatement q(db,"SELECT file_id,area_id,revision,filename,short_description,long_description,public_uploader,uploader_account_id,byte_count,content_sha256,native_volume,native_file_id,original_upload_utc,published_utc,publication_sequence,download_count,origin,catalog_state,sysop_only,filename_key FROM sfx_file_catalog WHERE file_id=?1","read file metadata");q.bind(1,id.value);if(!q.row())unavailable();
    NativeEntry result;auto& f=result.file;
    f.id={q.int64(0)};f.area={q.int64(1)};f.revision=q.int64(2);f.name=q.text(3);f.descriptions={q.text(4),q.text(5)};f.public_uploader=q.text(6);
    if(!q.is_null(7))f.uploader=q.int64(7);
    f.bytes=static_cast<std::uint64_t>(q.int64(8));f.sha256=digest(q,9);
    result.payload={f.bytes,f.sha256,q.text(10),q.text(11)};f.uploaded_utc=q.int64(12);f.published_utc=q.int64(13);f.publication_sequence=q.int64(14);f.downloads=static_cast<std::uint64_t>(q.int64(15));
    f.origin=static_cast<app::FileOrigin>(q.int64(16));f.state=static_cast<app::FileCatalogState>(q.int64(17));f.sysop_only=q.boolean(18);result.storage_key=q.text(19);
    if(!app::valid_file_name(f.name) || !app::valid_file_descriptions(f.descriptions) || f.revision<1 || f.origin>app::FileOrigin::ftn)corrupt();
    if(f.origin==app::FileOrigin::ftn){
        if(f.uploader)corrupt();
        SqliteStatement provenance(db,"SELECT r.payload_sha256,r.payload_bytes FROM sfx_ftn_file_lineage l JOIN sfx_ftn_file_receipts r ON r.operation_id=l.receipt_operation JOIN sfx_ftn_ingress t ON t.receipt_id=r.tic_receipt JOIN sfx_ftn_ingress p ON p.receipt_id=r.payload_receipt WHERE l.file_id=?1 AND t.content_sha256=r.tic_sha256 AND p.content_sha256=r.payload_sha256 AND p.declared_size=r.payload_bytes AND ((l.source_file_id IS NULL AND r.initial_file_id=l.file_id) OR (l.source_file_id IS NOT NULL AND EXISTS(SELECT 1 FROM sfx_ftn_file_lineage s JOIN sfx_file_operations o ON o.operation_id=l.copy_operation WHERE s.file_id=l.source_file_id AND s.receipt_operation=l.receipt_operation AND o.action=2 AND o.file_id=l.source_file_id AND o.result_file_id=l.file_id AND o.state=1)))","verify immutable received or derived FTN content lineage");provenance.bind(1,f.id.value);
        if(!provenance.row()||digest(provenance,0)!=f.sha256||provenance.int64(1)!=static_cast<std::int64_t>(f.bytes))corrupt();
    }else if(f.public_uploader.empty())corrupt();return result;
}
NativeEntry visible_entry(sqlite3* db,app::FileId id,app::FileAuthority a,bool downloading=false) {
    if(ftn_file_pending(db,id))unavailable();
    auto result=entry(db,id);const auto ar=area(db,result.file.area).area;const auto g=group(db,ar.group);
    if(!app::file_entry_visible(g,ar,result.file,a) || (downloading && !app::file_area_downloadable(g,ar,a)))unavailable();
    SqliteStatement pending(db,"SELECT 1 FROM sfx_file_operations WHERE file_id=?1 AND state IN(0,2)","exclude unresolved native mutation");pending.bind(1,id.value);if(pending.row())unavailable();return result;
}
bool payload_valid(const app::FilePayloadIdentity& p) {
    return p.bytes<=INT64_MAX && !p.volume.empty() && p.volume.size()<=128 && !p.file_id.empty() && p.file_id.size()<=128 &&
        std::any_of(p.sha256.begin(),p.sha256.end(),[](auto b){return b!=0;});
}
bool same_payload(const app::FilePayloadIdentity& a,const app::FilePayloadIdentity& b) {
    return a.bytes==b.bytes && a.sha256==b.sha256 && a.volume==b.volume && a.file_id==b.file_id;
}
bool reupload_blocked(sqlite3* db,const app::FilePayloadIdentity& payload){
    SqliteStatement q(db,"SELECT blocked FROM sfx_file_reupload_blocks WHERE content_sha256=?1 AND byte_count=?2","check explicit content re-upload policy");q.bind(1,payload.sha256);q.bind(2,static_cast<std::int64_t>(payload.bytes));return q.row()&&q.boolean(0);
}
std::optional<app::FileChangeOutcome> receipt(sqlite3* db,const app::FileConfigurationActor& a,app::Identifier128 operation,const std::array<std::uint8_t,32>& fingerprint) {
    SqliteStatement q(db,"SELECT request_fingerprint,actor_account_id,installation_owner,resulting_revision,resulting_id FROM sfx_file_change_receipts WHERE operation_id=?1","reconcile file change");q.bind(1,operation);if(!q.row())return {};
    if(digest(q,0)!=fingerprint || q.boolean(2)!=a.installation_owner || (a.caller?q.is_null(1) || q.int64(1)!=a.caller->account:!q.is_null(1)))conflict();
    return app::FileChangeOutcome{q.int64(3),q.int64(4),true};
}
void save_receipt(sqlite3* db,const app::FileConfigurationActor& a,app::Identifier128 operation,const std::array<std::uint8_t,32>& fingerprint,const app::FileChangeOutcome& result,std::int64_t now) {
    SqliteStatement q(db,"INSERT INTO sfx_file_change_receipts VALUES(?1,?2,?3,?4,?5,?6,?7)","retain file change receipt");q.bind(1,operation);q.bind(2,fingerprint);if(a.caller)q.bind(3,a.caller->account);else q.bind_null(3);q.bind(4,a.installation_owner);q.bind(5,result.revision);q.bind(6,result.identity);q.bind(7,now);q.step();
}
void audit(sqlite3* db,const app::FileConfigurationActor& a,app::Identifier128 operation,std::string_view kind,app::FileId file,app::FileAreaId area_id,std::int64_t now,std::string_view detail="") {
    SqliteStatement q(db,"INSERT INTO sfx_file_audit(operation_id,actor_account_id,installation_owner,event_kind,file_id,area_id,occurred_utc,detail) VALUES(?1,?2,?3,?4,?5,?6,?7,?8)","audit file operation");
    q.bind(1,operation);if(a.caller)q.bind(2,a.caller->account);else q.bind_null(2);q.bind(3,a.installation_owner);q.bind(4,kind);if(file.value)q.bind(5,file.value);else q.bind_null(5);if(area_id.value)q.bind(6,area_id.value);else q.bind_null(6);q.bind(7,now);
    // SQLite interprets a null text pointer as SQL NULL, even at length zero.
    // Optional audit detail is empty TEXT, not a missing required field.
    q.bind(8,detail.empty()?std::string_view(""):detail);q.step();
}
std::int64_t publication_fence(sqlite3* db) {
    SqliteStatement q(db,"SELECT coalesce(max(publication_sequence),0) FROM sfx_file_publications","capture publication fence");if(!q.row())corrupt();return q.int64(0);
}
app::FileCallerState caller_state(sqlite3* db,app::AccountId account) {
    SqliteStatement q(db,"SELECT scan_since_utc,last_seen_publication,current_area_id,lightbar,new_file_scan,revision,scan_completed_utc FROM sfx_file_caller_state WHERE account_id=?1","read file caller state");q.bind(1,account);
    if(q.row()){app::FileCallerState result{q.int64(0),q.int64(1),q.int64(5),{q.is_null(2)?0:q.int64(2)},q.boolean(3),q.boolean(4),{}};if(!q.is_null(6))result.scan_completed_utc=q.int64(6);return result;}
    SqliteStatement joined(db,"SELECT created_utc_seconds FROM sfx_accounts WHERE account_id=?1","seed new-file checkpoint from caller join date");joined.bind(1,account);if(!joined.row())unavailable();
    app::FileCallerState result;result.since_utc=joined.int64(0);result.lightbar=policy(db).lightbar_default;return result;
}
app::FileCallerStatistics statistics(sqlite3* db,const app::FileActor& a,app::FileAuthority auth,std::string_view date) {
    app::FileCallerStatistics result;result.rule=policy(db).counts;
    SqliteStatement rules(db,"SELECT daily_downloads,ratio_downloads,ratio_uploads,daily_exempt,ratio_exempt FROM sfx_file_security_rules WHERE security_level=?1","read explicit level file policy");rules.bind(1,auth.level);
    if(rules.row()){result.rule=counts(rules,0);result.daily_exempt=rules.boolean(3);result.ratio_exempt=rules.boolean(4);}
    SqliteStatement exempt(db,"SELECT daily_exempt,ratio_exempt FROM sfx_file_caller_exemptions WHERE account_id=?1","read caller file exemptions");exempt.bind(1,a.account);
    if(exempt.row()){result.daily_exempt=result.daily_exempt||exempt.boolean(0);result.ratio_exempt=result.ratio_exempt||exempt.boolean(1);}
    SqliteStatement s(db,"SELECT upload_count,download_count,upload_bytes,download_bytes,eligible_uploads,eligible_downloads FROM sfx_file_statistics WHERE account_id=?1","read success-only file statistics");s.bind(1,a.account);
    if(s.row()){result.counts.successful_uploads=static_cast<std::uint64_t>(s.int64(0));result.counts.successful_downloads=static_cast<std::uint64_t>(s.int64(1));result.upload_bytes=static_cast<std::uint64_t>(s.int64(2));result.download_bytes=static_cast<std::uint64_t>(s.int64(3));result.counts.eligible_uploads=static_cast<std::uint64_t>(s.int64(4));result.counts.eligible_downloads=static_cast<std::uint64_t>(s.int64(5));}
    SqliteStatement transfers(db,"SELECT coalesce(sum(state=1 AND bbs_local_date=?2),0),coalesce(sum(state=0),0),coalesce(sum(state=0 AND ratio_participating=1),0),coalesce(sum(state=0 AND bbs_local_date=?2),0) FROM sfx_file_transfers WHERE account_id=?1 AND direction=0 AND state IN(0,1)","read committed daily count and concurrent reservations");transfers.bind(1,a.account);transfers.bind(2,date);if(!transfers.row())corrupt();
    result.counts.today_downloads=static_cast<std::uint64_t>(transfers.int64(0));result.counts.reserved_downloads=static_cast<std::uint64_t>(transfers.int64(1));result.counts.reserved_eligible_downloads=static_cast<std::uint64_t>(transfers.int64(2));result.counts.reserved_today_downloads=static_cast<std::uint64_t>(transfers.int64(3));return result;
}
} // namespace file_detail

application::StorageResult<application::FileCatalog> SqliteAccountRepository::file_catalog(const application::FileActor& a) {
    return impl_->readers_.read<application::FileCatalog>("read accessible file catalog",[a](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto auth=file_detail::authority(db,a);application::FileCatalog result;result.revision=file_detail::policy(db).catalog_revision;
        SqliteStatement groups(db,"SELECT group_id FROM sfx_file_groups WHERE enabled=1 AND access_minimum<=?1 ORDER BY display_order,group_id LIMIT 1000","list accessible file groups");groups.bind(1,auth.level);
        while(groups.row()){if(result.groups.size()>=999)file_detail::corrupt();result.groups.push_back(file_detail::group(db,{groups.int64(0)}));}
        for(const auto& g:result.groups){SqliteStatement areas(db,"SELECT area_id FROM sfx_file_areas WHERE group_id=?1 ORDER BY display_order,area_id","list visible file areas");areas.bind(1,g.id.value);
            while(areas.row()){auto area=file_detail::area(db,{areas.int64(0)}).area;if(application::file_area_visible(g,area,auth))result.areas.push_back(std::move(area));}}
        return result;
    });
}
application::StorageResult<application::FileConfiguration> SqliteAccountRepository::file_configuration(const application::FileConfigurationActor& a) {
    return impl_->readers_.read<application::FileConfiguration>("read protected file configuration",[a](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);application::FileConfiguration result;result.policy=file_detail::policy(db);
        const bool deletion=file_detail::group_deletions_available(db);
        SqliteStatement groups(db,deletion?"SELECT group_id FROM sfx_file_groups g WHERE NOT EXISTS(SELECT 1 FROM sfx_file_group_deletions d WHERE d.group_id=g.group_id AND d.completed=1) ORDER BY display_order,group_id LIMIT 1000":"SELECT group_id FROM sfx_file_groups ORDER BY display_order,group_id LIMIT 1000","file configuration groups");while(groups.row()){if(result.groups.size()>=999)file_detail::corrupt();result.groups.push_back(file_detail::group(db,{groups.int64(0)}));}
        SqliteStatement areas(db,deletion?"SELECT area_id FROM sfx_file_areas a WHERE NOT EXISTS(SELECT 1 FROM sfx_file_group_deletions d WHERE d.group_id=a.group_id AND d.completed=1) ORDER BY group_id,display_order,area_id LIMIT 1000":"SELECT area_id FROM sfx_file_areas ORDER BY group_id,display_order,area_id LIMIT 1000","file configuration areas");while(areas.row()){if(result.areas.size()>=999)file_detail::corrupt();result.areas.push_back(file_detail::area(db,{areas.int64(0)}));}
        SqliteStatement rules(db,"SELECT security_level,revision,daily_downloads,ratio_downloads,ratio_uploads,daily_exempt,ratio_exempt FROM sfx_file_security_rules ORDER BY security_level","file security overrides");
        while(rules.row())result.security_rules.push_back({static_cast<std::uint16_t>(rules.int64(0)),rules.int64(1),false,rules.boolean(5),rules.boolean(6),file_detail::counts(rules,2)});
        return result;
    });
}
application::StorageResult<application::FileCallerExemption> SqliteAccountRepository::file_caller_exemption(const application::FileConfigurationActor& a,application::AccountId account) {
    if(account<=0)return read_failure<application::FileCallerExemption>(invalid("validate file exemption account"));
    return impl_->readers_.read<application::FileCallerExemption>("read protected caller file exemptions",[a,account](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);
        SqliteStatement exists(db,"SELECT 1 FROM sfx_accounts WHERE account_id=?1","exemption account exists");exists.bind(1,account);if(!exists.row())file_detail::unavailable();
        application::FileCallerExemption result;result.account=account;
        SqliteStatement q(db,"SELECT revision,daily_exempt,ratio_exempt FROM sfx_file_caller_exemptions WHERE account_id=?1","read exact caller exemption");q.bind(1,account);
        if(q.row()){result.revision=q.int64(0);result.daily_exempt=q.boolean(1);result.ratio_exempt=q.boolean(2);}return result;
    });
}
application::StorageResult<std::uint64_t> SqliteAccountRepository::file_content_duplicate_count(const application::FileConfigurationActor& a,const application::FilePayloadIdentity& payload) {
    if(!file_detail::payload_valid(payload))return read_failure<std::uint64_t>(invalid("validate duplicate-content preview"));
    return impl_->readers_.read<std::uint64_t>("read protected content duplicate warning",[a,payload](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);
        SqliteStatement q(db,"SELECT count(*) FROM sfx_file_catalog WHERE content_sha256=?1 AND byte_count=?2 AND catalog_state<>1","match import duplicate-content rule");q.bind(1,payload.sha256);q.bind(2,static_cast<std::int64_t>(payload.bytes));if(!q.row())file_detail::corrupt();return static_cast<std::uint64_t>(q.int64(0));
    });
}
application::StorageResult<application::FileCallerState> SqliteAccountRepository::file_caller_state(const application::FileActor& a) {
    return impl_->readers_.read<application::FileCallerState>("read own file preferences",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);file_detail::authority(db,a);return file_detail::caller_state(db,a.account);});
}
application::StorageResult<application::FileCallerState> SqliteAccountRepository::save_file_caller_state(const application::FileActor& a,const application::FileCallerState& desired) {
    if(desired.revision<1 || desired.revision==INT64_MAX || !application::valid_utc_seconds(desired.since_utc) || desired.last_seen_publication<0 || desired.current_area.value<0)return read_failure<application::FileCallerState>(invalid("validate file preferences"));
    return impl_->write_result<application::FileCallerState>("save own file preferences",1024,[a,desired](sqlite3* db,auto& output){
        const auto auth=file_detail::authority(db,a);const auto prior=file_detail::caller_state(db,a.account);if(prior.revision!=desired.revision || desired.last_seen_publication!=prior.last_seen_publication)return SQLITE_ABORT;
        if(desired.current_area.value){const auto area=file_detail::area(db,desired.current_area).area;if(!application::file_area_visible(file_detail::group(db,area.group),area,auth))return SQLITE_NOTFOUND;}
        SqliteStatement q(db,"INSERT INTO sfx_file_caller_state VALUES(?1,?2,?3,?4,?5,?6,?7,NULL) ON CONFLICT(account_id) DO UPDATE SET scan_since_utc=excluded.scan_since_utc,current_area_id=excluded.current_area_id,lightbar=excluded.lightbar,new_file_scan=excluded.new_file_scan,revision=excluded.revision","commit caller-owned file preferences, never whole-BBS scan progress");
        q.bind(1,a.account);q.bind(2,desired.since_utc);q.bind(3,desired.last_seen_publication);if(desired.current_area.value)q.bind(4,desired.current_area.value);else q.bind_null(4);q.bind(5,desired.lightbar);q.bind(6,desired.new_file_scan);q.bind(7,desired.revision+1);q.step();output=file_detail::caller_state(db,a.account);return SQLITE_OK;
    });
}
application::StorageResult<application::FileEntry> SqliteAccountRepository::file_information(const application::FileActor& a,application::FileId file) {
    return impl_->readers_.read<application::FileEntry>("read authorized file information",[a,file](sqlite3* db){message_detail::ReadSnapshot snapshot(db);return file_detail::visible_entry(db,file,file_detail::authority(db,a)).file;});
}
application::StorageResult<application::FileCallerStatistics> SqliteAccountRepository::file_caller_statistics(const application::FileActor& a,std::string_view date) {
    if(!application::valid_iso_civil_date(date))return read_failure<application::FileCallerStatistics>(invalid("validate BBS local file-accounting date"));
    return impl_->readers_.read<application::FileCallerStatistics>("read own file statistics",[a,date=std::string(date)](sqlite3* db){message_detail::ReadSnapshot snapshot(db);return file_detail::statistics(db,a,file_detail::authority(db,a),date);});
}

application::StorageResult<application::FileGroupDeletionState> SqliteAccountRepository::file_group_deletion(const application::FileConfigurationActor& a,application::FileGroupId group){
    if(group.value<=0)return read_failure<application::FileGroupDeletionState>(invalid("validate File Group deletion lookup"));
    return impl_->readers_.read<application::FileGroupDeletionState>("read protected File Group deletion",[a,group](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);application::FileGroupDeletionState result;result.group=group;
        SqliteStatement q(db,"SELECT operation_id,delete_disk_files,completed FROM sfx_file_group_deletions WHERE group_id=?1","read immutable deletion intent");q.bind(1,group.value);if(!q.row())file_detail::unavailable();result.operation=q.identifier(0);result.delete_disk_files=q.boolean(1);result.completed=q.boolean(2);
        SqliteStatement items(db,"SELECT area_id,file_id,name,bytes,sha256,volume,native_id FROM sfx_file_group_delete_items WHERE group_id=?1 ORDER BY ordinal LIMIT 100001","read reviewed deletion files");items.bind(1,group.value);while(items.row()){if(result.items.size()>=100000)file_detail::corrupt();application::FileGroupDeletionItem item;item.area={items.int64(0)};item.file={items.is_null(1)?0:items.int64(1)};item.name=items.text(2);item.payload={static_cast<std::uint64_t>(items.int64(3)),file_detail::digest(items,4),items.text(5),items.text(6)};result.items.push_back(std::move(item));}return result;
    });
}
