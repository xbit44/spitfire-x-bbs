// Additive schema30. No Door program/source file belongs to this schema.
const std::string door_bulletin_migration_sql=R"SQL(
CREATE TABLE sfx_door_bulletin_policy (
 singleton_id INTEGER PRIMARY KEY CHECK(singleton_id=1),
 revision INTEGER NOT NULL CHECK(revision>0)
);
INSERT INTO sfx_door_bulletin_policy VALUES(1,1);
CREATE TABLE sfx_door_bulletins (
 bulletin_id INTEGER PRIMARY KEY CHECK(bulletin_id BETWEEN 1 AND 999),
 title TEXT NOT NULL CHECK(length(title) BETWEEN 1 AND 64),
 enabled INTEGER NOT NULL CHECK(enabled IN(0,1)),
 source_kind INTEGER NOT NULL CHECK(source_kind IN(0,1)),
 door_id INTEGER NOT NULL CHECK(door_id BETWEEN 0 AND 999),
 relative_path TEXT NOT NULL CHECK(length(relative_path)<=240),
 format INTEGER NOT NULL CHECK(format IN(0,1)),
 revision INTEGER NOT NULL CHECK(revision>0),
 CHECK((source_kind=0 AND door_id=0 AND relative_path='' AND format=0)
    OR (source_kind=1 AND door_id>0 AND length(relative_path)>0))
);
CREATE TABLE sfx_door_bulletin_state (
 bulletin_id INTEGER PRIMARY KEY REFERENCES sfx_door_bulletins(bulletin_id) ON DELETE CASCADE,
 health INTEGER NOT NULL DEFAULT 0 CHECK(health BETWEEN 0 AND 2),
 published_utc INTEGER CHECK(published_utc BETWEEN -62135596800 AND 253402300799),
 snapshot BLOB NOT NULL DEFAULT X'' CHECK(length(snapshot)<=262144)
);
CREATE TABLE sfx_door_bulletin_fingerprints (
 bulletin_id INTEGER NOT NULL REFERENCES sfx_door_bulletins(bulletin_id) ON DELETE CASCADE,
 variant TEXT NOT NULL CHECK(length(variant) BETWEEN 1 AND 32),
 content_sha256 BLOB NOT NULL CHECK(length(content_sha256)=32),
 PRIMARY KEY(bulletin_id,variant)
);
CREATE TABLE sfx_door_bulletin_receipts (
 operation_id BLOB PRIMARY KEY CHECK(length(operation_id)=16 AND operation_id<>zeroblob(16)),
 fingerprint BLOB NOT NULL CHECK(length(fingerprint)=32),
 actor_account_id INTEGER NOT NULL,
 revision INTEGER NOT NULL UNIQUE CHECK(revision>1),
 bulletin_id INTEGER NOT NULL CHECK(bulletin_id BETWEEN 1 AND 999),
 removed INTEGER NOT NULL CHECK(removed IN(0,1)),
 changed_utc INTEGER NOT NULL CHECK(changed_utc BETWEEN -62135596800 AND 253402300799)
);
)SQL";
std::vector<SfxBaselineSchemaObject> door_bulletin_manifest(){
    static const auto manifest=[] {
        auto fixed=ftn_mail_predecessor();
        for(const auto* sql:{&ftn_mail_migration_sql,&optional_subject_migration_sql,&file_group_deletion_migration_sql,&ftn_wire_body_migration_sql,&ftn_subject_migration_sql,&message_location_migration_sql,&ftn_external_migration_sql,&ftn_recovery_migration_sql,&door_bulletin_migration_sql})
            if(sqlite3_exec(fixed.get(),sql->c_str(),nullptr,nullptr,nullptr)!=SQLITE_OK)throw std::runtime_error("Door Bulletin fixed manifest");
        std::vector<SfxBaselineSchemaObject> actual;
        if(!collect_schema_manifest(fixed.get(),&actual))throw std::runtime_error("Door Bulletin manifest collection");
        auto result=ftn_recovery_manifest();
        for(const auto& object:actual)if(object.name.rfind("sfx_door_bulletin",0)==0)result.push_back(object);
        return result;
    }();return manifest;
}
