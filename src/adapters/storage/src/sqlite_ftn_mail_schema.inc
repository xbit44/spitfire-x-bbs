// Additive migration22. Existing migration1-21 definitions stay byte-exact.
// QA08 adds typed private-FTN kind2 as well as nullable remote authors. That
// CHECK change requires the documented one-table rebuild, not author-only ALTER.
// Derive copied trigger/index DDL ONLY from fixed predecessor migrations in an
// isolated memory database, never from an installation's untrusted schema.
using FtnSchemaDb=std::unique_ptr<sqlite3,decltype(&sqlite3_close)>;
FtnSchemaDb ftn_mail_predecessor(){
    sqlite3* raw{};if(sqlite3_open(":memory:",&raw)!=SQLITE_OK){if(raw)sqlite3_close(raw);throw std::runtime_error("FTN mail manifest database");}
    FtnSchemaDb db(raw,sqlite3_close);
    if(sqlite3_create_function_v2(raw,"sfx_secure_lineage",0,SQLITE_UTF8|SQLITE_DIRECTONLY,nullptr,secure_lineage_sql,nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN manifest secure identity function");
    const auto execute=[&](const std::string& sql){if(sqlite3_exec(raw,sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN mail fixed predecessor DDL");};
    execute(proposed_migration_sql);
    for(const auto* migration:{&sfx_caller_journal_migration(),&sfx_menu_preferences_migration(),&sfx_caller_utilities_migration(),&sfx_classic_information_migration(),&sfx_classic_delete_migration(),&sfx_other_board_text_migration(),&sfx_local_message_migration(),&sfx_message_admin_migration(),&sfx_message_group_migration(),&sfx_message_delete_migration(),&sfx_message_source_migration(),&sfx_message_maintenance_migration(),&sfx_qwk_migration(),&sfx_file_migration(),&sfx_door_migration(),&sfx_door_security_migration(),&sfx_door_program_migration(),&sfx_chat_migration(),&sfx_private_chat_migration(),&sfx_ftn_migration()})execute(migration->sql);
    return db;
}
#include "sqlite_ftn_file_schema.inc"
#include "sqlite_ftn_held_schema.inc"
#include "sqlite_ftn_forward_plan_schema.inc"
#include "sqlite_ftn_tic_review_schema.inc"
#include "sqlite_ftn_continuation_schema.inc"
#include "sqlite_ftn_retention_schema.inc"
const std::string ftn_mail_migration_sql=[] {
    auto fixed=ftn_mail_predecessor();sqlite3_stmt* raw{};
    if(sqlite3_prepare_v2(fixed.get(),"SELECT type,name,sql FROM sqlite_schema WHERE type IN('index','trigger') AND sql IS NOT NULL AND instr(sql,'sfx_local_messages')>0 ORDER BY type,name",-1,&raw,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN fixed dependent definitions");
    std::unique_ptr<sqlite3_stmt,decltype(&sqlite3_finalize)> query(raw,sqlite3_finalize);std::string drops,recreate;
    while(sqlite3_step(raw)==SQLITE_ROW){const auto type=std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw,0))),name=std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw,1))),ddl=std::string(reinterpret_cast<const char*>(sqlite3_column_text(raw,2)));drops+="DROP "+type+" "+name+";\n";recreate+=ddl+";\n";}
    std::string sql=drops+R"SQL(
CREATE TABLE sfx_ftn_sequence22(seq INTEGER NOT NULL);
INSERT INTO sfx_ftn_sequence22 SELECT seq FROM sqlite_sequence WHERE name='sfx_local_messages';
CREATE TABLE sfx_ftn_messages22 (
 message_id INTEGER PRIMARY KEY AUTOINCREMENT,
 kind INTEGER NOT NULL CHECK(kind IN(0,1,2)),
 echo_id INTEGER REFERENCES sfx_local_echoes(echo_id),
 area_number INTEGER CHECK(area_number>=1),
 author_account_id INTEGER REFERENCES sfx_accounts(account_id),
 recipient_account_id INTEGER REFERENCES sfx_accounts(account_id),
 subject BLOB NOT NULL CHECK(length(subject) BETWEEN 1 AND 70),
 body BLOB NOT NULL CHECK(length(body) BETWEEN 1 AND 1048576),
 encoding TEXT NOT NULL CHECK(encoding='cp437'),
 body_lines INTEGER NOT NULL CHECK(body_lines BETWEEN 1 AND 65535),
 arrival_ordinal INTEGER NOT NULL UNIQUE CHECK(arrival_ordinal>=1),
 created_utc_seconds INTEGER NOT NULL CHECK(created_utc_seconds BETWEEN -62135596800 AND 253402300799),
 parent_message_id INTEGER REFERENCES sfx_local_messages(message_id),
 CHECK((kind=0 AND echo_id IS NOT NULL AND area_number IS NOT NULL) OR
       (kind=1 AND echo_id IS NULL AND area_number IS NULL AND recipient_account_id IS NOT NULL) OR
       (kind=2 AND echo_id IS NULL AND area_number IS NULL AND author_account_id IS NOT NULL AND recipient_account_id IS NULL)),
 UNIQUE(echo_id,area_number),UNIQUE(message_id,recipient_account_id)
) STRICT;
INSERT INTO sfx_ftn_messages22(message_id,kind,echo_id,area_number,author_account_id,recipient_account_id,subject,body,encoding,body_lines,arrival_ordinal,created_utc_seconds,parent_message_id)
 SELECT message_id,kind,echo_id,area_number,author_account_id,recipient_account_id,subject,body,encoding,body_lines,arrival_ordinal,created_utc_seconds,parent_message_id FROM sfx_local_messages;
DROP TABLE sfx_local_messages;
ALTER TABLE sfx_ftn_messages22 RENAME TO sfx_local_messages;
DELETE FROM sqlite_sequence WHERE name IN('sfx_local_messages','sfx_ftn_messages22');
INSERT INTO sqlite_sequence(name,seq) SELECT 'sfx_local_messages',seq FROM sfx_ftn_sequence22;
DROP TABLE sfx_ftn_sequence22;
)SQL"+recreate+R"SQL(

CREATE TABLE sfx_ftn_ingress (
 receipt_id BLOB PRIMARY KEY CHECK(length(receipt_id)=16),
 identity_hash BLOB NOT NULL UNIQUE CHECK(length(identity_hash)=32),
 network TEXT NOT NULL CHECK(length(network)<=48),
 link TEXT NOT NULL CHECK(length(link)<=48),
 proof INTEGER NOT NULL CHECK(proof BETWEEN 0 AND 6),
 outbound INTEGER NOT NULL CHECK(outbound IN(0,1)),
 session_id BLOB NOT NULL CHECK(length(session_id)=16),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>=1),
 wire_name TEXT NOT NULL CHECK(length(wire_name) BETWEEN 1 AND 128),
 declared_size INTEGER NOT NULL CHECK(declared_size BETWEEN 0 AND 1073741824),
 wire_time INTEGER NOT NULL CHECK(wire_time BETWEEN 0 AND 4294967295),
 received_bytes INTEGER NOT NULL CHECK(received_bytes BETWEEN 0 AND declared_size),
 content_sha256 BLOB CHECK(content_sha256 IS NULL OR length(content_sha256)=32),
 state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),
 reason TEXT NOT NULL CHECK(length(reason)<=64),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0),
 updated_utc INTEGER NOT NULL CHECK(updated_utc>=created_utc),
 CHECK((state=0 AND content_sha256 IS NULL) OR state=3 OR
       (state IN(1,2,4) AND received_bytes=declared_size AND content_sha256 IS NOT NULL)),
 CHECK((proof=0 AND link='') OR (proof>0 AND network<>'' AND link<>'')),
 CHECK((proof IN(1,2,6) AND outbound=0) OR (proof IN(3,4) AND outbound=1) OR proof IN(0,5))
) STRICT;
CREATE INDEX sfx_ftn_ingress_pending ON sfx_ftn_ingress(state,created_utc,receipt_id);
CREATE TABLE sfx_ftn_ingress_sessions (
 receipt_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 session_id BLOB NOT NULL CHECK(length(session_id)=16),
 proof INTEGER NOT NULL CHECK(proof BETWEEN 0 AND 6),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>=1),
 resume_offset INTEGER NOT NULL CHECK(resume_offset>=0),
 started_utc INTEGER NOT NULL CHECK(started_utc>=0),
 PRIMARY KEY(receipt_id,session_id)
) STRICT;
CREATE TRIGGER sfx_ftn_ingress_sessions_update BEFORE UPDATE ON sfx_ftn_ingress_sessions
BEGIN SELECT RAISE(ABORT,'immutable ingress session proof'); END;
CREATE TRIGGER sfx_ftn_ingress_sessions_delete BEFORE DELETE ON sfx_ftn_ingress_sessions
BEGIN SELECT RAISE(ABORT,'immutable ingress session proof'); END;
CREATE TABLE sfx_ftn_ingress_native (
 receipt_id BLOB PRIMARY KEY REFERENCES sfx_ftn_ingress(receipt_id),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 20 AND volume NOT GLOB '*[^0-9]*'),
 file_id TEXT NOT NULL CHECK(length(file_id) BETWEEN 1 AND 20 AND file_id NOT GLOB '*[^0-9]*'),
 checkpoint_bytes INTEGER NOT NULL CHECK(checkpoint_bytes BETWEEN 0 AND 1073741824),
 prefix_sha256 BLOB NOT NULL CHECK(length(prefix_sha256)=32)
) STRICT;
CREATE TRIGGER sfx_ftn_ingress_native_update BEFORE UPDATE ON sfx_ftn_ingress_native
WHEN NEW.receipt_id IS NOT OLD.receipt_id OR NEW.volume IS NOT OLD.volume OR NEW.file_id IS NOT OLD.file_id OR NEW.checkpoint_bytes<OLD.checkpoint_bytes
BEGIN SELECT RAISE(ABORT,'native FTN custody identity changed'); END;
CREATE TRIGGER sfx_ftn_ingress_native_delete BEFORE DELETE ON sfx_ftn_ingress_native
BEGIN SELECT RAISE(ABORT,'native FTN custody proof retained'); END;

