constexpr const char* ftn_forward_plan_schema=R"SQL(
CREATE TABLE sfx_ftn_file_forward_plans (
 operation_id BLOB PRIMARY KEY REFERENCES sfx_ftn_file_publications(operation_id),
 publication_fingerprint BLOB NOT NULL CHECK(length(publication_fingerprint)=32),
 plan_sha256 BLOB NOT NULL CHECK(length(plan_sha256)=32),
 plan BLOB NOT NULL CHECK(length(plan) BETWEEN 1 AND 2097152)
) STRICT;
CREATE TRIGGER sfx_ftn_file_forward_plans_update BEFORE UPDATE ON sfx_ftn_file_forward_plans
BEGIN SELECT RAISE(ABORT,'immutable complete original forwarding plan'); END;
CREATE TRIGGER sfx_ftn_file_forward_plans_delete BEFORE DELETE ON sfx_ftn_file_forward_plans
BEGIN SELECT RAISE(ABORT,'original forwarding plan retained'); END;
)SQL";
