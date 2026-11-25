// Additive schema24: preserve permanent identities and historical receipts.
const std::string file_group_deletion_migration_sql=R"sql(
CREATE TABLE sfx_file_group_deletions (
 group_id INTEGER PRIMARY KEY REFERENCES sfx_file_groups(group_id),
 operation_id BLOB NOT NULL UNIQUE CHECK(length(operation_id)=16),
 delete_disk_files INTEGER NOT NULL CHECK(delete_disk_files IN(0,1)),
 completed INTEGER NOT NULL CHECK(completed IN(0,1)),
 started_utc INTEGER NOT NULL
) STRICT;
CREATE TABLE sfx_file_group_delete_items (
 group_id INTEGER NOT NULL REFERENCES sfx_file_group_deletions(group_id),
 ordinal INTEGER NOT NULL CHECK(ordinal>=0),
 area_id INTEGER NOT NULL REFERENCES sfx_file_areas(area_id),
 file_id INTEGER REFERENCES sfx_file_catalog(file_id),
 name TEXT NOT NULL CHECK(length(CAST(name AS BLOB)) BETWEEN 1 AND 128),
 bytes INTEGER NOT NULL CHECK(bytes>=0),
 sha256 BLOB NOT NULL CHECK(length(sha256)=32),
 volume TEXT NOT NULL,
 native_id TEXT NOT NULL,
 PRIMARY KEY(group_id,ordinal)
) STRICT;
CREATE TRIGGER sfx_file_group_delete_items_retain BEFORE DELETE ON sfx_file_group_delete_items
BEGIN SELECT RAISE(ABORT,'Reviewed deletion files are retained'); END;
CREATE TRIGGER sfx_file_group_delete_items_immutable BEFORE UPDATE ON sfx_file_group_delete_items
BEGIN SELECT RAISE(ABORT,'Reviewed deletion files are immutable'); END;
CREATE TRIGGER sfx_file_group_deletions_retain BEFORE DELETE ON sfx_file_group_deletions
BEGIN SELECT RAISE(ABORT,'File Group deletion history is retained'); END;
CREATE TRIGGER sfx_file_group_deletions_immutable BEFORE UPDATE ON sfx_file_group_deletions
WHEN NEW.group_id<>OLD.group_id OR NEW.operation_id<>OLD.operation_id OR
 NEW.delete_disk_files<>OLD.delete_disk_files OR NEW.started_utc<>OLD.started_utc OR
 OLD.completed<>0 OR NEW.completed<>1
BEGIN SELECT RAISE(ABORT,'File Group deletion intent is immutable'); END;
)sql";
std::vector<SfxBaselineSchemaObject> file_group_deletion_manifest(){
    static const auto manifest=[] {
        auto fixed=ftn_mail_predecessor();
        for(const auto* sql:{&ftn_mail_migration_sql,&optional_subject_migration_sql,&file_group_deletion_migration_sql})
            if(sqlite3_exec(fixed.get(),sql->c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("File Group deletion fixed predecessor");
        std::vector<SfxBaselineSchemaObject> actual;
        if(!collect_schema_manifest(fixed.get(),&actual))throw std::runtime_error("File Group deletion manifest collection");
        auto result=optional_subject_manifest();
        for(const auto& object:actual)if(object.name.find("sfx_file_group_deletions")==0||object.name.find("sfx_file_group_delete_items")==0)result.push_back(object);
        return result;
    }();return manifest;
}