CREATE TABLE sfx_ftn_import_receipts (
 import_id BLOB PRIMARY KEY CHECK(length(import_id)=16),
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 message_index INTEGER NOT NULL CHECK(message_index BETWEEN 0 AND 4095),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag)<=128),
 origin TEXT NOT NULL CHECK(length(origin) BETWEEN 1 AND 96),
 identity_hash BLOB NOT NULL CHECK(length(identity_hash)=32),
 content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
 original_message_id INTEGER CHECK(original_message_id>0),
 committed_utc INTEGER NOT NULL CHECK(committed_utc>=0),
 UNIQUE(ingress_id,message_index),
 UNIQUE(network,echo_tag,origin,identity_hash)
) STRICT;
CREATE INDEX sfx_ftn_import_identity ON sfx_ftn_import_receipts(identity_hash);
CREATE TRIGGER sfx_ftn_import_receipts_update BEFORE UPDATE ON sfx_ftn_import_receipts
BEGIN SELECT RAISE(ABORT,'immutable FTN import receipt'); END;
CREATE TRIGGER sfx_ftn_import_receipts_delete BEFORE DELETE ON sfx_ftn_import_receipts
BEGIN SELECT RAISE(ABORT,'immutable FTN import receipt'); END;
CREATE TABLE sfx_ftn_toss_progress (
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 message_index INTEGER NOT NULL CHECK(message_index BETWEEN 0 AND 4095),
 operation_id BLOB NOT NULL UNIQUE CHECK(length(operation_id)=16),
 import_id BLOB NOT NULL REFERENCES sfx_ftn_import_receipts(import_id),
 request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
 PRIMARY KEY(ingress_id,message_index)
) STRICT;
CREATE TRIGGER sfx_ftn_toss_progress_update BEFORE UPDATE ON sfx_ftn_toss_progress
BEGIN SELECT RAISE(ABORT,'immutable FTN toss progress'); END;
CREATE TRIGGER sfx_ftn_toss_progress_delete BEFORE DELETE ON sfx_ftn_toss_progress
BEGIN SELECT RAISE(ABORT,'immutable FTN toss progress'); END;

