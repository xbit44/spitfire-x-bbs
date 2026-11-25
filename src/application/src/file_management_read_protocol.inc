namespace {
bool file_read_signed_time(Reader& r,std::int64_t& value){std::uint64_t n{};if(!r.u64(n))return false;value=n<=INT64_MAX?static_cast<std::int64_t>(n):-1-static_cast<std::int64_t>(UINT64_MAX-n);return valid_utc_seconds(value);}
void file_write_managed(Writer& w,const ManagedFile& m){const auto& f=m.file;
    w.u64(f.id.value);w.u64(f.area.value);w.u64(f.revision);w.u64(f.publication_sequence);w.u64(static_cast<std::uint64_t>(f.published_utc));w.u64(static_cast<std::uint64_t>(f.uploaded_utc));w.text(f.name,kFileNameBytes);file_write_descriptions(w,f.descriptions);w.text(f.public_uploader,64);w.u8(f.uploader?1:0);if(f.uploader)w.u64(*f.uploader);w.u64(f.bytes);w.u64(f.downloads);w.raw(f.sha256.data(),f.sha256.size());w.u8(static_cast<std::uint8_t>(f.origin));w.u8(static_cast<std::uint8_t>(f.state));w.u8(f.sysop_only?1:0);
    file_write_payload(w,m.payload);w.text(m.removed_key,128);w.u8(m.purged?1:0);w.u8(m.operation_pending?1:0);w.u64(m.intended_area.value);w.u8(m.reupload_blocked?1:0);w.text(m.native_basename,32);
}
bool file_read_managed(Reader& r,ManagedFile& m){auto& f=m.file;bool uploader{};std::uint8_t origin{},state{};
    if(!nonnegative64(r,f.id.value)||!nonnegative64(r,f.area.value)||!nonnegative64(r,f.revision)||!nonnegative64(r,f.publication_sequence)||!file_read_signed_time(r,f.published_utc)||!file_read_signed_time(r,f.uploaded_utc)||!r.text(f.name,kFileNameBytes)||!file_read_descriptions(r,f.descriptions)||!r.text(f.public_uploader,64)||!file_read_bool(r,uploader))return false;
    if(uploader){AccountId id{};if(!nonnegative64(r,id)||!id)return false;f.uploader=id;}
    if(!r.u64(f.bytes)||!r.u64(f.downloads)||!r.raw(f.sha256.data(),f.sha256.size())||!r.u8(origin)||!r.u8(state)||!file_read_bool(r,f.sysop_only)||!file_read_payload(r,m.payload)||!r.text(m.removed_key,128)||!file_read_bool(r,m.purged)||!file_read_bool(r,m.operation_pending)||!nonnegative64(r,m.intended_area.value)||!file_read_bool(r,m.reupload_blocked)||!r.text(m.native_basename,32))return false;
    f.origin=static_cast<FileOrigin>(origin);f.state=static_cast<FileCatalogState>(state);return true;
}
}
std::vector<std::uint8_t> encode_file_management_read_request(const FileManagementReadRequest& q){
    if(!valid_file_management_read_request(q))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(q.kind));
    if(q.kind==FileManagementQueryKind::records){const auto& f=q.filter;w.u64(f.area.value);w.u8(f.state?static_cast<std::uint8_t>(*f.state)+1:0);w.u8(static_cast<std::uint8_t>(f.field));w.text(f.search,128);w.u64(q.after.value);}
    else if(q.kind==FileManagementQueryKind::detail)w.u64(q.selection.value);else{if(q.kind==FileManagementQueryKind::hatches)w.u64(q.selection.value);w.raw(q.pending_after.bytes.data(),q.pending_after.bytes.size());}
    return w.take();
}
std::optional<FileManagementReadRequest> decode_file_management_read_request(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>65536)return {};
    Reader r(bytes);FileManagementReadRequest q;std::uint8_t kind{};if(!r.u8(kind)||kind>static_cast<unsigned>(FileManagementQueryKind::hatches))return {};q.kind=static_cast<FileManagementQueryKind>(kind);
    if(q.kind==FileManagementQueryKind::records){std::uint8_t state{},field{};if(!nonnegative64(r,q.filter.area.value)||!r.u8(state)||state>3||!r.u8(field)||field>3||!r.text(q.filter.search,128)||!nonnegative64(r,q.after.value))return {};if(state)q.filter.state=static_cast<FileCatalogState>(state-1);q.filter.field=static_cast<FileManagementSearch>(field);}
    else if(q.kind==FileManagementQueryKind::detail){if(!nonnegative64(r,q.selection.value))return {};}
    else{if(q.kind==FileManagementQueryKind::hatches&&!nonnegative64(r,q.selection.value))return {};if(!r.raw(q.pending_after.bytes.data(),q.pending_after.bytes.size()))return {};}
    return r.complete()&&valid_file_management_read_request(q)?std::optional<FileManagementReadRequest>{q}:std::nullopt;
}
std::vector<std::uint8_t> encode_file_management_read_snapshot(const FileManagementReadSnapshot& s){
    if(!valid_file_management_read_snapshot(s))return {};
    Writer w;w.u8(static_cast<std::uint8_t>(s.code));w.u8(static_cast<std::uint8_t>(s.kind));if(s.code!=LocalAccountResultCode::ok)return w.take();
    if(s.kind==FileManagementQueryKind::records||s.kind==FileManagementQueryKind::detail){w.u8(static_cast<std::uint8_t>(s.records.size()));for(const auto& row:s.records)file_write_managed(w,row);w.u64(s.next.value);}
    else{w.u8(static_cast<std::uint8_t>(s.pending.size()));for(const auto& row:s.pending){file_write_identifier(w,row.operation);w.u64(row.file.value);w.u64(row.area.value);w.u8(row.state);w.u8(row.action);w.text(row.filename,kFileNameBytes);w.text(row.detail,512);if(s.kind==FileManagementQueryKind::hatches){const auto hatch=encode_ftn_hatch_request(*row.hatch);w.u32(static_cast<std::uint32_t>(hatch.size()));w.raw(hatch.data(),hatch.size());}}w.raw(s.pending_next.bytes.data(),s.pending_next.bytes.size());}
    auto result=w.take();return result.size()<=kLocalControlMaximumPayloadBytes?result:std::vector<std::uint8_t>{};
}
std::optional<FileManagementReadSnapshot> decode_file_management_read_snapshot(const std::vector<std::uint8_t>& bytes){
    if(bytes.size()>65536)return {};
    Reader r(bytes);FileManagementReadSnapshot s;std::uint8_t kind{},count{};
    if(!file_result_code(r,s.code)||!r.u8(kind)||kind>static_cast<unsigned>(FileManagementQueryKind::hatches))return {};
    s.kind=static_cast<FileManagementQueryKind>(kind);
    if(s.code!=LocalAccountResultCode::ok)return r.complete()?std::optional<FileManagementReadSnapshot>{s}:std::nullopt;
    if(!r.u8(count))return {};
    if(s.kind==FileManagementQueryKind::records||s.kind==FileManagementQueryKind::detail){if(count>kFileManagementRecordPage)return {};for(unsigned n=0;n<count;++n){ManagedFile row;if(!file_read_managed(r,row))return {};s.records.push_back(std::move(row));}if(!nonnegative64(r,s.next.value))return {};}
    else{if(count>kFileManagementPendingPage)return {};for(unsigned n=0;n<count;++n){FileManagementPending row;if(!file_read_identifier(r,row.operation)||!nonnegative64(r,row.file.value)||!nonnegative64(r,row.area.value)||!r.u8(row.state)||!r.u8(row.action)||!r.text(row.filename,kFileNameBytes)||!r.text(row.detail,512))return {};if(s.kind==FileManagementQueryKind::hatches){std::uint32_t length{};if(!r.u32(length)||!length||length>60000)return {};std::vector<std::uint8_t> nested(length);if(!r.raw(nested.data(),nested.size()))return {};row.hatch=decode_ftn_hatch_request(nested);if(!row.hatch)return {};}s.pending.push_back(std::move(row));}if(!r.raw(s.pending_next.bytes.data(),s.pending_next.bytes.size()))return {};}
    return r.complete()&&valid_file_management_read_snapshot(s)?std::optional<FileManagementReadSnapshot>{s}:std::nullopt;
}
