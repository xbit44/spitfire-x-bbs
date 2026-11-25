namespace message_detail {
std::int64_t catalog_revision(sqlite3* db) {
    SqliteStatement q(db,"SELECT revision FROM sfx_message_configuration_state WHERE singleton_id=1","read message configuration revision");
    if(!q.row())throw StorageFailure(StorageErrorCode::corrupt,"message configuration missing",SQLITE_CORRUPT);
    return q.int64(0);
}
bool valid_catalog_cursor(const app::MessageCatalogCursor& c) {
    return c.configuration_revision>=0 && c.after_order>=-1 && c.after_id>=0 &&
        c.page_size>0 && c.page_size<=app::kMessagePageMaximum;
}
bool valid_query_options(const app::MessageQueryOptions& o) {
    if(o.unread_only && (o.mode!=app::MessageQueryMode::personal || o.scope!=app::MessageQueryScope::all_accessible || !o.search.empty()))return false;
    if(static_cast<unsigned>(o.mode)>static_cast<unsigned>(app::MessageQueryMode::correspondence) ||
        static_cast<unsigned>(o.scope)>static_cast<unsigned>(app::MessageQueryScope::all_accessible) ||
        o.echo.value<0 || o.group.value<0 || o.search.size()>128) return false;
    for(unsigned char c:o.search)if(c<32 || c>126)return false;
    if(o.mode==app::MessageQueryMode::addressed &&
        (o.addressed_to ? *o.addressed_to<=0 || !o.remote_name.empty() : o.remote_name.empty()))return false;
    if(o.remote_name.size()>128 || o.remote_network.size()>128 || o.remote_address.size()>96 ||
        ((!o.remote_network.empty() || !o.remote_address.empty()) && o.remote_name.empty()))return false;
    if(!o.remote_name.empty() && o.mode!=app::MessageQueryMode::addressed)return false;
    if(static_cast<unsigned>(o.caller_direction)>2 || (o.mode!=app::MessageQueryMode::addressed &&
        o.mode!=app::MessageQueryMode::correspondence && o.caller_direction!=app::MessageCallerDirection::to))return false;
    if(o.mode!=app::MessageQueryMode::addressed && o.addressed_to)return false;
    if((o.mode==app::MessageQueryMode::echo || o.mode==app::MessageQueryMode::addressed ||
        (o.mode==app::MessageQueryMode::regular_new && o.scope==app::MessageQueryScope::current_echo)) && o.echo.value<=0)return false;
    if(o.mode==app::MessageQueryMode::regular_new && o.scope==app::MessageQueryScope::current_group && o.group.value<=0)return false;
    return true;
}
bool contains_ascii(std::string_view source,std::string_view needle) {
    const auto fold=[](unsigned char c){return c>='A' && c<='Z'?static_cast<unsigned char>(c+32):c;};
    return std::search(source.begin(),source.end(),needle.begin(),needle.end(),
        [&](unsigned char a,unsigned char b){return fold(a)==fold(b);})!=source.end();
}
bool same_header_name(std::string_view a,std::string_view b) {
    return a.size()==b.size() && contains_ascii(a,b);
}
bool addressed_match(const app::MessageHeader& h,const app::MessageQueryOptions& o) {
    using Direction=app::MessageCallerDirection;
    bool to{},from{};
    if(o.addressed_to){
        to=h.target.recipient==o.addressed_to;
        from=h.author==o.addressed_to;
    }else{
        // Public To headers do not attest a recipient address. Match their
        // displayed name in the selected network, even for an addressed From identity.
        to=!h.target.recipient && same_header_name(h.to,o.remote_name) &&
            (!h.remote_author ? o.remote_network.empty() : same_header_name(h.remote_author->network,o.remote_network));
        from=!h.author && same_header_name(h.from,o.remote_name) &&
            (!h.remote_author ? o.remote_network.empty() && o.remote_address.empty() :
                same_header_name(h.remote_author->network,o.remote_network) &&
                (o.remote_address.empty() || h.remote_author->address==o.remote_address));
    }
    return (o.caller_direction!=Direction::from && to) || (o.caller_direction!=Direction::to && from);
}
bool correspondence_match(sqlite3* db,const app::MessageActor& caller,const app::MessageHeader& h,
    app::MessageCallerDirection direction) {
    const bool pub=h.target.kind==app::MessageKind::public_echo;
    const bool to=h.target.recipient==caller.account &&
        (pub ? read_preference(db,caller,h.target.echo).to_you : !h.inbox_removed);
    return (direction!=app::MessageCallerDirection::from && to) ||
        (direction!=app::MessageCallerDirection::to && h.author==caller.account);
}
bool inbox_list(const app::MessageQueryOptions& options) {
    using Mode=app::MessageQueryMode;
    return options.mode==Mode::inbox || options.mode==Mode::personal ||
        (options.mode==Mode::correspondence && options.caller_direction!=app::MessageCallerDirection::from);
}
void list_deletion_status(sqlite3* db,const app::MessageActor& caller,const Authority& authority,
    const app::MessageQueryOptions& options,app::MessageHeader& header) {
    using Scope=app::MessageListDeleteScope;
    const auto marks=list_deletion(db,header.identity.id);
    const bool own_inbox=inbox_list(options) && header.target.kind==app::MessageKind::private_local &&
        header.target.recipient==caller.account;
    if (marks.everyone) {
        header.list_deleted=true;
        if (can_delete_for_everyone(authority) && !header.permanent) header.list_delete_scope=Scope::everyone;
    } else if (own_inbox && marks.inbox && header.inbox_removed) {
        header.list_deleted=true;
        header.list_delete_scope=Scope::inbox;
    } else if (can_delete_for_everyone(authority) && !header.permanent) {
        header.list_delete_scope=Scope::everyone;
    } else if (own_inbox && !header.inbox_removed) {
        header.list_delete_scope=Scope::inbox;
    }
    header.list_delete_allowed=header.list_delete_scope!=Scope::none;
}
bool valid_query_context(const app::MessageActor& caller,const app::MessageQueryContext& c) {
    if(!valid_query_options(c.options) || c.owner!=caller.account || c.arrival_fence<0 || c.recipient_read_fence<0 || c.configuration_revision<=0 || c.initial_progress.size()>999)return false;
    std::int64_t previous{};
    for(const auto& p:c.initial_progress){if(p.first.value<=previous || p.second<0)return false;previous=p.first.value;}
    return true;
}
void check_query_context(sqlite3* db,const app::MessageQueryContext& c) {
    if(c.configuration_revision!=catalog_revision(db))conflict();
    SqliteStatement bounds(db,"SELECT last_arrival_ordinal,last_read_sequence FROM sfx_message_counters WHERE singleton_id=1","validate query fences");
    if(!bounds.row() || c.arrival_fence>bounds.int64(0) || c.recipient_read_fence>bounds.int64(1))unavailable();
}
bool in_query(sqlite3* db,const app::MessageActor& caller,const app::MessageQueryContext& c,
    const app::OpenedMessage& message,bool traversal,bool history) {
    const auto& h=message.header;const auto& o=c.options;using Mode=app::MessageQueryMode;
    if(h.identity.arrival_ordinal>c.arrival_fence)return false;
    if(!o.search.empty() && !contains_ascii(h.subject,o.search) && !contains_ascii(message.body.canonical_lf,o.search))return false;
    const bool pub=h.target.kind==app::MessageKind::public_echo;
    if(o.mode==Mode::echo || o.mode==Mode::addressed)return pub && h.target.echo==o.echo &&
        (o.mode!=Mode::addressed || addressed_match(h,o));
    if(o.mode==Mode::correspondence)return correspondence_match(db,caller,h,o.caller_direction);
    if(o.mode==Mode::sent)return !pub && h.author==caller.account;
    if(o.mode==Mode::inbox)return !pub && h.target.recipient==caller.account && !h.inbox_removed;
    if(o.mode==Mode::personal) {
        if(h.target.recipient!=caller.account || (!pub && h.inbox_removed))return false;
        if(pub){const auto p=read_preference(db,caller,h.target.echo);if(!p.to_you)return false;}
        if(o.unread_only && (history || (h.recipient_opened && h.recipient_opened_sequence<=c.recipient_read_fence)))return false;
        if(!traversal)return true;
        return history ? h.recipient_opened && h.recipient_opened_sequence<=c.recipient_read_fence : !h.recipient_opened;
    }
    if(!pub || (o.scope==app::MessageQueryScope::current_echo && !(h.target.echo==o.echo)) ||
        (o.scope==app::MessageQueryScope::current_group && h.position.group_id!=o.group.value))return false;
    const auto p=read_preference(db,caller,h.target.echo);
    if(!traversal)return true;
    return (p.unread && h.identity.area_number>p.highest_opened) || (p.to_you && h.target.recipient==caller.account && !h.recipient_opened);
}
} // namespace message_detail