CREATE TABLE sfx_ftn_import_write_scope (
 import_id BLOB PRIMARY KEY REFERENCES sfx_ftn_import_receipts(import_id)
) STRICT;
CREATE TABLE sfx_ftn_message_targets (
 message_id INTEGER PRIMARY KEY REFERENCES sfx_local_messages(message_id) DEFERRABLE INITIALLY DEFERRED,
 operation_id BLOB NOT NULL UNIQUE REFERENCES sfx_message_commit_receipts(operation_id) DEFERRABLE INITIALLY DEFERRED CHECK(length(operation_id)=16),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 source_aka TEXT NOT NULL CHECK(length(source_aka) BETWEEN 1 AND 96),
 destination TEXT NOT NULL CHECK(length(destination) BETWEEN 1 AND 96),
 recipient_name BLOB NOT NULL CHECK(length(recipient_name) BETWEEN 1 AND 35),
 msgid BLOB NOT NULL CHECK(length(msgid) BETWEEN 1 AND 4096),
 reply BLOB NOT NULL CHECK(length(reply)<=4096),
 parent_sysop_authorized INTEGER NOT NULL CHECK(parent_sysop_authorized IN(0,1))
) STRICT;
CREATE TRIGGER sfx_ftn_message_targets_update BEFORE UPDATE ON sfx_ftn_message_targets
BEGIN SELECT RAISE(ABORT,'immutable FTN message target'); END;
CREATE TRIGGER sfx_ftn_message_targets_delete BEFORE DELETE ON sfx_ftn_message_targets
WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_message_commit_receipts r USING(message_id)
 JOIN sfx_message_purged_receipts p ON p.operation_id=r.operation_id AND p.message_id=m.message_id
 AND p.actor_account_id=r.actor_account_id AND p.request_fingerprint=r.request_fingerprint
 WHERE m.message_id=OLD.message_id AND (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=m.message_id) OR
 (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=m.echo_id))))
