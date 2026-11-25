namespace message_detail {
void config_authority(sqlite3* db,const app::MessageConfigurationActor& a) {
    if(a.installation_owner) {if(a.caller)unavailable();return;}
    if(!a.caller)unavailable();
    const auto current=actor(db,*a.caller);
    if(current.level<current.threshold)unavailable();
}
bool change_valid(const app::MessageConfigurationChange& c) {return app::valid_message_configuration_change(c);}
std::optional<std::uint32_t> retention_value(SqliteStatement& q,int column,std::uint32_t maximum){
    if(q.is_null(column))return {};
    const auto value=q.int64(column);
    if(value<1 || value>static_cast<std::int64_t>(maximum))throw StorageFailure(StorageErrorCode::corrupt,"invalid retention policy",SQLITE_CORRUPT);
    return static_cast<std::uint32_t>(value);
}
app::MessageRetentionPolicy echo_retention(sqlite3* db,app::LocalEchoId echo){
    SqliteStatement q(db,"SELECT maximum_count,maximum_age_days FROM sfx_echo_retention WHERE echo_id=?1","read saved echo retention");
    q.bind(1,echo.value);if(!q.row())throw StorageFailure(StorageErrorCode::corrupt,"echo retention missing",SQLITE_CORRUPT);
    return {retention_value(q,0,app::kMaximumRetainedMessageCount),retention_value(q,1,app::kMaximumMessageAgeDays)};
}
void save_echo_retention(sqlite3* db,app::LocalEchoId echo,const app::MessageRetentionPolicy& policy,std::uint8_t mask){
    if(!mask)return;
    SqliteStatement q(db,"UPDATE sfx_echo_retention SET maximum_count=CASE WHEN ?1 THEN ?2 ELSE maximum_count END,maximum_age_days=CASE WHEN ?3 THEN ?4 ELSE maximum_age_days END WHERE echo_id=?5 RETURNING echo_id","save only selected echo retention policy fields");
    q.bind(1,(mask&1)!=0);if(policy.maximum_count)q.bind(2,static_cast<std::int64_t>(*policy.maximum_count));else q.bind_null(2);
    q.bind(3,(mask&2)!=0);if(policy.maximum_age_days)q.bind(4,static_cast<std::int64_t>(*policy.maximum_age_days));else q.bind_null(4);
    q.bind(5,echo.value);if(!q.row())conflict();q.step();
}
std::array<std::uint8_t,32> config_fingerprint(const app::MessageConfigurationActor& a,const app::MessageConfigurationChange& c){
    const bool presentation=c.presentation_mask || c.action==app::MessageConfigurationAction::private_presentation;
    const bool retention=c.retention_mask || c.action==app::MessageConfigurationAction::unread_mail_retention;
    std::string data=retention?"SFX/MessageConfiguration/v3":presentation?"SFX/MessageConfiguration/v2":"SFX/MessageConfiguration/v1";
    const auto n=[&](std::int64_t x){for(int shift=56;shift>=0;shift-=8)data.push_back(static_cast<char>(static_cast<std::uint64_t>(x)>>shift));};
    const auto t=[&](std::string_view x){n(static_cast<std::int64_t>(x.size()));data.append(x);};
    n(a.installation_owner);n(a.caller?a.caller->account:0);n(static_cast<unsigned>(c.action));n(c.expected_configuration_revision);
    switch(c.action){
    case app::MessageConfigurationAction::unread_mail_retention:n(c.expected_unread_retention_revision);n(c.unread_mail_age_days?1:0);n(c.unread_mail_age_days.value_or(0));break;
    case app::MessageConfigurationAction::private_presentation:break;
    case app::MessageConfigurationAction::delete_group:n(c.group.id.value);n(c.group.revision);break;
    case app::MessageConfigurationAction::group:
        n(c.group.id.value);n(c.group.revision);t(c.group.name);n(c.group.order);n(c.group.access);n(c.group.enabled);break;
    case app::MessageConfigurationAction::insert_echo:
        n(c.insert_before.value);n(c.insert_before_revision);
        [[fallthrough]];
    case app::MessageConfigurationAction::echo:
        n(c.echo.id.value);n(c.echo.revision);n(c.echo.group.value);n(c.echo.conference_number);t(c.echo.name);n(c.echo.order);
        n(c.echo.permissions.access);n(c.echo.permissions.reading);n(c.echo.permissions.posting);n(c.echo.enabled);break;
    case app::MessageConfigurationAction::delete_echo:
    case app::MessageConfigurationAction::move_echo_up:
    case app::MessageConfigurationAction::move_echo_down:n(c.echo.id.value);n(c.echo.revision);break;
    case app::MessageConfigurationAction::clone_permissions:
        n(c.clone_source.value);n(c.source_revision);n(c.permission_mask);n(static_cast<std::int64_t>(c.clone_targets.size()));
        for(const auto& target:c.clone_targets){n(target.echo.value);n(target.revision);}break;
    case app::MessageConfigurationAction::system_sysop:
        n(c.sysop);n(c.expected_assignment_revision);n(c.expected_account_revision);break;
    case app::MessageConfigurationAction::line_limit:
        n(c.security_level);n(c.maximum_lines);n(c.expected_limit_revision);break;
    }
    if(presentation){n(c.presentation_mask);n(c.echo.renegade_pipe_colors);n(c.expected_private_presentation_revision);n(c.private_pipe_colors);}
    if(retention){n(c.retention_mask);if(c.action==app::MessageConfigurationAction::echo || c.action==app::MessageConfigurationAction::insert_echo){
        n(c.echo.retention.maximum_count?1:0);n(c.echo.retention.maximum_count.value_or(0));
        n(c.echo.retention.maximum_age_days?1:0);n(c.echo.retention.maximum_age_days.value_or(0));}}
    std::array<std::uint8_t,32> digest{};
    if(sodium_init()<0 || crypto_hash_sha256(digest.data(),reinterpret_cast<const unsigned char*>(data.data()),data.size())!=0)
        throw StorageFailure(StorageErrorCode::internal,"fingerprint message configuration",SQLITE_ERROR);
    return digest;
}
std::optional<app::MessageConfigurationResult> config_receipt(sqlite3* db,const app::MessageConfigurationChange& c,const std::array<std::uint8_t,32>& fingerprint) {
    SqliteStatement q(db,"SELECT request_fingerprint,result_configuration_revision,result_entity_id,result_entity_revision FROM sfx_message_configuration_audit WHERE operation_id=?1","reconcile message configuration");
    q.bind(1,c.operation);if(!q.row())return {};
    const auto stored=q.blob(0);if(stored.size()!=32 || sodium_memcmp(stored.data(),fingerprint.data(),32)!=0)conflict();
    return app::MessageConfigurationResult{q.int64(1),q.int64(2),q.int64(3),true};
}
std::string recipient_key(std::string_view name) {
    std::string result(name);for(auto& c:result)if(c>='A' && c<='Z')c=static_cast<char>(c+32);return result;
}
} // namespace message_detail

