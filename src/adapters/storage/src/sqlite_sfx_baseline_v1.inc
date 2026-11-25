R"SQL(
CREATE TABLE sfx_schema_migrations(
    version INTEGER PRIMARY KEY CHECK(version > 0),
    name TEXT NOT NULL UNIQUE CHECK(length(name) BETWEEN 1 AND 128),
    content_sha256 TEXT NOT NULL CHECK(length(content_sha256) = 64),
    applied_utc INTEGER NOT NULL CHECK(applied_utc >= 0)
) STRICT;

CREATE TABLE sfx_security_levels(
    security_level INTEGER PRIMARY KEY CHECK(security_level BETWEEN 1 AND 999),
    label TEXT NOT NULL CHECK(length(label) BETWEEN 1 AND 32 AND label=trim(label,' ') AND label NOT GLOB '*[^ -~]*'),
    description TEXT CHECK(description IS NULL OR (length(description) BETWEEN 1 AND 128 AND description=trim(description,' ') AND description NOT GLOB '*[^ -~]*')),
    level_state TEXT NOT NULL CHECK(level_state IN ('active','retired')),
    created_utc_seconds INTEGER NOT NULL CHECK(created_utc_seconds BETWEEN -62135596800 AND 253402300799),
    modified_utc_seconds INTEGER NOT NULL CHECK(modified_utc_seconds BETWEEN created_utc_seconds AND 253402300799),
    retired_utc_seconds INTEGER CHECK((level_state='active' AND retired_utc_seconds IS NULL) OR (level_state='retired' AND retired_utc_seconds BETWEEN created_utc_seconds AND 253402300799))
) STRICT;

CREATE TABLE sfx_security_policy(
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    revision INTEGER NOT NULL CHECK(revision>=1),
    new_user_level INTEGER NOT NULL REFERENCES sfx_security_levels(security_level),
    sysop_threshold INTEGER NOT NULL REFERENCES sfx_security_levels(security_level),
    policy_hash TEXT NOT NULL CHECK(length(policy_hash)=64 AND policy_hash NOT GLOB '*[^0-9a-f]*'),
    published_revision INTEGER CHECK(published_revision IS NULL OR published_revision>=1),
    published_policy_hash TEXT CHECK(published_policy_hash IS NULL OR (length(published_policy_hash)=64 AND published_policy_hash NOT GLOB '*[^0-9a-f]*')),
    modified_utc_seconds INTEGER NOT NULL CHECK(modified_utc_seconds BETWEEN -62135596800 AND 253402300799),
    CHECK((published_revision IS NULL)=(published_policy_hash IS NULL)),
    CHECK(published_revision IS NULL OR (published_revision=revision AND published_policy_hash=policy_hash))
) STRICT;