BEGIN SELECT RAISE(ABORT,'FTN target deletion outside authorized purge'); END;
CREATE TRIGGER sfx_local_messages_ftn_target_insert BEFORE INSERT ON sfx_local_messages
WHEN NEW.kind=2 AND NOT EXISTS(SELECT 1 FROM sfx_ftn_message_targets WHERE message_id=NEW.message_id)
BEGIN SELECT RAISE(ABORT,'FTN target outside atomic post'); END;
CREATE TABLE sfx_ftn_message_intents (
 message_id INTEGER NOT NULL REFERENCES sfx_local_messages(message_id),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 operation_id BLOB NOT NULL REFERENCES sfx_message_commit_receipts(operation_id),
 source_aka TEXT NOT NULL CHECK(length(source_aka) BETWEEN 1 AND 96),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag)<=128),
 msgid BLOB NOT NULL CHECK(length(msgid) BETWEEN 1 AND 4096),
 via_link TEXT NOT NULL CHECK(length(via_link)<=48),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>=1),
 state INTEGER NOT NULL CHECK(state IN(0,1,2)),
 reason TEXT NOT NULL CHECK(length(reason)<=64),
 PRIMARY KEY(message_id,network),UNIQUE(operation_id,network)
) STRICT;
)SQL"+R"SQL(CREATE TABLE sfx_ftn_serial_allocator (
 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
 high_water INTEGER NOT NULL CHECK(high_water BETWEEN 0 AND 4294967295),
 recovery_required INTEGER NOT NULL CHECK(recovery_required IN(0,1)),
 lineage BLOB NOT NULL CHECK(length(lineage)=16 AND lineage<>zeroblob(16))
) STRICT;
INSERT INTO sfx_ftn_serial_allocator VALUES(1,0,0,sfx_secure_lineage());
CREATE TRIGGER sfx_ftn_serial_allocator_update BEFORE UPDATE ON sfx_ftn_serial_allocator
WHEN NEW.singleton_id<>OLD.singleton_id OR NEW.high_water<OLD.high_water OR NEW.recovery_required<OLD.recovery_required OR NEW.lineage IS NOT OLD.lineage
BEGIN SELECT RAISE(ABORT,'FTN serial high water cannot decrease'); END;
CREATE TRIGGER sfx_ftn_serial_allocator_delete BEFORE DELETE ON sfx_ftn_serial_allocator
BEGIN SELECT RAISE(ABORT,'FTN serial high water retained'); END;
CREATE TABLE sfx_ftn_serial_allocations (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 serial INTEGER NOT NULL UNIQUE CHECK(serial BETWEEN 1 AND 4294967295)
) STRICT;
CREATE TRIGGER sfx_ftn_serial_allocations_insert BEFORE INSERT ON sfx_ftn_serial_allocations
WHEN NEW.serial<>(SELECT high_water FROM sfx_ftn_serial_allocator WHERE singleton_id=1)
 OR NOT EXISTS(SELECT 1 FROM sfx_ftn_serial_allocator WHERE singleton_id=1)
BEGIN SELECT RAISE(ABORT,'FTN serial outside current allocation'); END;
CREATE TRIGGER sfx_ftn_serial_allocations_update BEFORE UPDATE ON sfx_ftn_serial_allocations
BEGIN SELECT RAISE(ABORT,'immutable FTN serial allocation'); END;
CREATE TRIGGER sfx_ftn_serial_allocations_delete BEFORE DELETE ON sfx_ftn_serial_allocations
BEGIN SELECT RAISE(ABORT,'FTN serial allocation retained independently of message'); END;
CREATE TRIGGER sfx_ftn_message_intents_update BEFORE UPDATE ON sfx_ftn_message_intents
WHEN NOT(NEW.message_id IS OLD.message_id AND NEW.network IS OLD.network AND NEW.operation_id IS OLD.operation_id
 AND NEW.source_aka IS OLD.source_aka AND NEW.echo_tag IS OLD.echo_tag AND NEW.via_link IS OLD.via_link
 AND NEW.msgid IS OLD.msgid
 AND NEW.configuration_revision IS OLD.configuration_revision)
BEGIN SELECT RAISE(ABORT,'immutable FTN scan intent source'); END;
CREATE TRIGGER sfx_ftn_message_intents_delete BEFORE DELETE ON sfx_ftn_message_intents
WHEN OLD.state<>1 OR NOT EXISTS(SELECT 1 FROM sfx_local_messages m WHERE m.message_id=OLD.message_id AND
 (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=m.message_id) OR
 (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=m.echo_id))))
