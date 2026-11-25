// Schema28 retains every old proof value. Only a new explicit local-hook
// observation can authenticate external custody; migrated rows default false.
const std::string ftn_external_migration_sql=R"SQL(
ALTER TABLE sfx_ftn_ingress ADD COLUMN external_session_authenticated INTEGER NOT NULL DEFAULT 0
 CHECK(external_session_authenticated IN(0,1) AND
 (external_session_authenticated=0 OR (proof=5 AND outbound=0)));
ALTER TABLE sfx_ftn_ingress_sessions ADD COLUMN external_session_authenticated INTEGER NOT NULL DEFAULT 0
 CHECK(external_session_authenticated IN(0,1) AND (external_session_authenticated=0 OR proof=5));
CREATE TABLE sfx_ftn_external_publications (
 job_id BLOB PRIMARY KEY REFERENCES sfx_ftn_outbound(job_id),
 claim_id BLOB NOT NULL CHECK(length(claim_id)=16),
 busy_path TEXT NOT NULL CHECK(length(busy_path) BETWEEN 1 AND 512),
 flow_path TEXT NOT NULL CHECK(length(flow_path) BETWEEN 1 AND 512),
 copy_path TEXT NOT NULL UNIQUE CHECK(length(copy_path) BETWEEN 1 AND 512),
 state INTEGER NOT NULL DEFAULT 0 CHECK(state IN(0,1,2)),
 created_utc INTEGER NOT NULL CHECK(created_utc>=0)
) STRICT;
CREATE INDEX sfx_ftn_external_pending ON sfx_ftn_external_publications(state,claim_id);
CREATE TRIGGER sfx_ftn_external_publications_update BEFORE UPDATE ON sfx_ftn_external_publications
WHEN NEW.job_id IS NOT OLD.job_id OR NEW.claim_id IS NOT OLD.claim_id OR
 NEW.busy_path IS NOT OLD.busy_path OR NEW.flow_path IS NOT OLD.flow_path OR
 NEW.copy_path IS NOT OLD.copy_path OR NEW.created_utc IS NOT OLD.created_utc OR
 (OLD.state<>0 AND NEW.state<>OLD.state)
BEGIN SELECT RAISE(ABORT,'external publication identity retained'); END;
CREATE TRIGGER sfx_ftn_external_publications_delete BEFORE DELETE ON sfx_ftn_external_publications
BEGIN SELECT RAISE(ABORT,'external handoff history retained'); END;
CREATE TABLE sfx_ftn_external_releases (
 claim_id BLOB PRIMARY KEY CHECK(length(claim_id)=16),
 released_utc INTEGER NOT NULL CHECK(released_utc>=0)
) STRICT;
CREATE TRIGGER sfx_ftn_external_releases_update BEFORE UPDATE ON sfx_ftn_external_releases
BEGIN SELECT RAISE(ABORT,'external lock release retained'); END;
CREATE TRIGGER sfx_ftn_external_releases_delete BEFORE DELETE ON sfx_ftn_external_releases
BEGIN SELECT RAISE(ABORT,'external lock release retained'); END;
)SQL";
std::vector<SfxBaselineSchemaObject> ftn_external_manifest(){
    static const auto manifest=[] {
        auto fixed=ftn_mail_predecessor();
        for(const auto* sql:{&ftn_mail_migration_sql,&optional_subject_migration_sql,&file_group_deletion_migration_sql,&ftn_wire_body_migration_sql,&ftn_subject_migration_sql,&message_location_migration_sql,&ftn_external_migration_sql})
            if(sqlite3_exec(fixed.get(),sql->c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("external FTN fixed manifest");
        std::vector<SfxBaselineSchemaObject> actual;
        if(!collect_schema_manifest(fixed.get(),&actual))throw std::runtime_error("external FTN manifest collection");
        auto result=message_location_manifest();
        for(const auto& object:actual){
            auto prior=std::find_if(result.begin(),result.end(),[&](const auto& row){return row.name==object.name;});
            if(prior!=result.end())*prior=object;
            else if(object.name.rfind("sfx_ftn_external_",0)==0)result.push_back(object);
        }return result;
    }();return manifest;
}