application::StorageResult<application::MessageCatalogPage<application::MessageGroup>>
SqliteAccountRepository::message_group_page(const application::MessageActor& caller,const application::MessageCatalogCursor& cursor) {
    using Page=application::MessageCatalogPage<application::MessageGroup>;
    if(!message_detail::valid_catalog_cursor(cursor))return read_failure<Page>(invalid("validate group continuation"));
    return impl_->readers_.read<Page>("read message group page",[caller,cursor](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db);const auto authority=message_detail::actor(db,caller);
        const auto revision=message_detail::catalog_revision(db);
        if(cursor.configuration_revision && cursor.configuration_revision!=revision)message_detail::conflict();
        SqliteStatement q(db,"SELECT group_id,name,display_order,revision,access_minimum FROM sfx_message_groups WHERE enabled=1 AND access_minimum<=?1 AND (display_order,group_id)>(?2,?3) ORDER BY display_order,group_id LIMIT ?4", "read eligible group page");
        q.bind(1,authority.level);q.bind(2,cursor.after_order);q.bind(3,cursor.after_id);q.bind(4,static_cast<std::int64_t>(cursor.page_size+1));
        Page result;result.configuration_revision=revision;
        while(q.row()) {
            if(result.entries.size()==cursor.page_size){const auto& last=result.entries.back();result.next=application::MessageCatalogCursor{revision,last.order,last.id.value,cursor.page_size};break;}
            result.entries.push_back({{q.int64(0)},q.text(1),q.int64(2),q.int64(3),static_cast<std::uint16_t>(q.int64(4)),true});
        }
        return result;
    });
}
application::StorageResult<application::MessageCatalogPage<application::LocalEcho>>
SqliteAccountRepository::local_echo_page(const application::MessageActor& caller,application::MessageGroupId group,const application::MessageCatalogCursor& cursor) {
    using Page=application::MessageCatalogPage<application::LocalEcho>;
    if(group.value<=0 || !message_detail::valid_catalog_cursor(cursor))return read_failure<Page>(invalid("validate echo continuation"));
    return impl_->readers_.read<Page>("read local echo page",[caller,group,cursor](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db);const auto authority=message_detail::actor(db,caller);const auto revision=message_detail::catalog_revision(db);
        if(cursor.configuration_revision && cursor.configuration_revision!=revision)message_detail::conflict();
        SqliteStatement q(db,"SELECT e.echo_id,e.conference_number,e.name,e.display_order,e.revision,e.access_minimum,e.reading_minimum,e.posting_minimum FROM sfx_local_echoes e JOIN sfx_message_groups g ON g.group_id=e.group_id WHERE g.group_id=?1 AND g.enabled=1 AND e.enabled=1 AND g.access_minimum<=?2 AND e.access_minimum<=?2 AND e.reading_minimum<=?2 AND (e.display_order,e.echo_id)>(?3,?4) ORDER BY e.display_order,e.echo_id LIMIT ?5", "read eligible echo page");
        q.bind(1,group.value);q.bind(2,authority.level);q.bind(3,cursor.after_order);q.bind(4,cursor.after_id);q.bind(5,static_cast<std::int64_t>(cursor.page_size+1));
        Page result;result.configuration_revision=revision;
        while(q.row()) {
            if(result.entries.size()==cursor.page_size){const auto& last=result.entries.back();result.next=application::MessageCatalogCursor{revision,last.order,last.id.value,cursor.page_size};break;}
            result.entries.push_back({{q.int64(0)},group,static_cast<std::uint16_t>(q.int64(1)),q.text(2),q.int64(3),q.int64(4),{static_cast<std::uint16_t>(q.int64(5)),static_cast<std::uint16_t>(q.int64(6)),static_cast<std::uint16_t>(q.int64(7))},true});
        }
        for(auto& e:result.entries)e.conference_number=message_detail::visible_conference_number(db,e.id,authority);
        return result;
    });
}
application::StorageResult<application::MessageQueryContext> SqliteAccountRepository::begin_message_query(
    const application::MessageActor& caller,const application::MessageQueryOptions& options) {
    if(!message_detail::valid_query_options(options))return read_failure<application::MessageQueryContext>(invalid("validate message query"));
    return impl_->readers_.read<application::MessageQueryContext>("begin bounded message query",[caller,options](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);
        if(options.mode==application::MessageQueryMode::echo || options.mode==application::MessageQueryMode::addressed ||
            (options.mode==application::MessageQueryMode::regular_new && options.scope==application::MessageQueryScope::current_echo))
            message_detail::echo(db,options.echo,a,true,false);
        SqliteStatement q(db,"SELECT last_arrival_ordinal,last_read_sequence FROM sfx_message_counters WHERE singleton_id=1","capture message scan fences");
        if(!q.row())message_detail::unavailable();
        application::MessageQueryContext result{options,caller.account,q.int64(0),q.int64(1),message_detail::catalog_revision(db)};
        if(options.mode==application::MessageQueryMode::regular_new) {
            SqliteStatement positions(db,"SELECT e.echo_id,coalesce(p.highest_opened,0) FROM sfx_local_echoes e JOIN sfx_message_groups g ON g.group_id=e.group_id LEFT JOIN sfx_caller_echo_state p ON p.echo_id=e.echo_id AND p.account_id=?1 WHERE e.enabled=1 AND g.enabled=1 AND e.access_minimum<=?2 AND e.reading_minimum<=?2 AND g.access_minimum<=?2 ORDER BY e.echo_id LIMIT 1000","capture bounded echo positions");
            positions.bind(1,caller.account);positions.bind(2,a.level);
            while(positions.row()){if(result.initial_progress.size()==999)message_detail::unavailable();result.initial_progress.push_back({{positions.int64(0)},positions.int64(1)});}
        }
        return result;
    });
}
application::StorageResult<application::MessageHeaderPage> SqliteAccountRepository::query_message_headers(
    const application::MessageActor& caller,const application::MessageQueryContext& context,
    const application::MessageQueryCursor& cursor,std::size_t page_size,bool listing) {
    using Mode=application::MessageQueryMode;
    if(!message_detail::valid_query_context(caller,context) || !page_size || page_size>application::kMessagePageMaximum ||
        cursor.group_order<-1 || cursor.echo_order<-1 || cursor.group_id<0 || cursor.echo_id<0 || cursor.area_number<0 || cursor.message_id<0 || cursor.arrival<0 ||
        (cursor.history && (context.options.mode!=Mode::personal || context.options.unread_only)))return read_failure<application::MessageHeaderPage>(invalid("validate message query continuation"));
    return impl_->readers_.read<application::MessageHeaderPage>("query authorized message headers",[caller,context,cursor,page_size,listing](sqlite3* db) {
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);
        message_detail::check_query_context(db,context);
        const auto& o=context.options;const bool public_order=o.mode==Mode::echo || o.mode==Mode::addressed || o.mode==Mode::regular_new;
        if(o.mode==Mode::echo || o.mode==Mode::addressed || (o.mode==Mode::regular_new && o.scope==application::MessageQueryScope::current_echo))message_detail::echo(db,o.echo,a,true,false);
        // Fixed SQL clauses are selected by a validated enum; no submitted text
        // is concatenated into SQL. Search itself examines at most one page of
        // eligible candidates, and an empty matching page can have continuation.
        const bool reversible=listing && message_detail::has_list_deletion_schema(db);
        const bool show_global=reversible && message_detail::can_delete_for_everyone(a);
        const auto table=show_global ? "sfx_local_messages" : "sfx_active_local_messages";
        std::string sql="SELECT m.message_id,m.arrival_ordinal,coalesce(g.display_order,0),coalesce(g.group_id,0),coalesce(e.display_order,0),coalesce(e.echo_id,0),coalesce(m.area_number,0) FROM "+
            std::string(table)+" m LEFT JOIN sfx_local_echoes e ON e.echo_id=m.echo_id LEFT JOIN sfx_message_groups g ON g.group_id=e.group_id LEFT JOIN sfx_message_recipient_state r ON r.message_id=m.message_id LEFT JOIN sfx_caller_echo_state p ON p.account_id=?1 AND p.echo_id=m.echo_id WHERE m.arrival_ordinal<=?2 AND ";
        const std::string inbox_visible=reversible ?
            "(r.inbox_removed=0 OR EXISTS(SELECT 1 FROM sfx_message_list_deletion d WHERE d.message_id=m.message_id AND d.scope=1))" :
            "r.inbox_removed=0";
        const std::string public_access="(m.kind=0 AND g.enabled=1 AND e.enabled=1 AND g.access_minimum<=?3 AND e.access_minimum<=?3 AND e.reading_minimum<=?3)";
        if(o.mode==Mode::echo || o.mode==Mode::addressed) {
            sql+=public_access+" AND m.echo_id=?4";
            if(o.mode==Mode::addressed && o.addressed_to)sql+=o.caller_direction==application::MessageCallerDirection::to?" AND m.recipient_account_id=?6":o.caller_direction==application::MessageCallerDirection::from?" AND m.author_account_id=?6":" AND (m.recipient_account_id=?6 OR m.author_account_id=?6)";
        } else if(o.mode==Mode::regular_new) {
            sql+=public_access+(cursor.reverse ? " AND (coalesce(p.unread_enabled,1)=1 OR (coalesce(p.to_you_enabled,1)=1 AND m.recipient_account_id=?1 AND (r.opened=0 OR r.opened_sequence>?7)))" : " AND ((coalesce(p.unread_enabled,1)=1 AND m.area_number>coalesce(p.highest_opened,0)) OR (coalesce(p.to_you_enabled,1)=1 AND m.recipient_account_id=?1 AND r.opened=0))");
            if(o.scope==application::MessageQueryScope::current_echo)sql+=" AND m.echo_id=?4";
            if(o.scope==application::MessageQueryScope::current_group)sql+=" AND g.group_id=?5";
        } else if(o.mode==Mode::personal) {
            sql+="m.recipient_account_id=?1 AND ((m.kind=1 AND "+inbox_visible+") OR ("+public_access+" AND coalesce(p.to_you_enabled,1)=1))";
            sql+=cursor.history?" AND r.opened=1 AND r.opened_sequence<=?7":cursor.reverse?" AND (r.opened=0 OR r.opened_sequence>?7)":" AND r.opened=0";
        } else if(o.mode==Mode::correspondence) {
            const std::string to="(m.recipient_account_id=?1 AND ((m.kind=1 AND "+inbox_visible+") OR ("+public_access+" AND coalesce(p.to_you_enabled,1)=1)))";
            const std::string from="(m.author_account_id=?1 AND (m.kind IN(1,2) OR "+public_access+"))";
            sql+=o.caller_direction==application::MessageCallerDirection::to?to:
                o.caller_direction==application::MessageCallerDirection::from?from:"("+to+" OR "+from+")";
        } else if(o.mode==Mode::inbox)sql+="m.kind=1 AND m.recipient_account_id=?1 AND "+inbox_visible;
        else sql+="m.kind IN(1,2) AND m.author_account_id=?1";
        if(public_order)sql+=cursor.reverse ? " AND (?13=0 OR (g.display_order,g.group_id,e.display_order,e.echo_id,m.area_number,m.message_id)<(?8,?9,?10,?11,?12,?13)) ORDER BY g.display_order DESC,g.group_id DESC,e.display_order DESC,e.echo_id DESC,m.area_number DESC,m.message_id DESC LIMIT ?15" : " AND (g.display_order,g.group_id,e.display_order,e.echo_id,m.area_number,m.message_id)>(?8,?9,?10,?11,?12,?13) ORDER BY g.display_order,g.group_id,e.display_order,e.echo_id,m.area_number,m.message_id LIMIT ?15";
        else if(cursor.history!=cursor.reverse)sql+=" AND (?14=0 OR (m.arrival_ordinal,m.message_id)<(?14,?13)) ORDER BY m.arrival_ordinal DESC,m.message_id DESC LIMIT ?15";
        else sql+=" AND (?14=0 OR (m.arrival_ordinal,m.message_id)>(?14,?13)) ORDER BY m.arrival_ordinal,m.message_id LIMIT ?15";
        SqliteStatement q(db,message_detail::active_message_sql(db,std::move(sql)).c_str(),
            "read bounded authorized candidate headers");
        q.bind(1,caller.account);q.bind(2,context.arrival_fence);q.bind(3,a.level);q.bind(4,o.echo.value);q.bind(5,o.group.value);
        q.bind(6,o.addressed_to.value_or(0));q.bind(7,context.recipient_read_fence);q.bind(8,cursor.group_order);q.bind(9,cursor.group_id);
        q.bind(10,cursor.echo_order);q.bind(11,cursor.echo_id);q.bind(12,cursor.area_number);q.bind(13,cursor.message_id);q.bind(14,cursor.arrival);q.bind(15,static_cast<std::int64_t>(page_size+1));
        application::MessageHeaderPage result;application::MessageQueryCursor last=cursor;std::size_t examined{};bool more{};
        while(q.row()) {
            if(examined==page_size){more=true;break;}
            ++examined;const application::MessageId id{q.int64(0)};
            last={cursor.history,q.int64(2),q.int64(3),q.int64(4),q.int64(5),q.int64(6),id.value,q.int64(1),cursor.reverse};
            auto message=message_detail::load(db,caller,a,id,!o.search.empty(),reversible);
            if (reversible) {
                message_detail::list_deletion_status(db,caller,a,o,message.header);
                if (message.header.list_deleted && !message.header.list_delete_allowed) continue;
            }
            if(o.mode==Mode::addressed && !message_detail::addressed_match(message.header,o))continue;
            if(o.mode==Mode::regular_new && cursor.reverse) {
                const auto p=std::find_if(context.initial_progress.begin(),context.initial_progress.end(),[&](const auto& item){return item.first==message.header.target.echo;});
                SqliteStatement pref(db,"SELECT coalesce((SELECT unread_enabled FROM sfx_caller_echo_state WHERE account_id=?1 AND echo_id=?2),1),coalesce((SELECT to_you_enabled FROM sfx_caller_echo_state WHERE account_id=?1 AND echo_id=?2),1)","recheck reverse scan columns");pref.bind(1,caller.account);pref.bind(2,message.header.target.echo.value);pref.row();
                if(!((pref.boolean(0) && p!=context.initial_progress.end() && message.header.identity.area_number>p->second) || (pref.boolean(1) && message.header.target.recipient==caller.account && (!message.header.recipient_opened || message.header.recipient_opened_sequence>context.recipient_read_fence))))continue;
            }
            if(!o.search.empty() && !message_detail::contains_ascii(message.header.subject,o.search) && !message_detail::contains_ascii(message.body.canonical_lf,o.search))continue;
            message.header.position=last;result.entries.push_back(std::move(message.header));
        }
        if(more)result.next=last;
        else if(o.mode==Mode::personal && !o.unread_only && cursor.history==cursor.reverse) {application::MessageQueryCursor phase;phase.history=!cursor.history;phase.reverse=cursor.reverse;result.next=phase;}
        return result;
    });
}
application::StorageResult<application::MessageHeader> SqliteAccountRepository::message_number_header(
    const application::MessageActor& caller,const application::MessageQueryContext& context,std::int64_t number) {
    if(number<=0 || !message_detail::valid_query_context(caller,context))return read_failure<application::MessageHeader>(invalid("validate manual message reference"));
    return impl_->readers_.read<application::MessageHeader>("resolve scoped message number",[caller,context,number](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);const auto a=message_detail::actor(db,caller);message_detail::check_query_context(db,context);
        const auto& o=context.options;const bool public_number=o.mode==application::MessageQueryMode::echo || o.mode==application::MessageQueryMode::addressed;
        // Multi-echo scans have no ambiguous conference-local numeric opening.
        if(o.mode==application::MessageQueryMode::regular_new)message_detail::unavailable();
        SqliteStatement q(db,public_number?"SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND area_number=?2":"SELECT message_id FROM sfx_local_messages WHERE message_id=?2", "resolve manual identity");
        if(public_number)q.bind(1,o.echo.value);
        q.bind(2,number);if(!q.row())message_detail::unavailable();
        auto message=message_detail::load(db,caller,a,{q.int64(0)},true);
        if(!message_detail::in_query(db,caller,context,message,false,false))message_detail::unavailable();
        message.header.position.history=o.mode==application::MessageQueryMode::personal && message.header.recipient_opened && message.header.recipient_opened_sequence<=context.recipient_read_fence;
        return message.header;
    });
}
application::StorageResult<application::OpenedMessage> SqliteAccountRepository::open_query_message(
    const application::MessageActor& caller,const application::MessageQueryContext& context,application::MessageId id,bool traversal,bool history,bool preview) {
    if(id.value<=0 || !message_detail::valid_query_context(caller,context))return read_failure<application::OpenedMessage>(invalid("validate scoped message open"));
    if(preview)return impl_->readers_.read<application::OpenedMessage>("preview scoped message",[caller,context,id,traversal,history](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);
        const auto a=message_detail::actor(db,caller);
        message_detail::check_query_context(db,context);
        SqliteStatement assigned(db,"SELECT s.account_id FROM sfx_system_sysop s JOIN sfx_accounts a ON a.account_id=s.account_id JOIN sfx_security_policy p WHERE s.singleton_id=1 AND a.account_state='active' AND a.security_level>=p.sysop_threshold","check System Sysop Preview authority");
        if(!assigned.row() || assigned.int64(0)!=caller.account)
            throw StorageFailure(StorageErrorCode::permission_denied,"Preview requires current System Sysop designation",SQLITE_AUTH);
        auto message=message_detail::load(db,caller,a,id);
        if(!message_detail::in_query(db,caller,context,message,traversal,history))message_detail::unavailable();
        message.header.position.history=history;
        return message;
    });
    return impl_->write_result<application::OpenedMessage>("open scoped local message",4096,[caller,context,id,traversal,history](sqlite3* db,auto& output){
        const auto a=message_detail::actor(db,caller);message_detail::check_query_context(db,context);
        auto message=message_detail::load(db,caller,a,id);
        if(!message_detail::in_query(db,caller,context,message,traversal,history))return SQLITE_NOTFOUND;
        // A self-addressed sent copy is still a Sent view, not a recipient open.
        const bool sent_view=context.options.mode==application::MessageQueryMode::sent ||
            (context.options.mode==application::MessageQueryMode::correspondence &&
                context.options.caller_direction==application::MessageCallerDirection::from &&
                message.header.target.kind!=application::MessageKind::public_echo);
        const auto code=sent_view?SQLITE_OK:
            message_detail::accept_open(db,caller,message);if(code!=SQLITE_OK)return code;
        message.header.position.history=history;output=std::move(message);return SQLITE_OK;
    });
}