CREATE TABLE sfx_account_retention_policy(
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    revision INTEGER NOT NULL CHECK(revision>=1),
    audit_online_months INTEGER NOT NULL CHECK(audit_online_months BETWEEN 1 AND 1200),
    audit_total_months INTEGER NOT NULL CHECK(audit_total_months BETWEEN audit_online_months AND 1200),
    session_online_months INTEGER NOT NULL CHECK(session_online_months BETWEEN 1 AND 1200),
    session_total_months INTEGER NOT NULL CHECK(session_total_months BETWEEN session_online_months AND 1200),
    modified_utc_seconds INTEGER NOT NULL CHECK(modified_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;

CREATE TABLE sfx_accounts(
    account_id INTEGER PRIMARY KEY AUTOINCREMENT,
    display_name TEXT,
    name_key TEXT,
    security_level INTEGER REFERENCES sfx_security_levels(security_level),
    account_state TEXT NOT NULL CHECK(account_state IN ('active','locked','deletion_scheduled','tombstoned')),
    revision INTEGER NOT NULL CHECK(revision>=1),
    purge_allowed INTEGER CHECK(purge_allowed IN (0,1)),
    created_utc_seconds INTEGER NOT NULL CHECK(created_utc_seconds BETWEEN -62135596800 AND 253402300799),
    modified_utc_seconds INTEGER NOT NULL CHECK(modified_utc_seconds BETWEEN created_utc_seconds AND 253402300799),
    deletion_scheduled_utc_seconds INTEGER CHECK(deletion_scheduled_utc_seconds IS NULL OR deletion_scheduled_utc_seconds BETWEEN created_utc_seconds AND 253402300799),
    tombstoned_utc_seconds INTEGER CHECK(tombstoned_utc_seconds IS NULL OR tombstoned_utc_seconds BETWEEN created_utc_seconds AND 253402300799),
    tombstone_reason_code TEXT CHECK(tombstone_reason_code IS NULL OR tombstone_reason_code IN ('sysop_action','inactivity_policy')),
    CHECK((account_state='tombstoned' AND display_name IS NULL AND name_key IS NULL AND security_level IS NULL AND purge_allowed IS NULL AND tombstoned_utc_seconds IS NOT NULL AND tombstone_reason_code IS NOT NULL) OR
          (account_state<>'tombstoned' AND display_name IS NOT NULL AND name_key IS NOT NULL AND security_level IS NOT NULL AND purge_allowed IS NOT NULL AND tombstoned_utc_seconds IS NULL AND tombstone_reason_code IS NULL)),
    CHECK(display_name IS NULL OR (length(display_name) BETWEEN 1 AND 30 AND display_name=trim(display_name,' ') AND instr(display_name,'  ')=0 AND display_name NOT GLOB '*[^ -~]*')),
    CHECK(name_key IS NULL OR (length(name_key) BETWEEN 1 AND 30 AND name_key=trim(name_key,' ') AND instr(name_key,'  ')=0 AND name_key NOT GLOB '*[^ -~]*' AND name_key NOT GLOB '*[A-Z]*')),
    CHECK((account_state='deletion_scheduled')=(deletion_scheduled_utc_seconds IS NOT NULL))
) STRICT;

CREATE UNIQUE INDEX sfx_accounts_name_key_unique ON sfx_accounts(name_key) WHERE name_key IS NOT NULL;
CREATE INDEX sfx_accounts_state_name ON sfx_accounts(account_state,name_key,account_id);
CREATE INDEX sfx_accounts_security_name ON sfx_accounts(security_level,name_key,account_id);

CREATE TABLE sfx_account_name_reservations(
    name_key TEXT PRIMARY KEY CHECK(length(name_key) BETWEEN 1 AND 30 AND name_key=trim(name_key,' ') AND instr(name_key,'  ')=0 AND name_key NOT GLOB '*[^ -~]*' AND name_key NOT GLOB '*[A-Z]*'),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    is_current INTEGER NOT NULL CHECK(is_current IN (0,1)),
    display_name TEXT CHECK(display_name IS NULL OR (length(display_name) BETWEEN 1 AND 30 AND display_name=trim(display_name,' ') AND instr(display_name,'  ')=0 AND display_name NOT GLOB '*[^ -~]*')),
    reserved_utc_seconds INTEGER NOT NULL CHECK(reserved_utc_seconds BETWEEN -62135596800 AND 253402300799),
    retired_utc_seconds INTEGER CHECK(retired_utc_seconds IS NULL OR retired_utc_seconds BETWEEN reserved_utc_seconds AND 253402300799),
    CHECK((is_current=1 AND display_name IS NOT NULL AND retired_utc_seconds IS NULL) OR (is_current=0 AND display_name IS NULL AND retired_utc_seconds IS NOT NULL))
) STRICT;

CREATE INDEX sfx_name_reservations_account ON sfx_account_name_reservations(account_id,is_current);

CREATE TABLE sfx_account_profiles(
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    real_name TEXT CHECK(real_name IS NULL OR (length(real_name) BETWEEN 1 AND 64 AND real_name=trim(real_name,' ') AND instr(real_name,'  ')=0 AND real_name NOT GLOB '*[^ -~]*')),
    location TEXT CHECK(location IS NULL OR (length(location) BETWEEN 1 AND 64 AND location=trim(location,' ') AND location NOT GLOB '*[^ -~]*')),
    phone TEXT CHECK(phone IS NULL OR (length(phone) BETWEEN 1 AND 32 AND phone=trim(phone,' ') AND phone NOT GLOB '*[^ -~]*')),
    email TEXT CHECK(email IS NULL OR (length(email) BETWEEN 1 AND 254 AND email=trim(email,' ') AND email NOT GLOB '*[^ -~]*')),
    birth_date TEXT CHECK(birth_date IS NULL OR (
        length(birth_date)=10 AND birth_date GLOB '[0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]' AND
        CAST(substr(birth_date,1,4) AS INTEGER) BETWEEN 1 AND 9999 AND
        CAST(substr(birth_date,6,2) AS INTEGER) BETWEEN 1 AND 12 AND
        CAST(substr(birth_date,9,2) AS INTEGER) BETWEEN 1 AND CASE CAST(substr(birth_date,6,2) AS INTEGER)
            WHEN 2 THEN CASE WHEN (CAST(substr(birth_date,1,4) AS INTEGER)%4=0 AND
                (CAST(substr(birth_date,1,4) AS INTEGER)%100<>0 OR CAST(substr(birth_date,1,4) AS INTEGER)%400=0))
                THEN 29 ELSE 28 END
            WHEN 4 THEN 30 WHEN 6 THEN 30 WHEN 9 THEN 30 WHEN 11 THEN 30 ELSE 31 END)),
    sex TEXT CHECK(sex IS NULL OR sex IN ('M','F','X'))
) STRICT;

CREATE TABLE sfx_account_credentials(
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    envelope_format_version INTEGER NOT NULL CHECK(envelope_format_version BETWEEN 1 AND 65535),
    scheme_identifier TEXT NOT NULL CHECK(length(scheme_identifier) BETWEEN 1 AND 32 AND scheme_identifier NOT GLOB '*[^A-Za-z0-9_.-]*'),
    parameter_encoding_version INTEGER NOT NULL CHECK(parameter_encoding_version BETWEEN 1 AND 65535),
    canonical_parameters BLOB NOT NULL,
    salt BLOB NOT NULL,
    verifier BLOB NOT NULL CHECK(length(verifier)>0),
    pepper_key_identifier TEXT CHECK(pepper_key_identifier IS NULL OR (length(pepper_key_identifier) BETWEEN 1 AND 32 AND pepper_key_identifier NOT GLOB '*[^A-Za-z0-9_.-]*')),
    created_utc_seconds INTEGER NOT NULL CHECK(created_utc_seconds BETWEEN -62135596800 AND 253402300799),
    password_change_required INTEGER NOT NULL CHECK(password_change_required IN (0,1)),
    CHECK(5+length(scheme_identifier)+length(canonical_parameters)+length(salt)+length(verifier)+coalesce(length(pepper_key_identifier),0)<=16384)
) STRICT;

CREATE TABLE sfx_account_statistics(
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    successful_login_count INTEGER NOT NULL DEFAULT 0 CHECK(successful_login_count>=0),
    authenticated_session_seconds INTEGER NOT NULL DEFAULT 0 CHECK(authenticated_session_seconds>=0),
    last_successful_login_utc_seconds INTEGER CHECK(last_successful_login_utc_seconds IS NULL OR last_successful_login_utc_seconds BETWEEN -62135596800 AND 253402300799),
    last_completed_session_utc_seconds INTEGER CHECK(last_completed_session_utc_seconds IS NULL OR last_completed_session_utc_seconds BETWEEN -62135596800 AND 253402300799),
    last_session_outcome_code TEXT CHECK(last_session_outcome_code IS NULL OR last_session_outcome_code IN ('normal','disconnected','timed_out','shutdown','runtime_error'))
) STRICT;

CREATE INDEX sfx_account_statistics_last_login ON sfx_account_statistics(last_successful_login_utc_seconds,account_id);

CREATE TABLE sfx_account_sessions(
    session_id BLOB PRIMARY KEY CHECK(length(session_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    node_id INTEGER NOT NULL CHECK(node_id BETWEEN 1 AND 128),
    started_utc_seconds INTEGER NOT NULL CHECK(started_utc_seconds BETWEEN -62135596800 AND 253402300799),
    persisted_seconds INTEGER NOT NULL DEFAULT 0 CHECK(persisted_seconds>=0),
    last_persisted_utc_seconds INTEGER NOT NULL CHECK(last_persisted_utc_seconds BETWEEN started_utc_seconds AND 253402300799),
    completed_utc_seconds INTEGER CHECK(completed_utc_seconds IS NULL OR completed_utc_seconds BETWEEN started_utc_seconds AND 253402300799),
    outcome_code TEXT CHECK(outcome_code IS NULL OR outcome_code IN ('normal','disconnected','timed_out','shutdown','runtime_error')),
    correlation_id BLOB NOT NULL CHECK(length(correlation_id)=16),
    CHECK((completed_utc_seconds IS NULL AND outcome_code IS NULL) OR (completed_utc_seconds IS NOT NULL AND outcome_code IS NOT NULL))
) STRICT;

CREATE INDEX sfx_account_sessions_account_time ON sfx_account_sessions(account_id,started_utc_seconds DESC,session_id);

CREATE TABLE sfx_account_audit(
    event_id BLOB PRIMARY KEY CHECK(length(event_id)=16),
    event_kind TEXT NOT NULL CHECK(event_kind IN ('account.created','account.renamed','account.state_changed','account.profile_changed','account.credential_replaced','account.tombstoned','security.policy_changed','retention.policy_changed','first_run.completed','session.completed')),
    result_code TEXT NOT NULL CHECK(result_code='succeeded'),
    subject_account_id INTEGER REFERENCES sfx_accounts(account_id),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799),
    correlation_id BLOB NOT NULL CHECK(length(correlation_id)=16),
    reason_code TEXT NOT NULL CHECK(reason_code IN ('initial_setup','registration','sysop_action','inactivity_policy','security_policy','retention_policy','session_finalization'))
) STRICT;

CREATE INDEX sfx_account_audit_subject_time ON sfx_account_audit(subject_account_id,occurred_utc_seconds DESC,event_id);
CREATE INDEX sfx_account_audit_kind_time ON sfx_account_)SQL"
R"SQL(audit(event_kind,occurred_utc_seconds DESC,event_id);
CREATE INDEX sfx_account_audit_time ON sfx_account_audit(occurred_utc_seconds DESC,event_id);

CREATE TABLE sfx_installation_state(
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    setup_state TEXT NOT NULL CHECK(setup_state IN ('incomplete','complete')),
    staged_sysop_account_id INTEGER REFERENCES sfx_accounts(account_id),
    completed_utc_seconds INTEGER CHECK(completed_utc_seconds IS NULL OR completed_utc_seconds BETWEEN -62135596800 AND 253402300799),
    CHECK((setup_state='incomplete' AND completed_utc_seconds IS NULL) OR (setup_state='complete' AND staged_sysop_account_id IS NOT NULL AND completed_utc_seconds IS NOT NULL))
) STRICT;

INSERT INTO sfx_security_levels(security_level,label,description,level_state,created_utc_seconds,modified_utc_seconds)
VALUES(10,'New User',NULL,'active',0,0),(999,'Sysop',NULL,'active',0,0);
INSERT INTO sfx_security_policy(singleton_id,revision,new_user_level,sysop_threshold,policy_hash,published_revision,published_policy_hash,modified_utc_seconds)
VALUES(1,1,10,999,'71cb5a767e365786bbedaa779e4dc743473fb70c2b9dd3a99e4f5ce154062a95',NULL,NULL,0);
INSERT INTO sfx_account_retention_policy(singleton_id,revision,audit_online_months,audit_total_months,session_online_months,session_total_months,modified_utc_seconds)
VALUES(1,1,24,84,12,24,0);
INSERT INTO sfx_installation_state(singleton_id,setup_state,staged_sysop_account_id,completed_utc_seconds)
VALUES(1,'incomplete',NULL,NULL);

CREATE TABLE sfx_system_credentials(
    singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
    revision INTEGER NOT NULL CHECK(revision>=1),
    envelope_format_version INTEGER NOT NULL CHECK(envelope_format_version BETWEEN 1 AND 65535),
    scheme_identifier TEXT NOT NULL CHECK(length(scheme_identifier) BETWEEN 1 AND 32 AND scheme_identifier NOT GLOB '*[^A-Za-z0-9_.-]*'),
    parameter_encoding_version INTEGER NOT NULL CHECK(parameter_encoding_version BETWEEN 1 AND 65535),
    canonical_parameters BLOB NOT NULL,
    salt BLOB NOT NULL,
    verifier BLOB NOT NULL CHECK(length(verifier)>0),
    pepper_key_identifier TEXT CHECK(pepper_key_identifier IS NULL OR (length(pepper_key_identifier) BETWEEN 1 AND 32 AND pepper_key_identifier NOT GLOB '*[^A-Za-z0-9_.-]*')),
    created_utc_seconds INTEGER NOT NULL CHECK(created_utc_seconds BETWEEN -62135596800 AND 253402300799),
    password_change_required INTEGER NOT NULL CHECK(password_change_required=0),
    modified_utc_seconds INTEGER NOT NULL CHECK(modified_utc_seconds BETWEEN created_utc_seconds AND 253402300799),
    CHECK(5+length(scheme_identifier)+length(canonical_parameters)+length(salt)+length(verifier)+coalesce(length(pepper_key_identifier),0)<=16384)
) STRICT;

CREATE TABLE sfx_account_password_resets(
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    revision INTEGER NOT NULL CHECK(revision>=1),
    reset_state TEXT NOT NULL CHECK(reset_state IN ('active','consumed','cancelled','expired')),
    issued_utc_seconds INTEGER NOT NULL CHECK(issued_utc_seconds BETWEEN -62135596800 AND 253402300799),
    expires_utc_seconds INTEGER NOT NULL CHECK(expires_utc_seconds=issued_utc_seconds+259200),
    ended_utc_seconds INTEGER CHECK(ended_utc_seconds IS NULL OR ended_utc_seconds BETWEEN issued_utc_seconds AND 253402300799),
    CHECK((reset_state='active' AND ended_utc_seconds IS NULL) OR (reset_state<>'active' AND ended_utc_seconds IS NOT NULL))
) STRICT;

CREATE TABLE sfx_successful_login_ip_history(
    event_id BLOB PRIMARY KEY CHECK(length(event_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    source_ip TEXT NOT NULL CHECK(length(source_ip) BETWEEN 2 AND 45 AND source_ip NOT GLOB '*[^0-9A-Fa-f:.]*'),
    protocol_code TEXT NOT NULL CHECK(protocol_code IN ('telnet','ssh')),
    succeeded_utc_seconds INTEGER NOT NULL CHECK(succeeded_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;

CREATE INDEX sfx_successful_login_ip_account_time
ON sfx_successful_login_ip_history(account_id,succeeded_utc_seconds DESC,event_id DESC);

CREATE TABLE sfx_authentication_rate_state(
    scope_code TEXT NOT NULL CHECK(scope_code IN ('account','source','system_password','global')),
    subject_hash BLOB NOT NULL CHECK(length(subject_hash)=32),
    failure_count INTEGER NOT NULL CHECK(failure_count BETWEEN 1 AND 4294967295),
    window_started_utc_seconds INTEGER NOT NULL CHECK(window_started_utc_seconds BETWEEN -62135596800 AND 253402300799),
    last_failure_utc_seconds INTEGER NOT NULL CHECK(last_failure_utc_seconds BETWEEN window_started_utc_seconds AND 253402300799),
    blocked_until_utc_seconds INTEGER NOT NULL CHECK(blocked_until_utc_seconds BETWEEN window_started_utc_seconds AND 253402300799),
    expires_utc_seconds INTEGER NOT NULL CHECK(expires_utc_seconds BETWEEN last_failure_utc_seconds AND 253402300799),
    CHECK(blocked_until_utc_seconds<=expires_utc_seconds),
    PRIMARY KEY(scope_code,subject_hash)
) STRICT, WITHOUT ROWID;

CREATE INDEX sfx_authentication_rate_expiry
ON sfx_authentication_rate_state(expires_utc_seconds,scope_code,subject_hash);

CREATE TABLE sfx_authentication_audit(
    event_id BLOB PRIMARY KEY CHECK(length(event_id)=16),
    event_kind TEXT NOT NULL CHECK(event_kind IN (
        'account.password_changed','account.credential_rehashed','account.temp_password_issued','account.temp_password_consumed',
        'account.temp_password_cancelled','account.temp_password_expired','system.password_replaced','system.credential_rehashed',
        'authentication.throttling_cleared')),
    subject_account_id INTEGER REFERENCES sfx_accounts(account_id),
    actor_account_id INTEGER REFERENCES sfx_accounts(account_id),
    occurred_utc_seconds INTEGER NOT NULL CHECK(occurred_utc_seconds BETWEEN -62135596800 AND 253402300799),
    correlation_id BLOB NOT NULL CHECK(length(correlation_id)=16),
    CHECK((event_kind LIKE 'account.%' AND subject_account_id IS NOT NULL) OR
          (event_kind NOT LIKE 'account.%' AND subject_account_id IS NULL))
) STRICT;

CREATE INDEX sfx_authentication_audit_subject_time
ON sfx_authentication_audit(subject_account_id,occurred_utc_seconds DESC,event_id DESC);
CREATE INDEX sfx_authentication_audit_kind_time
ON sfx_authentication_audit(event_kind,occurred_utc_seconds DESC,event_id DESC);

CREATE TABLE sfx_account_transfer_preferences(
    account_id INTEGER PRIMARY KEY REFERENCES sfx_accounts(account_id),
    preference_code TEXT NOT NULL CHECK(preference_code IN ('xmodem','ymodem','ymodem_g','zmodem','select_at_transfer')),
    revision INTEGER NOT NULL CHECK(revision>=1),
    modified_utc_seconds INTEGER NOT NULL CHECK(modified_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;

INSERT INTO sfx_account_transfer_preferences(account_id,preference_code,revision,modified_utc_seconds)
SELECT account_id,'select_at_transfer',1,modified_utc_seconds
FROM sfx_accounts WHERE account_state<>'tombstoned';

CREATE TRIGGER sfx_accounts_default_transfer_preference
AFTER INSERT ON sfx_accounts
WHEN NEW.account_state<>'tombstoned'
BEGIN
    INSERT INTO sfx_account_transfer_preferences(account_id,preference_code,revision,modified_utc_seconds)
    VALUES(NEW.account_id,'select_at_transfer',1,NEW.modified_utc_seconds);
END;

CREATE TABLE sfx_failed_login_notice_claims(
    claim_ordinal INTEGER PRIMARY KEY,
    claim_id BLOB NOT NULL UNIQUE CHECK(length(claim_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    session_id BLOB NOT NULL UNIQUE REFERENCES sfx_account_sessions(session_id) ON DELETE CASCADE,
    claim_state TEXT NOT NULL CHECK(claim_state IN ('active','acknowledged','recovered')),
    claimed_utc_seconds INTEGER NOT NULL CHECK(claimed_utc_seconds BETWEEN -62135596800 AND 253402300499),
    claim_expires_utc_seconds INTEGER NOT NULL CHECK(
        claim_expires_utc_seconds=claimed_utc_seconds+300 AND
        claim_expires_utc_seconds BETWEEN -62135596500 AND 253402300799)
) STRICT;

CREATE UNIQUE INDEX sfx_failed_login_notice_claims_one_active
ON sfx_failed_login_notice_claims(account_id)
WHERE claim_state='active';

CREATE INDEX sfx_failed_login_notice_claims_expiry
ON sfx_failed_login_notice_claims(claim_state,claim_expires_utc_seconds,account_id,claim_id);

CREATE TRIGGER sfx_failed_login_notice_claims_retain_terminal
AFTER UPDATE OF claim_state ON sfx_failed_login_notice_claims
WHEN NEW.claim_state<>'active'
BEGIN
    DELETE FROM sfx_failed_login_notice_claims
    WHERE account_id=NEW.account_id AND claim_state<>'active' AND claim_ordinal IN (
        SELECT claim_ordinal FROM sfx_failed_login_notice_claims
        WHERE account_id=NEW.account_id AND claim_state<>'active'
        ORDER BY claim_ordinal DESC
        LIMIT -1 OFFSET 64
    );
END;

CREATE TABLE sfx_failed_login_notice_enqueues(
    enqueue_ordinal INTEGER PRIMARY KEY,
    notice_id BLOB NOT NULL UNIQUE CHECK(length(notice_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    source_address_family INTEGER NOT NULL CHECK(source_address_family IN (4,6)),
    source_address BLOB NOT NULL CHECK(
        (source_address_family=4 AND length(source_address)=4) OR
        (source_address_family=6 AND length(source_address)=16)),
    event_utc_seconds INTEGER NOT NULL CHECK(event_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;

CREATE INDEX sfx_failed_login_notice_enqueues_account
ON sfx_failed_login_notice_enqueues(account_id,enqueue_ordinal DESC);

CREATE TRIGGER sfx_failed_login_notice_enqueues_retain_recent
AFTER INSERT ON sfx_failed_login_notice_enqueues
BEGIN
    DELETE FROM sfx_failed_login_notice_enqueues
    WHERE account_id=NEW.account_id AND enqueue_ordinal IN (
        SELECT enqueue_ordinal FROM sfx_failed_login_notice_enqueues
        WHERE account_id=NEW.account_id
        ORDER BY enqueue_ordinal DESC
        LIMIT -1 OFFSET 64
    );
END;

CREATE TABLE sfx_failed_login_notices(
    notice_id BLOB PRIMARY KEY CHECK(length(notice_id)=16),
    account_id INTEGER NOT NULL REFERENCES sfx_accounts(account_id),
    source_address_family INTEGER NOT NULL CHECK(source_address_family IN (4,6)),
    source_address BLOB NOT NULL CHECK(
        (source_address_family=4 AND length(source_address)=4) OR
        (source_address_family=6 AND length(source_address)=16)),
    event_utc_seconds INTEGER NOT NULL CHECK(event_utc_seconds BETWEEN -62135596800 AND 253402300799),
    claim_id BLOB REFERENCES sfx_failed_login_notice_claims(claim_id) CHECK(claim_id IS NULL OR length(claim_id)=16),
    claimed_session_id BLOB CHECK(claimed_session_id IS NULL OR length(claimed_session_id)=16),
    claimed_utc_seconds INTEGER CHECK(claimed_utc_seconds IS NULL OR claimed_utc_seconds BETWEEN -62135596800 AND 253402300499),
    claim_expires_utc_seconds INTEGER CHECK(claim_expires_utc_seconds IS NULL OR claim_expires_utc_seconds BETWEEN -62135596500 AND 253402300799),
    CHECK(
        (claim_id IS NULL AND claimed_session_id IS NULL AND claimed_utc_seconds IS NULL AND claim_expires_utc_seconds IS NULL) OR
        (claim_id IS NOT NULL AND claimed_session_id IS NOT NULL AND claimed_utc_seconds IS NOT NULL AND
         claim_expires_utc_seconds=claimed_utc_seconds+300))
) STRICT;

CREATE INDEX sfx_failed_login_notices_unread
ON sfx_failed_login_notices(account_id,event_utc_seconds DESC,notice_id DESC)
WHERE claim_id IS NULL;

CREATE INDEX sfx_failed_login_notices_claim
ON sfx_failed_login_notices(claim_id,account_id,claimed_session_id,event_utc_seconds DESC,notice_id DESC)
WHERE claim_id IS NOT NULL;

CREATE TRIGGER sfx_failed_login_notices_retain_five
AFTER INSERT ON sfx_failed_login_notices
BEGIN
    DELETE FROM sfx_failed_login_notices
    WHERE account_id=NEW.account_id AND claim_id IS NULL AND notice_id IN (
        SELECT notice_id FROM sfx_failed_login_notices
        WHERE account_id=NEW.account_id AND claim_id IS NULL
        ORDER BY event_utc_seconds DESC,notice_id DESC
        LIMIT -1 OFFSET 5
    );
END;

CREAT)SQL"
R"SQL(E TRIGGER sfx_account_sessions_release_failed_login_claim
BEFORE DELETE ON sfx_account_sessions
BEGIN
    UPDATE sfx_failed_login_notices
    SET claim_id=NULL,claimed_session_id=NULL,claimed_utc_seconds=NULL,claim_expires_utc_seconds=NULL
    WHERE claim_id IN (
        SELECT claim_id FROM sfx_failed_login_notice_claims
        WHERE session_id=OLD.session_id AND claim_state='active'
    );

    DELETE FROM sfx_failed_login_notices
    WHERE account_id=OLD.account_id AND claim_id IS NULL AND notice_id IN (
        SELECT notice_id FROM sfx_failed_login_notices
        WHERE account_id=OLD.account_id AND claim_id IS NULL
        ORDER BY event_utc_seconds DESC,notice_id DESC
        LIMIT -1 OFFSET 5
    );

    UPDATE sfx_failed_login_notice_claims
    SET claim_state='recovered'
    WHERE session_id=OLD.session_id AND claim_state='active';
END;

CREATE TABLE sfx_registration_commit_receipts(
    registration_event_id BLOB PRIMARY KEY CHECK(length(registration_event_id)=16),
    fingerprint_format_version INTEGER NOT NULL CHECK(fingerprint_format_version=1),
    request_fingerprint BLOB NOT NULL CHECK(length(request_fingerprint)=32),
    account_id INTEGER NOT NULL UNIQUE REFERENCES sfx_accounts(account_id) CHECK(account_id>0),
    committed_account_revision INTEGER NOT NULL CHECK(committed_account_revision>0),
    committed_utc_seconds INTEGER NOT NULL CHECK(committed_utc_seconds BETWEEN -62135596800 AND 253402300799)
) STRICT;

CREATE TRIGGER sfx_registration_commit_receipts_immutable_update
BEFORE UPDATE ON sfx_registration_commit_receipts
BEGIN
    SELECT RAISE(ABORT,'registration commit receipts are immutable');
END;

CREATE TRIGGER sfx_registration_commit_receipts_immutable_delete
BEFORE DELETE ON sfx_registration_commit_receipts
BEGIN
    SELECT RAISE(ABORT,'registration commit receipts are immutable');
END;
)SQL"
