constexpr const char* ftn_continuation_schema=R"SQL(
CREATE TABLE sfx_ftn_file_authorizations (
 attempt_id BLOB PRIMARY KEY REFERENCES sfx_ftn_tic_retries(operation_id),
 publication_id BLOB NOT NULL REFERENCES sfx_ftn_file_publications(operation_id),
 original_fingerprint BLOB NOT NULL CHECK(length(original_fingerprint)=32),
 review_sha256 BLOB NOT NULL CHECK(length(review_sha256)=32),
 frontier_sha256 BLOB NOT NULL CHECK(length(frontier_sha256)=32),
 start_phase INTEGER NOT NULL CHECK(start_phase BETWEEN 0 AND 5),
 configuration_revision INTEGER NOT NULL CHECK(configuration_revision>0),
 area_revision INTEGER NOT NULL CHECK(area_revision>0),
 file_catalog_revision INTEGER NOT NULL CHECK(file_catalog_revision>0),
 password_verified INTEGER NOT NULL CHECK(password_verified IN(0,1)),
 authenticated_same_link INTEGER NOT NULL CHECK(authenticated_same_link IN(0,1)),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0)
) STRICT;
CREATE INDEX sfx_ftn_file_authorizations_publication ON sfx_ftn_file_authorizations(publication_id);
CREATE TRIGGER sfx_ftn_file_authorizations_update BEFORE UPDATE ON sfx_ftn_file_authorizations
BEGIN SELECT RAISE(ABORT,'immutable exact reviewed continuation authority'); END;
CREATE TRIGGER sfx_ftn_file_authorizations_delete BEFORE DELETE ON sfx_ftn_file_authorizations
BEGIN SELECT RAISE(ABORT,'reviewed continuation authority retained'); END;
CREATE TABLE sfx_ftn_file_step_authorities (
 operation_id BLOB NOT NULL,
 phase INTEGER NOT NULL,
 attempt_id BLOB NOT NULL REFERENCES sfx_ftn_file_authorizations(attempt_id),
 PRIMARY KEY(operation_id,phase),
 FOREIGN KEY(operation_id,phase) REFERENCES sfx_ftn_file_steps(operation_id,phase)
) STRICT;
CREATE TRIGGER sfx_ftn_file_step_authorities_update BEFORE UPDATE ON sfx_ftn_file_step_authorities
BEGIN SELECT RAISE(ABORT,'immutable exact continuation step authority'); END;
CREATE TRIGGER sfx_ftn_file_step_authorities_delete BEFORE DELETE ON sfx_ftn_file_step_authorities
BEGIN SELECT RAISE(ABORT,'continuation step authority retained'); END;
CREATE TABLE sfx_ftn_spool_authorities (
 job_id BLOB NOT NULL REFERENCES sfx_ftn_spool_objects(job_id),
 stage INTEGER NOT NULL CHECK(stage IN(1,2)),
 attempt_id BLOB NOT NULL REFERENCES sfx_ftn_file_authorizations(attempt_id),
 PRIMARY KEY(job_id,stage)
) STRICT;
CREATE TRIGGER sfx_ftn_spool_authorities_update BEFORE UPDATE ON sfx_ftn_spool_authorities
BEGIN SELECT RAISE(ABORT,'immutable exact spool continuation authority'); END;
CREATE TRIGGER sfx_ftn_spool_authorities_delete BEFORE DELETE ON sfx_ftn_spool_authorities
BEGIN SELECT RAISE(ABORT,'spool continuation authority retained'); END;
CREATE TABLE sfx_ftn_file_commit_authorities (
 operation_id BLOB PRIMARY KEY REFERENCES sfx_ftn_file_receipts(operation_id),
 attempt_id BLOB NOT NULL REFERENCES sfx_ftn_file_authorizations(attempt_id)
) STRICT;
CREATE TRIGGER sfx_ftn_file_commit_authorities_update BEFORE UPDATE ON sfx_ftn_file_commit_authorities
BEGIN SELECT RAISE(ABORT,'immutable actual publication authority'); END;
CREATE TRIGGER sfx_ftn_file_commit_authorities_delete BEFORE DELETE ON sfx_ftn_file_commit_authorities
BEGIN SELECT RAISE(ABORT,'actual publication authority retained'); END;
)SQL";
