// Section9 additive migration15. No accepted predecessor SQL is modified.
const std::string file_migration_sql = std::string(R"SQL(
CREATE TABLE sfx_file_policy (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    revision INTEGER NOT NULL CHECK(revision>=1),
    catalog_revision INTEGER NOT NULL CHECK(catalog_revision>=1),
    maximum_upload_bytes INTEGER NOT NULL CHECK(maximum_upload_bytes BETWEEN 1 AND 1073741824),
    daily_downloads INTEGER CHECK(daily_downloads BETWEEN 1 AND 1000000),
    ratio_downloads INTEGER CHECK(ratio_downloads BETWEEN 1 AND 1000000),
    ratio_uploads INTEGER CHECK(ratio_uploads BETWEEN 1 AND 1000000),
    lightbar_default INTEGER NOT NULL CHECK(lightbar_default IN(0,1)),
    scan_uploads INTEGER NOT NULL CHECK(scan_uploads IN(0,1)),
    CHECK((ratio_downloads IS NULL)=(ratio_uploads IS NULL))
) STRICT;
INSERT INTO sfx_file_policy VALUES(1,1,1,104857600,NULL,NULL,NULL,1,0);
CREATE TABLE sfx_file_groups (
    group_id INTEGER PRIMARY KEY AUTOINCREMENT,
    name TEXT NOT NULL CHECK(length(CAST(name AS BLOB)) BETWEEN 1 AND 64),
    display_order INTEGER NOT NULL,
    access_minimum INTEGER NOT NULL CHECK(access_minimum BETWEEN 1 AND 999),
    enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
INSERT INTO sfx_file_groups VALUES(1,'Main',0,1,1,1);
CREATE TABLE sfx_file_areas (
    area_id INTEGER PRIMARY KEY AUTOINCREMENT,
    group_id INTEGER NOT NULL REFERENCES sfx_file_groups(group_id),
    area_number INTEGER NOT NULL UNIQUE CHECK(area_number BETWEEN 1 AND 999),
    name TEXT NOT NULL CHECK(length(CAST(name AS BLOB)) BETWEEN 1 AND 64),
    description TEXT NOT NULL CHECK(length(CAST(description AS BLOB))<=256),
    display_order INTEGER NOT NULL,
    kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 2),
    access_minimum INTEGER NOT NULL CHECK(access_minimum BETWEEN 1 AND 999),
    upload_minimum INTEGER NOT NULL CHECK(upload_minimum BETWEEN 1 AND 999),
    download_minimum INTEGER NOT NULL CHECK(download_minimum BETWEEN 1 AND 999),
    uploads_enabled INTEGER NOT NULL CHECK(uploads_enabled IN(0,1)),
    downloads_sysop_only INTEGER NOT NULL CHECK(downloads_sysop_only IN(0,1)),
    enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),
    own_description_edit INTEGER NOT NULL CHECK(own_description_edit IN(0,1)),
    backup_contents INTEGER NOT NULL CHECK(backup_contents IN(0,1)),
    ratio_uploads INTEGER NOT NULL CHECK(ratio_uploads IN(0,1)),
    ratio_downloads INTEGER NOT NULL CHECK(ratio_downloads IN(0,1)),
    maximum_upload_bytes INTEGER CHECK(maximum_upload_bytes BETWEEN 1 AND 1073741824),
    revision INTEGER NOT NULL CHECK(revision>=1),
    sysop_review INTEGER NOT NULL DEFAULT 0 CHECK(sysop_review IN(0,1) AND (kind=0 OR sysop_review=0)),
    CHECK(kind=0 OR downloads_sysop_only=1)
) STRICT;
CREATE UNIQUE INDEX sfx_file_one_special_area ON sfx_file_areas(kind) WHERE kind<>0;
CREATE INDEX sfx_file_areas_group_order ON sfx_file_areas(group_id,display_order,area_id);
INSERT INTO sfx_file_areas VALUES(1,1,1,'General Files','',0,0,1,1,1,0,0,1,0,0,1,1,NULL,1,0);
INSERT INTO sfx_file_areas VALUES(2,1,2,'Uploads','Awaiting Sysop review',1,1,1,1,1,1,1,1,0,0,1,1,NULL,1,0);
INSERT INTO sfx_file_areas VALUES(3,1,3,'Upload to Sysop','Private submissions',2,2,1,1,1,1,1,1,0,0,0,0,NULL,1,0);
CREATE TABLE sfx_file_area_paths (
    area_id INTEGER PRIMARY KEY REFERENCES sfx_file_areas(area_id),
    storage_path TEXT NOT NULL CHECK(length(CAST(storage_path AS BLOB)) BETWEEN 1 AND 2048)
) STRICT;
INSERT INTO sfx_file_area_paths VALUES(1,'Files/General');
INSERT INTO sfx_file_area_paths VALUES(2,'Files/Uploads');
INSERT INTO sfx_file_area_paths VALUES(3,'Files/UploadToSysop');
CREATE TRIGGER sfx_file_area_identity BEFORE UPDATE OF area_id,area_number,kind ON sfx_file_areas
BEGIN SELECT RAISE(ABORT,'File area identities and built-in roles are permanent'); END;
CREATE TABLE sfx_file_security_rules (
    security_level INTEGER PRIMARY KEY REFERENCES sfx_security_levels(security_level),
    daily_downloads INTEGER CHECK(daily_downloads BETWEEN 1 AND 1000000),
    ratio_downloads INTEGER CHECK(ratio_downloads BETWEEN 1 AND 1000000),
    ratio_uploads INTEGER CHECK(ratio_uploads BETWEEN 1 AND 1000000),
    daily_exempt INTEGER NOT NULL CHECK(daily_exempt IN(0,1)),
    ratio_exempt INTEGER NOT NULL CHECK(ratio_exempt IN(0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1),
    CHECK((ratio_downloads IS NULL)=(ratio_uploads IS NULL))
) STRICT;
CREATE TABLE sfx_file_caller_exemptions (
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    daily_exempt INTEGER NOT NULL CHECK(daily_exempt IN(0,1)),
    ratio_exempt INTEGER NOT NULL CHECK(ratio_exempt IN(0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
CREATE TABLE sfx_file_catalog (
    file_id INTEGER PRIMARY KEY AUTOINCREMENT,
    area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
    filename TEXT NOT NULL CHECK(length(CAST(filename AS BLOB)) BETWEEN 1 AND 128),
    filename_key TEXT NOT NULL CHECK(length(CAST(filename_key AS BLOB)) BETWEEN 1 AND 384),
    short_description TEXT NOT NULL CHECK(length(CAST(short_description AS BLOB)) BETWEEN 1 AND 80),
    long_description TEXT NOT NULL CHECK(length(CAST(long_description AS BLOB))<=16384),
    public_uploader TEXT NOT NULL CHECK(length(CAST(public_uploader AS BLOB)) BETWEEN 1 AND 64),
    uploader_account_id INTEGER REFERENCES sfx_accounts(account_id),
    byte_count INTEGER NOT NULL CHECK(byte_count>=0),
    content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
    native_volume TEXT NOT NULL CHECK(length(native_volume) BETWEEN 1 AND 128),
    native_file_id TEXT NOT NULL CHECK(length(native_file_id) BETWEEN 1 AND 128),
    original_upload_utc INTEGER NOT NULL CHECK(original_upload_utc BETWEEN -62135596800 AND 253402300799),
    published_utc INTEGER NOT NULL CHECK(published_utc BETWEEN -62135596800 AND 253402300799),
    publication_sequence INTEGER NOT NULL CHECK(publication_sequence>=0),
    download_count INTEGER NOT NULL CHECK(download_count>=0),
    origin INTEGER NOT NULL CHECK(origin BETWEEN 0 AND 2),
    catalog_state INTEGER NOT NULL CHECK(catalog_state BETWEEN 0 AND 2),
    sysop_only INTEGER NOT NULL CHECK(sysop_only IN(0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
CREATE UNIQUE INDEX sfx_file_current_name ON sfx_file_catalog(area_id,filename_key) WHERE catalog_state<>1;
CREATE UNIQUE INDEX sfx_file_current_payload ON sfx_file_catalog(native_volume,native_file_id) WHERE catalog_state<>1;
CREATE INDEX sfx_file_content_digest ON sfx_file_catalog(content_sha256,byte_count);
CREATE INDEX sfx_file_area_listing ON sfx_file_catalog(area_id,catalog_state,filename_key,file_id);
CREATE TABLE sfx_file_publications (
    publication_sequence INTEGER PRIMARY KEY AUTOINCREMENT,
    file_id INTEGER NOT NULL REFERENCES sfx_file_catalog(file_id),
    area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
    published_utc INTEGER NOT NULL CHECK(published_utc BETWEEN -62135596800 AND 253402300799)
) STRICT;
CREATE INDEX sfx_file_publication_scan ON sfx_file_publications(published_utc,publication_sequence);
CREATE TABLE sfx_file_caller_state (
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    scan_since_utc INTEGER NOT NULL CHECK(scan_since_utc BETWEEN -62135596800 AND 253402300799),
    last_seen_publication INTEGER NOT NULL CHECK(last_seen_publication>=0),
    current_area_id INTEGER REFERENCES sfx_file_areas(area_id),
    lightbar INTEGER NOT NULL CHECK(lightbar IN(0,1)),
    new_file_scan INTEGER NOT NULL CHECK(new_file_scan IN(0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1),
    scan_completed_utc INTEGER CHECK(scan_completed_utc BETWEEN -62135596800 AND 253402300799)
) STRICT;
CREATE TABLE sfx_file_statistics (
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    upload_count INTEGER NOT NULL CHECK(upload_count>=0),
    download_count INTEGER NOT NULL CHECK(download_count>=0),
    upload_bytes INTEGER NOT NULL CHECK(upload_bytes>=0),
    download_bytes INTEGER NOT NULL CHECK(download_bytes>=0),
    eligible_uploads INTEGER NOT NULL CHECK(eligible_uploads>=0),
    eligible_downloads INTEGER NOT NULL CHECK(eligible_downloads>=0)
) STRICT;
CREATE TABLE sfx_file_transfers (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    session_id BLOB NOT NULL REFERENCES sfx_account_sessions(session_id),
    direction INTEGER NOT NULL CHECK(direction IN(0,1)),
    file_id INTEGER REFERENCES sfx_file_catalog(file_id),
    area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
    file_revision INTEGER NOT NULL CHECK(file_revision>=0),
    state INTEGER NOT NULL CHECK(state IN(0,1,2)),
    byte_count INTEGER NOT NULL CHECK(byte_count>=0),
    content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
    ratio_participating INTEGER NOT NULL CHECK(ratio_participating IN(0,1)),
    bbs_local_date TEXT NOT NULL CHECK(length(bbs_local_date)=10),
    started_utc INTEGER NOT NULL CHECK(started_utc BETWEEN -62135596800 AND 253402300799),
    finished_utc INTEGER CHECK(finished_utc BETWEEN -62135596800 AND 253402300799),
    CHECK((state=0)=(finished_utc IS NULL))
) STRICT;
CREATE INDEX sfx_file_transfer_allowance ON sfx_file_transfers(account_id,direction,state,bbs_local_date);
CREATE TRIGGER sfx_file_transfer_terminal BEFORE UPDATE ON sfx_file_transfers WHEN OLD.state<>0
BEGIN SELECT RAISE(ABORT,'Completed file transfer receipts are immutable'); END;
CREATE TRIGGER sfx_file_transfer_retain BEFORE DELETE ON sfx_file_transfers
BEGIN SELECT RAISE(ABORT,'File transfer receipts are retained'); END;
)SQL") + R"SQL(CREATE TABLE sfx_file_operations (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    action INTEGER NOT NULL CHECK(action BETWEEN 0 AND 7),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN(0,1)),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    file_id INTEGER REFERENCES sfx_file_catalog(file_id),
    source_area_id INTEGER REFERENCES sfx_file_areas(area_id),
    destination_area_id INTEGER REFERENCES sfx_file_areas(area_id),
    expected_revision INTEGER NOT NULL CHECK(expected_revision>=0),
    source_name TEXT NOT NULL CHECK(length(CAST(source_name AS BLOB))<=128),
    destination_name TEXT NOT NULL CHECK(length(CAST(destination_name AS BLOB))<=128),
    destination_revision INTEGER NOT NULL CHECK(destination_revision>=0),
    destination_key TEXT NOT NULL CHECK(length(CAST(destination_key AS BLOB))<=384),
    short_description TEXT NOT NULL CHECK(length(CAST(short_description AS BLOB))<=80),
    long_description TEXT NOT NULL CHECK(length(CAST(long_description AS BLOB))<=16384),
    public_uploader TEXT NOT NULL CHECK(length(CAST(public_uploader AS BLOB))<=64),
    sysop_only INTEGER NOT NULL CHECK(sysop_only IN(0,1)),
    allow_duplicate INTEGER NOT NULL CHECK(allow_duplicate IN(0,1)),
    source_bytes INTEGER NOT NULL CHECK(source_bytes>=0),
    source_sha256 BLOB NOT NULL CHECK(length(source_sha256)=32),
    source_volume TEXT NOT NULL CHECK(length(source_volume) BETWEEN 1 AND 128),
    source_native_id TEXT NOT NULL CHECK(length(source_native_id) BETWEEN 1 AND 128),
    removed_key TEXT NOT NULL CHECK(length(removed_key) IN(0,32)),
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 3),
    result_file_id INTEGER REFERENCES sfx_file_catalog(file_id),
    detail TEXT NOT NULL CHECK(length(CAST(detail AS BLOB))<=512),
    completion_fingerprint BLOB CHECK(completion_fingerprint IS NULL OR length(completion_fingerprint)=32),
    occurred_utc INTEGER NOT NULL CHECK(occurred_utc BETWEEN -62135596800 AND 253402300799),
    placed_volume TEXT NOT NULL DEFAULT '' CHECK(length(placed_volume)<=128),
    placed_native_id TEXT NOT NULL DEFAULT '' CHECK(length(placed_native_id)<=128),
    CHECK((placed_volume='' AND placed_native_id='') OR (placed_volume<>'' AND placed_native_id<>'')),
    CHECK((installation_owner=1 AND actor_account_id IS NULL) OR (installation_owner=0 AND actor_account_id IS NOT NULL))
) STRICT;
CREATE UNIQUE INDEX sfx_file_operation_source ON sfx_file_operations(file_id) WHERE state IN(0,2);
CREATE UNIQUE INDEX sfx_file_operation_destination ON sfx_file_operations(destination_area_id,destination_key) WHERE state IN(0,2) AND action IN(1,2,3,5);
CREATE TRIGGER sfx_file_operation_terminal BEFORE UPDATE ON sfx_file_operations WHEN OLD.state IN(1,3)
BEGIN SELECT RAISE(ABORT,'Completed file operations are immutable'); END;
CREATE TRIGGER sfx_file_operation_retain BEFORE DELETE ON sfx_file_operations
BEGIN SELECT RAISE(ABORT,'File operation receipts are retained'); END;
CREATE TABLE sfx_file_removed (
    file_id INTEGER PRIMARY KEY REFERENCES sfx_file_catalog(file_id),
    operation_id BLOB NOT NULL REFERENCES sfx_file_operations(operation_id),
    stage_key TEXT NOT NULL UNIQUE CHECK(length(stage_key)=32 AND stage_key NOT GLOB '*[^0-9a-f]*'),
    purged INTEGER NOT NULL CHECK(purged IN(0,1))
) STRICT;
)SQL" + R"SQL(CREATE TABLE sfx_file_imports (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN(0,1)),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    import_mode INTEGER NOT NULL CHECK(import_mode BETWEEN 0 AND 2),
    source_path TEXT NOT NULL CHECK(length(CAST(source_path AS BLOB)) BETWEEN 1 AND 8192),
    area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
    area_revision INTEGER NOT NULL CHECK(area_revision>=1),
    filename TEXT NOT NULL CHECK(length(CAST(filename AS BLOB)) BETWEEN 1 AND 128),
    filename_key TEXT NOT NULL CHECK(length(CAST(filename_key AS BLOB)) BETWEEN 1 AND 384),
    short_description TEXT NOT NULL CHECK(length(CAST(short_description AS BLOB))<=80),
    long_description TEXT NOT NULL CHECK(length(CAST(long_description AS BLOB))<=16384),
    public_uploader TEXT NOT NULL CHECK(length(CAST(public_uploader AS BLOB)) BETWEEN 1 AND 64),
    uploader_account_id INTEGER REFERENCES sfx_accounts(account_id),
    original_upload_utc INTEGER NOT NULL CHECK(original_upload_utc BETWEEN -62135596800 AND 253402300799),
    source_bytes INTEGER NOT NULL CHECK(source_bytes>=0),
    source_sha256 BLOB NOT NULL CHECK(length(source_sha256)=32),
    source_volume TEXT NOT NULL CHECK(length(source_volume) BETWEEN 1 AND 128),
    source_native_id TEXT NOT NULL CHECK(length(source_native_id) BETWEEN 1 AND 128),
    sysop_only INTEGER NOT NULL CHECK(sysop_only IN(0,1)),
    allow_duplicate INTEGER NOT NULL CHECK(allow_duplicate IN(0,1)),
    placed_volume TEXT NOT NULL DEFAULT '' CHECK(length(placed_volume)<=128),
    placed_native_id TEXT NOT NULL DEFAULT '' CHECK(length(placed_native_id)<=128),
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 3),
    result_file_id INTEGER REFERENCES sfx_file_catalog(file_id),
    detail TEXT NOT NULL CHECK(length(CAST(detail AS BLOB))<=512),
    occurred_utc INTEGER NOT NULL CHECK(occurred_utc BETWEEN -62135596800 AND 253402300799),
    completion_fingerprint BLOB CHECK(completion_fingerprint IS NULL OR length(completion_fingerprint)=32),
    CHECK((installation_owner=1 AND actor_account_id IS NULL) OR (installation_owner=0 AND actor_account_id IS NOT NULL)),
    CHECK((placed_volume='' AND placed_native_id='') OR (placed_volume<>'' AND placed_native_id<>''))
) STRICT;
CREATE UNIQUE INDEX sfx_file_import_destination ON sfx_file_imports(area_id,filename_key) WHERE state IN(0,2);
CREATE UNIQUE INDEX sfx_file_import_source ON sfx_file_imports(source_volume,source_native_id) WHERE state IN(0,2);
CREATE TRIGGER sfx_file_import_terminal BEFORE UPDATE ON sfx_file_imports WHEN OLD.state IN(1,3)
BEGIN SELECT RAISE(ABORT,'Terminal file import receipts are immutable'); END;
CREATE TRIGGER sfx_file_import_retain BEFORE DELETE ON sfx_file_imports
BEGIN SELECT RAISE(ABORT,'File import receipts are retained'); END;
CREATE TABLE sfx_file_intake (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    session_id BLOB NOT NULL REFERENCES sfx_account_sessions(session_id),
    stage_key TEXT NOT NULL UNIQUE CHECK(length(stage_key)=32 AND stage_key NOT GLOB '*[^0-9a-f]*'),
    filename TEXT NOT NULL CHECK(length(CAST(filename AS BLOB))<=128),
    filename_key TEXT NOT NULL CHECK(length(CAST(filename_key AS BLOB))<=384),
    intended_area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
    area_revision INTEGER NOT NULL CHECK(area_revision>=1),
    maximum_bytes INTEGER NOT NULL CHECK(maximum_bytes BETWEEN 1 AND 1073741824),
    private_submission INTEGER NOT NULL CHECK(private_submission IN(0,1)),
    scan_required INTEGER NOT NULL CHECK(scan_required IN(0,1)),
    transfer_complete INTEGER NOT NULL CHECK(transfer_complete IN(0,1)),
    received_bytes INTEGER NOT NULL CHECK(received_bytes>=0),
    content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
    native_volume TEXT NOT NULL CHECK(length(native_volume)<=128),
    native_file_id TEXT NOT NULL CHECK(length(native_file_id)<=128),
    scan_verdict INTEGER NOT NULL CHECK(scan_verdict BETWEEN 0 AND 5),
    detections TEXT NOT NULL CHECK(length(CAST(detections AS BLOB))<=8192),
    detail TEXT NOT NULL CHECK(length(CAST(detail AS BLOB))<=512),
    state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5),
    created_utc INTEGER NOT NULL CHECK(created_utc BETWEEN -62135596800 AND 253402300799),
    file_id INTEGER REFERENCES sfx_file_catalog(file_id),
    bbs_local_date TEXT NOT NULL CHECK(length(bbs_local_date)=10),
    public_uploader TEXT NOT NULL CHECK(length(CAST(public_uploader AS BLOB)) BETWEEN 1 AND 64),
    short_description TEXT NOT NULL CHECK(length(CAST(short_description AS BLOB))<=80),
    long_description TEXT NOT NULL CHECK(length(CAST(long_description AS BLOB))<=16384),
    sysop_only INTEGER NOT NULL CHECK(sysop_only IN(0,1)),
    placement_area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
    placement_revision INTEGER NOT NULL CHECK(placement_revision>=1),
    CHECK((state=3)=(file_id IS NOT NULL))
) STRICT;
CREATE UNIQUE INDEX sfx_file_intake_reserved_name ON sfx_file_intake(placement_area_id,filename_key) WHERE state=2;
CREATE TRIGGER sfx_file_intake_terminal BEFORE UPDATE ON sfx_file_intake WHEN OLD.state IN(3,4)
BEGIN SELECT RAISE(ABORT,'Terminal intake receipts are immutable'); END;
CREATE TRIGGER sfx_file_intake_retain BEFORE DELETE ON sfx_file_intake
BEGIN SELECT RAISE(ABORT,'Intake receipts are retained'); END;
CREATE TABLE sfx_file_audit (
    audit_id INTEGER PRIMARY KEY AUTOINCREMENT,
    operation_id BLOB NOT NULL CHECK(length(operation_id)=16),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN(0,1)),
    event_kind TEXT NOT NULL CHECK(length(event_kind) BETWEEN 1 AND 64),
    file_id INTEGER REFERENCES sfx_file_catalog(file_id),
    area_id INTEGER REFERENCES sfx_file_areas(area_id),
    occurred_utc INTEGER NOT NULL CHECK(occurred_utc BETWEEN -62135596800 AND 253402300799),
    detail TEXT NOT NULL CHECK(length(CAST(detail AS BLOB))<=2048)
) STRICT;
CREATE INDEX sfx_file_audit_time ON sfx_file_audit(occurred_utc,audit_id);
CREATE TABLE sfx_file_change_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN(0,1)),
    resulting_revision INTEGER NOT NULL CHECK(resulting_revision>=1),
    resulting_id INTEGER NOT NULL CHECK(resulting_id>=0),
    occurred_utc INTEGER NOT NULL CHECK(occurred_utc BETWEEN -62135596800 AND 253402300799)
) STRICT;
CREATE TRIGGER sfx_file_change_receipt_update BEFORE UPDATE ON sfx_file_change_receipts
BEGIN SELECT RAISE(ABORT,'File change receipts are immutable'); END;
CREATE TRIGGER sfx_file_change_receipt_delete BEFORE DELETE ON sfx_file_change_receipts
BEGIN SELECT RAISE(ABORT,'File change receipts are retained'); END;
)SQL" + R"SQL(
CREATE TABLE sfx_file_scan_areas (
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
    selected INTEGER NOT NULL CHECK(selected IN(0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1),
    PRIMARY KEY(account_id,area_id)
) STRICT;
CREATE TABLE sfx_file_scan_seen (
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    file_id INTEGER NOT NULL REFERENCES sfx_file_catalog(file_id),
    publication_sequence INTEGER NOT NULL REFERENCES sfx_file_publications(publication_sequence),
    seen_utc INTEGER NOT NULL CHECK(seen_utc BETWEEN -62135596800 AND 253402300799),
    PRIMARY KEY(account_id,file_id)
) STRICT;
CREATE TABLE sfx_file_reupload_blocks (
    content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
    byte_count INTEGER NOT NULL CHECK(byte_count>=0),
    blocked INTEGER NOT NULL CHECK(blocked IN(0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1),
    source_file_id INTEGER NOT NULL REFERENCES sfx_file_catalog(file_id),
    changed_utc INTEGER NOT NULL CHECK(changed_utc BETWEEN -62135596800 AND 253402300799),
    PRIMARY KEY(content_sha256,byte_count)
) STRICT;
CREATE TABLE sfx_file_intake_objects (
    operation_id BLOB PRIMARY KEY REFERENCES sfx_file_intake(operation_id),
    stage_volume TEXT NOT NULL CHECK(length(stage_volume) BETWEEN 1 AND 128),
    stage_file_id TEXT NOT NULL CHECK(length(stage_file_id) BETWEEN 1 AND 128),
    placed_volume TEXT NOT NULL CHECK(length(placed_volume)<=128),
    placed_file_id TEXT NOT NULL CHECK(length(placed_file_id)<=128),
    staging_clean INTEGER NOT NULL CHECK(staging_clean IN(0,1)),
    CHECK((placed_volume='')=(placed_file_id=''))
) STRICT;
CREATE TRIGGER sfx_file_intake_objects_retain BEFORE DELETE ON sfx_file_intake_objects
BEGIN SELECT RAISE(ABORT,'Native intake bindings are retained'); END;
CREATE TRIGGER sfx_file_intake_objects_immutable BEFORE UPDATE ON sfx_file_intake_objects
WHEN OLD.operation_id<>NEW.operation_id OR OLD.stage_volume<>NEW.stage_volume OR
 OLD.stage_file_id<>NEW.stage_file_id OR (OLD.placed_volume<>'' AND
 (OLD.placed_volume<>NEW.placed_volume OR OLD.placed_file_id<>NEW.placed_file_id)) OR
 OLD.staging_clean>NEW.staging_clean
BEGIN SELECT RAISE(ABORT,'Native intake bindings cannot be reassigned'); END;
CREATE TABLE sfx_file_intake_purges (
    operation_id BLOB PRIMARY KEY REFERENCES sfx_file_intake(operation_id),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    stage_bytes INTEGER NOT NULL CHECK(stage_bytes>=0),
    stage_sha256 BLOB NOT NULL CHECK(length(stage_sha256)=32),
    stage_volume TEXT NOT NULL CHECK(length(stage_volume)<=128),
    stage_file_id TEXT NOT NULL CHECK(length(stage_file_id)<=128),
    placed_bytes INTEGER NOT NULL CHECK(placed_bytes>=0),
    placed_sha256 BLOB NOT NULL CHECK(length(placed_sha256)=32),
    placed_volume TEXT NOT NULL CHECK(length(placed_volume)<=128),
    placed_file_id TEXT NOT NULL CHECK(length(placed_file_id)<=128),
    completed INTEGER NOT NULL CHECK(completed IN(0,1)),
    created_utc INTEGER NOT NULL CHECK(created_utc BETWEEN -62135596800 AND 253402300799),
    CHECK((stage_volume='')=(stage_file_id='')),
    CHECK((placed_volume='')=(placed_file_id=''))
) STRICT;
CREATE TRIGGER sfx_file_intake_purges_retain BEFORE DELETE ON sfx_file_intake_purges
BEGIN SELECT RAISE(ABORT,'Explicit intake purge intents are retained'); END;
CREATE TRIGGER sfx_file_intake_purges_immutable BEFORE UPDATE ON sfx_file_intake_purges
WHEN OLD.operation_id<>NEW.operation_id OR OLD.request_fingerprint<>NEW.request_fingerprint OR
 OLD.stage_bytes<>NEW.stage_bytes OR OLD.stage_sha256<>NEW.stage_sha256 OR
 OLD.stage_volume<>NEW.stage_volume OR OLD.stage_file_id<>NEW.stage_file_id OR
 OLD.placed_bytes<>NEW.placed_bytes OR OLD.placed_sha256<>NEW.placed_sha256 OR
 OLD.placed_volume<>NEW.placed_volume OR OLD.placed_file_id<>NEW.placed_file_id OR
 OLD.created_utc<>NEW.created_utc OR OLD.completed=1
BEGIN SELECT RAISE(ABORT,'Purge intent objects and terminal receipts are immutable'); END;
)SQL";
