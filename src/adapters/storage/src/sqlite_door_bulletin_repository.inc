namespace door_bulletin_detail {
namespace app=application;
std::int64_t revision(sqlite3* db){SqliteStatement q(db,"SELECT revision FROM sfx_door_bulletin_policy WHERE singleton_id=1","Door Bulletin revision");if(!q.row()||q.int64(0)<1)file_detail::corrupt();return q.int64(0);}
app::DoorBulletinEntry read(SqliteStatement& q){
    app::DoorBulletinEntry e;auto& d=e.definition;
    d.id={static_cast<std::uint16_t>(q.int64(0))};d.title=q.text(1);d.enabled=q.boolean(2);d.source=static_cast<app::DoorBulletinSourceKind>(q.int64(3));d.door=static_cast<app::DoorId>(q.int64(4));d.relative_path=q.text(5);d.format=static_cast<app::GraphicsMode>(q.int64(6));d.revision=q.int64(7);e.health=static_cast<app::DoorBulletinHealth>(q.int64(8));if(!q.is_null(9))e.published_utc=q.int64(9);
    if(!app::valid_door_bulletin_definition(d)||d.revision<1||static_cast<unsigned>(e.health)>2){
        file_detail::corrupt();
    }
    return e;
}
const char* const selection="SELECT b.bulletin_id,b.title,b.enabled,b.source_kind,b.door_id,b.relative_path,b.format,b.revision,s.health,s.published_utc FROM sfx_door_bulletins b JOIN sfx_door_bulletin_state s USING(bulletin_id) ";
app::DoorBulletinEntry entry(sqlite3* db,app::DoorBulletinId id){const auto sql=std::string(selection)+"WHERE b.bulletin_id=?1";SqliteStatement q(db,sql.c_str(),"one Door Bulletin");q.bind(1,id.value);if(!q.row())file_detail::unavailable();return read(q);}
void authority(sqlite3* db,const app::MessageConfigurationActor& a,const app::DoorBulletinEntry& e){
    if(a.installation_owner){message_detail::config_authority(db,a);return;}
    if(!a.caller||!e.definition.enabled)file_detail::unavailable();
    (void)message_detail::actor(db,*a.caller);
}
app::DoorBulletinSource source(sqlite3* db,const app::DoorBulletinEntry& e){
    app::DoorBulletinSource r;r.definition=e.definition;
    if(r.definition.source==app::DoorBulletinSourceKind::door_file){
        r.door_revision=door_detail::revision(db);
        SqliteStatement q(db,"SELECT definition FROM sfx_door_definitions WHERE door_id=?1","current Door Bulletin working directory");q.bind(1,r.definition.door);
        if(q.row()){const auto door=app::decode_door_definition(q.blob(0));if(!door||door->id!=r.definition.door)file_detail::corrupt();if(door->kind==app::DoorExecutionKind::local){r.door_available=true;r.work_directory=door->program.startup_directory;}}
    }return r;
}
}
application::StorageResult<application::DoorBulletinCatalog> SqliteAccountRepository::door_bulletin_configuration(const application::MessageConfigurationActor& a){
    return impl_->readers_.read<application::DoorBulletinCatalog>("Door Bulletin configuration",[a](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);
        application::DoorBulletinCatalog c;c.revision=door_bulletin_detail::revision(db);
        const auto sql=std::string(door_bulletin_detail::selection)+"ORDER BY b.bulletin_id LIMIT 1000";SqliteStatement q(db,sql.c_str(),"bounded Door Bulletin configuration");while(q.row())c.entries.push_back(door_bulletin_detail::read(q));
        if(application::encode_door_bulletin_catalog(c).empty()){
            file_detail::corrupt();
        }
        return c;
    });
}
application::StorageResult<application::DoorChangeOutcome> SqliteAccountRepository::change_door_bulletin(const application::MessageConfigurationActor& a,const application::DoorBulletinChange& r){
    if(!application::valid_door_bulletin_change(r))return read_failure<application::DoorChangeOutcome>(invalid("Door Bulletin definition"));
    const auto encoded=application::encode_door_bulletin_change(r);file_detail::Fingerprint f("Door Bulletin definition/v1");f.actor(a);f.text({reinterpret_cast<const char*>(encoded.data()),encoded.size()});const auto fingerprint=f.value();const auto now=impl_->message_utc_now_();
    if(!application::valid_utc_seconds(now))return read_failure<application::DoorChangeOutcome>(invalid("Door Bulletin clock"));
    return impl_->write_result<application::DoorChangeOutcome>("save Door Bulletin definition",4096,[a,r,fingerprint,now](sqlite3* db,auto& out){
        message_detail::config_authority(db,a);
        SqliteStatement receipt(db,"SELECT fingerprint,actor_account_id,revision FROM sfx_door_bulletin_receipts WHERE operation_id=?1","Door Bulletin save receipt");receipt.bind(1,r.operation);
        if(receipt.row()){const auto bytes=receipt.blob(0);if(bytes.size()!=fingerprint.size()||!std::equal(bytes.begin(),bytes.end(),fingerprint.begin())||receipt.int64(1)!=(a.caller?a.caller->account:0))return SQLITE_ABORT;out=application::DoorChangeOutcome{receipt.int64(2),true};return SQLITE_OK;}
        const auto revision=door_bulletin_detail::revision(db);if(revision!=r.expected_revision||revision==INT64_MAX)return SQLITE_ABORT;
        SqliteStatement exists(db,"SELECT 1 FROM sfx_door_bulletins WHERE bulletin_id=?1","Door Bulletin permanent identity");exists.bind(1,r.definition.id.value);const bool present=exists.row();
        if((r.create_only&&present)||(!r.create_only&&!present))return SQLITE_ABORT;
        bool remapped{};if(present&&!r.remove){const auto old=door_bulletin_detail::entry(db,r.definition.id).definition;const auto& d=r.definition;remapped=old.source!=d.source||old.door!=d.door||old.relative_path!=d.relative_path||old.format!=d.format;}
        if(r.remove){SqliteStatement q(db,"DELETE FROM sfx_door_bulletins WHERE bulletin_id=?1","remove definition only, never source files");q.bind(1,r.definition.id.value);q.step();}
        else{
            const auto& d=r.definition;
            SqliteStatement q(db,"INSERT INTO sfx_door_bulletins VALUES(?1,?2,?3,?4,?5,?6,?7,?8) ON CONFLICT(bulletin_id) DO UPDATE SET title=excluded.title,enabled=excluded.enabled,source_kind=excluded.source_kind,door_id=excluded.door_id,relative_path=excluded.relative_path,format=excluded.format,revision=excluded.revision","save permanent Door Bulletin definition");
            q.bind(1,d.id.value);q.bind(2,d.title);q.bind(3,d.enabled);q.bind(4,static_cast<std::int64_t>(d.source));q.bind(5,d.door);q.bind(6,d.relative_path);q.bind(7,static_cast<std::int64_t>(d.format));q.bind(8,revision+1);q.step();
            // Runtime health is separate from editor revision. Any explicit
            // definition edit requires a fresh source check before viewing.
            SqliteStatement state(db,"INSERT INTO sfx_door_bulletin_state(bulletin_id) VALUES(?1) ON CONFLICT(bulletin_id) DO UPDATE SET health=0,snapshot=X''","invalidate only current availability");state.bind(1,d.id.value);state.step();
            if(remapped){SqliteStatement clear(db,"DELETE FROM sfx_door_bulletin_fingerprints WHERE bulletin_id=?1","new source mapping establishes new publication identity");clear.bind(1,d.id.value);clear.step();SqliteStatement reset(db,"UPDATE sfx_door_bulletin_state SET published_utc=NULL WHERE bulletin_id=?1","failed remap must not announce old source");reset.bind(1,d.id.value);reset.step();}
        }
        SqliteStatement advance(db,"UPDATE sfx_door_bulletin_policy SET revision=?1 WHERE singleton_id=1","advance definition revision only");advance.bind(1,revision+1);advance.step();
        SqliteStatement save(db,"INSERT INTO sfx_door_bulletin_receipts VALUES(?1,?2,?3,?4,?5,?6,?7)","audit Door Bulletin save");save.bind(1,r.operation);save.bind(2,fingerprint);save.bind(3,a.caller?a.caller->account:0);save.bind(4,revision+1);save.bind(5,r.definition.id.value);save.bind(6,r.remove);save.bind(7,now);save.step();
        SqliteStatement trim(db,"DELETE FROM sfx_door_bulletin_receipts WHERE revision<(?1-4096)","bound Door Bulletin receipts");trim.bind(1,revision+1);trim.step();out=application::DoorChangeOutcome{revision+1,false};return SQLITE_OK;
    });
}
application::StorageResult<application::DoorBulletinListing> SqliteAccountRepository::door_bulletins(const application::MessageActor& a){
    return impl_->readers_.read<application::DoorBulletinListing>("caller Door Bulletin index",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);(void)message_detail::actor(db,a);application::DoorBulletinListing list;list.revision=door_bulletin_detail::revision(db);
        SqliteStatement q(db,"SELECT b.bulletin_id,b.title,b.revision,s.health FROM sfx_door_bulletins b JOIN sfx_door_bulletin_state s USING(bulletin_id) WHERE b.enabled=1 ORDER BY b.bulletin_id LIMIT 999","enabled IDs and titles only");while(q.row())list.entries.push_back({{static_cast<std::uint16_t>(q.int64(0))},q.text(1),q.int64(2),static_cast<application::DoorBulletinHealth>(q.int64(3))});return list;});
}
application::StorageResult<std::vector<application::DoorBulletinNotice>> SqliteAccountRepository::door_bulletin_changes(const application::MessageActor& a,std::int64_t since){
    if(!application::valid_utc_seconds(since))return read_failure<std::vector<application::DoorBulletinNotice>>(invalid("Door Bulletin previous login"));
    return impl_->readers_.read<std::vector<application::DoorBulletinNotice>>("published Door Bulletin changes since previous login",[a,since](sqlite3* db){message_detail::ReadSnapshot snapshot(db);(void)message_detail::actor(db,a);std::vector<application::DoorBulletinNotice> result;
        SqliteStatement q(db,"SELECT b.bulletin_id,b.title FROM sfx_door_bulletins b JOIN sfx_door_bulletin_state s USING(bulletin_id) WHERE b.enabled=1 AND s.published_utc>?1 ORDER BY b.bulletin_id LIMIT 999","published changes only; never rescan source files at login");q.bind(1,since);while(q.row())result.push_back({{static_cast<std::uint16_t>(q.int64(0))},q.text(1)});return result;});
}
application::StorageResult<application::DoorBulletinSource> SqliteAccountRepository::door_bulletin_source(const application::MessageConfigurationActor& a,application::DoorBulletinId id){
    if(!application::valid_door_bulletin_id(id))return read_failure<application::DoorBulletinSource>(invalid("Door Bulletin identity"));
    return impl_->readers_.read<application::DoorBulletinSource>("capture current Door Bulletin source definition",[a,id](sqlite3* db){message_detail::ReadSnapshot snapshot(db);const auto e=door_bulletin_detail::entry(db,id);door_bulletin_detail::authority(db,a,e);return door_bulletin_detail::source(db,e);});
}
application::StorageResult<application::DoorBulletinPublicationResult> SqliteAccountRepository::publish_door_bulletin(const application::MessageConfigurationActor& a,const application::DoorBulletinPublication& r){
    if(!application::valid_door_bulletin_publication(r))return read_failure<application::DoorBulletinPublicationResult>(invalid("Door Bulletin captured publication"));
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::DoorBulletinPublicationResult>(invalid("Door Bulletin publication clock"));const auto fingerprint=message_detail::source_digest(r.fingerprint_input);
    // The writer needs only the digest, not another copy of every raw static
    // include. Keep queue accounting proportional to the captured payload.
    application::DoorBulletinPublication captured;captured.id=r.id;captured.definition_revision=r.definition_revision;captured.door_revision=r.door_revision;captured.variant=r.variant;captured.available=r.available;captured.captured_content=r.captured_content;
    return impl_->write_result<application::DoorBulletinPublicationResult>("publish validated Door Bulletin snapshot",captured.captured_content.size()+4096,[a,r=std::move(captured),now,fingerprint](sqlite3* db,auto& out){
        const auto e=door_bulletin_detail::entry(db,r.id);door_bulletin_detail::authority(db,a,e);const auto source=door_bulletin_detail::source(db,e);
        if(source.definition.revision!=r.definition_revision||source.door_revision!=r.door_revision)return SQLITE_ABORT;
        bool changed{};auto published=e.published_utc;
        if(r.available){
            if(source.definition.source==application::DoorBulletinSourceKind::display&&(!r.captured_content.empty()||r.variant.rfind("DB",0)!=0))return SQLITE_CONSTRAINT;
            if(source.definition.source==application::DoorBulletinSourceKind::door_file&&(!source.door_available||r.variant!=(source.definition.format==application::GraphicsMode::ansi?"GENERATED-ANS":"GENERATED-ASC")))return SQLITE_ABORT;
            SqliteStatement previous(db,"SELECT content_sha256 FROM sfx_door_bulletin_fingerprints WHERE bulletin_id=?1 AND variant=?2","prior matching source variant");previous.bind(1,r.id.value);previous.bind(2,r.variant);
            const bool known=previous.row();if(known){const auto bytes=previous.blob(0);if(bytes.size()!=32)file_detail::corrupt();changed=!std::equal(bytes.begin(),bytes.end(),fingerprint.begin());}
            // The first ever publication is new. A previously unseen graphics
            // or exact-security variant establishes only its own baseline.
            if(!published||changed)published=now;
            SqliteStatement q(db,"INSERT INTO sfx_door_bulletin_fingerprints VALUES(?1,?2,?3) ON CONFLICT(bulletin_id,variant) DO UPDATE SET content_sha256=excluded.content_sha256","retain bounded current variant fingerprint");q.bind(1,r.id.value);q.bind(2,r.variant);q.bind(3,fingerprint);q.step();
        }
        SqliteStatement q(db,"UPDATE sfx_door_bulletin_state SET health=?2,published_utc=?3,snapshot=?4 WHERE bulletin_id=?1","publish health/raw snapshot without changing config revision");q.bind(1,r.id.value);q.bind(2,static_cast<std::int64_t>(r.available?1:2));if(published)q.bind(3,*published);else q.bind_null(3);q.bind(4,r.available?r.captured_content:std::vector<std::uint8_t>{});q.step();
        out=application::DoorBulletinPublicationResult{r.available?application::DoorBulletinHealth::available:application::DoorBulletinHealth::unavailable,published,changed};return SQLITE_OK;
    });
}
