namespace {
void file_write_identifier(Writer& w,const Identifier128& id){w.raw(id.bytes.data(),id.bytes.size());}
bool file_read_identifier(Reader& r,Identifier128& id){return r.raw(id.bytes.data(),id.bytes.size())&&id.valid();}
void file_write_payload(Writer& w,const FilePayloadIdentity& p){w.u64(p.bytes);w.raw(p.sha256.data(),p.sha256.size());w.text(p.volume,128);w.text(p.file_id,128);}
bool file_read_payload(Reader& r,FilePayloadIdentity& p){return r.u64(p.bytes)&&r.raw(p.sha256.data(),p.sha256.size())&&r.text(p.volume,128)&&r.text(p.file_id,128);}
void file_write_descriptions(Writer& w,const FileDescriptions& d){w.text(d.short_text,kFileShortDescriptionBytes);w.text(d.long_text,kFileLongDescriptionBytes);}
bool file_read_descriptions(Reader& r,FileDescriptions& d){return r.text(d.short_text,kFileShortDescriptionBytes)&&r.text(d.long_text,kFileLongDescriptionBytes);}
void file_write_operation(Writer& w,const FileOperationRequest& p){
    file_write_identifier(w,p.operation);w.u8(static_cast<std::uint8_t>(p.action));w.u64(p.file.value);w.u64(p.expected_revision);w.u64(p.destination.value);w.u64(p.expected_destination_revision);w.text(p.filename,kFileNameBytes);file_write_descriptions(w,p.descriptions);w.text(p.public_uploader,64);w.u8(p.sysop_only?1:0);w.u8(p.allow_duplicate_content?1:0);
}
bool file_read_operation(Reader& r,FileOperationRequest& p){
    std::uint8_t action{};if(!file_read_identifier(r,p.operation)||!r.u8(action)||action>static_cast<std::uint8_t>(FileOperationAction::mark_missing)||!nonnegative64(r,p.file.value)||!nonnegative64(r,p.expected_revision)||!nonnegative64(r,p.destination.value)||!nonnegative64(r,p.expected_destination_revision)||!r.text(p.filename,kFileNameBytes)||!file_read_descriptions(r,p.descriptions)||!r.text(p.public_uploader,64)||!file_read_bool(r,p.sysop_only)||!file_read_bool(r,p.allow_duplicate_content))return false;
    p.action=static_cast<FileOperationAction>(action);return valid_file_management_operation(p);
}
void file_write_import(Writer& w,const FileImportRequest& p){
    const auto& f=p.registration;w.u8(static_cast<std::uint8_t>(p.mode));w.text(p.source_path,8192);file_write_identifier(w,f.operation);w.u64(f.area.value);w.u64(f.expected_area_revision);w.text(f.filename,kFileNameBytes);file_write_descriptions(w,f.descriptions);w.text(f.public_uploader,64);file_write_payload(w,f.payload);w.u64(static_cast<std::uint64_t>(f.original_upload_utc));w.u8(f.sysop_only?1:0);w.u8(f.allow_duplicate_content?1:0);
    // Origin is always Sysop import; an owner UI cannot fabricate a caller
    // upload or attach a private account identity/transfer-credit assertion.
}
bool file_read_import(Reader& r,FileImportRequest& p){
    auto& f=p.registration;std::uint8_t mode{};std::uint64_t utc{};
    if(!r.u8(mode)||mode>static_cast<std::uint8_t>(FileImportMode::move)||!r.text(p.source_path,8192)||!file_read_identifier(r,f.operation)||!nonnegative64(r,f.area.value)||!nonnegative64(r,f.expected_area_revision)||!r.text(f.filename,kFileNameBytes)||!file_read_descriptions(r,f.descriptions)||!r.text(f.public_uploader,64)||!file_read_payload(r,f.payload)||!r.u64(utc)||!file_read_bool(r,f.sysop_only)||!file_read_bool(r,f.allow_duplicate_content))return false;
    // Explicit two's-complement UTC decoding, including legitimate pre-1970
    // dates, without implementation-defined unsigned-to-signed conversion.
    f.original_upload_utc=utc<=static_cast<std::uint64_t>(INT64_MAX)?static_cast<std::int64_t>(utc):-1-static_cast<std::int64_t>(UINT64_MAX-utc);
    p.mode=static_cast<FileImportMode>(mode);f.origin=FileOrigin::sysop_import;return true;
}
void file_write_purge(Writer& w,const FileIntakePurge& p,bool include_completed){
    file_write_identifier(w,p.operation);w.u8(p.staging?1:0);if(p.staging)file_write_payload(w,*p.staging);w.u8(p.placed?1:0);if(p.placed)file_write_payload(w,*p.placed);if(include_completed)w.u8(p.completed?1:0);
}
bool file_read_purge(Reader& r,FileIntakePurge& p,bool include_completed){
    bool present{};if(!file_read_identifier(r,p.operation)||!file_read_bool(r,present))return false;
    if(present){FilePayloadIdentity value;if(!file_read_payload(r,value))return false;p.staging=value;}
    if(!file_read_bool(r,present))return false;
    if(present){FilePayloadIdentity value;if(!file_read_payload(r,value))return false;p.placed=value;}
    return !include_completed||file_read_bool(r,p.completed);
}
void file_write_view(Writer& w,const FileManagementViewPage& v){
    file_write_identifier(w,v.cursor);w.u64(v.file.value);w.u64(v.revision);w.text(v.filename,kFileNameBytes);w.text(v.member,512);w.u8(v.archive?1:0);w.u16(v.page);w.u32(v.total);
    w.u16(static_cast<std::uint16_t>(v.cells.size()));for(const auto& c:v.cells){w.u8(c.glyph);w.u8(c.attribute);}
    w.u8(static_cast<std::uint8_t>(v.entries.size()));for(const auto& e:v.entries){w.u16(e.index);w.text(e.name,512);w.u64(e.bytes);w.u8(e.directory?1:0);w.u8(e.encrypted?1:0);w.u8(e.readable?1:0);}
}
bool file_read_view(Reader& r,FileManagementViewPage& v){
    std::uint16_t cells{};std::uint8_t entries{};
    if(!file_read_identifier(r,v.cursor)||!nonnegative64(r,v.file.value)||!nonnegative64(r,v.revision)||!r.text(v.filename,kFileNameBytes)||!r.text(v.member,512)||!file_read_bool(r,v.archive)||!r.u16(v.page)||!r.u32(v.total)||!r.u16(cells)||cells>kFileViewRows*kFileViewColumns)return false;
    for(unsigned n=0;n<cells;++n){FileViewCell c;if(!r.u8(c.glyph)||!r.u8(c.attribute))return false;v.cells.push_back(c);}
    if(!r.u8(entries)||entries>kFileViewArchivePage)return false;
    for(unsigned n=0;n<entries;++n){FileViewArchiveEntry e;if(!r.u16(e.index)||!r.text(e.name,512)||!r.u64(e.bytes)||!file_read_bool(r,e.directory)||!file_read_bool(r,e.encrypted)||!file_read_bool(r,e.readable))return false;v.entries.push_back(std::move(e));}return valid_file_management_view(v);
}
}
std::vector<std::uint8_t> encode_file_management_task(const FileManagementTask& t){
    if(!valid_file_management_task(t))return {};
    Writer w;file_write_identifier(w,t.job);w.u8(static_cast<std::uint8_t>(t.kind));
    switch(t.kind){
    case FileManagementTaskKind::hatch_file:{const auto bytes=encode_ftn_hatch_request(t.hatch);w.u32(static_cast<std::uint32_t>(bytes.size()));w.raw(bytes.data(),bytes.size());break;}
    case FileManagementTaskKind::operation:file_write_operation(w,t.operation);break;
    case FileManagementTaskKind::preview_import:{const auto& c=t.choice;file_write_identifier(w,c.operation);w.u64(c.area.value);w.text(c.source_path,8192);w.text(c.filename,kFileNameBytes);w.text(c.public_uploader,64);w.u8(static_cast<std::uint8_t>(c.mode));w.u8(c.descriptions?1:0);if(c.descriptions)file_write_descriptions(w,*c.descriptions);w.u8(c.use_diz?1:0);w.u8(c.allow_duplicate_content?1:0);w.u8(c.sysop_only?1:0);break;}
    case FileManagementTaskKind::import_file:file_write_import(w,t.import);break;
    case FileManagementTaskKind::purge_intake:file_write_purge(w,t.purge,false);break;
    case FileManagementTaskKind::reupload_block:file_write_identifier(w,t.block.operation);w.u64(t.block.file.value);w.u64(t.block.expected_revision);w.u8(t.block.blocked?1:0);break;
    case FileManagementTaskKind::discover_unregistered:w.u64(t.discovery_area.value);break;
    case FileManagementTaskKind::discover_import_folder:w.u64(t.discovery_area.value);w.text(t.discovery_folder,8192);break;
    case FileManagementTaskKind::inspect_files:{const auto query=encode_file_management_read_request(t.inspection_request);w.u16(static_cast<std::uint16_t>(query.size()));w.raw(query.data(),query.size());break;}
    case FileManagementTaskKind::open_view:w.u64(t.view_file.value);w.u8(t.view_archive?1:0);break;
    case FileManagementTaskKind::scan_report:
    case FileManagementTaskKind::page_view:
    case FileManagementTaskKind::view_archive_index:file_write_identifier(w,t.target);w.u16(t.view_page);break;
    case FileManagementTaskKind::read_view_member:file_write_identifier(w,t.target);w.u16(t.view_member);break;
    default:file_write_identifier(w,t.target);break;
    }
    auto bytes=w.take();return bytes.size()<=kLocalControlMaximumPayloadBytes?bytes:std::vector<std::uint8_t>{};
}
std::vector<std::uint8_t> encode_file_management_job(Identifier128 id){if(!id.valid())return {};Writer w;file_write_identifier(w,id);return w.take();}
std::optional<Identifier128> decode_file_management_job(const std::vector<std::uint8_t>& bytes){Reader r(bytes);Identifier128 id;return file_read_identifier(r,id)&&r.complete()?std::optional<Identifier128>{id}:std::nullopt;}
std::optional<FileManagementTask> decode_file_management_task(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);FileManagementTask t;std::uint8_t kind{};
    if(!file_read_identifier(r,t.job)||!r.u8(kind)||kind>static_cast<std::uint8_t>(FileManagementTaskKind::reconcile_hatch))return {};
    t.kind=static_cast<FileManagementTaskKind>(kind);
    switch(t.kind){
    case FileManagementTaskKind::hatch_file:{std::uint32_t count{};if(!r.u32(count)||!count||count>60000)return {};std::vector<std::uint8_t> nested(count);if(!r.raw(nested.data(),nested.size()))return {};const auto hatch=decode_ftn_hatch_request(nested);if(!hatch)return {};t.hatch=*hatch;break;}
    case FileManagementTaskKind::operation:if(!file_read_operation(r,t.operation))return {};break;
    case FileManagementTaskKind::preview_import:{auto& c=t.choice;std::uint8_t mode{};bool descriptions{};if(!file_read_identifier(r,c.operation)||!nonnegative64(r,c.area.value)||!r.text(c.source_path,8192)||!r.text(c.filename,kFileNameBytes)||!r.text(c.public_uploader,64)||!r.u8(mode)||mode>static_cast<std::uint8_t>(FileImportMode::move)||!file_read_bool(r,descriptions))return {};c.mode=static_cast<FileImportMode>(mode);if(descriptions){FileDescriptions d;if(!file_read_descriptions(r,d))return {};c.descriptions=d;}if(!file_read_bool(r,c.use_diz)||!file_read_bool(r,c.allow_duplicate_content)||!file_read_bool(r,c.sysop_only))return {};break;}
    case FileManagementTaskKind::import_file:if(!file_read_import(r,t.import))return {};break;
    case FileManagementTaskKind::purge_intake:if(!file_read_purge(r,t.purge,false))return {};break;
    case FileManagementTaskKind::reupload_block:if(!file_read_identifier(r,t.block.operation)||!nonnegative64(r,t.block.file.value)||!nonnegative64(r,t.block.expected_revision)||!file_read_bool(r,t.block.blocked))return {};break;
    case FileManagementTaskKind::discover_unregistered:if(!nonnegative64(r,t.discovery_area.value))return {};break;
    case FileManagementTaskKind::discover_import_folder:if(!nonnegative64(r,t.discovery_area.value)||!r.text(t.discovery_folder,8192))return {};break;
    case FileManagementTaskKind::inspect_files:{std::uint16_t length{};if(!r.u16(length)||!length||length>512)return {};std::vector<std::uint8_t> nested(length);if(!r.raw(nested.data(),nested.size()))return {};const auto query=decode_file_management_read_request(nested);if(!query)return {};t.inspection_request=*query;break;}
    case FileManagementTaskKind::open_view:if(!nonnegative64(r,t.view_file.value)||!file_read_bool(r,t.view_archive))return {};break;
    case FileManagementTaskKind::scan_report:
    case FileManagementTaskKind::page_view:
    case FileManagementTaskKind::view_archive_index:if(!file_read_identifier(r,t.target)||!r.u16(t.view_page))return {};break;
    case FileManagementTaskKind::read_view_member:if(!file_read_identifier(r,t.target)||!r.u16(t.view_member))return {};break;
    default:if(!file_read_identifier(r,t.target))return {};break;
    }
    return r.complete()&&valid_file_management_task(t)?std::optional<FileManagementTask>{t}:std::nullopt;
}
std::vector<std::uint8_t> encode_file_management_status(const FileManagementJobStatus& s){
    if(!valid_file_management_status(s))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(s.code));if(s.code!=LocalAccountResultCode::ok)return w.take();
    file_write_identifier(w,s.job);w.u8(static_cast<std::uint8_t>(s.kind));w.u8(static_cast<std::uint8_t>(s.state));w.u64(s.bytes_done);w.u64(s.bytes_total);w.u64(s.elapsed_millis);w.u8(s.cancellation_requested?1:0);w.u8(s.journal_confirmed?1:0);w.u64(s.result.value);w.text(s.detail,16384);
    w.u64(s.duplicate_contents);w.u8(s.preview?1:0);if(s.preview)file_write_import(w,*s.preview);w.u8(s.purge_preview?1:0);if(s.purge_preview)file_write_purge(w,*s.purge_preview,true);
    w.u8(s.discovery?1:0);if(s.discovery){const auto& d=*s.discovery;file_write_identifier(w,d.cursor);w.u64(d.area.value);w.u8(d.finished?1:0);w.u8(static_cast<std::uint8_t>(d.names.size()));for(const auto& name:d.names)w.text(name,kFileNameBytes);w.text(d.source_folder,8192);}
    w.u8(s.inspection?1:0);if(s.inspection){const auto nested=encode_file_management_read_snapshot(s.inspection->catalog);w.u32(static_cast<std::uint32_t>(nested.size()));w.raw(nested.data(),nested.size());w.u8(static_cast<std::uint8_t>(s.inspection->presence.size()));for(const auto p:s.inspection->presence)w.u8(static_cast<std::uint8_t>(p));}
    w.u8(s.view?1:0);if(s.view)file_write_view(w,*s.view);
    w.u8(s.report?1:0);if(s.report){w.u16(s.report->page);w.u16(s.report->pages);}
    auto bytes=w.take();return bytes.size()<=kLocalControlMaximumPayloadBytes?bytes:std::vector<std::uint8_t>{};
}
std::optional<FileManagementJobStatus> decode_file_management_status(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>kLocalControlMaximumPayloadBytes)return {};
    Reader r(bytes);FileManagementJobStatus s;
    if(!file_result_code(r,s.code))return {};
    if(s.code!=LocalAccountResultCode::ok)return r.complete()?std::optional<FileManagementJobStatus>{s}:std::nullopt;
    std::uint8_t kind{},state{};bool present{};
    if(!file_read_identifier(r,s.job)||!r.u8(kind)||!r.u8(state)||!r.u64(s.bytes_done)||!r.u64(s.bytes_total)||!r.u64(s.elapsed_millis)||!file_read_bool(r,s.cancellation_requested)||!file_read_bool(r,s.journal_confirmed)||!nonnegative64(r,s.result.value)||!r.text(s.detail,16384)||!r.u64(s.duplicate_contents)||!file_read_bool(r,present))return {};
    s.kind=static_cast<FileManagementTaskKind>(kind);s.state=static_cast<FileManagementJobState>(state);
    if(present){FileImportRequest p;if(!file_read_import(r,p))return {};s.preview=p;}
    if(!file_read_bool(r,present))return {};
    if(present){FileIntakePurge p;if(!file_read_purge(r,p,true))return {};s.purge_preview=p;}
    if(!file_read_bool(r,present))return {};
    if(present){FileManagementDiscoveryPage d;std::uint8_t count{};if(!file_read_identifier(r,d.cursor)||!nonnegative64(r,d.area.value)||!file_read_bool(r,d.finished)||!r.u8(count)||count>100)return {};for(unsigned n=0;n<count;++n){std::string name;if(!r.text(name,kFileNameBytes))return {};d.names.push_back(std::move(name));}if(!r.text(d.source_folder,8192))return {};s.discovery=std::move(d);}
    if(!file_read_bool(r,present))return {};
    if(present){FileManagementInspectionPage p;std::uint32_t length{};std::uint8_t count{};if(!r.u32(length)||!length||length>65536)return {};std::vector<std::uint8_t> nested(length);if(!r.raw(nested.data(),nested.size()))return {};const auto catalog=decode_file_management_read_snapshot(nested);if(!catalog||!r.u8(count)||count>kFileManagementRecordPage)return {};p.catalog=*catalog;for(unsigned n=0;n<count;++n){std::uint8_t value{};if(!r.u8(value)||value>static_cast<unsigned>(FilePayloadPresence::pending))return {};p.presence.push_back(static_cast<FilePayloadPresence>(value));}s.inspection=std::move(p);}
    if(!file_read_bool(r,present))return {};
    if(present){FileManagementViewPage v;if(!file_read_view(r,v))return {};s.view=std::move(v);}
    if(!file_read_bool(r,present))return {};
    if(present){FileManagementJobStatus::Report page;if(!r.u16(page.page)||!r.u16(page.pages))return {};s.report=page;}
    return r.complete()&&valid_file_management_status(s)?std::optional<FileManagementJobStatus>{s}:std::nullopt;
}