BEGIN SELECT RAISE(ABORT,'outstanding FTN mail protected'); END;
CREATE TABLE sfx_ftn_message_metadata (
 message_id INTEGER PRIMARY KEY REFERENCES sfx_local_messages(message_id) DEFERRABLE INITIALLY DEFERRED,
 import_id BLOB NOT NULL UNIQUE REFERENCES sfx_ftn_import_receipts(import_id),
 from_name BLOB NOT NULL CHECK(length(from_name) BETWEEN 1 AND 35),
 to_name BLOB NOT NULL CHECK(length(to_name) BETWEEN 1 AND 35),
 msgid BLOB NOT NULL CHECK(length(msgid)<=4096),
 reply BLOB NOT NULL CHECK(length(reply)<=4096),
 original_date BLOB NOT NULL CHECK(length(original_date) BETWEEN 1 AND 19),
 wire_from BLOB NOT NULL CHECK(length(wire_from) BETWEEN 1 AND 35),
 wire_to BLOB NOT NULL CHECK(length(wire_to) BETWEEN 1 AND 35),
 wire_subject BLOB NOT NULL CHECK(length(wire_subject)<=71),
 wire_text BLOB NOT NULL CHECK(length(wire_text) BETWEEN 1 AND 262144),
 wire_attributes INTEGER NOT NULL CHECK(wire_attributes BETWEEN 0 AND 65535)
) STRICT;
CREATE TRIGGER sfx_ftn_message_metadata_insert BEFORE INSERT ON sfx_ftn_message_metadata
WHEN NOT EXISTS(SELECT 1 FROM sfx_ftn_import_receipts r JOIN sfx_ftn_import_write_scope s USING(import_id)
 WHERE r.import_id=NEW.import_id AND r.original_message_id=NEW.message_id)
BEGIN SELECT RAISE(ABORT,'FTN metadata outside atomic import'); END;
CREATE TRIGGER sfx_ftn_message_metadata_update BEFORE UPDATE ON sfx_ftn_message_metadata
BEGIN SELECT RAISE(ABORT,'immutable FTN identity and source'); END;
CREATE TRIGGER sfx_ftn_message_metadata_delete BEFORE DELETE ON sfx_ftn_message_metadata
WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_ftn_import_receipts r
 ON r.import_id=OLD.import_id AND r.original_message_id=m.message_id
 WHERE m.message_id=OLD.message_id AND m.author_account_id IS NULL AND
 (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=m.message_id) OR
 (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=m.echo_id))))
BEGIN SELECT RAISE(ABORT,'FTN source deletion outside authorized purge'); END;
CREATE TRIGGER sfx_local_messages_remote_insert BEFORE INSERT ON sfx_local_messages
WHEN NEW.author_account_id IS NULL AND NOT EXISTS(
 SELECT 1 FROM sfx_ftn_message_metadata m JOIN sfx_ftn_import_receipts r USING(import_id)
 JOIN sfx_ftn_import_write_scope s USING(import_id)
 WHERE m.message_id=NEW.message_id AND r.original_message_id=NEW.message_id)
BEGIN SELECT RAISE(ABORT,'remote author outside atomic import'); END;

DROP TRIGGER sfx_message_sources_guard_insert;
CREATE TRIGGER sfx_message_sources_guard_insert BEFORE INSERT ON sfx_message_sources
WHEN NOT EXISTS(SELECT 1 FROM sfx_message_source_write_scope s JOIN sfx_message_commit_receipts r USING(operation_id) WHERE s.message_id=NEW.message_id AND r.message_id=NEW.message_id)
 AND NOT EXISTS(SELECT 1 FROM sfx_ftn_message_metadata m JOIN sfx_ftn_import_receipts r USING(import_id) JOIN sfx_ftn_import_write_scope s USING(import_id) WHERE m.message_id=NEW.message_id AND r.original_message_id=NEW.message_id)
