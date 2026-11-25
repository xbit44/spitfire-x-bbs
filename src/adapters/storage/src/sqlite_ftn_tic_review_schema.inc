// Fixed unpublished migration22. Rejection history never changes transport
// state, native checkpoint, original publication intent or forwarding plan.
constexpr const char* ftn_tic_review_schema=R"SQL(
CREATE TABLE sfx_ftn_tic_rejections (
 sequence INTEGER PRIMARY KEY CHECK(sequence>0),
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 attempt_id BLOB NOT NULL CHECK(length(attempt_id)=16),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 20),
 native_id TEXT NOT NULL CHECK(length(native_id) BETWEEN 1 AND 20),
 byte_count INTEGER NOT NULL CHECK(byte_count BETWEEN 0 AND 1073741824),
 sha256 BLOB NOT NULL CHECK(length(sha256)=32),
 reason TEXT NOT NULL CHECK(length(reason) BETWEEN 1 AND 64),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0),
 claimed_from TEXT NOT NULL CHECK(length(claimed_from)<=128),
 echo_tag TEXT NOT NULL CHECK(length(echo_tag)<=128),
 payload_name TEXT NOT NULL CHECK(length(payload_name)<=128),
 UNIQUE(ingress_id,attempt_id,configuration_revision,reason)
) STRICT;
CREATE INDEX sfx_ftn_tic_rejections_latest ON sfx_ftn_tic_rejections(ingress_id,sequence DESC);
CREATE TRIGGER sfx_ftn_tic_rejections_update BEFORE UPDATE ON sfx_ftn_tic_rejections
BEGIN SELECT RAISE(ABORT,'immutable TIC rejection observation'); END;
CREATE TRIGGER sfx_ftn_tic_rejections_delete BEFORE DELETE ON sfx_ftn_tic_rejections
BEGIN SELECT RAISE(ABORT,'TIC rejection provenance retained'); END;
CREATE TABLE sfx_ftn_tic_retries (
 sequence INTEGER PRIMARY KEY CHECK(sequence>0),
 operation_id BLOB NOT NULL UNIQUE CHECK(length(operation_id)=16),
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_ingress(receipt_id),
 previous_request BLOB REFERENCES sfx_ftn_tic_retries(operation_id),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 review_sha256 BLOB NOT NULL CHECK(length(review_sha256)=32),
 rejection_sequence INTEGER NOT NULL REFERENCES sfx_ftn_tic_rejections(sequence),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0),
 finished INTEGER NOT NULL CHECK(finished IN(0,1)),
 processed INTEGER NOT NULL CHECK(processed IN(0,1) AND processed<=finished),
 reason TEXT NOT NULL CHECK(length(reason)<=64)
) STRICT;
CREATE UNIQUE INDEX sfx_ftn_tic_retries_pending ON sfx_ftn_tic_retries(ingress_id) WHERE finished=0;
CREATE INDEX sfx_ftn_tic_retries_latest ON sfx_ftn_tic_retries(ingress_id,sequence DESC);
CREATE TRIGGER sfx_ftn_tic_retries_update BEFORE UPDATE ON sfx_ftn_tic_retries
WHEN OLD.finished=1 OR NEW.sequence IS NOT OLD.sequence OR NEW.operation_id IS NOT OLD.operation_id OR NEW.ingress_id IS NOT OLD.ingress_id OR NEW.previous_request IS NOT OLD.previous_request OR NEW.configuration_revision IS NOT OLD.configuration_revision OR NEW.review_sha256 IS NOT OLD.review_sha256 OR NEW.rejection_sequence IS NOT OLD.rejection_sequence OR NEW.created_utc IS NOT OLD.created_utc OR NEW.finished<>1
BEGIN SELECT RAISE(ABORT,'immutable reviewed TIC attempt and final result'); END;
CREATE TRIGGER sfx_ftn_tic_retries_delete BEFORE DELETE ON sfx_ftn_tic_retries
BEGIN SELECT RAISE(ABORT,'reviewed TIC attempt retained'); END;
)SQL";