application::StorageResult<application::MessageConfigurationSnapshot> SqliteAccountRepository::message_configuration(
    const application::MessageConfigurationActor& authority,std::optional<application::MessageGroupId> group,const application::MessageCatalogCursor& cursor) {
    using Snapshot=application::MessageConfigurationSnapshot;
    if((group && group->value<=0) || !message_detail::valid_catalog_cursor(cursor))return read_failure<Snapshot>(invalid("validate message configuration page"));
    return impl_->readers_.read<Snapshot>("read message configuration",[authority,group,cursor](sqlite3* db){
        message_detail::ReadSnapshot tx(db);message_detail::config_authority(db,authority);
        Snapshot result;result.revision=message_detail::catalog_revision(db);
        const auto private_policy=message_detail::presentation(db,{application::MessageKind::private_local,{},{},{}});
        result.private_presentation_revision=private_policy.first;result.private_pipe_colors=private_policy.second;
        SqliteStatement unread(db,"SELECT maximum_age_days,revision FROM sfx_unread_mail_retention WHERE singleton_id=1","read saved unread-mail retention");
        if(!unread.row())throw StorageFailure(StorageErrorCode::corrupt,"unread retention missing",SQLITE_CORRUPT);
        result.unread_mail_age_days=message_detail::retention_value(unread,0,application::kMaximumMessageAgeDays);result.unread_retention_revision=unread.int64(1);
        if(cursor.configuration_revision && cursor.configuration_revision!=result.revision)message_detail::conflict();
        SqliteStatement sysop(db,"SELECT account_id,revision FROM sfx_system_sysop WHERE singleton_id=1","read System Sysop");
        if(!sysop.row())message_detail::unavailable();
        if(!sysop.is_null(0))result.system_sysop=sysop.int64(0);
        result.assignment_revision=sysop.int64(1);
        SqliteStatement limits(db,"SELECT m.security_level,m.maximum_lines,m.revision FROM sfx_security_message_limits m JOIN sfx_security_levels l USING(security_level) WHERE l.level_state='active' ORDER BY m.security_level LIMIT 999","read finite message limits");
        while(limits.row())result.line_limits.push_back({static_cast<std::uint16_t>(limits.int64(0)),static_cast<std::uint32_t>(limits.int64(1)),limits.int64(2)});
        if(!group){
            SqliteStatement q(db,"SELECT group_id,name,display_order,revision,access_minimum,enabled FROM sfx_message_groups WHERE (display_order,group_id)>(?1,?2) ORDER BY display_order,group_id LIMIT ?3","read administration group page");
            q.bind(1,cursor.after_order);q.bind(2,cursor.after_id);q.bind(3,static_cast<std::int64_t>(cursor.page_size+1));
            while(q.row()) {
                if(result.groups.size()==cursor.page_size){const auto& last=result.groups.back();result.next=application::MessageCatalogCursor{result.revision,last.order,last.id.value,cursor.page_size};break;}
                result.groups.push_back({{q.int64(0)},q.text(1),q.int64(2),q.int64(3),static_cast<std::uint16_t>(q.int64(4)),q.boolean(5)});
            }
        }else{
            SqliteStatement q(db,"SELECT echo_id,conference_number,name,display_order,revision,access_minimum,reading_minimum,posting_minimum,enabled,renegade_pipe_colors FROM sfx_local_echoes WHERE group_id=?1 AND (display_order,echo_id)>(?2,?3) ORDER BY display_order,echo_id LIMIT ?4","read administration echo page");
            q.bind(1,group->value);q.bind(2,cursor.after_order);q.bind(3,cursor.after_id);q.bind(4,static_cast<std::int64_t>(cursor.page_size+1));
            while(q.row()){
                if(result.echoes.size()==cursor.page_size){const auto& last=result.echoes.back();result.next=application::MessageCatalogCursor{result.revision,last.order,last.id.value,cursor.page_size};break;}
                application::LocalEcho echo{{q.int64(0)},*group,static_cast<std::uint16_t>(q.int64(1)),q.text(2),q.int64(3),q.int64(4),{static_cast<std::uint16_t>(q.int64(5)),static_cast<std::uint16_t>(q.int64(6)),static_cast<std::uint16_t>(q.int64(7))},q.boolean(8),q.boolean(9)};
                echo.retention=message_detail::echo_retention(db,echo.id);result.echoes.push_back(std::move(echo));
            }
        }return result;
    });
}

