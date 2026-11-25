namespace area_delete_detail {
namespace app=application;
struct Digest {
    crypto_hash_sha256_state state{};
    Digest(){if(sodium_init()<0 || crypto_hash_sha256_init(&state)!=0)throw StorageFailure(StorageErrorCode::internal,"area deletion digest",SQLITE_ERROR);text("SFX/PublicAreaDeletion/v1");}
    void raw(const void* p,std::size_t n){if(crypto_hash_sha256_update(&state,static_cast<const unsigned char*>(p),n)!=0)throw StorageFailure(StorageErrorCode::internal,"area deletion digest",SQLITE_ERROR);}
    void number(std::int64_t n){std::array<unsigned char,8> bytes{};for(unsigned i=0;i<8;++i)bytes[i]=static_cast<unsigned char>(static_cast<std::uint64_t>(n)>>(56-i*8));raw(bytes.data(),bytes.size());}
    void text(std::string_view s){number(static_cast<std::int64_t>(s.size()));raw(s.data(),s.size());}
    void blob(const std::vector<std::uint8_t>& bytes){number(static_cast<std::int64_t>(bytes.size()));if(!bytes.empty())raw(bytes.data(),bytes.size());}
    std::array<std::uint8_t,32> finish(){std::array<std::uint8_t,32> hash{};if(crypto_hash_sha256_final(&state,hash.data())!=0)throw StorageFailure(StorageErrorCode::internal,"area deletion digest",SQLITE_ERROR);return hash;}
};
bool target_valid(const app::MessageDeletionTarget& t){return t.kind<=app::MessageDeletionKind::group && t.id>0 && t.expected_revision>0;}
bool owner(const app::MessageConfigurationActor& actor){return actor.installation_owner && !actor.caller;}
bool display_name_valid(const app::MessageDeletionPreview& preview){
    app::MessageConfigurationChange label;label.operation.bytes[0]=1;label.action=app::MessageConfigurationAction::group;
    label.expected_configuration_revision=preview.configuration_revision;label.group.id={preview.target.id};label.group.revision=preview.target.expected_revision;label.group.name=preview.name;
    return app::valid_message_configuration_change(label);
}
app::MessageDeletionPreview preview(sqlite3* db,const app::MessageDeletionTarget& target,const std::function<bool()>& cancelled){
    maintenance_detail::SqlBudget budget(db,cancelled,300000);budget.poll();
    app::MessageDeletionPreview result;result.target=target;result.configuration_revision=message_detail::catalog_revision(db);
    SqliteStatement selected(db,target.kind==app::MessageDeletionKind::echo?
        "SELECT name,revision FROM sfx_local_echoes WHERE echo_id=?1":"SELECT name,revision FROM sfx_message_groups WHERE group_id=?1","bind exact stable deletion target");
    selected.bind(1,target.id);if(!selected.row())message_detail::unavailable();if(selected.int64(1)!=target.expected_revision)message_detail::conflict();result.name=selected.text(0);
    if(!display_name_valid(result))throw StorageFailure(StorageErrorCode::corrupt,"exact deletion target name cannot be safely displayed",SQLITE_CORRUPT);
    Digest hash;hash.number(static_cast<std::int64_t>(target.kind));hash.number(target.id);hash.number(target.expected_revision);hash.number(result.configuration_revision);hash.text(result.name);
    SqliteStatement echoes(db,"SELECT e.echo_id,e.revision,e.group_id,e.conference_number,e.name,e.display_order,e.access_minimum,e.reading_minimum,e.posting_minimum,e.enabled,e.next_message_number,p.maximum_count,p.maximum_age_days FROM sfx_local_echoes e JOIN sfx_echo_retention p USING(echo_id) WHERE (?1=0 AND e.echo_id=?2) OR (?1=1 AND e.group_id=?2) ORDER BY e.echo_id LIMIT 1000","capture complete bounded echo membership/settings");
    echoes.bind(1,static_cast<std::int64_t>(target.kind));echoes.bind(2,target.id);
    while(echoes.row()){
        budget.poll();if(++result.echo_count>999)message_detail::conflict();const auto echo=echoes.int64(0);
        hash.number(echo);hash.number(echoes.int64(1));hash.number(echoes.int64(2));hash.number(echoes.int64(3));hash.text(echoes.text(4));
        for(int column=5;column<13;++column){hash.number(echoes.is_null(column)?0:1);if(!echoes.is_null(column))hash.number(echoes.int64(column));}
        SqliteStatement messages(db,"SELECT m.message_id,m.area_number,m.author_account_id,coalesce(m.recipient_account_id,0),m.subject,m.body,m.body_lines,m.arrival_ordinal,m.created_utc_seconds,coalesce(m.parent_message_id,0),m.encoding,coalesce(p.permanent,0),coalesce(p.revision,0) FROM sfx_local_messages m LEFT JOIN sfx_message_permanent p USING(message_id) WHERE m.kind=0 AND m.echo_id=?1 ORDER BY m.message_id","stream exact selected public content/flags, never private mail");messages.bind(1,echo);
        while(messages.row()){
            budget.poll();if(result.message_count==INT64_MAX)message_detail::conflict();++result.message_count;
            const auto message=messages.int64(0);if(messages.boolean(11))++result.permanent_count;
            for(int col=0;col<4;++col)hash.number(messages.int64(col));
            hash.blob(messages.blob(4));hash.blob(messages.blob(5));
            for(int col=6;col<10;++col)hash.number(messages.int64(col));
            hash.text(messages.text(10));hash.number(messages.int64(11));hash.number(messages.int64(12));
            SqliteStatement source(db,"SELECT format,profile_version,source,source_sha256 FROM sfx_message_sources WHERE message_id=?1","bind original source, not a hash-only content substitution");source.bind(1,message);
            const bool original=source.row();hash.number(original);if(original){hash.number(source.int64(0));hash.number(source.int64(1));hash.blob(source.blob(2));hash.blob(source.blob(3));}
            SqliteStatement recipient(db,"SELECT recipient_account_id,opened,inbox_removed,opened_sequence FROM sfx_message_recipient_state WHERE message_id=?1","bind selected public recipient state");recipient.bind(1,message);
            const bool addressed=recipient.row();hash.number(addressed);if(addressed)for(int col=0;col<4;++col)hash.number(recipient.int64(col));
            // Parent detachment is the only permitted surviving-child change.
            // Bind its exact stable set, including any anomalous cross-area link.
            SqliteStatement children(db,"SELECT message_id,parent_message_id FROM sfx_local_messages WHERE parent_message_id=?1 ORDER BY message_id","bind exact parent-detachment dependency set");children.bind(1,message);
            while(children.row()){budget.poll();hash.number(children.int64(0));hash.number(children.int64(1));}hash.number(0);
        }
        hash.number(0);
    }
    if(target.kind==app::MessageDeletionKind::echo && result.echo_count!=1)message_detail::conflict();
    hash.number(result.echo_count);hash.number(result.message_count);hash.number(result.permanent_count);result.fingerprint=hash.finish();return result;
}
bool same(const app::MessageDeletionPreview& a,const app::MessageDeletionPreview& b){
    return a.target.kind==b.target.kind && a.target.id==b.target.id && a.target.expected_revision==b.target.expected_revision &&
        a.configuration_revision==b.configuration_revision && a.name==b.name && a.echo_count==b.echo_count && a.message_count==b.message_count && a.permanent_count==b.permanent_count &&
        sodium_memcmp(a.fingerprint.data(),b.fingerprint.data(),32)==0;
}
std::array<std::uint8_t,32> request_hash(const app::MessageDeletionPreview& p){
    Digest hash;hash.text("confirmed-deletion-request");hash.number(static_cast<std::int64_t>(p.target.kind));hash.number(p.target.id);hash.number(p.target.expected_revision);
    hash.number(p.configuration_revision);hash.text(p.name);hash.number(p.echo_count);hash.number(p.message_count);hash.number(p.permanent_count);hash.raw(p.fingerprint.data(),32);return hash.finish();
}
void purge(sqlite3* db,std::int64_t echo){
    const auto apply=[&](const char* sql){SqliteStatement q(db,sql,"confirmed whole-echo deletion dependencies");q.bind(1,echo);q.step();};
    apply("INSERT INTO sfx_message_purge_scope VALUES(?1)");
    apply("INSERT INTO sfx_message_purged_receipts SELECT r.operation_id,r.actor_account_id,r.request_fingerprint,r.message_id FROM sfx_message_commit_receipts r JOIN sfx_local_messages m USING(message_id) WHERE m.echo_id=?1 AND m.kind=0");
    apply("UPDATE sfx_local_messages SET parent_message_id=NULL WHERE parent_message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
    apply("DELETE FROM sfx_message_arrival_intents WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
    apply("DELETE FROM sfx_message_recipient_state WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
    apply("DELETE FROM sfx_message_sources WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
    apply("DELETE FROM sfx_ftn_message_intents WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0) AND state=1");
    apply("DELETE FROM sfx_ftn_message_metadata WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
    apply("DELETE FROM sfx_ftn_message_targets WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
    apply("DELETE FROM sfx_message_commit_receipts WHERE message_id IN (SELECT message_id FROM sfx_local_messages WHERE echo_id=?1 AND kind=0)");
    apply("DELETE FROM sfx_local_messages WHERE echo_id=?1 AND kind=0");
    apply("DELETE FROM sfx_caller_echo_state WHERE echo_id=?1");apply("DELETE FROM sfx_message_purge_scope WHERE echo_id=?1");
    apply("DELETE FROM sfx_local_echoes WHERE echo_id=?1");if(sqlite3_changes(db)!=1)message_detail::conflict();
}
}
application::StorageResult<application::MessageDeletionPreview> SqliteAccountRepository::preview_message_deletion(
    const application::MessageConfigurationActor& actor,const application::MessageDeletionTarget& target,std::function<bool()> cancelled){
    using Preview=application::MessageDeletionPreview;
    if(!area_delete_detail::owner(actor))return read_failure<Preview>({StorageErrorCode::permission_denied,"offline installation owner deletion only"});
    if(!area_delete_detail::target_valid(target))return read_failure<Preview>(invalid("validate stable deletion target"));
    return impl_->maintenance_read<Preview>("offline exact public deletion preview",[actor,target,cancelled](sqlite3* db){
        message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,actor);return area_delete_detail::preview(db,target,cancelled);
    });
}
application::StorageResult<application::MessageDeletionResult> SqliteAccountRepository::commit_message_deletion(
    const application::MessageConfigurationActor& actor,application::Identifier128 operation,const application::MessageDeletionPreview& preview,std::function<bool()> cancelled){
    using Result=application::MessageDeletionResult;
    if(!area_delete_detail::owner(actor))return read_failure<Result>({StorageErrorCode::permission_denied,"offline installation owner deletion only"});
    if(!operation.valid() || !area_delete_detail::target_valid(preview.target) || preview.configuration_revision<1 || !area_delete_detail::display_name_valid(preview) || preview.echo_count<0 || preview.echo_count>999 || preview.message_count<0 || preview.permanent_count<0 || preview.permanent_count>preview.message_count)return read_failure<Result>(invalid("validate exact deletion confirmation"));
    const auto request_hash=area_delete_detail::request_hash(preview);const auto now=impl_->message_utc_now_?impl_->message_utc_now_():std::int64_t{};
    if(!application::valid_utc_seconds(now))return read_failure<Result>(invalid("deletion clock"));
    return impl_->write_result<Result>("commit exact stopped public area/group deletion",65536,[actor,operation,preview,request_hash,now,cancelled](sqlite3* db,std::optional<Result>& out){
        message_detail::config_authority(db,actor);
        {SqliteStatement receipt(db,"SELECT request_fingerprint,result_configuration_revision,echo_count,message_count,permanent_count FROM sfx_message_area_delete_receipts WHERE operation_id=?1","reconcile only exact prior confirmed deletion");receipt.bind(1,operation);
            if(receipt.row()){const auto digest=receipt.blob(0);if(digest.size()!=32 || sodium_memcmp(digest.data(),request_hash.data(),32)!=0)message_detail::conflict();out=Result{receipt.int64(1),receipt.int64(2),receipt.int64(3),receipt.int64(4),true};return SQLITE_OK;}}
        const auto current=area_delete_detail::preview(db,preview.target,cancelled);if(!area_delete_detail::same(current,preview))message_detail::conflict();
        maintenance_detail::SqlBudget budget(db,cancelled,300000);budget.poll();
        SqliteStatement children(db,"SELECT echo_id FROM sfx_local_echoes WHERE (?1=0 AND echo_id=?2) OR (?1=1 AND group_id=?2) ORDER BY echo_id LIMIT 1000","confirmed full echo membership");children.bind(1,static_cast<std::int64_t>(preview.target.kind));children.bind(2,preview.target.id);
        std::vector<std::int64_t> echoes;while(children.row())echoes.push_back(children.int64(0));if(static_cast<std::int64_t>(echoes.size())!=preview.echo_count)message_detail::conflict();
        for(const auto echo:echoes){budget.poll();area_delete_detail::purge(db,echo);}
        if(preview.target.kind==application::MessageDeletionKind::group){SqliteStatement q(db,"DELETE FROM sfx_message_groups WHERE group_id=?1 AND revision=?2","remove confirmed complete group only");q.bind(1,preview.target.id);q.bind(2,preview.target.expected_revision);q.step();if(sqlite3_changes(db)!=1)message_detail::conflict();}
        SqliteStatement revision(db,"UPDATE sfx_message_configuration_state SET revision=revision+1 WHERE singleton_id=1 AND revision=?1 AND revision<9223372036854775807 RETURNING revision","publish whole deletion catalog revision atomically");revision.bind(1,preview.configuration_revision);if(!revision.row())message_detail::conflict();const auto result_revision=revision.int64(0);revision.step();
        {SqliteStatement audit(db,"INSERT INTO sfx_message_configuration_audit VALUES(?1,?2,?3,NULL,1,?4,?5,0,?6)","content-free confirmed deletion audit");audit.bind(1,operation);audit.bind(2,request_hash);audit.bind(3,static_cast<std::int64_t>(preview.target.kind==application::MessageDeletionKind::echo?application::MessageConfigurationAction::delete_echo:application::MessageConfigurationAction::delete_group));audit.bind(4,result_revision);audit.bind(5,preview.target.id);audit.bind(6,now);audit.step();}
        {SqliteStatement receipt(db,"INSERT INTO sfx_message_area_delete_receipts VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)","immutable exact confirmed deletion receipt");receipt.bind(1,operation);receipt.bind(2,request_hash);receipt.bind(3,static_cast<std::int64_t>(preview.target.kind));receipt.bind(4,preview.target.id);receipt.bind(5,result_revision);receipt.bind(6,preview.echo_count);receipt.bind(7,preview.message_count);receipt.bind(8,preview.permanent_count);receipt.bind(9,now);receipt.step();}
        out=Result{result_revision,preview.echo_count,preview.message_count,preview.permanent_count,false};return SQLITE_OK;
    });
}