BEGIN SELECT RAISE(ABORT,'source outside atomic commit'); END;
DROP TRIGGER sfx_message_sources_guard_delete;
CREATE TRIGGER sfx_message_sources_guard_delete BEFORE DELETE ON sfx_message_sources
WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_message_commit_receipts r USING(message_id) JOIN sfx_message_purged_receipts t ON t.operation_id=r.operation_id AND t.message_id=m.message_id AND t.actor_account_id=r.actor_account_id AND t.request_fingerprint=r.request_fingerprint WHERE m.message_id=OLD.message_id AND (EXISTS(SELECT 1 FROM sfx_message_delete_scope d WHERE d.message_id=m.message_id) OR (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope p WHERE p.echo_id=m.echo_id))))
 AND NOT EXISTS(SELECT 1 FROM sfx_local_messages m JOIN sfx_ftn_message_metadata f USING(message_id) JOIN sfx_ftn_import_receipts r USING(import_id) WHERE m.message_id=OLD.message_id AND m.author_account_id IS NULL AND r.original_message_id=m.message_id AND (EXISTS(SELECT 1 FROM sfx_message_delete_scope d WHERE d.message_id=m.message_id) OR (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope p WHERE p.echo_id=m.echo_id))))
BEGIN SELECT RAISE(ABORT,'source deletion outside authorized purge'); END;