application::StorageResult<application::MessageConfigurationResult> SqliteAccountRepository::change_message_configuration(
    const application::MessageConfigurationActor& authority,const application::MessageConfigurationChange& c){
    using Action=application::MessageConfigurationAction;using Result=application::MessageConfigurationResult;
    if(!message_detail::change_valid(c))return read_failure<Result>(invalid("validate message configuration change"));
    const auto fingerprint=message_detail::config_fingerprint(authority,c);
    const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return read_failure<Result>(invalid("message configuration clock"));
    return impl_->write_result<Result>("change message configuration",4096+c.clone_targets.size()*32,[authority,c,fingerprint,now](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,authority);
        if(auto replay=message_detail::config_receipt(db,c,fingerprint)){out=*replay;return SQLITE_OK;}
        const auto revision=message_detail::catalog_revision(db);if(revision!=c.expected_configuration_revision)message_detail::conflict();
        Result result{revision,0,0,false};
        if(c.action==Action::group){
            const bool creating=c.group.id.value==0;
            SqliteStatement q(db,creating?"INSERT INTO sfx_message_groups(name,display_order,access_minimum,enabled,revision) VALUES(?1,?2,?3,?4,1) RETURNING group_id,revision":"UPDATE sfx_message_groups SET name=?1,display_order=?2,access_minimum=?3,enabled=?4,revision=revision+1 WHERE group_id=?5 AND revision=?6 RETURNING group_id,revision","change message group");
            q.bind(1,c.group.name);q.bind(2,c.group.order);q.bind(3,c.group.access);q.bind(4,c.group.enabled);
            if(!creating){q.bind(5,c.group.id.value);q.bind(6,c.group.revision);}if(!q.row())message_detail::conflict();result.entity_id=q.int64(0);result.entity_revision=q.int64(1);q.step();
        }else if(c.action==Action::delete_group){
            // This write transaction serializes emptiness with child insertion.
            // Disabled echoes count; never cascade or use a paginated UI count.
            SqliteStatement q(db,"DELETE FROM sfx_message_groups WHERE group_id=?1 AND revision=?2 AND NOT EXISTS(SELECT 1 FROM sfx_local_echoes WHERE group_id=?1) RETURNING group_id,revision","delete only empty message group");
            q.bind(1,c.group.id.value);q.bind(2,c.group.revision);
            if(!q.row())message_detail::conflict();
            result.entity_id=q.int64(0);result.entity_revision=q.int64(1);q.step();
        }else if(c.action==Action::delete_echo){
            SqliteStatement old(db,"SELECT 1 FROM sfx_local_echoes WHERE echo_id=?1 AND revision=?2","validate permanent echo deletion");old.bind(1,c.echo.id.value);old.bind(2,c.echo.revision);if(!old.row())message_detail::conflict();
            SqliteStatement scope(db,"INSERT INTO sfx_message_purge_scope VALUES(?1)","authorize transaction-local echo purge");scope.bind(1,c.echo.id.value);scope.step();
            const auto purge=[&](const char* sql){SqliteStatement q(db,sql,"purge only selected echo dependencies");q.bind(1,c.echo.id.value);q.step();};
            purge("INSERT INTO sfx_message_purged_receipts SELECT r.operation_id,r.actor_account_id,r.request_fingerprint,r.message_id FROM sfx_message_commit_receipts r JOIN sfx_local_messages m USING(message_id) WHERE m.echo_id=?1 AND m.kind=0");
            purge("UPDATE sfx_local_messages SET parent_message_id=NULL WHERE parent_message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
            purge("DELETE FROM sfx_message_arrival_intents WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
            purge("DELETE FROM sfx_message_recipient_state WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
            purge("DELETE FROM sfx_message_sources WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
            purge("DELETE FROM sfx_ftn_message_intents WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0) AND state=1");
            purge("DELETE FROM sfx_ftn_message_metadata WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
            purge("DELETE FROM sfx_ftn_message_targets WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
            purge("DELETE FROM sfx_message_commit_receipts WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
            purge("DELETE FROM sfx_local_messages WHERE echo_id=?1 AND kind=0");
            purge("DELETE FROM sfx_caller_echo_state WHERE echo_id=?1");
            purge("DELETE FROM sfx_message_purge_scope WHERE echo_id=?1");
            purge("DELETE FROM sfx_local_echoes WHERE echo_id=?1");
            result.entity_id=c.echo.id.value;
            // Entry/arrival/read counters remain monotonic historical totals.
        }else if(c.action==Action::move_echo_up || c.action==Action::move_echo_down){
            SqliteStatement old(db,"SELECT group_id FROM sfx_local_echoes WHERE echo_id=?1 AND revision=?2","validate echo move");old.bind(1,c.echo.id.value);old.bind(2,c.echo.revision);if(!old.row())message_detail::conflict();
            const auto group=old.int64(0);std::vector<std::int64_t> ids;
            SqliteStatement list(db,"SELECT echo_id FROM sfx_local_echoes WHERE group_id=?1 ORDER BY display_order,echo_id LIMIT 1000","read bounded echo ordering");list.bind(1,group);while(list.row())ids.push_back(list.int64(0));if(ids.size()>999)message_detail::conflict();
            auto at=std::find(ids.begin(),ids.end(),c.echo.id.value);const auto index=static_cast<std::size_t>(at-ids.begin());
            if(c.action==Action::move_echo_up && index>0)std::swap(ids[index],ids[index-1]);
            if(c.action==Action::move_echo_down && index+1<ids.size())std::swap(ids[index],ids[index+1]);
            for(std::size_t i=0;i<ids.size();++i){SqliteStatement q(db,"UPDATE sfx_local_echoes SET display_order=?1,revision=revision+1 WHERE echo_id=?2 RETURNING revision","apply atomic echo ordering");q.bind(1,static_cast<std::int64_t>(i));q.bind(2,ids[i]);if(!q.row())message_detail::conflict();if(ids[i]==c.echo.id.value)result.entity_revision=q.int64(0);q.step();}
            result.entity_id=c.echo.id.value;
        }else if(c.action==Action::echo || c.action==Action::insert_echo){
            SqliteStatement parent(db,"SELECT 1 FROM sfx_message_groups WHERE group_id=?1","validate echo parent");parent.bind(1,c.echo.group.value);if(!parent.row())message_detail::conflict();
            const bool creating=c.echo.id.value==0;std::int64_t number=c.echo.conference_number;auto order=c.echo.order;
            if(c.action==Action::insert_echo){
                SqliteStatement anchor(db,"SELECT display_order FROM sfx_local_echoes WHERE echo_id=?1 AND revision=?2 AND group_id=?3","validate insertion anchor");anchor.bind(1,c.insert_before.value);anchor.bind(2,c.insert_before_revision);anchor.bind(3,c.echo.group.value);if(!anchor.row())message_detail::conflict();
                std::vector<std::int64_t> ids;SqliteStatement list(db,"SELECT echo_id FROM sfx_local_echoes WHERE group_id=?1 ORDER BY display_order,echo_id LIMIT 1000","read insertion order");list.bind(1,c.echo.group.value);while(list.row())ids.push_back(list.int64(0));
                if(ids.size()>=999)message_detail::conflict();
                order=std::find(ids.begin(),ids.end(),c.insert_before.value)-ids.begin();
                for(std::size_t i=0;i<ids.size();++i){SqliteStatement shift(db,"UPDATE sfx_local_echoes SET display_order=?1,revision=revision+1 WHERE echo_id=?2","make insertion slot");shift.bind(1,static_cast<std::int64_t>(i)+(static_cast<std::int64_t>(i)>=order?1:0));shift.bind(2,ids[i]);shift.step();}
            }
            if(creating){
                if(number==0){SqliteStatement available(db,"WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<999) SELECT x FROM n WHERE NOT EXISTS(SELECT 1 FROM sfx_local_echoes WHERE conference_number=x) ORDER BY x LIMIT 1","allocate unique conference number");if(!available.row())message_detail::conflict();number=available.int64(0);}
                if(number<1 || number>999)message_detail::conflict();
            }else{
                SqliteStatement old(db,"SELECT conference_number FROM sfx_local_echoes WHERE echo_id=?1 AND revision=?2","validate echo stable number");old.bind(1,c.echo.id.value);old.bind(2,c.echo.revision);if(!old.row() || number!=old.int64(0))message_detail::conflict();
            }
            SqliteStatement q(db,creating?"INSERT INTO sfx_local_echoes(group_id,name,display_order,access_minimum,reading_minimum,posting_minimum,enabled,conference_number,revision,next_message_number) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,1,1) RETURNING echo_id,revision":"UPDATE sfx_local_echoes SET group_id=?1,name=?2,display_order=?3,access_minimum=?4,reading_minimum=?5,posting_minimum=?6,enabled=?7,revision=revision+1 WHERE conference_number=?8 AND echo_id=?9 AND revision=?10 RETURNING echo_id,revision","change local echo");
            q.bind(1,c.echo.group.value);q.bind(2,c.echo.name);q.bind(3,order);q.bind(4,c.echo.permissions.access);q.bind(5,c.echo.permissions.reading);q.bind(6,c.echo.permissions.posting);q.bind(7,c.echo.enabled);q.bind(8,number);
            if(!creating){q.bind(9,c.echo.id.value);q.bind(10,c.echo.revision);}if(!q.row())message_detail::conflict();result.entity_id=q.int64(0);result.entity_revision=q.int64(1);q.step();
            if(c.presentation_mask){
                SqliteStatement pipe(db,"UPDATE sfx_local_echoes SET renegade_pipe_colors=?1 WHERE echo_id=?2","set echo pipe color policy in same Save");pipe.bind(1,c.echo.renegade_pipe_colors);pipe.bind(2,result.entity_id);pipe.step();
            }
            message_detail::save_echo_retention(db,{result.entity_id},c.echo.retention,c.retention_mask);
        }else if(c.action==Action::clone_permissions){
            if((!c.permission_mask && !c.retention_mask) || c.clone_targets.empty()){out=result;return SQLITE_OK;}
            SqliteStatement source(db,"SELECT access_minimum,reading_minimum,posting_minimum FROM sfx_local_echoes WHERE echo_id=?1 AND revision=?2","validate clone source");source.bind(1,c.clone_source.value);source.bind(2,c.source_revision);if(!source.row())message_detail::conflict();
            const auto access=source.int64(0),reading=source.int64(1),posting=source.int64(2);
            const auto retention=message_detail::echo_retention(db,c.clone_source);
            for(const auto& target:c.clone_targets){
                SqliteStatement q(db,"UPDATE sfx_local_echoes SET access_minimum=CASE WHEN ?1 THEN ?2 ELSE access_minimum END,reading_minimum=CASE WHEN ?3 THEN ?4 ELSE reading_minimum END,posting_minimum=CASE WHEN ?5 THEN ?6 ELSE posting_minimum END,revision=revision+1 WHERE echo_id=?7 AND revision=?8 RETURNING echo_id","copy selected echo permissions");
                q.bind(1,(c.permission_mask&1)!=0);q.bind(2,access);q.bind(3,(c.permission_mask&2)!=0);q.bind(4,reading);q.bind(5,(c.permission_mask&4)!=0);q.bind(6,posting);q.bind(7,target.echo.value);q.bind(8,target.revision);if(!q.row())message_detail::conflict();q.step();
                message_detail::save_echo_retention(db,target.echo,retention,c.retention_mask);
            }
        }else if(c.action==Action::system_sysop){
            SqliteStatement eligible(db,"SELECT 1 FROM sfx_accounts a,sfx_security_policy p WHERE a.account_id=?1 AND a.revision=?2 AND a.account_state='active' AND a.security_level>=p.sysop_threshold","validate System Sysop candidate");eligible.bind(1,c.sysop);eligible.bind(2,c.expected_account_revision);if(!eligible.row())message_detail::conflict();
            SqliteStatement q(db,"UPDATE sfx_system_sysop SET account_id=?1,revision=revision+1 WHERE singleton_id=1 AND revision=?2 RETURNING revision","assign System Sysop");q.bind(1,c.sysop);q.bind(2,c.expected_assignment_revision);if(!q.row())message_detail::conflict();result.entity_id=c.sysop;result.entity_revision=q.int64(0);q.step();
        }else if(c.action==Action::private_presentation){
            SqliteStatement pipe(db,"UPDATE sfx_message_presentation_policy SET private_pipe_colors=?1,revision=revision+1 WHERE singleton_id=1 AND revision=?2 RETURNING revision","CAS private/no-area pipe color policy");
            pipe.bind(1,c.private_pipe_colors);pipe.bind(2,c.expected_private_presentation_revision);if(!pipe.row())message_detail::conflict();result.entity_revision=pipe.int64(0);pipe.step();
        }else if(c.action==Action::unread_mail_retention){
            SqliteStatement q(db,"UPDATE sfx_unread_mail_retention SET maximum_age_days=?1,revision=revision+1 WHERE singleton_id=1 AND revision=?2 RETURNING revision","save unread-mail retention without expiry");
            if(c.unread_mail_age_days)q.bind(1,static_cast<std::int64_t>(*c.unread_mail_age_days));else q.bind_null(1);
            q.bind(2,c.expected_unread_retention_revision);if(!q.row())message_detail::conflict();result.entity_revision=q.int64(0);q.step();
        }else{
            SqliteStatement q(db,"UPDATE sfx_security_message_limits SET maximum_lines=?1,revision=revision+1 WHERE security_level=?2 AND revision=?3 AND EXISTS(SELECT 1 FROM sfx_security_levels WHERE security_level=?2 AND level_state='active') RETURNING revision","change line limit");q.bind(1,static_cast<std::int64_t>(c.maximum_lines));q.bind(2,c.security_level);q.bind(3,c.expected_limit_revision);if(!q.row())message_detail::conflict();result.entity_id=c.security_level;result.entity_revision=q.int64(0);q.step();
        }
        SqliteStatement rev(db,"UPDATE sfx_message_configuration_state SET revision=revision+1 WHERE singleton_id=1 RETURNING revision","advance message configuration");if(!rev.row())message_detail::conflict();result.configuration_revision=rev.int64(0);rev.step();
        SqliteStatement audit(db,"INSERT INTO sfx_message_configuration_audit VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)","record message configuration receipt");audit.bind(1,c.operation);audit.bind(2,fingerprint);audit.bind(3,static_cast<std::int64_t>(c.action));if(authority.caller)audit.bind(4,authority.caller->account);else audit.bind_null(4);audit.bind(5,authority.installation_owner);audit.bind(6,result.configuration_revision);audit.bind(7,result.entity_id);audit.bind(8,result.entity_revision);audit.bind(9,now);audit.step();out=result;return SQLITE_OK;
    });
}

application::StorageResult<application::MessageRecipientPage> SqliteAccountRepository::resolve_message_recipient(
    const application::MessageActor& caller,std::string_view query,const std::optional<application::MessageRecipient>& after,bool name_only){
    using Page=application::MessageRecipientPage;
    if(query.empty() || query.size()>application::kAccountNameMaximum || (after && (after->account<=0 || after->name.size()>application::kAccountNameMaximum)))return read_failure<Page>(invalid("validate recipient query"));
    for(unsigned char c:query)if(c<32 || c>126)return read_failure<Page>(invalid("validate recipient query"));
    std::int64_t numeric{};bool digits=!name_only;if(digits)for(unsigned char c:query){if(c<'0' || c>'9'){digits=false;break;}if(numeric>(INT64_MAX-(c-'0'))/10)return read_failure<Page>(invalid("validate recipient reference"));numeric=numeric*10+c-'0';}
    return impl_->readers_.read<Page>("resolve eligible local recipient",[caller,key=message_detail::recipient_key(query),after,digits,numeric](sqlite3* db){
        message_detail::ReadSnapshot tx(db);message_detail::actor(db,caller);
        SqliteStatement q(db,"SELECT a.account_id,a.revision,a.display_name FROM sfx_accounts a JOIN sfx_security_levels l ON l.security_level=a.security_level WHERE a.account_state='active' AND l.level_state='active' AND ((?1=1 AND a.account_id=?2) OR (?1=0 AND instr(a.name_key,?3)>0)) AND (a.name_key,a.account_id)>(?4,?5) ORDER BY a.name_key,a.account_id LIMIT 51","read bounded recipient candidates");
        q.bind(1,digits);q.bind(2,numeric);q.bind(3,key);q.bind(4,after?message_detail::recipient_key(after->name):std::string{});q.bind(5,after?after->account:std::int64_t{});
        Page result;while(q.row()){if(result.entries.size()==50){result.next_after=result.entries.back();break;}result.entries.push_back({q.int64(0),q.int64(1),q.text(2)});}return result;
    });
}
application::StorageResult<application::MessageTarget> SqliteAccountRepository::system_sysop_target(const application::MessageActor& caller){
    return impl_->readers_.read<application::MessageTarget>("resolve System Sysop target",[caller](sqlite3* db){
        message_detail::ReadSnapshot tx(db);message_detail::actor(db,caller);
        SqliteStatement q(db,"SELECT s.account_id,s.revision FROM sfx_system_sysop s JOIN sfx_accounts a ON a.account_id=s.account_id JOIN sfx_security_policy p WHERE s.singleton_id=1 AND a.account_state='active' AND a.security_level>=p.sysop_threshold","read eligible System Sysop");if(!q.row())message_detail::unavailable();return application::MessageTarget{application::MessageKind::private_local,{},q.int64(0),q.int64(1)};
    });
}
