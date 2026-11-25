// Schema29: no history hold is cleared by migration. Only a matching immutable
// receipt and generation advance may clear a validated history-only hold.
const std::string ftn_recovery_migration_sql=R"SQL(
ALTER TABLE sfx_ftn_serial_allocator ADD COLUMN recovery_generation INTEGER NOT NULL DEFAULT 0
 CHECK(recovery_generation>=0);
CREATE TABLE sfx_ftn_serial_recoveries (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16 AND operation_id<>zeroblob(16)),
 generation INTEGER NOT NULL UNIQUE CHECK(generation>0),
 lineage BLOB NOT NULL CHECK(length(lineage)=16 AND lineage<>zeroblob(16)),
 old_floor INTEGER NOT NULL CHECK(old_floor BETWEEN 0 AND 4294967295),
 new_floor INTEGER NOT NULL CHECK(new_floor BETWEEN old_floor AND 4294967294),
 recovered_utc INTEGER NOT NULL CHECK(recovered_utc BETWEEN 0 AND 4294967295),
 prior_known INTEGER NOT NULL CHECK(prior_known IN(0,1)),
 held_outbound INTEGER NOT NULL CHECK(held_outbound>=0),
 held_ingress INTEGER NOT NULL CHECK(held_ingress>=0)
) STRICT;
CREATE TRIGGER sfx_ftn_serial_recoveries_insert BEFORE INSERT ON sfx_ftn_serial_recoveries
WHEN NOT EXISTS(SELECT 1 FROM sfx_ftn_serial_allocator a WHERE a.singleton_id=1
 AND a.recovery_required=1 AND NEW.generation=a.recovery_generation+1
 AND NEW.lineage IS a.lineage AND NEW.old_floor=a.high_water)
BEGIN SELECT RAISE(ABORT,'FTN recovery receipt must match held allocator'); END;
CREATE TRIGGER sfx_ftn_serial_recoveries_update BEFORE UPDATE ON sfx_ftn_serial_recoveries
BEGIN SELECT RAISE(ABORT,'FTN recovery receipt retained'); END;
CREATE TRIGGER sfx_ftn_serial_recoveries_delete BEFORE DELETE ON sfx_ftn_serial_recoveries
BEGIN SELECT RAISE(ABORT,'FTN recovery receipt retained'); END;
DROP TRIGGER sfx_ftn_serial_allocator_update;
CREATE TRIGGER sfx_ftn_serial_allocator_update BEFORE UPDATE ON sfx_ftn_serial_allocator
WHEN NEW.singleton_id<>OLD.singleton_id OR NEW.high_water<OLD.high_water
 OR NEW.lineage IS NOT OLD.lineage
 OR (NEW.recovery_required>=OLD.recovery_required AND NEW.recovery_generation<>OLD.recovery_generation)
 OR (NEW.recovery_required<OLD.recovery_required AND (
     OLD.recovery_required<>1 OR NEW.recovery_required<>0
     OR NEW.recovery_generation<>OLD.recovery_generation+1
     OR NOT EXISTS(SELECT 1 FROM sfx_ftn_serial_recoveries r
         WHERE r.generation=NEW.recovery_generation AND r.lineage IS OLD.lineage
         AND r.old_floor=OLD.high_water AND r.new_floor=NEW.high_water)))
BEGIN SELECT RAISE(ABORT,'FTN serial floor and receipted recovery retained'); END;
)SQL";
std::vector<SfxBaselineSchemaObject> ftn_recovery_manifest(){
    static const auto manifest=[] {
        auto fixed=ftn_mail_predecessor();
        for(const auto* sql:{&ftn_mail_migration_sql,&optional_subject_migration_sql,&file_group_deletion_migration_sql,&ftn_wire_body_migration_sql,&ftn_subject_migration_sql,&message_location_migration_sql,&ftn_external_migration_sql,&ftn_recovery_migration_sql})
            if(sqlite3_exec(fixed.get(),sql->c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("FTN recovery fixed manifest");
        std::vector<SfxBaselineSchemaObject> actual;
        if(!collect_schema_manifest(fixed.get(),&actual))throw std::runtime_error("FTN recovery manifest collection");
        auto result=ftn_external_manifest();
        for(const auto& object:actual){
            auto prior=std::find_if(result.begin(),result.end(),[&](const auto& row){return row.name==object.name;});
            if(prior!=result.end())*prior=object;
            else if(object.name.rfind("sfx_ftn_serial_recoveries",0)==0)result.push_back(object);
        }return result;
    }();return manifest;
}
