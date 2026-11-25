namespace ftn_hatch_detail {
namespace app=application;
app::FtnDigest fingerprint(const app::FtnHatchRecord& record){
    file_detail::Fingerprint f("FTN-local-hatch-v1");const auto bytes=app::encode_ftn_hatch_request(record.request);f.text({reinterpret_cast<const char*>(bytes.data()),bytes.size()});f.payload(record.payload);f.text(record.filename);f.number(record.source_area.value);f.number(static_cast<unsigned>(record.source_origin));
    if(record.source_receipt)f.text({reinterpret_cast<const char*>(record.source_receipt->bytes.data()),record.source_receipt->bytes.size()});else f.text({});f.number(record.created);f.number(record.crc);return f.value();
}
app::FtnHatchRecord read(sqlite3* db,const app::Identifier128& id){
    SqliteStatement q(db,"SELECT request,fingerprint,source_file,source_area,filename,source_origin,source_receipt,payload_bytes,payload_sha256,native_volume,native_file_id,payload_crc,created_utc,state,reason FROM sfx_ftn_local_hatches WHERE operation_id=?1","read immutable LOCAL hatch and original source snapshot");q.bind(1,id);if(!q.row())message_detail::unavailable();
    app::FtnHatchRecord out;const auto request=app::decode_ftn_hatch_request(q.blob(0));if(!request||!(request->operation==id)||request->file.value!=q.int64(2))file_detail::corrupt();out.request=*request;out.source_area={q.int64(3)};out.filename=q.text(4);out.source_origin=static_cast<app::FileOrigin>(q.int64(5));if(!q.is_null(6))out.source_receipt=ftn_work_detail::identifier(q,6);out.payload={static_cast<std::uint64_t>(q.int64(7)),file_detail::digest(q,8),q.text(9),q.text(10)};out.crc=static_cast<std::uint32_t>(q.int64(11));out.created=q.int64(12);out.state=static_cast<app::FtnHatchState>(q.int64(13));out.reason=q.text(14);if(fingerprint(out)!=file_detail::digest(q,1))file_detail::corrupt();SqliteStatement claim(db,"SELECT volume,native_id FROM sfx_ftn_hatch_preparers WHERE operation_id=?1","read exact retained hatch preparer object");claim.bind(1,id);if(claim.row())out.preparer=app::FileObjectIdentity{claim.text(0),claim.text(1)};return out;
}
void current(sqlite3* db,const app::FtnHatchRecord& h){
    if(h.state==app::FtnHatchState::queued)message_detail::conflict();
    const auto c=ftn_detail::config(db);if(c.revision!=h.request.configuration_revision||c.settings.mode==app::FtnMailerMode::disabled||read_ftn_serial_state(db).recovery_required)message_detail::conflict();
    const auto& network=ftn_work_detail::network(c,h.request.network);const auto origin=app::parse_ftn_address(h.request.origin);if(!network.enabled||!origin||!app::ftn_local_address(network,*origin))message_detail::unavailable();
    for(const auto& link:h.request.destinations)if(!app::ftn_echo_permission(c,h.request.network,link,h.request.tag,true,false))message_detail::unavailable();
    const auto source=file_detail::managed(db,h.request.file);if(source.purged||source.operation_pending||source.file.state!=app::FileCatalogState::available||source.file.revision!=h.request.file_revision||source.file.name!=h.filename||!(source.file.area==h.source_area)||source.file.origin!=h.source_origin||!file_detail::same_payload(source.payload,h.payload))message_detail::conflict();
}
void authorize_spool(sqlite3* db,const app::FtnSpoolObject& value){
    const auto hatch=read(db,value.job.operation);current(db,hatch);if(!hatch.preparer)message_detail::conflict();const auto& r=hatch.request;
    if(r.configuration_revision!=value.configuration_revision||!ftn_detail::same(r.network,value.job.network)||std::none_of(r.destinations.begin(),r.destinations.end(),[&](const auto& link){return ftn_detail::same(link,value.job.link);}))message_detail::conflict();
    if(value.job.kind==app::FtnOutboundKind::payload&&(value.job.file.name!=hatch.filename||value.job.file.size!=hatch.payload.bytes||value.job.digest!=hatch.payload.sha256))message_detail::conflict();
}
}
