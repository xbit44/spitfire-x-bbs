// QA18: derive only from the fixed migration21 predecessor, never installed
// schema text. Every copied column is named; IDs and AUTOINCREMENT history stay.
std::string ftn_file_catalog_migration(sqlite3* fixed){
    sqlite3_stmt* raw{};
    if(sqlite3_prepare_v2(fixed,"SELECT type,name,sql FROM sqlite_schema WHERE type IN('index','trigger') AND sql IS NOT NULL AND instr(sql,'sfx_file_catalog')>0 ORDER BY type,name",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN fixed file dependencies");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> dependent(raw,sqlite3_finalize);std::string drops,recreate;
    while(sqlite3_step(raw)==SQLITE_ROW){const auto type=std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw,0))),name=std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw,1)));drops+="DROP "+type+" "+name+";\n";recreate+=reinterpret_cast<const char*>(sqlite3_column_text(raw,2));recreate+=";\n";}
    raw=nullptr;if(sqlite3_prepare_v2(fixed,"SELECT sql FROM sqlite_schema WHERE type='table' AND name='sfx_file_catalog'",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN fixed file definition");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> table(raw,sqlite3_finalize);if(sqlite3_step(raw)!=SQLITE_ROW)throw std::runtime_error("FTN fixed file table absent");std::string ddl=reinterpret_cast<const char*>(sqlite3_column_text(raw,0));
    const auto replace_once=[&](std::string_view before,std::string_view after){const auto at=ddl.find(before);if(at==ddl.npos||ddl.find(before,at+before.size())!=ddl.npos)throw std::runtime_error("FTN fixed file definition changed");ddl.replace(at,before.size(),after);};
    replace_once("CREATE TABLE sfx_file_catalog (","CREATE TABLE sfx_ftn_catalog22 (");
    replace_once("public_uploader TEXT NOT NULL CHECK(length(CAST(public_uploader AS BLOB)) BETWEEN 1 AND 64)","public_uploader TEXT NOT NULL CHECK(length(CAST(public_uploader AS BLOB))<=64 AND (length(CAST(public_uploader AS BLOB))>=1 OR origin=3))");
    replace_once("origin INTEGER NOT NULL CHECK(origin BETWEEN 0 AND 2)","origin INTEGER NOT NULL CHECK(origin BETWEEN 0 AND 3) CHECK(origin<>3 OR uploader_account_id IS NULL)");
    const std::string columns="file_id,area_id,filename,filename_key,short_description,long_description,public_uploader,uploader_account_id,byte_count,content_sha256,native_volume,native_file_id,original_upload_utc,published_utc,publication_sequence,download_count,origin,catalog_state,sysop_only,revision";
    return drops+"CREATE TABLE sfx_ftn_file_sequence22(seq INTEGER NOT NULL);\nINSERT INTO sfx_ftn_file_sequence22 SELECT seq FROM sqlite_sequence WHERE name='sfx_file_catalog';\n"+ddl+";\nINSERT INTO sfx_ftn_catalog22("+columns+") SELECT "+columns+" FROM sfx_file_catalog;\nDROP TABLE sfx_file_catalog;\nALTER TABLE sfx_ftn_catalog22 RENAME TO sfx_file_catalog;\nDELETE FROM sqlite_sequence WHERE name IN('sfx_file_catalog','sfx_ftn_catalog22');\nINSERT INTO sqlite_sequence(name,seq) SELECT 'sfx_file_catalog',seq FROM sfx_ftn_file_sequence22;\nDROP TABLE sfx_ftn_file_sequence22;\n"+recreate+R"SQL(
CREATE TABLE sfx_ftn_file_receipts (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),
 tic_receipt BLOB NOT NULL UNIQUE REFERENCES sfx_ftn_ingress(receipt_id),
 payload_receipt BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 link TEXT NOT NULL CHECK(length(link) BETWEEN 1 AND 48),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128),
 area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>=1),
 area_revision INTEGER NOT NULL CHECK(area_revision>=1),
 tic_sha256 BLOB NOT NULL CHECK(length(tic_sha256)=32),
 payload_sha256 BLOB NOT NULL CHECK(length(payload_sha256)=32),
 payload_bytes INTEGER NOT NULL CHECK(payload_bytes BETWEEN 0 AND 1073741824),
 payload_crc INTEGER NOT NULL CHECK(payload_crc BETWEEN 0 AND 4294967295),
 native_volume TEXT NOT NULL CHECK(length(native_volume) BETWEEN 1 AND 128),
 native_file_id TEXT NOT NULL CHECK(length(native_file_id) BETWEEN 1 AND 128),
 origin_address TEXT NOT NULL CHECK(length(origin_address) BETWEEN 1 AND 128),
 from_address TEXT NOT NULL CHECK(length(from_address) BETWEEN 1 AND 128),
 filename TEXT NOT NULL CHECK(length(CAST(filename AS BLOB)) BETWEEN 1 AND 128),
 initial_file_id INTEGER NOT NULL UNIQUE REFERENCES sfx_file_catalog(file_id),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0)
) STRICT;
CREATE TABLE sfx_ftn_file_lineage (
 file_id INTEGER PRIMARY KEY REFERENCES sfx_file_catalog(file_id),
 receipt_operation BLOB NOT NULL REFERENCES sfx_ftn_file_receipts(operation_id),
 source_file_id INTEGER REFERENCES sfx_file_catalog(file_id),
 copy_operation BLOB UNIQUE REFERENCES sfx_file_operations(operation_id),
 CHECK((source_file_id IS NULL AND copy_operation IS NULL) OR (source_file_id IS NOT NULL AND copy_operation IS NOT NULL AND source_file_id<>file_id))
) STRICT;
CREATE INDEX sfx_ftn_payload_candidates ON sfx_ftn_ingress(network,link,wire_name COLLATE NOCASE,state,receipt_id);
CREATE INDEX sfx_ftn_file_payload_receipt ON sfx_ftn_file_receipts(payload_receipt);
CREATE TABLE sfx_ftn_file_publications (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),
 tic_receipt BLOB NOT NULL UNIQUE REFERENCES sfx_ftn_ingress(receipt_id),
 payload_receipt BLOB NOT NULL UNIQUE REFERENCES sfx_ftn_ingress(receipt_id),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>=1),
 area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
 area_revision INTEGER NOT NULL CHECK(area_revision>=1),
 storage_path TEXT NOT NULL CHECK(length(storage_path) BETWEEN 1 AND 2048),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 link TEXT NOT NULL CHECK(length(link) BETWEEN 1 AND 48),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128),
 filename TEXT NOT NULL CHECK(length(CAST(filename AS BLOB)) BETWEEN 1 AND 128),
 filename_key TEXT NOT NULL,
 replaces_pattern TEXT NOT NULL CHECK(length(replaces_pattern)<=128),
 origin_address TEXT NOT NULL CHECK(length(origin_address) BETWEEN 1 AND 128),
 from_address TEXT NOT NULL CHECK(length(from_address) BETWEEN 1 AND 128),
 short_description TEXT NOT NULL,
 long_description TEXT NOT NULL,
 tic_bytes INTEGER NOT NULL CHECK(tic_bytes BETWEEN 1 AND 65536),
 tic_sha256 BLOB NOT NULL CHECK(length(tic_sha256)=32),
 tic_volume TEXT NOT NULL CHECK(length(tic_volume) BETWEEN 1 AND 128),
 tic_native_id TEXT NOT NULL CHECK(length(tic_native_id) BETWEEN 1 AND 128),
 payload_bytes INTEGER NOT NULL CHECK(payload_bytes BETWEEN 0 AND 1073741824),
 payload_sha256 BLOB NOT NULL CHECK(length(payload_sha256)=32),
 payload_volume TEXT NOT NULL CHECK(length(payload_volume) BETWEEN 1 AND 128),
 payload_native_id TEXT NOT NULL CHECK(length(payload_native_id) BETWEEN 1 AND 128),
 payload_crc INTEGER NOT NULL CHECK(payload_crc BETWEEN 0 AND 4294967295),
 password_verified INTEGER NOT NULL CHECK(password_verified IN(0,1)),
 authenticated_same_link INTEGER NOT NULL CHECK(authenticated_same_link IN(0,1)),
 displaced_file INTEGER REFERENCES sfx_file_catalog(file_id),
 displaced_revision INTEGER NOT NULL CHECK(displaced_revision>=0),
 displaced_bytes INTEGER NOT NULL CHECK(displaced_bytes>=0),
 displaced_sha256 BLOB NOT NULL CHECK(length(displaced_sha256)=32),
 displaced_volume TEXT NOT NULL,
 displaced_native_id TEXT NOT NULL,
 stage_name TEXT NOT NULL,
 recovery_key TEXT NOT NULL UNIQUE CHECK(length(recovery_key)=32),
 phase INTEGER NOT NULL CHECK(phase BETWEEN 0 AND 6),
 result_file INTEGER REFERENCES sfx_file_catalog(file_id),
 reason TEXT NOT NULL CHECK(length(reason)<=64),
 completion_fingerprint BLOB CHECK(completion_fingerprint IS NULL OR length(completion_fingerprint)=32),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0)
) STRICT;
CREATE INDEX sfx_ftn_file_publication_names ON sfx_ftn_file_publications(area_id,filename_key,phase);
CREATE INDEX sfx_ftn_file_publication_displaced ON sfx_ftn_file_publications(displaced_file,phase);
CREATE TABLE sfx_ftn_file_steps (
 operation_id BLOB NOT NULL REFERENCES sfx_ftn_file_publications(operation_id),
 phase INTEGER NOT NULL CHECK(phase BETWEEN 1 AND 5),
 byte_count INTEGER NOT NULL CHECK(byte_count BETWEEN 0 AND 1073741824),
 sha256 BLOB NOT NULL CHECK(length(sha256)=32),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 128),
 native_id TEXT NOT NULL CHECK(length(native_id) BETWEEN 1 AND 128),
 PRIMARY KEY(operation_id,phase)
) STRICT;
CREATE TABLE sfx_ftn_file_displacements (
 operation_id BLOB PRIMARY KEY REFERENCES sfx_ftn_file_publications(operation_id),
 file_id INTEGER NOT NULL REFERENCES sfx_file_catalog(file_id),
 stage_key TEXT NOT NULL UNIQUE CHECK(length(stage_key)=32),
 state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 2),
 owner_operation BLOB REFERENCES sfx_file_operations(operation_id)
) STRICT;
CREATE TABLE sfx_ftn_processing_owners (
 receipt_id BLOB PRIMARY KEY REFERENCES sfx_ftn_ingress(receipt_id),
 kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 2),
 owner_id BLOB NOT NULL CHECK(length(owner_id)=16)
) STRICT;
CREATE TABLE sfx_ftn_local_hatches (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 request BLOB NOT NULL CHECK(length(request) BETWEEN 1 AND 60000),
 fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),
 source_file INTEGER NOT NULL CHECK(source_file>0),
 source_area INTEGER NOT NULL CHECK(source_area>0),
 filename TEXT NOT NULL CHECK(length(CAST(filename AS BLOB)) BETWEEN 1 AND 128),
 source_origin INTEGER NOT NULL CHECK(source_origin BETWEEN 0 AND 3),
 source_receipt BLOB REFERENCES sfx_ftn_file_receipts(operation_id),
 payload_bytes INTEGER NOT NULL CHECK(payload_bytes BETWEEN 0 AND 1073741824),
 payload_sha256 BLOB NOT NULL CHECK(length(payload_sha256)=32),
 native_volume TEXT NOT NULL CHECK(length(native_volume) BETWEEN 1 AND 128),
 native_file_id TEXT NOT NULL CHECK(length(native_file_id) BETWEEN 1 AND 128),
 payload_crc INTEGER NOT NULL CHECK(payload_crc BETWEEN 0 AND 4294967295),
 created_utc INTEGER NOT NULL CHECK(created_utc BETWEEN 0 AND 4294967295),
 state INTEGER NOT NULL DEFAULT 0 CHECK(state BETWEEN 0 AND 2),
 reason TEXT NOT NULL DEFAULT '' CHECK(length(reason)<=64),
 jobs_fingerprint BLOB CHECK(jobs_fingerprint IS NULL OR length(jobs_fingerprint)=32),
 CHECK((source_origin=3 AND source_receipt IS NOT NULL) OR (source_origin<>3 AND source_receipt IS NULL)),
 CHECK((state=1 AND jobs_fingerprint IS NOT NULL) OR (state<>1 AND jobs_fingerprint IS NULL))
) STRICT;
CREATE INDEX sfx_ftn_hatch_source ON sfx_ftn_local_hatches(source_file,state);
CREATE TRIGGER sfx_ftn_hatch_immutable BEFORE UPDATE OF operation_id,request,fingerprint,source_file,source_area,filename,source_origin,source_receipt,payload_bytes,payload_sha256,native_volume,native_file_id,payload_crc,created_utc ON sfx_ftn_local_hatches
BEGIN SELECT RAISE(ABORT,'immutable local hatch intent'); END;
CREATE TRIGGER sfx_ftn_hatch_queued BEFORE UPDATE ON sfx_ftn_local_hatches WHEN OLD.state=1
BEGIN SELECT RAISE(ABORT,'queued local hatch retained'); END;
CREATE TRIGGER sfx_ftn_hatch_delete BEFORE DELETE ON sfx_ftn_local_hatches
BEGIN SELECT RAISE(ABORT,'local hatch history retained'); END;
CREATE TABLE sfx_ftn_hatch_preparers (
 operation_id BLOB PRIMARY KEY REFERENCES sfx_ftn_local_hatches(operation_id),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 20),
 native_id TEXT NOT NULL CHECK(length(native_id) BETWEEN 1 AND 20),
 UNIQUE(volume,native_id)
) STRICT;
CREATE TRIGGER sfx_ftn_hatch_preparer_update BEFORE UPDATE ON sfx_ftn_hatch_preparers
BEGIN SELECT RAISE(ABORT,'immutable local hatch preparer'); END;
CREATE TRIGGER sfx_ftn_hatch_preparer_delete BEFORE DELETE ON sfx_ftn_hatch_preparers
BEGIN SELECT RAISE(ABORT,'local hatch preparer retained'); END;
CREATE TABLE sfx_ftn_spool_owners (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 publication_id BLOB UNIQUE REFERENCES sfx_ftn_file_publications(operation_id),
 hatch_id BLOB UNIQUE REFERENCES sfx_ftn_local_hatches(operation_id),
 CHECK((publication_id IS NOT NULL AND publication_id=operation_id AND hatch_id IS NULL) OR
       (hatch_id IS NOT NULL AND hatch_id=operation_id AND publication_id IS NULL))
) STRICT;
CREATE TRIGGER sfx_ftn_spool_owner_update BEFORE UPDATE ON sfx_ftn_spool_owners
BEGIN SELECT RAISE(ABORT,'immutable typed spool owner'); END;
CREATE TRIGGER sfx_ftn_spool_owner_delete BEFORE DELETE ON sfx_ftn_spool_owners
BEGIN SELECT RAISE(ABORT,'spool owner history retained'); END;
CREATE TABLE sfx_ftn_spool_objects (
 job_id BLOB PRIMARY KEY CHECK(length(job_id)=16),
 publication_id BLOB NOT NULL REFERENCES sfx_ftn_spool_owners(operation_id),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),
 ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 4095),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 link TEXT NOT NULL CHECK(length(link) BETWEEN 1 AND 48),
 kind INTEGER NOT NULL CHECK(kind IN(1,2)),
 flavor INTEGER NOT NULL CHECK(flavor BETWEEN 0 AND 2),
 wire_name TEXT NOT NULL CHECK(length(wire_name) BETWEEN 1 AND 128),
 size INTEGER NOT NULL CHECK(size BETWEEN 0 AND 1073741824),
 wire_time INTEGER NOT NULL CHECK(wire_time>=0),
 content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
 UNIQUE(publication_id,ordinal),CHECK(kind<>1 OR size<=65536)
) STRICT;
CREATE TABLE sfx_ftn_spool_bindings (
 job_id BLOB PRIMARY KEY REFERENCES sfx_ftn_spool_objects(job_id),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 20),
 native_id TEXT NOT NULL CHECK(length(native_id) BETWEEN 1 AND 20),
 UNIQUE(volume,native_id)
) STRICT;
CREATE TABLE sfx_ftn_spool_ready (job_id BLOB PRIMARY KEY REFERENCES sfx_ftn_spool_bindings(job_id)) STRICT;
CREATE TRIGGER sfx_ftn_spool_object_update BEFORE UPDATE ON sfx_ftn_spool_objects
BEGIN SELECT RAISE(ABORT,'immutable FTN spool intent'); END;
CREATE TRIGGER sfx_ftn_spool_object_delete BEFORE DELETE ON sfx_ftn_spool_objects
BEGIN SELECT RAISE(ABORT,'FTN spool intent retained'); END;
CREATE TRIGGER sfx_ftn_spool_binding_update BEFORE UPDATE ON sfx_ftn_spool_bindings
BEGIN SELECT RAISE(ABORT,'immutable FTN spool object'); END;
CREATE TRIGGER sfx_ftn_spool_binding_delete BEFORE DELETE ON sfx_ftn_spool_bindings
BEGIN SELECT RAISE(ABORT,'FTN spool object retained'); END;
CREATE TRIGGER sfx_ftn_spool_ready_update BEFORE UPDATE ON sfx_ftn_spool_ready
BEGIN SELECT RAISE(ABORT,'immutable FTN verified spool'); END;
CREATE TRIGGER sfx_ftn_spool_ready_delete BEFORE DELETE ON sfx_ftn_spool_ready
BEGIN SELECT RAISE(ABORT,'FTN verified spool retained'); END;
CREATE TABLE sfx_ftn_tic_inspections (
 receipt_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 payload_name TEXT NOT NULL CHECK(length(payload_name)<=128),
 reason TEXT NOT NULL CHECK(length(reason)<=96),
 PRIMARY KEY(receipt_id,configuration_revision)
) STRICT;
CREATE TRIGGER sfx_ftn_processing_owner_update BEFORE UPDATE ON sfx_ftn_processing_owners
BEGIN SELECT RAISE(ABORT,'immutable FTN processing ownership'); END;
CREATE TRIGGER sfx_ftn_processing_owner_delete BEFORE DELETE ON sfx_ftn_processing_owners
BEGIN SELECT RAISE(ABORT,'FTN processing ownership retained'); END;
CREATE UNIQUE INDEX sfx_ftn_retained_displacement ON sfx_ftn_file_displacements(file_id) WHERE state=0;
CREATE TABLE sfx_ftn_file_job_echoes (
 job_id BLOB PRIMARY KEY REFERENCES sfx_ftn_outbound(job_id),
 publication_id BLOB NOT NULL REFERENCES sfx_ftn_spool_owners(operation_id),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128),
 payload_job BLOB REFERENCES sfx_ftn_outbound(job_id)
) STRICT;
CREATE TRIGGER sfx_ftn_file_steps_update BEFORE UPDATE ON sfx_ftn_file_steps
BEGIN SELECT RAISE(ABORT,'immutable FTN publication object binding'); END;
CREATE TRIGGER sfx_ftn_file_steps_delete BEFORE DELETE ON sfx_ftn_file_steps
BEGIN SELECT RAISE(ABORT,'FTN publication object binding retained'); END;
CREATE TRIGGER sfx_ftn_file_receipts_update BEFORE UPDATE ON sfx_ftn_file_receipts
BEGIN SELECT RAISE(ABORT,'immutable FTN file receipt'); END;
CREATE TRIGGER sfx_ftn_file_receipts_delete BEFORE DELETE ON sfx_ftn_file_receipts
BEGIN SELECT RAISE(ABORT,'FTN file receipt retained'); END;
CREATE TRIGGER sfx_ftn_file_lineage_update BEFORE UPDATE ON sfx_ftn_file_lineage
BEGIN SELECT RAISE(ABORT,'immutable FTN file lineage'); END;
CREATE TRIGGER sfx_ftn_file_lineage_delete BEFORE DELETE ON sfx_ftn_file_lineage
BEGIN SELECT RAISE(ABORT,'FTN file lineage retained'); END;
CREATE TRIGGER sfx_ftn_file_origin_update BEFORE UPDATE OF origin,uploader_account_id ON sfx_file_catalog
WHEN OLD.origin<>NEW.origin OR (OLD.origin=3 AND NEW.uploader_account_id IS NOT NULL)
BEGIN SELECT RAISE(ABORT,'file content origin retained'); END;
)SQL";
}
