// Part of unpublished migration22 only. Original custody and message bytes
// remain immutable; this ledger contains no subject, body or credential.
constexpr const char* ftn_held_schema=R"SQL(
CREATE TABLE sfx_ftn_held_messages (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 message_index INTEGER NOT NULL CHECK(message_index BETWEEN 0 AND 4095),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 identity_hash BLOB NOT NULL CHECK(length(identity_hash)=32),
 content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 20 AND volume NOT GLOB '*[^0-9]*'),
 file_id TEXT NOT NULL CHECK(length(file_id) BETWEEN 1 AND 20 AND file_id NOT GLOB '*[^0-9]*'),
 payload_bytes INTEGER NOT NULL CHECK(payload_bytes BETWEEN 1 AND 1073741824),
 payload_sha256 BLOB NOT NULL CHECK(length(payload_sha256)=32),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 envelope_destination TEXT NOT NULL CHECK(length(envelope_destination) BETWEEN 1 AND 128),
 origin TEXT NOT NULL CHECK(length(origin) BETWEEN 1 AND 128),
 destination TEXT NOT NULL CHECK(length(destination) BETWEEN 1 AND 128),
 sender TEXT NOT NULL CHECK(length(sender) BETWEEN 1 AND 140),
 recipient TEXT NOT NULL CHECK(length(recipient) BETWEEN 1 AND 140),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag)<=128),
 reason TEXT NOT NULL CHECK(length(reason) BETWEEN 1 AND 64),
 local_private INTEGER NOT NULL CHECK(local_private IN(0,1)),
 UNIQUE(ingress_id,message_index)
) STRICT;
CREATE TRIGGER sfx_ftn_held_messages_update BEFORE UPDATE ON sfx_ftn_held_messages
BEGIN SELECT RAISE(ABORT,'immutable held message observation'); END;
CREATE TRIGGER sfx_ftn_held_messages_delete BEFORE DELETE ON sfx_ftn_held_messages
BEGIN SELECT RAISE(ABORT,'held message provenance retained'); END;
CREATE TABLE sfx_ftn_recipient_resolutions (
 sequence INTEGER PRIMARY KEY CHECK(sequence>0),
 operation_id BLOB NOT NULL UNIQUE CHECK(length(operation_id)=16),
 held_message_id BLOB NOT NULL REFERENCES sfx_ftn_held_messages(operation_id),
 previous_resolution BLOB REFERENCES sfx_ftn_recipient_resolutions(operation_id),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 recipient_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
 recipient_revision INTEGER NOT NULL CHECK(recipient_revision>0),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0)
) STRICT;
CREATE INDEX sfx_ftn_recipient_resolutions_latest ON sfx_ftn_recipient_resolutions(held_message_id,sequence DESC);
CREATE TRIGGER sfx_ftn_recipient_resolutions_update BEFORE UPDATE ON sfx_ftn_recipient_resolutions
BEGIN SELECT RAISE(ABORT,'immutable reviewed recipient resolution'); END;
CREATE TRIGGER sfx_ftn_recipient_resolutions_delete BEFORE DELETE ON sfx_ftn_recipient_resolutions
BEGIN SELECT RAISE(ABORT,'recipient resolution provenance retained'); END;
CREATE TABLE sfx_ftn_reprocess_requests (
 sequence INTEGER PRIMARY KEY CHECK(sequence>0),
 operation_id BLOB NOT NULL UNIQUE CHECK(length(operation_id)=16),
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 previous_request BLOB REFERENCES sfx_ftn_reprocess_requests(operation_id),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 20 AND volume NOT GLOB '*[^0-9]*'),
 file_id TEXT NOT NULL CHECK(length(file_id) BETWEEN 1 AND 20 AND file_id NOT GLOB '*[^0-9]*'),
 payload_bytes INTEGER NOT NULL CHECK(payload_bytes BETWEEN 1 AND 1073741824),
 payload_sha256 BLOB NOT NULL CHECK(length(payload_sha256)=32),
 resolutions_sha256 BLOB NOT NULL CHECK(length(resolutions_sha256)=32),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0),
 finished INTEGER NOT NULL CHECK(finished IN(0,1)),
 processed INTEGER NOT NULL CHECK(processed IN(0,1) AND processed<=finished),
 reason TEXT NOT NULL CHECK(length(reason)<=64)
) STRICT;
CREATE UNIQUE INDEX sfx_ftn_reprocess_pending ON sfx_ftn_reprocess_requests(ingress_id) WHERE finished=0;
CREATE INDEX sfx_ftn_reprocess_latest ON sfx_ftn_reprocess_requests(ingress_id,sequence DESC);
CREATE TRIGGER sfx_ftn_reprocess_requests_update BEFORE UPDATE ON sfx_ftn_reprocess_requests
WHEN OLD.finished=1 OR NEW.sequence IS NOT OLD.sequence OR NEW.operation_id IS NOT OLD.operation_id OR NEW.ingress_id IS NOT OLD.ingress_id OR NEW.previous_request IS NOT OLD.previous_request OR NEW.configuration_revision IS NOT OLD.configuration_revision OR NEW.volume IS NOT OLD.volume OR NEW.file_id IS NOT OLD.file_id OR NEW.payload_bytes IS NOT OLD.payload_bytes OR NEW.payload_sha256 IS NOT OLD.payload_sha256 OR NEW.resolutions_sha256 IS NOT OLD.resolutions_sha256 OR NEW.created_utc IS NOT OLD.created_utc OR NEW.finished<>1
BEGIN SELECT RAISE(ABORT,'immutable reprocess request and final result'); END;
CREATE TRIGGER sfx_ftn_reprocess_requests_delete BEFORE DELETE ON sfx_ftn_reprocess_requests
BEGIN SELECT RAISE(ABORT,'reprocess provenance retained'); END;
)SQL";
