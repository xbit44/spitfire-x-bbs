namespace file_detail {
bool path_text_valid(std::string_view path) {
#if defined(__linux__)
    if(path.size()>2048)return false;
    if(!path.empty()&&path.front()=='/'){
        if(path.size()==1||path[1]=='/'||path.find_first_of("\\:")!=path.npos)return false;
        path.remove_prefix(1);
    }
#endif
    if(path.empty() || path.size()>2048 || path.front()=='/' || path.front()=='\\')return false;
    for(unsigned char c:path)if(c<32 || c==127 || c=='*' || c=='?' || c=='"' || c=='<' || c=='>' || c=='|')return false;
    const bool absolute=path.size()>2 && path[1]==':' && (path[2]=='/' || path[2]=='\\') &&
        ((path[0]>='A' && path[0]<='Z') || (path[0]>='a' && path[0]<='z'));
    const auto colon=path.find(':');if(colon!=std::string_view::npos && (!absolute || path.find(':',2)!=std::string_view::npos))return false;
    if(absolute)path.remove_prefix(3);
    while(!path.empty()){
        const auto end=path.find_first_of("/\\");const auto part=path.substr(0,end);
        if(!application::valid_file_name(part))return false;
        if(end==std::string_view::npos)break;
        path.remove_prefix(end+1);if(path.empty())return false;
    }
    return true;
}
bool valid_change(const application::FileConfigurationChange& r) {
    if(!r.operation.valid() || r.expected_catalog_revision<1 || r.expected_catalog_revision==INT64_MAX)return false;
    switch(r.action){
    case application::FileConfigurationAction::policy:return application::valid_file_policy(r.policy) && r.policy.revision<INT64_MAX;
    case application::FileConfigurationAction::group:return application::valid_file_group(r.group) && r.group.revision<INT64_MAX;
    case application::FileConfigurationAction::insert_area_before:case application::FileConfigurationAction::insert_area_after:case application::FileConfigurationAction::move_area_up:case application::FileConfigurationAction::move_area_down:
        if(!application::valid_file_configuration_change(r))return false;
        [[fallthrough]];
    case application::FileConfigurationAction::area:return application::valid_file_area(r.area.area) && r.area.area.revision<INT64_MAX && path_text_valid(r.area.storage_path) &&
        (r.area.area.id.value!=0 || r.area.area.kind==application::FileAreaKind::ordinary);
    case application::FileConfigurationAction::security_rule:return r.security.level>=1 && r.security.level<=999 && r.security.revision>=1 && r.security.revision<INT64_MAX && application::valid_file_count_rule(r.security.counts);
    case application::FileConfigurationAction::preview_group_delete:case application::FileConfigurationAction::begin_group_delete:case application::FileConfigurationAction::finish_group_delete:return r.group.id.value>0&&application::valid_file_group(r.group);
    case application::FileConfigurationAction::caller_exemption:return r.exemption.account>0 && r.exemption.revision>=1 && r.exemption.revision<INT64_MAX;
    }
    return false;
}
std::array<std::uint8_t,32> change_fingerprint(const application::FileConfigurationActor& actor,const application::FileConfigurationChange& r) {
    Fingerprint f("configuration");f.actor(actor);f.number(static_cast<unsigned>(r.action));f.number(r.expected_catalog_revision);
    switch(r.action){
    case application::FileConfigurationAction::policy:f.number(r.policy.revision);f.number(static_cast<std::int64_t>(r.policy.maximum_upload_bytes));f.counts(r.policy.counts);f.number(r.policy.lightbar_default);f.number(r.policy.scan_uploads);break;
    case application::FileConfigurationAction::group:f.number(r.group.id.value);f.number(r.group.revision);f.text(r.group.name);f.number(r.group.order);f.number(r.group.access);f.number(r.group.enabled);break;
    case application::FileConfigurationAction::insert_area_before:case application::FileConfigurationAction::insert_area_after:case application::FileConfigurationAction::move_area_up:case application::FileConfigurationAction::move_area_down:
    case application::FileConfigurationAction::area:{f.number(r.anchor.value);f.number(r.anchor_revision);const auto& a=r.area.area;f.number(a.id.value);f.number(a.revision);f.number(a.group.value);f.number(a.number);f.text(a.name);f.text(a.description);f.number(a.order);f.number(static_cast<unsigned>(a.kind));
        f.number(a.permissions.access);f.number(a.permissions.upload);f.number(a.permissions.download);f.number(a.permissions.uploads_enabled);f.number(a.permissions.downloads_sysop_only);f.number(a.enabled);f.number(a.own_description_edit);f.number(a.backup_contents);f.number(a.ratio_uploads);f.number(a.ratio_downloads);f.number(static_cast<std::int64_t>(a.maximum_upload_bytes.value_or(0)));f.number(a.sysop_review);f.text(r.area.storage_path);break;}
    case application::FileConfigurationAction::security_rule:f.number(r.security.level);f.number(r.security.revision);f.number(r.security.inherit);f.number(r.security.daily_exempt);f.number(r.security.ratio_exempt);f.counts(r.security.counts);break;
    case application::FileConfigurationAction::preview_group_delete:case application::FileConfigurationAction::begin_group_delete:case application::FileConfigurationAction::finish_group_delete:f.number(r.group.id.value);f.number(r.group.revision);f.text({reinterpret_cast<const char*>(r.group.deletion_operation.bytes.data()),16});f.number(r.delete_disk_files);for(const auto& item:r.deletion_items){f.number(item.area.value);f.number(item.file.value);f.text(item.name);f.payload(item.payload);}break;
    case application::FileConfigurationAction::caller_exemption:f.number(r.exemption.account);f.number(r.exemption.revision);f.number(r.exemption.daily_exempt);f.number(r.exemption.ratio_exempt);break;
    }
    return f.value();
}
} // namespace file_detail
application::StorageResult<application::FileChangeOutcome> SqliteAccountRepository::change_file_configuration(const application::FileConfigurationActor& a,const application::FileConfigurationChange& r) {
    if(!file_detail::valid_change(r))return read_failure<application::FileChangeOutcome>(invalid("validate file configuration"));
    const auto fingerprint=file_detail::change_fingerprint(a,r);
    const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::FileChangeOutcome>(invalid("validate file configuration clock"));
    return impl_->write_result<application::FileChangeOutcome>("change authorized file configuration",8192+r.deletion_items.size()*512,[a,r,fingerprint,now](sqlite3* db,auto& output){
        message_detail::config_authority(db,a);
        if(auto previous=file_detail::receipt(db,a,r.operation,fingerprint)){output=*previous;return SQLITE_OK;}
        const auto prior=file_detail::policy(db);if(prior.catalog_revision!=r.expected_catalog_revision)return SQLITE_ABORT;
        application::FileChangeOutcome result;result.revision=prior.catalog_revision+1;
        const auto active_group=[&](application::FileGroupId id){if(!file_detail::group_deletions_available(db))return true;SqliteStatement d(db,"SELECT 1 FROM sfx_file_group_deletions WHERE group_id=?1","protect deleted and deleting Groups");d.bind(1,id.value);return !d.row();};
        const auto ordering=[&](application::FileGroupId id){std::vector<std::int64_t> ids;SqliteStatement q(db,"SELECT area_id FROM sfx_file_areas WHERE group_id=?1 ORDER BY display_order,area_id","read current Area display order");q.bind(1,id.value);while(q.row())ids.push_back(q.int64(0));return ids;};
        const auto publish_order=[&](const std::vector<std::int64_t>& ids){for(std::size_t n=0;n<ids.size();++n){SqliteStatement q(db,"UPDATE sfx_file_areas SET display_order=?1,revision=revision+1 WHERE area_id=?2 AND display_order<>?1","publish atomic Area ordering without changing identity");q.bind(1,static_cast<std::int64_t>(n));q.bind(2,ids[n]);q.step();}};
        switch(r.action){
        case application::FileConfigurationAction::policy:{
            if(prior.revision!=r.policy.revision)return SQLITE_ABORT;
            SqliteStatement q(db,"UPDATE sfx_file_policy SET revision=revision+1,maximum_upload_bytes=?1,daily_downloads=?2,ratio_downloads=?3,ratio_uploads=?4,lightbar_default=?5,scan_uploads=?6 WHERE singleton_id=1","save explicit file policy");
            q.bind(1,static_cast<std::int64_t>(r.policy.maximum_upload_bytes));file_detail::bind_counts(q,2,r.policy.counts);q.bind(5,r.policy.lightbar_default);q.bind(6,r.policy.scan_uploads);q.step();break;
        }
        case application::FileConfigurationAction::group:{
            const auto& g=r.group;if(g.id.value&&!active_group(g.id))return SQLITE_CONSTRAINT;
            if(!g.id.value){
                SqliteStatement count(db,"SELECT count(*) FROM sfx_file_groups","bound file groups");if(!count.row() || count.int64(0)>=999)return SQLITE_FULL;
                SqliteStatement q(db,"INSERT INTO sfx_file_groups(name,display_order,access_minimum,enabled,revision) VALUES(?1,?2,?3,?4,1)","create file group");q.bind(1,g.name);q.bind(2,g.order);q.bind(3,g.access);q.bind(4,g.enabled);q.step();result.identity=sqlite3_last_insert_rowid(db);
            }else{
                SqliteStatement q(db,"UPDATE sfx_file_groups SET name=?1,display_order=?2,access_minimum=?3,enabled=?4,revision=revision+1 WHERE group_id=?5 AND revision=?6","CAS file group");q.bind(1,g.name);q.bind(2,g.order);q.bind(3,g.access);q.bind(4,g.enabled);q.bind(5,g.id.value);q.bind(6,g.revision);q.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;result.identity=g.id.value;
            }break;
        }
        case application::FileConfigurationAction::move_area_up:case application::FileConfigurationAction::move_area_down:{
            const auto old=file_detail::area(db,r.area.area.id);if(old.area.revision!=r.area.area.revision||!(old.area.group==r.area.area.group)||!active_group(old.area.group))return SQLITE_ABORT;
            auto ids=ordering(old.area.group);const auto at=std::find(ids.begin(),ids.end(),old.area.id.value);if(at==ids.end())return SQLITE_ABORT;
            if(r.action==application::FileConfigurationAction::move_area_up){if(at!=ids.begin())std::iter_swap(at,at-1);}else if(at+1!=ids.end())std::iter_swap(at,at+1);
            publish_order(ids);result.identity=old.area.id.value;break;
        }
        case application::FileConfigurationAction::insert_area_before:case application::FileConfigurationAction::insert_area_after:
        case application::FileConfigurationAction::area:{
            const auto& ar=r.area.area;file_detail::group(db,ar.group);if(!active_group(ar.group))return SQLITE_CONSTRAINT;
            std::vector<std::int64_t> ids;std::size_t insertion{};
            const bool insert=r.action!=application::FileConfigurationAction::area;
            if(insert){const auto anchor=file_detail::area(db,r.anchor);if(!(anchor.area.group==ar.group)||anchor.area.revision!=r.anchor_revision)return SQLITE_ABORT;ids=ordering(ar.group);const auto at=std::find(ids.begin(),ids.end(),r.anchor.value);if(at==ids.end())return SQLITE_ABORT;insertion=static_cast<std::size_t>(at-ids.begin())+(r.action==application::FileConfigurationAction::insert_area_after?1:0);}
            std::int64_t number=ar.number;
            if(!ar.id.value){SqliteStatement maximum(db,"SELECT coalesce(max(area_number),0)+1 FROM sfx_file_areas","allocate permanent file area number");if(!maximum.row() || maximum.int64(0)>999)return SQLITE_FULL;number=maximum.int64(0);if(ar.number!=0)return SQLITE_ABORT;}
            else{
                const auto old=file_detail::area(db,ar.id);if(old.area.revision!=ar.revision || old.area.number!=ar.number || old.area.kind!=ar.kind)return SQLITE_ABORT;
                if(old.storage_path!=r.area.storage_path){
                    if(file_detail::ftn_area_pending(db,ar.id))return SQLITE_CONSTRAINT;
                    SqliteStatement contents(db,"SELECT 1 FROM sfx_file_catalog WHERE area_id=?1 LIMIT 1","no silent relocation of registered payloads");contents.bind(1,ar.id.value);if(contents.row())return SQLITE_CONSTRAINT;
                    SqliteStatement incoming(db,"SELECT 1 FROM sfx_file_intake WHERE (intended_area_id=?1 OR placement_area_id=?1) AND state IN(0,1,2,5) LIMIT 1","retain pending intake path bindings");incoming.bind(1,ar.id.value);if(incoming.row())return SQLITE_CONSTRAINT;
                    SqliteStatement mutation(db,"SELECT 1 FROM sfx_file_operations WHERE (source_area_id=?1 OR destination_area_id=?1) AND state IN(0,2) LIMIT 1","retain pending native mutation path bindings");mutation.bind(1,ar.id.value);if(mutation.row())return SQLITE_CONSTRAINT;
                    SqliteStatement imports(db,"SELECT 1 FROM sfx_file_imports WHERE area_id=?1 AND state IN(0,2) LIMIT 1","retain pending import path bindings");imports.bind(1,ar.id.value);if(imports.row())return SQLITE_CONSTRAINT;
                }
            }
            const auto distinct_path_sql=std::string("SELECT 1 FROM sfx_file_area_paths p WHERE ")+(file_detail::group_deletions_available(db)?"NOT EXISTS(SELECT 1 FROM sfx_file_areas a JOIN sfx_file_group_deletions d USING(group_id) WHERE a.area_id=p.area_id AND d.completed=1) AND ":"")+"sfx_file_fold(replace(storage_path,'\\','/'))=sfx_file_fold(replace(?1,'\\','/')) AND area_id<>?2 LIMIT 1";SqliteStatement duplicate(db,distinct_path_sql.c_str(),"distinct configured file area directories");duplicate.bind(1,r.area.storage_path);duplicate.bind(2,ar.id.value);if(duplicate.row())return SQLITE_CONSTRAINT;
            // Native configuration adapter additionally holds verified directory
            // handles and rejects aliases/reparse/overlapping/protected paths.
            const char* sql=ar.id.value?
                "UPDATE sfx_file_areas SET group_id=?1,name=?3,description=?4,display_order=?5,access_minimum=?7,upload_minimum=?8,download_minimum=?9,uploads_enabled=?10,downloads_sysop_only=?11,enabled=?12,own_description_edit=?13,backup_contents=?14,ratio_uploads=?15,ratio_downloads=?16,maximum_upload_bytes=?17,sysop_review=?18,revision=revision+1 WHERE area_id=?19 AND revision=?20 AND area_number=?2 AND kind=?6":
                "INSERT INTO sfx_file_areas(group_id,area_number,name,description,display_order,kind,access_minimum,upload_minimum,download_minimum,uploads_enabled,downloads_sysop_only,enabled,own_description_edit,backup_contents,ratio_uploads,ratio_downloads,maximum_upload_bytes,sysop_review,revision) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,?17,?18,1)";
            SqliteStatement q(db,sql,"save configured file area");q.bind(1,ar.group.value);q.bind(2,number);q.bind(3,ar.name);q.bind(4,ar.description);q.bind(5,ar.order);q.bind(6,static_cast<std::int64_t>(ar.kind));q.bind(7,ar.permissions.access);q.bind(8,ar.permissions.upload);q.bind(9,ar.permissions.download);q.bind(10,ar.permissions.uploads_enabled);q.bind(11,ar.permissions.downloads_sysop_only);q.bind(12,ar.enabled);q.bind(13,ar.own_description_edit);q.bind(14,ar.backup_contents);q.bind(15,ar.ratio_uploads);q.bind(16,ar.ratio_downloads);if(ar.maximum_upload_bytes)q.bind(17,static_cast<std::int64_t>(*ar.maximum_upload_bytes));else q.bind_null(17);
            q.bind(18,ar.sysop_review);if(ar.id.value){q.bind(19,ar.id.value);q.bind(20,ar.revision);}q.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;result.identity=ar.id.value?ar.id.value:sqlite3_last_insert_rowid(db);
            SqliteStatement path(db,"INSERT INTO sfx_file_area_paths VALUES(?1,?2) ON CONFLICT(area_id) DO UPDATE SET storage_path=excluded.storage_path","save validated area path");path.bind(1,result.identity);path.bind(2,r.area.storage_path);path.step();if(insert){ids.insert(ids.begin()+insertion,result.identity);publish_order(ids);}break;
        }
        case application::FileConfigurationAction::preview_group_delete:return SQLITE_MISUSE;
        case application::FileConfigurationAction::begin_group_delete:{
            const auto old=file_detail::group(db,r.group.id);if(old.revision!=r.group.revision||!active_group(old.id))return SQLITE_ABORT;
            SqliteStatement areas(db,"SELECT area_id,kind FROM sfx_file_areas WHERE group_id=?1","verify deletion dependencies");areas.bind(1,old.id.value);
            while(areas.row()){
                const auto id=areas.int64(0);if(areas.int64(1)!=0||file_detail::ftn_area_pending(db,{id}))return SQLITE_BUSY;
                for(const auto* sql:{"SELECT 1 FROM sfx_file_operations WHERE (source_area_id=?1 OR destination_area_id=?1) AND state IN(0,2) LIMIT 1","SELECT 1 FROM sfx_file_imports WHERE area_id=?1 AND state IN(0,2) LIMIT 1","SELECT 1 FROM sfx_file_intake WHERE (intended_area_id=?1 OR placement_area_id=?1) AND state IN(0,1,2,5) LIMIT 1","SELECT 1 FROM sfx_file_transfers WHERE area_id=?1 AND state=0 LIMIT 1"}){SqliteStatement pending(db,sql,"refuse Group deletion while file work is pending");pending.bind(1,id);if(pending.row())return SQLITE_BUSY;}
            }
            SqliteStatement intent(db,"INSERT INTO sfx_file_group_deletions VALUES(?1,?2,?3,0,?4)","retain owner-approved Group deletion intent");intent.bind(1,old.id.value);intent.bind(2,r.operation);intent.bind(3,r.delete_disk_files);intent.bind(4,now);intent.step();
            if(r.deletion_items.size()>100000)return SQLITE_FULL;
            for(std::size_t n=0;n<r.deletion_items.size();++n){const auto& item=r.deletion_items[n];if(!application::valid_file_name(item.name)||item.area.value<=0||item.payload.bytes>INT64_MAX)return SQLITE_CONSTRAINT;const auto ar=file_detail::area(db,item.area);if(!(ar.area.group==old.id))return SQLITE_ABORT;
                SqliteStatement q(db,"INSERT INTO sfx_file_group_delete_items VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)","retain exact reviewed Group deletion payload");q.bind(1,old.id.value);q.bind(2,static_cast<std::int64_t>(n));q.bind(3,item.area.value);if(item.file.value)q.bind(4,item.file.value);else q.bind_null(4);q.bind(5,item.name);q.bind(6,static_cast<std::int64_t>(item.payload.bytes));q.bind(7,item.payload.sha256);q.bind(8,item.payload.volume);q.bind(9,item.payload.file_id);q.step();}

            SqliteStatement disable(db,"UPDATE sfx_file_groups SET enabled=0,revision=revision+1 WHERE group_id=?1","freeze deleting Group");disable.bind(1,old.id.value);disable.step();
            SqliteStatement disable_areas(db,"UPDATE sfx_file_areas SET enabled=0,revision=revision+1 WHERE group_id=?1","freeze deleting Areas");disable_areas.bind(1,old.id.value);disable_areas.step();result.identity=old.id.value;break;
        }
        case application::FileConfigurationAction::finish_group_delete:{
            SqliteStatement intent(db,"SELECT operation_id,delete_disk_files,completed FROM sfx_file_group_deletions WHERE group_id=?1","verify exact native deletion completion");intent.bind(1,r.group.id.value);
            if(!intent.row()||intent.identifier(0)!=r.group.deletion_operation||intent.boolean(1)!=r.delete_disk_files||intent.boolean(2))return SQLITE_ABORT;
            if(r.delete_disk_files){SqliteStatement remain(db,"SELECT 1 FROM sfx_file_catalog c JOIN sfx_file_areas a USING(area_id) WHERE a.group_id=?1 AND NOT EXISTS(SELECT 1 FROM sfx_file_operations o WHERE o.file_id=c.file_id AND o.action=6 AND o.state=1) LIMIT 1","require journaled purges before retiring Group");remain.bind(1,r.group.id.value);if(remain.row())return SQLITE_BUSY;}
            SqliteStatement done(db,"UPDATE sfx_file_group_deletions SET completed=1 WHERE group_id=?1 AND completed=0","retire Group/Area definitions while retaining identities and receipts");done.bind(1,r.group.id.value);done.step();result.identity=r.group.id.value;break;
        }
        case application::FileConfigurationAction::security_rule:{
            const auto& rule=r.security;SqliteStatement old(db,"SELECT revision FROM sfx_file_security_rules WHERE security_level=?1","read level file revision");old.bind(1,rule.level);const bool exists=old.row();if((exists?old.int64(0):1)!=rule.revision)return SQLITE_ABORT;
            if(rule.inherit){SqliteStatement q(db,"DELETE FROM sfx_file_security_rules WHERE security_level=?1","restore global file policy inheritance");q.bind(1,rule.level);q.step();}
            else{SqliteStatement q(db,"INSERT INTO sfx_file_security_rules VALUES(?1,?2,?3,?4,?5,?6,?7) ON CONFLICT(security_level) DO UPDATE SET daily_downloads=excluded.daily_downloads,ratio_downloads=excluded.ratio_downloads,ratio_uploads=excluded.ratio_uploads,daily_exempt=excluded.daily_exempt,ratio_exempt=excluded.ratio_exempt,revision=excluded.revision","save explicit level file policy");q.bind(1,rule.level);file_detail::bind_counts(q,2,rule.counts);q.bind(5,rule.daily_exempt);q.bind(6,rule.ratio_exempt);q.bind(7,rule.revision+1);q.step();}result.identity=rule.level;break;
        }
        case application::FileConfigurationAction::caller_exemption:{
            const auto& e=r.exemption;SqliteStatement old(db,"SELECT revision FROM sfx_file_caller_exemptions WHERE account_id=?1","read file exemption revision");old.bind(1,e.account);if((old.row()?old.int64(0):1)!=e.revision)return SQLITE_ABORT;
            SqliteStatement q(db,"INSERT INTO sfx_file_caller_exemptions VALUES(?1,?2,?3,?4) ON CONFLICT(account_id) DO UPDATE SET daily_exempt=excluded.daily_exempt,ratio_exempt=excluded.ratio_exempt,revision=excluded.revision","save caller exemptions only");q.bind(1,e.account);q.bind(2,e.daily_exempt);q.bind(3,e.ratio_exempt);q.bind(4,e.revision+1);q.step();result.identity=e.account;break;
        }
        }
        SqliteStatement advance(db,"UPDATE sfx_file_policy SET catalog_revision=catalog_revision+1 WHERE singleton_id=1","publish file configuration revision");advance.step();
        file_detail::save_receipt(db,a,r.operation,fingerprint,result,now);file_detail::audit(db,a,r.operation,"configuration.changed",{},r.action==application::FileConfigurationAction::area?application::FileAreaId{result.identity}:application::FileAreaId{},now);
        output=result;return SQLITE_OK;
    });
}