CREATE TABLE sfx_ftn_outbound (
 job_id BLOB PRIMARY KEY CHECK(length(job_id)=16),
 operation_id BLOB NOT NULL CHECK(length(operation_id)=16),
 ordinal INTEGER NOT NULL CHECK(ordinal BETWEEN 0 AND 4095),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 link TEXT NOT NULL CHECK(length(link) BETWEEN 1 AND 48),
 kind INTEGER NOT NULL CHECK(kind BETWEEN 0 AND 2),
 flavor INTEGER NOT NULL CHECK(flavor BETWEEN 0 AND 2),
 wire_name TEXT NOT NULL CHECK(length(wire_name) BETWEEN 1 AND 128),
 size INTEGER NOT NULL CHECK(size BETWEEN 0 AND 1073741824),
 wire_time INTEGER NOT NULL CHECK(wire_time BETWEEN 0 AND 4294967295),
 content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
 state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5),
 claim_id BLOB CHECK(claim_id IS NULL OR length(claim_id)=16),
 generation BLOB CHECK(generation IS NULL OR length(generation)=16),
 attempts INTEGER NOT NULL CHECK(attempts BETWEEN 0 AND 1000000),
 reason TEXT NOT NULL CHECK(length(reason)<=64),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0),
 updated_utc INTEGER NOT NULL CHECK(updated_utc>=created_utc),
 UNIQUE(operation_id,network,link,ordinal),
 CHECK(state NOT IN(1,2,5) OR (claim_id IS NOT NULL AND generation IS NOT NULL))
) STRICT;
CREATE INDEX sfx_ftn_outbound_ready ON sfx_ftn_outbound(network,link,state,created_utc,job_id);
CREATE TABLE sfx_ftn_message_jobs (
 message_id INTEGER NOT NULL,
 network TEXT NOT NULL,
 job_id BLOB NOT NULL REFERENCES sfx_ftn_outbound(job_id),
 PRIMARY KEY(message_id,network,job_id),
 FOREIGN KEY(message_id,network) REFERENCES sfx_ftn_message_intents(message_id,network) ON DELETE CASCADE
) STRICT;
CREATE TABLE sfx_ftn_scan_cursors (
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128),
 link TEXT NOT NULL CHECK(length(link) BETWEEN 1 AND 48),
 after_arrival INTEGER NOT NULL CHECK(after_arrival>=0),
 PRIMARY KEY(network,echo_tag,link)
) STRICT;
CREATE TABLE sfx_ftn_link_state (
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 link TEXT NOT NULL CHECK(length(link) BETWEEN 1 AND 48),
 paused INTEGER NOT NULL CHECK(paused IN(0,1)),
 last_attempt INTEGER NOT NULL CHECK(last_attempt>=0),
 last_success INTEGER NOT NULL CHECK(last_success>=0),
 last_reason TEXT NOT NULL CHECK(length(last_reason)<=64),
 PRIMARY KEY(network,link)
) STRICT;
)SQL"+R"SQL(
CREATE TABLE sfx_ftn_local_wire_sources (
 message_id INTEGER NOT NULL,
 network TEXT NOT NULL,
 author_name BLOB NOT NULL CHECK(length(author_name) BETWEEN 1 AND 35),
 origin_line TEXT NOT NULL CHECK(length(origin_line)<=256),
 reply BLOB NOT NULL CHECK(length(reply)<=4096),
 PRIMARY KEY(message_id,network),
 FOREIGN KEY(message_id,network) REFERENCES sfx_ftn_message_intents(message_id,network) ON DELETE CASCADE
) STRICT;
CREATE TRIGGER sfx_ftn_local_wire_sources_update BEFORE UPDATE ON sfx_ftn_local_wire_sources
BEGIN SELECT RAISE(ABORT,'immutable published local FTN wire source'); END;
CREATE TRIGGER sfx_ftn_local_wire_sources_delete BEFORE DELETE ON sfx_ftn_local_wire_sources
WHEN NOT EXISTS(SELECT 1 FROM sfx_local_messages m WHERE m.message_id=OLD.message_id AND
 (EXISTS(SELECT 1 FROM sfx_message_delete_scope WHERE message_id=m.message_id) OR
 (m.kind=0 AND EXISTS(SELECT 1 FROM sfx_message_purge_scope WHERE echo_id=m.echo_id))))
BEGIN SELECT RAISE(ABORT,'published wire source deletion outside authorized purge'); END;
CREATE TABLE sfx_ftn_job_echoes (
 job_id BLOB PRIMARY KEY REFERENCES sfx_ftn_outbound(job_id),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128)
) STRICT;
CREATE TRIGGER sfx_ftn_job_echoes_update BEFORE UPDATE ON sfx_ftn_job_echoes
BEGIN SELECT RAISE(ABORT,'immutable outbound echo identity'); END;
CREATE TRIGGER sfx_ftn_job_echoes_delete BEFORE DELETE ON sfx_ftn_job_echoes
BEGIN SELECT RAISE(ABORT,'outbound echo identity retained'); END;
CREATE TABLE sfx_ftn_control_intents (
 operation_id BLOB PRIMARY KEY REFERENCES sfx_ftn_serial_allocations(operation_id) CHECK(length(operation_id)=16),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 source_aka TEXT NOT NULL CHECK(length(source_aka) BETWEEN 1 AND 96),
 target_link TEXT NOT NULL CHECK(length(target_link) BETWEEN 1 AND 48),
 destination TEXT NOT NULL CHECK(length(destination) BETWEEN 1 AND 96),
 recipient BLOB NOT NULL CHECK(length(recipient) BETWEEN 1 AND 35),
 msgid BLOB NOT NULL CHECK(length(msgid) BETWEEN 1 AND 128),
 reply BLOB NOT NULL CHECK(length(reply)<=4096),
 subject BLOB NOT NULL CHECK(length(subject) BETWEEN 1 AND 70),
 body BLOB NOT NULL CHECK(length(body) BETWEEN 1 AND 65536),
 password_subject INTEGER NOT NULL CHECK(password_subject IN(0,1)),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0),
 state INTEGER NOT NULL CHECK(state IN(0,1)),
 job_id BLOB REFERENCES sfx_ftn_outbound(job_id),
 CHECK((state=0 AND job_id IS NULL) OR (state=1 AND length(job_id)=16))
) STRICT;
CREATE TRIGGER sfx_ftn_control_intents_update BEFORE UPDATE ON sfx_ftn_control_intents
WHEN NOT(NEW.operation_id IS OLD.operation_id AND NEW.network IS OLD.network AND NEW.source_aka IS OLD.source_aka
 AND NEW.target_link IS OLD.target_link AND NEW.destination IS OLD.destination AND NEW.recipient IS OLD.recipient
 AND NEW.msgid IS OLD.msgid AND NEW.reply IS OLD.reply AND NEW.subject IS OLD.subject AND NEW.body IS OLD.body
 AND NEW.password_subject IS OLD.password_subject AND NEW.created_utc IS OLD.created_utc
 AND ((NEW.state=OLD.state AND NEW.job_id IS OLD.job_id) OR (OLD.state=0 AND NEW.state=1)))
