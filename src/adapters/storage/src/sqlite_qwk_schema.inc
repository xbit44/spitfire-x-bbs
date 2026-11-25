// Additive Section8D migration14, not present in the accepted Section8C release.
// No previous migration text or ordinary message/scan/mail row is rewritten.
// Owner selected export word-wrap Off on October14 after actual reader comparison.
// Seed only the initial policy; explicit saved caller On/Off choices remain owned.
const std::string qwk_migration_sql=R"SQL(
CREATE TABLE sfx_qwk_policy (
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    board_id TEXT NOT NULL CHECK(length(board_id) BETWEEN 1 AND 8
        AND substr(board_id,1,1) BETWEEN 'A' AND 'Z'
        AND board_id NOT GLOB '*[^A-Z0-9_!#$%&''()@^`{}~-]*'
        AND board_id NOT IN ('SYSOP','NETMAIL','CON','PRN','AUX','NUL','CLOCK$')
        AND board_id NOT GLOB 'COM[1-9]' AND board_id NOT GLOB 'LPT[1-9]'),
    maximum_messages INTEGER NOT NULL CHECK(maximum_messages BETWEEN 1 AND 65535),
    word_wrap_default INTEGER CHECK(word_wrap_default IS NULL OR word_wrap_default IN (0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1)
) STRICT;
INSERT INTO sfx_qwk_policy VALUES(1,'SFX'||substr(hex(randomblob(5)),1,5),10000,0,1);
CREATE TABLE sfx_qwk_caller_preferences (
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    private_mode INTEGER NOT NULL CHECK(private_mode BETWEEN 0 AND 2),
    own_public_posts INTEGER NOT NULL CHECK(own_public_posts IN (0,1)),
    encoding INTEGER NOT NULL CHECK(encoding IN (0,1)),
    preserve_colors INTEGER NOT NULL CHECK(preserve_colors IN (0,1)),
    word_wrap INTEGER NOT NULL CHECK(word_wrap IN (0,1)),
    indexes INTEGER NOT NULL CHECK(indexes IN (0,1)),
    full_headers INTEGER NOT NULL CHECK(full_headers IN (0,1)),
    revision INTEGER NOT NULL CHECK(revision>=1),
    CHECK(encoding=0 OR full_headers=1)
) STRICT;
CREATE TABLE sfx_qwk_conference_mapping (
    conference INTEGER PRIMARY KEY CHECK(conference BETWEEN 1 AND 65535),
    echo_id INTEGER NOT NULL UNIQUE CHECK(echo_id>0)
) STRICT;
INSERT INTO sfx_qwk_conference_mapping SELECT conference_number,echo_id FROM sfx_local_echoes;
CREATE TRIGGER sfx_qwk_conference_mapping_update BEFORE UPDATE ON sfx_qwk_conference_mapping
BEGIN SELECT RAISE(ABORT,'QWK conference identities are immutable'); END;
CREATE TRIGGER sfx_qwk_conference_mapping_delete BEFORE DELETE ON sfx_qwk_conference_mapping
BEGIN SELECT RAISE(ABORT,'QWK conference identities are never reused'); END;
CREATE TRIGGER sfx_qwk_conference_mapping_seed AFTER INSERT ON sfx_local_echoes
BEGIN
    SELECT CASE WHEN (SELECT coalesce(max(conference),0) FROM sfx_qwk_conference_mapping)>=65535
        THEN RAISE(ABORT,'QWK conference identity namespace exhausted') END;
    INSERT INTO sfx_qwk_conference_mapping SELECT coalesce(max(conference),0)+1,NEW.echo_id FROM sfx_qwk_conference_mapping;
END;
CREATE TABLE sfx_qwk_message_reference (
    reference INTEGER PRIMARY KEY CHECK(reference BETWEEN 1 AND 9999999),
    message_id INTEGER NOT NULL CHECK(message_id>0),
    creation_operation BLOB NOT NULL CHECK(length(creation_operation)=16),
    UNIQUE(message_id,creation_operation)
) STRICT;
CREATE TRIGGER sfx_qwk_message_reference_update BEFORE UPDATE ON sfx_qwk_message_reference
BEGIN SELECT RAISE(ABORT,'QWK message references are immutable'); END;
CREATE TRIGGER sfx_qwk_message_reference_delete BEFORE DELETE ON sfx_qwk_message_reference
BEGIN SELECT RAISE(ABORT,'QWK message references are never reused'); END;
CREATE TABLE sfx_qwk_continuation (
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    revision INTEGER NOT NULL CHECK(revision>=1),
    settings_revision INTEGER NOT NULL CHECK(settings_revision>=1),
    selection_fingerprint BLOB NOT NULL CHECK(length(selection_fingerprint)=32),
    arrival_fence INTEGER NOT NULL CHECK(arrival_fence>=0),
    next_queue INTEGER NOT NULL CHECK(next_queue BETWEEN 0 AND 999),
    private_after_arrival INTEGER NOT NULL CHECK(private_after_arrival>=0)
) STRICT;
CREATE TABLE sfx_qwk_continuation_echo (
    account_id INTEGER NOT NULL REFERENCES sfx_qwk_continuation(account_id) ON DELETE CASCADE,
    echo_id INTEGER NOT NULL REFERENCES sfx_local_echoes(echo_id) ON DELETE CASCADE,
    scan_revision INTEGER NOT NULL CHECK(scan_revision>=1),
    initial_highest_opened INTEGER NOT NULL CHECK(initial_highest_opened>=0),
    after_arrival INTEGER NOT NULL CHECK(after_arrival>=0),
    PRIMARY KEY(account_id,echo_id)
) STRICT;
CREATE TABLE sfx_qwk_delivery_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    zip_sha256 BLOB NOT NULL CHECK(length(zip_sha256)=32),
    compressed_bytes INTEGER NOT NULL CHECK(compressed_bytes BETWEEN 1 AND 16777216),
    expanded_bytes INTEGER NOT NULL CHECK(expanded_bytes BETWEEN 1 AND 67108864),
    delivered INTEGER NOT NULL CHECK(delivered BETWEEN 1 AND 65535),
    more INTEGER NOT NULL CHECK(more IN (0,1)),
    committed_utc_seconds INTEGER NOT NULL CHECK(committed_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;
CREATE TRIGGER sfx_qwk_delivery_receipts_update BEFORE UPDATE ON sfx_qwk_delivery_receipts
BEGIN SELECT RAISE(ABORT,'QWK delivery receipts are immutable'); END;
CREATE TRIGGER sfx_qwk_delivery_receipts_delete BEFORE DELETE ON sfx_qwk_delivery_receipts
BEGIN SELECT RAISE(ABORT,'QWK delivery receipts are durable'); END;
CREATE TABLE sfx_qwk_rep_receipts (
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    logical_fingerprint BLOB NOT NULL CHECK(length(logical_fingerprint)=32),
    board_id TEXT NOT NULL CHECK(length(board_id) BETWEEN 1 AND 8),
    record_count INTEGER NOT NULL CHECK(record_count>=1),
    messages INTEGER NOT NULL CHECK(messages>=0),
    commands INTEGER NOT NULL CHECK(commands>=0),
    missing_parents INTEGER NOT NULL CHECK(missing_parents>=0 AND missing_parents<=messages),
    committed_utc_seconds INTEGER NOT NULL CHECK(committed_utc_seconds BETWEEN -62135596800 AND 253402300799),
    PRIMARY KEY(account_id,logical_fingerprint),
    CHECK(record_count=messages+commands)
) STRICT;
CREATE TRIGGER sfx_qwk_rep_receipts_update BEFORE UPDATE ON sfx_qwk_rep_receipts
BEGIN SELECT RAISE(ABORT,'REP receipts are immutable'); END;
CREATE TRIGGER sfx_qwk_rep_receipts_delete BEFORE DELETE ON sfx_qwk_rep_receipts
BEGIN SELECT RAISE(ABORT,'REP receipts are durable'); END;
CREATE TABLE sfx_qwk_policy_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    installation_owner INTEGER NOT NULL CHECK(installation_owner IN (0,1)),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    resulting_revision INTEGER NOT NULL CHECK(resulting_revision>=1),
    committed_utc_seconds INTEGER NOT NULL CHECK(committed_utc_seconds BETWEEN -62135596800 AND 253402300799),
    CHECK((installation_owner=1 AND actor_account_id IS NULL) OR (installation_owner=0 AND actor_account_id IS NOT NULL))
) STRICT;
CREATE TRIGGER sfx_qwk_policy_receipts_update BEFORE UPDATE ON sfx_qwk_policy_receipts
BEGIN SELECT RAISE(ABORT,'QWK policy receipts are immutable'); END;
CREATE TRIGGER sfx_qwk_policy_receipts_delete BEFORE DELETE ON sfx_qwk_policy_receipts
BEGIN SELECT RAISE(ABORT,'QWK policy receipts are durable'); END;
CREATE TABLE sfx_qwk_pointer_receipts (
    operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    committed_utc_seconds INTEGER NOT NULL CHECK(committed_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;
CREATE TRIGGER sfx_qwk_pointer_receipts_update BEFORE UPDATE ON sfx_qwk_pointer_receipts
BEGIN SELECT RAISE(ABORT,'QWK pointer receipts are immutable'); END;
CREATE TRIGGER sfx_qwk_pointer_receipts_delete BEFORE DELETE ON sfx_qwk_pointer_receipts
BEGIN SELECT RAISE(ABORT,'QWK pointer receipts are durable'); END;
)SQL";
