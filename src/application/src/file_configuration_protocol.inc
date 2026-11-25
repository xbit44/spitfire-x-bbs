namespace {
bool file_read_bool(Reader& r,bool& value){std::uint8_t n{};if(!r.u8(n)||n>1)return false;value=n!=0;return true;}
void file_write_counts(Writer& w,const FileCountRule& v){
    w.u8(v.daily_downloads?1:0);if(v.daily_downloads)w.u32(*v.daily_downloads);
    w.u8(v.ratio?1:0);if(v.ratio){w.u32(v.ratio->downloads);w.u32(v.ratio->uploads);}
}
bool file_read_counts(Reader& r,FileCountRule& v){
    bool present{};if(!file_read_bool(r,present))return false;
    if(present){std::uint32_t n{};if(!r.u32(n))return false;v.daily_downloads=n;}
    if(!file_read_bool(r,present))return false;
    if(present){FileRatio ratio;if(!r.u32(ratio.downloads)||!r.u32(ratio.uploads))return false;v.ratio=ratio;}
    return valid_file_count_rule(v);
}
void file_write_policy(Writer& w,const FilePolicy& v){
    w.u64(v.revision);w.u64(v.catalog_revision);w.u64(v.maximum_upload_bytes);file_write_counts(w,v.counts);w.u8(v.lightbar_default?1:0);w.u8(v.scan_uploads?1:0);
}
bool file_read_policy(Reader& r,FilePolicy& v){
    return nonnegative64(r,v.revision)&&nonnegative64(r,v.catalog_revision)&&r.u64(v.maximum_upload_bytes)&&file_read_counts(r,v.counts)&&file_read_bool(r,v.lightbar_default)&&file_read_bool(r,v.scan_uploads)&&valid_file_policy(v);
}
void file_write_group(Writer& w,const FileGroup& v){w.u64(v.id.value);w.text(v.name,64);w.u64(v.order);w.u64(v.revision);w.u16(v.access);w.u8(v.enabled?1:0);w.raw(v.deletion_operation.bytes.data(),16);w.u8(v.delete_disk_files?1:0);}
bool file_read_group(Reader& r,FileGroup& v){
    return nonnegative64(r,v.id.value)&&r.text(v.name,64)&&nonnegative64(r,v.order)&&nonnegative64(r,v.revision)&&r.u16(v.access)&&file_read_bool(r,v.enabled)&&r.raw(v.deletion_operation.bytes.data(),16)&&file_read_bool(r,v.delete_disk_files)&&valid_file_group(v);
}
void file_write_area(Writer& w,const FileAreaConfiguration& v){
    const auto& a=v.area;w.u64(a.id.value);w.u64(a.group.value);w.u16(a.number);w.text(a.name,64);w.text(a.description,256);w.u64(a.order);w.u64(a.revision);w.u8(static_cast<std::uint8_t>(a.kind));
    w.u16(a.permissions.access);w.u16(a.permissions.upload);w.u16(a.permissions.download);w.u8(a.permissions.uploads_enabled?1:0);w.u8(a.permissions.downloads_sysop_only?1:0);
    w.u8(a.enabled?1:0);w.u8(a.own_description_edit?1:0);w.u8(a.backup_contents?1:0);w.u8(a.ratio_uploads?1:0);w.u8(a.ratio_downloads?1:0);
    w.u8(a.maximum_upload_bytes?1:0);if(a.maximum_upload_bytes)w.u64(*a.maximum_upload_bytes);w.u8(a.sysop_review?1:0);w.text(v.storage_path,2048);
}
bool file_read_area(Reader& r,FileAreaConfiguration& v){
    auto& a=v.area;std::uint8_t kind{};bool finite{};
    if(!nonnegative64(r,a.id.value)||!nonnegative64(r,a.group.value)||!r.u16(a.number)||!r.text(a.name,64)||!r.text(a.description,256)||!nonnegative64(r,a.order)||!nonnegative64(r,a.revision)||!r.u8(kind)||kind>static_cast<std::uint8_t>(FileAreaKind::private_sysop)||
        !r.u16(a.permissions.access)||!r.u16(a.permissions.upload)||!r.u16(a.permissions.download)||!file_read_bool(r,a.permissions.uploads_enabled)||!file_read_bool(r,a.permissions.downloads_sysop_only)||
        !file_read_bool(r,a.enabled)||!file_read_bool(r,a.own_description_edit)||!file_read_bool(r,a.backup_contents)||!file_read_bool(r,a.ratio_uploads)||!file_read_bool(r,a.ratio_downloads)||!file_read_bool(r,finite))return false;
    a.kind=static_cast<FileAreaKind>(kind);if(finite){std::uint64_t n{};if(!r.u64(n))return false;a.maximum_upload_bytes=n;}
    return file_read_bool(r,a.sysop_review)&&r.text(v.storage_path,2048)&&valid_file_configuration_path(v.storage_path)&&valid_file_area(a);
}
void file_write_security(Writer& w,const FileSecurityRule& v){w.u16(v.level);w.u64(v.revision);w.u8(v.inherit?1:0);w.u8(v.daily_exempt?1:0);w.u8(v.ratio_exempt?1:0);file_write_counts(w,v.counts);}
bool file_read_security(Reader& r,FileSecurityRule& v){return r.u16(v.level)&&nonnegative64(r,v.revision)&&file_read_bool(r,v.inherit)&&file_read_bool(r,v.daily_exempt)&&file_read_bool(r,v.ratio_exempt)&&file_read_counts(r,v.counts)&&v.level>=1&&v.level<=999&&v.revision>=1;}
void file_write_exemption(Writer& w,const FileCallerExemption& v){w.u64(v.account);w.u64(v.revision);w.u8(v.daily_exempt?1:0);w.u8(v.ratio_exempt?1:0);}
bool file_read_exemption(Reader& r,FileCallerExemption& v){return nonnegative64(r,v.account)&&nonnegative64(r,v.revision)&&file_read_bool(r,v.daily_exempt)&&file_read_bool(r,v.ratio_exempt)&&v.account>0&&v.revision>=1;}
bool file_result_code(Reader& r,LocalAccountResultCode& v){std::uint8_t n{};if(!r.u8(n)||n>static_cast<std::uint8_t>(LocalAccountResultCode::system_sysop_protected))return false;v=static_cast<LocalAccountResultCode>(n);return true;}
}
std::vector<std::uint8_t> encode_file_configuration_query(const LocalFileConfigurationQuery& q){
    if(!valid_file_configuration_query(q))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(q.part));w.u64(q.catalog_revision);w.u64(q.after_id);w.u64(q.group.value);w.u64(q.account);return w.take();
}
std::optional<LocalFileConfigurationQuery> decode_file_configuration_query(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);LocalFileConfigurationQuery q;std::uint8_t part{};
    if(!r.u8(part)||!nonnegative64(r,q.catalog_revision)||!nonnegative64(r,q.after_id)||!nonnegative64(r,q.group.value)||!nonnegative64(r,q.account)||!r.complete())return {};
    q.part=static_cast<FileConfigurationPart>(part);return valid_file_configuration_query(q)?std::optional<LocalFileConfigurationQuery>{q}:std::nullopt;
}
std::vector<std::uint8_t> encode_file_configuration_snapshot(const LocalFileConfigurationSnapshot& s){
    if(!valid_file_configuration_snapshot(s))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(s.code));if(s.code!=LocalAccountResultCode::ok)return w.take();
    w.u8(static_cast<std::uint8_t>(s.part));file_write_policy(w,s.policy);w.u64(s.next_after);
    const auto count=s.groups.size()+s.areas.size()+s.security_rules.size();w.u8(static_cast<std::uint8_t>(count));
    for(const auto& g:s.groups)file_write_group(w,g);
    for(const auto& a:s.areas)file_write_area(w,a);
    for(const auto& v:s.security_rules)file_write_security(w,v);
    if(s.exemption)file_write_exemption(w,*s.exemption);
    auto bytes=w.take();return bytes.size()<=kLocalControlMaximumPayloadBytes?bytes:std::vector<std::uint8_t>{};
}
std::optional<LocalFileConfigurationSnapshot> decode_file_configuration_snapshot(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);LocalFileConfigurationSnapshot s;
    if(!file_result_code(r,s.code))return {};
    if(s.code!=LocalAccountResultCode::ok)return r.complete()?std::optional<LocalFileConfigurationSnapshot>{s}:std::nullopt;
    std::uint8_t part{},count{};if(!r.u8(part)||part>static_cast<std::uint8_t>(FileConfigurationPart::caller_exemption)||!file_read_policy(r,s.policy)||!nonnegative64(r,s.next_after)||!r.u8(count)||count>kLocalFileConfigurationPageSize)return {};
    s.part=static_cast<FileConfigurationPart>(part);
    for(unsigned n=0;n<count;++n)switch(s.part){
    case FileConfigurationPart::groups:{FileGroup g;if(!file_read_group(r,g))return {};s.groups.push_back(std::move(g));break;}
    case FileConfigurationPart::areas:{FileAreaConfiguration a;if(!file_read_area(r,a))return {};s.areas.push_back(std::move(a));break;}
    case FileConfigurationPart::security_rules:{FileSecurityRule v;if(!file_read_security(r,v))return {};s.security_rules.push_back(v);break;}
    default:return {};
    }
    if(s.part==FileConfigurationPart::caller_exemption){FileCallerExemption e;if(!file_read_exemption(r,e))return {};s.exemption=e;}
    return r.complete()&&valid_file_configuration_snapshot(s)?std::optional<LocalFileConfigurationSnapshot>{s}:std::nullopt;
}
std::vector<std::uint8_t> encode_file_configuration_change(const FileConfigurationChange& c){
    if(!valid_file_configuration_change(c))return {};
    Writer w;w.raw(c.operation.bytes.data(),c.operation.bytes.size());w.u8(static_cast<std::uint8_t>(c.action));w.u64(c.expected_catalog_revision);
    switch(c.action){
    case FileConfigurationAction::policy:file_write_policy(w,c.policy);break;
    case FileConfigurationAction::group:file_write_group(w,c.group);break;
    case FileConfigurationAction::area:file_write_area(w,c.area);break;
    case FileConfigurationAction::security_rule:file_write_security(w,c.security);break;
    case FileConfigurationAction::caller_exemption:file_write_exemption(w,c.exemption);break;
    case FileConfigurationAction::insert_area_before:case FileConfigurationAction::insert_area_after:
        file_write_area(w,c.area);w.u64(c.anchor.value);w.u64(c.anchor_revision);break;
    case FileConfigurationAction::move_area_up:case FileConfigurationAction::move_area_down:file_write_area(w,c.area);break;
    case FileConfigurationAction::preview_group_delete:case FileConfigurationAction::begin_group_delete:case FileConfigurationAction::finish_group_delete:
        file_write_group(w,c.group);w.u8(c.delete_disk_files?1:0);break;
    }
    return w.take();
}
std::optional<FileConfigurationChange> decode_file_configuration_change(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);FileConfigurationChange c;std::uint8_t action{};
    if(!r.raw(c.operation.bytes.data(),c.operation.bytes.size())||!r.u8(action)||action>static_cast<std::uint8_t>(FileConfigurationAction::finish_group_delete)||!nonnegative64(r,c.expected_catalog_revision))return {};
    c.action=static_cast<FileConfigurationAction>(action);bool valid{};
    switch(c.action){
    case FileConfigurationAction::policy:valid=file_read_policy(r,c.policy);break;
    case FileConfigurationAction::group:valid=file_read_group(r,c.group);break;
    case FileConfigurationAction::area:valid=file_read_area(r,c.area);break;
    case FileConfigurationAction::security_rule:valid=file_read_security(r,c.security);break;
    case FileConfigurationAction::caller_exemption:valid=file_read_exemption(r,c.exemption);break;
    case FileConfigurationAction::insert_area_before:case FileConfigurationAction::insert_area_after:
        valid=file_read_area(r,c.area)&&nonnegative64(r,c.anchor.value)&&nonnegative64(r,c.anchor_revision);break;
    case FileConfigurationAction::move_area_up:case FileConfigurationAction::move_area_down:valid=file_read_area(r,c.area);break;
    case FileConfigurationAction::preview_group_delete:case FileConfigurationAction::begin_group_delete:case FileConfigurationAction::finish_group_delete:
        valid=file_read_group(r,c.group)&&file_read_bool(r,c.delete_disk_files);break;
    }
    return valid&&r.complete()&&valid_file_configuration_change(c)?std::optional<FileConfigurationChange>{c}:std::nullopt;
}
std::vector<std::uint8_t> encode_file_configuration_result(const LocalFileConfigurationResult& v){
    if(v.code>LocalAccountResultCode::system_sysop_protected)return {};
    Writer w;w.u8(static_cast<std::uint8_t>(v.code));
    if(v.code!=LocalAccountResultCode::ok){if(v.result.revision||v.result.identity||v.result.replayed)return {};}
    else {if(v.result.revision<1||v.result.identity<0)return {};w.u64(v.result.revision);w.u64(v.result.identity);w.u8(v.result.replayed?1:0);}
    if(!v.detail.empty()||v.area_count||v.catalog_files||v.disk_files){
        if(v.detail.size()>512||!std::all_of(v.detail.begin(),v.detail.end(),[](unsigned char c){return c>=32&&c<=126;}))return {};
        w.text(v.detail,512);w.u32(v.area_count);w.u32(v.catalog_files);w.u32(v.disk_files);
    }return w.take();
}
std::optional<LocalFileConfigurationResult> decode_file_configuration_result(const std::vector<std::uint8_t>& bytes){
    Reader r(bytes);LocalFileConfigurationResult v;if(!file_result_code(r,v.code))return {};
    if(v.code==LocalAccountResultCode::ok&&(!nonnegative64(r,v.result.revision)||!nonnegative64(r,v.result.identity)||!file_read_bool(r,v.result.replayed)||v.result.revision<1))return {};
    if(!r.complete())if(!r.text(v.detail,512)||!r.u32(v.area_count)||!r.u32(v.catalog_files)||!r.u32(v.disk_files)||v.area_count>999||v.catalog_files>1000000||v.disk_files>1000000||!std::all_of(v.detail.begin(),v.detail.end(),[](unsigned char c){return c>=32&&c<=126;}))return {};
    return r.complete()?std::optional<LocalFileConfigurationResult>{v}:std::nullopt;
}