BEGIN SELECT RAISE(ABORT,'immutable generated FTN control message'); END;
CREATE TRIGGER sfx_ftn_control_intents_delete BEFORE DELETE ON sfx_ftn_control_intents
BEGIN SELECT RAISE(ABORT,'FTN control identity retained'); END;
CREATE TABLE sfx_ftn_areafix_requests (
 import_id BLOB PRIMARY KEY REFERENCES sfx_ftn_import_receipts(import_id),
 requesting_link TEXT NOT NULL CHECK(length(requesting_link) BETWEEN 1 AND 48),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>=1),
 response_operation BLOB NOT NULL UNIQUE REFERENCES sfx_ftn_control_intents(operation_id)
) STRICT;
CREATE TRIGGER sfx_ftn_areafix_requests_update BEFORE UPDATE ON sfx_ftn_areafix_requests
BEGIN SELECT RAISE(ABORT,'immutable AreaFix request receipt'); END;
CREATE TRIGGER sfx_ftn_areafix_requests_delete BEFORE DELETE ON sfx_ftn_areafix_requests
BEGIN SELECT RAISE(ABORT,'AreaFix request receipt retained'); END;
CREATE TABLE sfx_ftn_provider_requests (
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 provider TEXT NOT NULL CHECK(length(provider) BETWEEN 1 AND 48),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128),
 operation_id BLOB NOT NULL REFERENCES sfx_ftn_control_intents(operation_id),
 state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 4),
 reason TEXT NOT NULL CHECK(length(reason)<=64),
 PRIMARY KEY(network,provider,echo_tag)
) STRICT;
CREATE TABLE sfx_ftn_provider_responses (
 import_id BLOB PRIMARY KEY REFERENCES sfx_ftn_import_receipts(import_id),
 request_operation BLOB NOT NULL REFERENCES sfx_ftn_control_intents(operation_id),
 reason TEXT NOT NULL CHECK(reason='response_unrecognized')
) STRICT;
CREATE TRIGGER sfx_ftn_provider_responses_update BEFORE UPDATE ON sfx_ftn_provider_responses
BEGIN SELECT RAISE(ABORT,'immutable provider response receipt'); END;
CREATE TRIGGER sfx_ftn_provider_responses_delete BEFORE DELETE ON sfx_ftn_provider_responses
BEGIN SELECT RAISE(ABORT,'provider response receipt retained'); END;
CREATE TABLE sfx_ftn_areafix_upstream_refs (
 import_id BLOB NOT NULL REFERENCES sfx_ftn_areafix_requests(import_id),
 provider TEXT NOT NULL CHECK(length(provider) BETWEEN 1 AND 48),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128),
 operation_id BLOB NOT NULL REFERENCES sfx_ftn_control_intents(operation_id),
 PRIMARY KEY(import_id,provider,echo_tag)
) STRICT;
CREATE TABLE sfx_ftn_rescan_requests (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 import_id BLOB NOT NULL REFERENCES sfx_ftn_areafix_requests(import_id),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 link TEXT NOT NULL CHECK(length(link) BETWEEN 1 AND 48),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag) BETWEEN 1 AND 128),
 remaining INTEGER NOT NULL CHECK(remaining BETWEEN 0 AND 1000),
 before_arrival INTEGER NOT NULL CHECK(before_arrival>=0),
 state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 2),
 reason TEXT NOT NULL CHECK(length(reason)<=64),
 UNIQUE(import_id,echo_tag)
) STRICT;
CREATE TABLE sfx_ftn_rescan_jobs (
 request_id BLOB NOT NULL REFERENCES sfx_ftn_rescan_requests(operation_id),
 arrival INTEGER NOT NULL CHECK(arrival>0),
 operation_id BLOB NOT NULL UNIQUE CHECK(length(operation_id)=16),
 fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),
 job_id BLOB NOT NULL UNIQUE REFERENCES sfx_ftn_outbound(job_id),
 PRIMARY KEY(request_id,arrival)
) STRICT;
CREATE TRIGGER sfx_ftn_rescan_jobs_update BEFORE UPDATE ON sfx_ftn_rescan_jobs
BEGIN SELECT RAISE(ABORT,'immutable rescan artifact binding'); END;
CREATE TRIGGER sfx_ftn_rescan_jobs_delete BEFORE DELETE ON sfx_ftn_rescan_jobs
BEGIN SELECT RAISE(ABORT,'rescan artifact receipt retained'); END;
)SQL";
    return sql+ftn_file_catalog_migration(fixed.get())+ftn_held_schema+ftn_forward_plan_schema+ftn_tic_review_schema+ftn_continuation_schema+ftn_retention_schema;
}();
std::vector<SfxBaselineSchemaObject> ftn_mail_manifest(){
    auto db=ftn_mail_predecessor();if(sqlite3_exec(db.get(),ftn_mail_migration_sql.c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN mail fixed manifest DDL");
    std::vector<SfxBaselineSchemaObject> changed;if(!collect_schema_manifest(db.get(),&changed))throw std::runtime_error("FTN mail manifest collection");auto result=ftn_manifest();
    for(const auto& object:changed){const auto old=std::find_if(result.begin(),result.end(),[&](const auto& previous){return previous.name==object.name;});
        if(old!=result.end())*old=object;
        else if(object.name.rfind("sfx_ftn_",0)==0||object.name=="sfx_local_messages_remote_insert"||object.name=="sfx_local_messages_ftn_target_insert")result.push_back(object);
    }return result;
}
