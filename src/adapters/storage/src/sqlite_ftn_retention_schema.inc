// Unpublished migration22 only. No backfill guesses a legacy successful time.
constexpr const char* ftn_retention_schema=R"SQL(
CREATE TABLE sfx_ftn_retention_clock (
 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
 high_water INTEGER NOT NULL CHECK(high_water>=0)
) STRICT;
INSERT INTO sfx_ftn_retention_clock VALUES(1,0);
CREATE TABLE sfx_ftn_retained_originals (
 ingress_id BLOB PRIMARY KEY REFERENCES sfx_ftn_ingress(receipt_id),
 network TEXT NOT NULL CHECK(length(network) BETWEEN 1 AND 48),
 successful_utc INTEGER NOT NULL CHECK(successful_utc>0),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 volume TEXT NOT NULL CHECK(length(volume) BETWEEN 1 AND 20),
 native_id TEXT NOT NULL CHECK(length(native_id) BETWEEN 1 AND 20),
 byte_count INTEGER NOT NULL CHECK(byte_count BETWEEN 0 AND 1073741824),
 sha256 BLOB NOT NULL CHECK(length(sha256)=32),
 protection_revision INTEGER NOT NULL CHECK(protection_revision>0),
 deadline INTEGER NOT NULL CHECK(deadline=0 OR deadline>=successful_utc)
) STRICT;
CREATE INDEX sfx_ftn_retained_network ON sfx_ftn_retained_originals(network COLLATE NOCASE,ingress_id);
CREATE TRIGGER sfx_ftn_retained_originals_update BEFORE UPDATE ON sfx_ftn_retained_originals
WHEN NEW.ingress_id IS NOT OLD.ingress_id OR NEW.network IS NOT OLD.network OR NEW.successful_utc IS NOT OLD.successful_utc OR NEW.configuration_revision IS NOT OLD.configuration_revision OR NEW.volume IS NOT OLD.volume OR NEW.native_id IS NOT OLD.native_id OR NEW.byte_count IS NOT OLD.byte_count OR NEW.sha256 IS NOT OLD.sha256 OR NEW.protection_revision<>OLD.protection_revision+1
BEGIN SELECT RAISE(ABORT,'immutable processed provenance and monotonic protection'); END;
CREATE TRIGGER sfx_ftn_retained_originals_delete BEFORE DELETE ON sfx_ftn_retained_originals
BEGIN SELECT RAISE(ABORT,'processed-original provenance retained'); END;
CREATE TABLE sfx_ftn_retention_plans (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 draft_sha256 BLOB NOT NULL CHECK(length(draft_sha256)=32),
 expected_revision INTEGER NOT NULL CHECK(expected_revision>0),
 reviewed_utc INTEGER NOT NULL CHECK(reviewed_utc>0),
 summary BLOB NOT NULL CHECK(length(summary) BETWEEN 1 AND 24576),
 applied_revision INTEGER CHECK(applied_revision>expected_revision)
) STRICT;
CREATE TRIGGER sfx_ftn_retention_plans_update BEFORE UPDATE ON sfx_ftn_retention_plans
WHEN OLD.applied_revision IS NOT NULL OR NEW.applied_revision IS NULL OR NEW.operation_id IS NOT OLD.operation_id OR NEW.draft_sha256 IS NOT OLD.draft_sha256 OR NEW.expected_revision IS NOT OLD.expected_revision OR NEW.reviewed_utc IS NOT OLD.reviewed_utc OR NEW.summary IS NOT OLD.summary
BEGIN SELECT RAISE(ABORT,'immutable exact retention preview'); END;
CREATE TRIGGER sfx_ftn_retention_plans_delete BEFORE DELETE ON sfx_ftn_retention_plans
BEGIN SELECT RAISE(ABORT,'retention review provenance retained'); END;
CREATE TABLE sfx_ftn_retention_plan_items (
 operation_id BLOB NOT NULL REFERENCES sfx_ftn_retention_plans(operation_id),
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_retained_originals(ingress_id),
 protection_revision INTEGER NOT NULL CHECK(protection_revision>0),
 successful_utc INTEGER NOT NULL CHECK(successful_utc>0),
 old_deadline INTEGER NOT NULL CHECK(old_deadline=0 OR old_deadline>=successful_utc),
 new_deadline INTEGER NOT NULL CHECK(new_deadline=0 OR new_deadline>=successful_utc),
 volume TEXT NOT NULL,native_id TEXT NOT NULL,
 byte_count INTEGER NOT NULL,sha256 BLOB NOT NULL CHECK(length(sha256)=32),
 PRIMARY KEY(operation_id,ingress_id)
) STRICT;
CREATE TRIGGER sfx_ftn_retention_plan_items_update BEFORE UPDATE ON sfx_ftn_retention_plan_items
BEGIN SELECT RAISE(ABORT,'immutable exact reviewed protection set'); END;
CREATE TRIGGER sfx_ftn_retention_plan_items_delete BEFORE DELETE ON sfx_ftn_retention_plan_items
BEGIN SELECT RAISE(ABORT,'retention review members retained'); END;
CREATE TABLE sfx_ftn_expiry_intents (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
 dispatch_id BLOB NOT NULL UNIQUE CHECK(length(dispatch_id)=16),
 ingress_id BLOB NOT NULL REFERENCES sfx_ftn_retained_originals(ingress_id),
 protection_revision INTEGER NOT NULL CHECK(protection_revision>0),
 deadline INTEGER NOT NULL CHECK(deadline>0),
 prepared_utc INTEGER NOT NULL CHECK(prepared_utc>=deadline),
 volume TEXT NOT NULL,native_id TEXT NOT NULL,
 byte_count INTEGER NOT NULL,sha256 BLOB NOT NULL CHECK(length(sha256)=32),
 outcome INTEGER NOT NULL CHECK(outcome BETWEEN 0 AND 6),
 native_error INTEGER NOT NULL CHECK(native_error BETWEEN 0 AND 4294967295),
 outcome_utc INTEGER NOT NULL CHECK(outcome_utc>=0)
) STRICT;
CREATE UNIQUE INDEX sfx_ftn_expiry_pending ON sfx_ftn_expiry_intents(ingress_id) WHERE outcome=0;
CREATE TRIGGER sfx_ftn_expiry_intents_update BEFORE UPDATE ON sfx_ftn_expiry_intents
WHEN OLD.outcome<>0 OR NEW.outcome=0 OR NEW.operation_id IS NOT OLD.operation_id OR NEW.dispatch_id IS NOT OLD.dispatch_id OR NEW.ingress_id IS NOT OLD.ingress_id OR NEW.protection_revision IS NOT OLD.protection_revision OR NEW.deadline IS NOT OLD.deadline OR NEW.prepared_utc IS NOT OLD.prepared_utc OR NEW.volume IS NOT OLD.volume OR NEW.native_id IS NOT OLD.native_id OR NEW.byte_count IS NOT OLD.byte_count OR NEW.sha256 IS NOT OLD.sha256
BEGIN SELECT RAISE(ABORT,'immutable exact expiry intent and final outcome'); END;
CREATE TRIGGER sfx_ftn_expiry_intents_delete BEFORE DELETE ON sfx_ftn_expiry_intents
BEGIN SELECT RAISE(ABORT,'expiry provenance retained'); END;
)SQL";
