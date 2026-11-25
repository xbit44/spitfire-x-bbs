namespace {
void write_retention_limit(Writer& w,const std::optional<std::uint32_t>& value){
    w.u8(value?1:0);if(value)w.u32(*value);
}
bool read_retention_limit(Reader& r,std::optional<std::uint32_t>& value,std::uint32_t maximum){
    std::uint8_t finite{};std::uint32_t number{};
    if(!r.u8(finite) || finite>1)return false;
    if(finite){if(!r.u32(number) || !number || number>maximum)return false;value=number;}else value.reset();
    return true;
}
void write_message_cursor(Writer& w,const MessageCatalogCursor& c){
    w.u64(static_cast<std::uint64_t>(c.configuration_revision));w.u64(static_cast<std::uint64_t>(c.after_order));
    w.u64(static_cast<std::uint64_t>(c.after_id));w.u16(static_cast<std::uint16_t>(c.page_size));
}
bool read_message_cursor(Reader& r,MessageCatalogCursor& c){
    std::uint64_t order{};std::uint16_t size{};
    if(!nonnegative64(r,c.configuration_revision) || !r.u64(order) || !nonnegative64(r,c.after_id) || !r.u16(size))return false;
    if(order==std::numeric_limits<std::uint64_t>::max())c.after_order=-1;
    else if(order<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))c.after_order=static_cast<std::int64_t>(order);
    else return false;
    c.page_size=size;return valid_message_catalog_cursor(c);
}
void write_message_group(Writer& w,const MessageGroup& g){
    w.u64(static_cast<std::uint64_t>(g.id.value));w.u64(static_cast<std::uint64_t>(g.revision));
    w.u64(static_cast<std::uint64_t>(g.order));w.text(g.name,64);w.u16(g.access);w.u8(g.enabled?1:0);
}
bool read_message_group(Reader& r,MessageGroup& g){
    std::uint8_t enabled{};
    if(!nonnegative64(r,g.id.value) || !nonnegative64(r,g.revision) || !nonnegative64(r,g.order) || !r.text(g.name,64) || !r.u16(g.access) || !r.u8(enabled) || enabled>1)return false;
    g.enabled=enabled!=0;return true;
}
void write_message_echo(Writer& w,const LocalEcho& e){
    w.u64(static_cast<std::uint64_t>(e.id.value));w.u64(static_cast<std::uint64_t>(e.revision));w.u64(static_cast<std::uint64_t>(e.group.value));w.u64(static_cast<std::uint64_t>(e.order));
    w.u16(e.conference_number);w.text(e.name,64);w.u16(e.permissions.access);w.u16(e.permissions.reading);w.u16(e.permissions.posting);w.u8(e.enabled?1:0);w.u8(e.renegade_pipe_colors?1:0);
    write_retention_limit(w,e.retention.maximum_count);write_retention_limit(w,e.retention.maximum_age_days);
}
bool read_message_echo(Reader& r,LocalEcho& e){
    std::uint8_t enabled{},pipes{};
    if(!nonnegative64(r,e.id.value) || !nonnegative64(r,e.revision) || !nonnegative64(r,e.group.value) || !nonnegative64(r,e.order) ||
        !r.u16(e.conference_number) || !r.text(e.name,64) || !r.u16(e.permissions.access) || !r.u16(e.permissions.reading) || !r.u16(e.permissions.posting) || !r.u8(enabled) || enabled>1 || !r.u8(pipes) || pipes>1)return false;
    e.enabled=enabled!=0;e.renegade_pipe_colors=pipes!=0;
    return read_retention_limit(r,e.retention.maximum_count,kMaximumRetainedMessageCount) &&
        read_retention_limit(r,e.retention.maximum_age_days,kMaximumMessageAgeDays);
}
bool valid_message_snapshot(const MessageConfigurationSnapshot& s){
    if(s.revision<=0 || s.assignment_revision<=0 || s.private_presentation_revision<=0 || s.unread_retention_revision<=0 ||
        (s.unread_mail_age_days && (!*s.unread_mail_age_days || *s.unread_mail_age_days>kMaximumMessageAgeDays)) ||
        (s.system_sysop && *s.system_sysop<=0) || s.groups.size()>100 || s.echoes.size()>100 ||
        (!s.groups.empty() && !s.echoes.empty()) || s.line_limits.size()>999 || (s.next && (!valid_message_catalog_cursor(*s.next) || s.next->configuration_revision!=s.revision)))return false;
    MessageConfigurationChange c;c.operation.bytes[0]=1;c.expected_configuration_revision=s.revision;
    std::set<std::int64_t> ids;std::set<std::uint16_t> conferences;
    for(const auto& g:s.groups){c.action=MessageConfigurationAction::group;c.group=g;if(g.id.value<=0 || !valid_message_configuration_change(c) || !ids.insert(g.id.value).second)return false;}
    ids.clear();for(const auto& e:s.echoes){c.action=MessageConfigurationAction::echo;c.echo=e;if(e.id.value<=0 || !valid_message_configuration_change(c) || !ids.insert(e.id.value).second || !conferences.insert(e.conference_number).second)return false;}
    std::uint16_t previous{};for(const auto& l:s.line_limits){if(l.security_level<=previous || l.security_level>999 || !l.maximum_lines || l.maximum_lines>kMaximumMessageLines || l.revision<=0)return false;previous=l.security_level;}
    return true;
}
}
std::vector<std::uint8_t> encode_message_configuration_query(const LocalMessageConfigurationQuery& q){
    if((q.group && q.group->value<=0) || !valid_message_catalog_cursor(q.cursor))return {};
    Writer w;w.u64(static_cast<std::uint64_t>(q.group?q.group->value:0));write_message_cursor(w,q.cursor);return w.take();
}
std::optional<LocalMessageConfigurationQuery> decode_message_configuration_query(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);LocalMessageConfigurationQuery q;std::int64_t group{};
    if(!nonnegative64(r,group) || !read_message_cursor(r,q.cursor) || !r.complete())return {};
    if(group)q.group=MessageGroupId{group};
    return q;
}
std::vector<std::uint8_t> encode_message_configuration_snapshot(const LocalMessageConfigurationSnapshot& v){
    if(v.code>LocalAccountResultCode::system_sysop_protected)return {};
    Writer w;w.u8(static_cast<std::uint8_t>(v.code));
    if(v.code!=LocalAccountResultCode::ok)return w.take();
    const auto& s=v.catalog;if(!valid_message_snapshot(s))return {};
    w.u64(static_cast<std::uint64_t>(s.revision));w.u64(static_cast<std::uint64_t>(s.system_sysop.value_or(0)));w.u64(static_cast<std::uint64_t>(s.assignment_revision));
    w.u16(static_cast<std::uint16_t>(s.groups.size()));for(const auto& g:s.groups)write_message_group(w,g);
    w.u16(static_cast<std::uint16_t>(s.echoes.size()));for(const auto& e:s.echoes)write_message_echo(w,e);
    w.u8(s.next?1:0);if(s.next)write_message_cursor(w,*s.next);
    w.u16(static_cast<std::uint16_t>(s.line_limits.size()));for(const auto& l:s.line_limits){w.u16(l.security_level);w.u32(l.maximum_lines);w.u64(static_cast<std::uint64_t>(l.revision));}
    w.u64(static_cast<std::uint64_t>(s.private_presentation_revision));w.u8(s.private_pipe_colors?1:0);
    w.u64(static_cast<std::uint64_t>(s.unread_retention_revision));write_retention_limit(w,s.unread_mail_age_days);
    auto bytes=w.take();return bytes.size()<=kLocalControlMaximumPayloadBytes?bytes:std::vector<std::uint8_t>{};
}
std::optional<LocalMessageConfigurationSnapshot> decode_message_configuration_snapshot(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);LocalMessageConfigurationSnapshot v;std::uint8_t code{},next{};std::uint16_t count{};std::int64_t sysop{};
    if(!r.u8(code) || code>static_cast<std::uint8_t>(LocalAccountResultCode::system_sysop_protected))return {};
    v.code=static_cast<LocalAccountResultCode>(code);
    if(v.code!=LocalAccountResultCode::ok)return r.complete()?std::optional<LocalMessageConfigurationSnapshot>{v}:std::nullopt;
    auto& s=v.catalog;if(!nonnegative64(r,s.revision) || !nonnegative64(r,sysop) || !nonnegative64(r,s.assignment_revision))return {};if(sysop)s.system_sysop=sysop;
    if(!r.u16(count) || count>100)return {};
    for(unsigned i=0;i<count;++i){MessageGroup g;if(!read_message_group(r,g))return {};s.groups.push_back(std::move(g));}
    if(!r.u16(count) || count>100)return {};
    for(unsigned i=0;i<count;++i){LocalEcho e;if(!read_message_echo(r,e))return {};s.echoes.push_back(std::move(e));}
    if(!r.u8(next) || next>1)return {};
    if(next){MessageCatalogCursor c;if(!read_message_cursor(r,c))return {};s.next=c;}
    if(!r.u16(count) || count>999)return {};
    for(unsigned i=0;i<count;++i){MessageLineLimit l;if(!r.u16(l.security_level) || !r.u32(l.maximum_lines) || !nonnegative64(r,l.revision))return {};s.line_limits.push_back(l);}
    std::uint8_t pipes{};if(!nonnegative64(r,s.private_presentation_revision) || !r.u8(pipes) || pipes>1)return {};s.private_pipe_colors=pipes!=0;
    if(!nonnegative64(r,s.unread_retention_revision) || !read_retention_limit(r,s.unread_mail_age_days,kMaximumMessageAgeDays))return {};
    if(!r.complete() || !valid_message_snapshot(s))return {};
    return v;
}
std::vector<std::uint8_t> encode_message_configuration_change(const MessageConfigurationChange& c){
    if(!valid_message_configuration_change(c))return {};
    Writer w;w.raw(c.operation.bytes.data(),c.operation.bytes.size());w.u8(static_cast<std::uint8_t>(c.action));w.u64(static_cast<std::uint64_t>(c.expected_configuration_revision));
    switch(c.action){
    case MessageConfigurationAction::unread_mail_retention:w.u64(c.expected_unread_retention_revision);write_retention_limit(w,c.unread_mail_age_days);break;
    case MessageConfigurationAction::private_presentation:break;
    case MessageConfigurationAction::delete_group:w.u64(c.group.id.value);w.u64(c.group.revision);break;
    case MessageConfigurationAction::group:write_message_group(w,c.group);break;
    case MessageConfigurationAction::echo:write_message_echo(w,c.echo);break;
    case MessageConfigurationAction::insert_echo:write_message_echo(w,c.echo);w.u64(c.insert_before.value);w.u64(c.insert_before_revision);break;
    case MessageConfigurationAction::delete_echo:
    case MessageConfigurationAction::move_echo_up:
    case MessageConfigurationAction::move_echo_down:w.u64(c.echo.id.value);w.u64(c.echo.revision);break;
    case MessageConfigurationAction::clone_permissions:
        w.u64(static_cast<std::uint64_t>(c.clone_source.value));w.u64(static_cast<std::uint64_t>(c.source_revision));w.u8(c.permission_mask);w.u16(static_cast<std::uint16_t>(c.clone_targets.size()));
        for(const auto& t:c.clone_targets){w.u64(static_cast<std::uint64_t>(t.echo.value));w.u64(static_cast<std::uint64_t>(t.revision));}break;
    case MessageConfigurationAction::system_sysop:w.u64(static_cast<std::uint64_t>(c.sysop));w.u64(static_cast<std::uint64_t>(c.expected_assignment_revision));w.u64(static_cast<std::uint64_t>(c.expected_account_revision));break;
    case MessageConfigurationAction::line_limit:w.u16(c.security_level);w.u32(c.maximum_lines);w.u64(static_cast<std::uint64_t>(c.expected_limit_revision));break;
    }
    w.u8(c.presentation_mask);w.u64(c.expected_private_presentation_revision);w.u8(c.private_pipe_colors?1:0);
    w.u8(c.retention_mask);
    return w.take();
}
std::optional<MessageConfigurationChange> decode_message_configuration_change(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);MessageConfigurationChange c;std::uint8_t action{};
    if(!r.raw(c.operation.bytes.data(),c.operation.bytes.size()) || !r.u8(action) || action>static_cast<std::uint8_t>(MessageConfigurationAction::unread_mail_retention) || !nonnegative64(r,c.expected_configuration_revision))return {};
    c.action=static_cast<MessageConfigurationAction>(action);
    switch(c.action){
    case MessageConfigurationAction::unread_mail_retention:if(!nonnegative64(r,c.expected_unread_retention_revision) || !read_retention_limit(r,c.unread_mail_age_days,kMaximumMessageAgeDays))return {};break;
    case MessageConfigurationAction::private_presentation:break;
    case MessageConfigurationAction::delete_group:if(!nonnegative64(r,c.group.id.value) || !nonnegative64(r,c.group.revision))return {};break;
    case MessageConfigurationAction::group:if(!read_message_group(r,c.group))return {};break;
    case MessageConfigurationAction::echo:if(!read_message_echo(r,c.echo))return {};break;
    case MessageConfigurationAction::insert_echo:if(!read_message_echo(r,c.echo) || !nonnegative64(r,c.insert_before.value) || !nonnegative64(r,c.insert_before_revision))return {};break;
    case MessageConfigurationAction::delete_echo:
    case MessageConfigurationAction::move_echo_up:
    case MessageConfigurationAction::move_echo_down:if(!nonnegative64(r,c.echo.id.value) || !nonnegative64(r,c.echo.revision))return {};break;
    case MessageConfigurationAction::clone_permissions: {
        std::uint16_t count{};if(!nonnegative64(r,c.clone_source.value) || !nonnegative64(r,c.source_revision) || !r.u8(c.permission_mask) || !r.u16(count) || count>999)return {};
        for(unsigned i=0;i<count;++i){MessageEchoRevision t;if(!nonnegative64(r,t.echo.value) || !nonnegative64(r,t.revision))return {};c.clone_targets.push_back(t);}break;
    }
    case MessageConfigurationAction::system_sysop:if(!nonnegative64(r,c.sysop) || !nonnegative64(r,c.expected_assignment_revision) || !nonnegative64(r,c.expected_account_revision))return {};break;
    case MessageConfigurationAction::line_limit:if(!r.u16(c.security_level) || !r.u32(c.maximum_lines) || !nonnegative64(r,c.expected_limit_revision))return {};break;
    }
    std::uint8_t pipes{};if(!r.u8(c.presentation_mask) || !nonnegative64(r,c.expected_private_presentation_revision) || !r.u8(pipes) || pipes>1)return {};c.private_pipe_colors=pipes!=0;
    if(!r.u8(c.retention_mask))return {};
    if(!r.complete() || !valid_message_configuration_change(c))return {};
    return c;
}
std::vector<std::uint8_t> encode_message_configuration_result(const LocalMessageConfigurationResult& r){
    if(r.code>LocalAccountResultCode::system_sysop_protected)return {};
    Writer w;w.u8(static_cast<std::uint8_t>(r.code));if(r.code!=LocalAccountResultCode::ok)return w.take();
    if(r.result.configuration_revision<=0 || r.result.entity_id<0 || r.result.entity_revision<0)return {};
    w.u64(static_cast<std::uint64_t>(r.result.configuration_revision));w.u64(static_cast<std::uint64_t>(r.result.entity_id));w.u64(static_cast<std::uint64_t>(r.result.entity_revision));w.u8(r.result.replayed?1:0);return w.take();
}
std::optional<LocalMessageConfigurationResult> decode_message_configuration_result(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);LocalMessageConfigurationResult v;std::uint8_t code{},replay{};
    if(!r.u8(code) || code>static_cast<std::uint8_t>(LocalAccountResultCode::system_sysop_protected))return {};
    v.code=static_cast<LocalAccountResultCode>(code);
    if(v.code==LocalAccountResultCode::ok){if(!nonnegative64(r,v.result.configuration_revision) || !nonnegative64(r,v.result.entity_id) || !nonnegative64(r,v.result.entity_revision) || !r.u8(replay) || replay>1)return {};v.result.replayed=replay!=0;}
    if(!r.complete() || encode_message_configuration_result(v).empty())return {};
    return v;
}
