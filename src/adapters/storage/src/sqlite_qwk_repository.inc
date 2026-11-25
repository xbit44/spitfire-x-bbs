namespace qwk_detail {
namespace app=application;
std::optional<std::uint16_t> netmail_conference(sqlite3* db){
    SqliteStatement version(db,"PRAGMA user_version","check native FTN capability");if(!version.row()||version.int64(0)<22)return {};
    // Use a currently unused number, including retired public mappings. No
    // permanent public number is changed or overloaded. A stale private REP
    // after a catalog change is rejected, never converted into a public post.
    SqliteStatement q(db,"SELECT conference FROM sfx_qwk_conference_mapping ORDER BY conference DESC","choose separate QWKE NetMail conference");
    std::uint32_t available=65535;while(q.row()){if(q.int64(0)==available)--available;else if(q.int64(0)<available)break;}
    if(!available)throw StorageFailure(StorageErrorCode::backpressure,"No QWK NetMail conference available",SQLITE_FULL);
    return static_cast<std::uint16_t>(available);
}
app::QwkPolicy policy(sqlite3* db){
    SqliteStatement q(db,"SELECT board_id,maximum_messages,revision,word_wrap_default FROM sfx_qwk_policy WHERE singleton_id=1","read durable QWK policy");
    if(!q.row())message_detail::unavailable();
    app::QwkPolicy result;
    const auto count=q.int64(1);if(count<1 || count>65535)throw StorageFailure(StorageErrorCode::corrupt,"invalid durable packet count",SQLITE_CORRUPT);
    result.board_id=q.text(0);result.maximum_messages=static_cast<std::uint32_t>(count);result.revision=q.int64(2);
    if(!q.is_null(3))result.word_wrap_default=q.boolean(3);
    if(!app::valid_qwk_board_id(result.board_id) || app::canonical_qwk_board_id(result.board_id)!=result.board_id || !app::valid_qwk_message_count(result.maximum_messages) || result.revision<1)
        throw StorageFailure(StorageErrorCode::corrupt,"invalid durable QWK policy",SQLITE_CORRUPT);
    return result;
}
std::optional<app::QwkSavedPreferences> saved_preferences(sqlite3* db,app::AccountId account){
    SqliteStatement q(db,"SELECT private_mode,own_public_posts,encoding,preserve_colors,word_wrap,indexes,full_headers,revision FROM sfx_qwk_caller_preferences WHERE account_id=?1","read stable caller packet preferences");
    q.bind(1,account);if(!q.row())return {};app::QwkSavedPreferences result;auto& p=result.preferences;
    const auto mail=q.int64(0),encoding=q.int64(2);
    if(mail<0 || mail>2 || encoding<0 || encoding>1)throw StorageFailure(StorageErrorCode::corrupt,"invalid durable packet enumeration",SQLITE_CORRUPT);
    p.private_mail=static_cast<app::QwkPrivateMode>(mail);p.own_public_posts=q.boolean(1);p.encoding=static_cast<app::QwkEncoding>(encoding);
    p.preserve_colors=q.boolean(3);p.word_wrap=q.boolean(4);p.indexes=q.boolean(5);p.full_headers=q.boolean(6);result.revision=q.int64(7);
    if(!app::valid_qwk_preferences(p) || result.revision<1)throw StorageFailure(StorageErrorCode::corrupt,"invalid caller packet preferences",SQLITE_CORRUPT);
    return result;
}
void insert_preferences(sqlite3* db,app::AccountId account,const app::QwkPreferences& p,std::int64_t revision){
    SqliteStatement q(db,"INSERT INTO sfx_qwk_caller_preferences VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)","save only missing caller packet preferences");
    q.bind(1,account);q.bind(2,static_cast<std::int64_t>(p.private_mail));q.bind(3,p.own_public_posts);q.bind(4,static_cast<std::int64_t>(p.encoding));
    q.bind(5,p.preserve_colors);q.bind(6,p.word_wrap.value());q.bind(7,p.indexes);q.bind(8,p.full_headers);q.bind(9,revision);q.step();
}
void clear_continuation(sqlite3* db,app::AccountId account){
    SqliteStatement q(db,"DELETE FROM sfx_qwk_continuation WHERE account_id=?1","invalidate only this caller finite packet continuation");q.bind(1,account);q.step();
}
std::array<std::uint8_t,32> policy_fingerprint(const app::MessageConfigurationActor& a,std::int64_t revision,std::string_view board,std::uint32_t count){
    const auto bytes=std::string("SFX/QWKPolicy/v1;")+std::to_string(a.installation_owner)+';'+std::to_string(a.caller?a.caller->account:0)+';'+std::to_string(revision)+';'+std::to_string(board.size())+':'+std::string(board)+';'+std::to_string(count);
    return message_detail::source_digest(bytes);
}
} // namespace qwk_detail
application::StorageResult<application::QwkPolicy> SqliteAccountRepository::qwk_policy(const application::MessageConfigurationActor& a){
    return impl_->readers_.read<application::QwkPolicy>("read protected QWK policy",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return qwk_detail::policy(db);});
}
application::StorageResult<application::QwkPolicy> SqliteAccountRepository::save_qwk_policy(const application::MessageConfigurationActor& a,
    application::Identifier128 operation,std::int64_t revision,std::string_view desired_board,std::uint32_t count){
    if(!operation.valid() || revision<1 || !application::valid_qwk_board_id(desired_board) || !application::valid_qwk_message_count(count))return read_failure<application::QwkPolicy>(invalid("validate QWK policy save"));
    const auto board=application::canonical_qwk_board_id(desired_board);const auto fingerprint=qwk_detail::policy_fingerprint(a,revision,board,count);
    const auto now=impl_->message_utc_now_();if(!application::valid_utc_seconds(now))return read_failure<application::QwkPolicy>(invalid("validate QWK policy clock"));
    return impl_->write_result<application::QwkPolicy>("save authorized QWK policy",4096,[a,operation,revision,board,count,fingerprint,now](sqlite3* db,auto& output){
        message_detail::config_authority(db,a);
        SqliteStatement receipt(db,"SELECT request_fingerprint,resulting_revision,installation_owner,actor_account_id FROM sfx_qwk_policy_receipts WHERE operation_id=?1","reconcile QWK policy save");receipt.bind(1,operation);
        if(receipt.row()){
            const auto stored=receipt.blob(0);if(stored.size()!=32 || sodium_memcmp(stored.data(),fingerprint.data(),32)!=0 || receipt.boolean(2)!=a.installation_owner ||
                (a.caller?receipt.is_null(3) || receipt.int64(3)!=a.caller->account:!receipt.is_null(3)))return SQLITE_ABORT;
            auto result=qwk_detail::policy(db);result.board_id=board;result.maximum_messages=count;result.revision=receipt.int64(1);result.replayed=true;output=std::move(result);return SQLITE_OK;
        }
        SqliteStatement change(db,"UPDATE sfx_qwk_policy SET board_id=?1,maximum_messages=?2,revision=revision+1 WHERE singleton_id=1 AND revision=?3 AND revision<9223372036854775807","CAS stable board/count policy");
        change.bind(1,board);change.bind(2,static_cast<std::int64_t>(count));change.bind(3,revision);change.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        auto result=qwk_detail::policy(db);
        SqliteStatement save(db,"INSERT INTO sfx_qwk_policy_receipts VALUES(?1,?2,?3,?4,?5,?6)","commit content-free QWK policy receipt");
        save.bind(1,operation);if(a.caller)save.bind(2,a.caller->account);else save.bind_null(2);save.bind(3,a.installation_owner);save.bind(4,fingerprint);save.bind(5,result.revision);save.bind(6,now);save.step();
        output=std::move(result);return SQLITE_OK;
    });
}
application::StorageResult<application::QwkSavedPreferences> SqliteAccountRepository::qwk_preferences(const application::MessageActor& a){
    return impl_->write_result<application::QwkSavedPreferences>("read/seed only missing stable caller packet preferences",1024,[a](sqlite3* db,auto& output){
        message_detail::actor(db,a);if(const auto saved=qwk_detail::saved_preferences(db,a.account)){output=*saved;return SQLITE_OK;}
        application::QwkSavedPreferences result;result.preferences.word_wrap=qwk_detail::policy(db).word_wrap_default;
        // Only development candidates have no owner-selected default. Returning
        // an unready value cannot silently seed a provisional caller choice.
        if(result.preferences.word_wrap.has_value())qwk_detail::insert_preferences(db,a.account,result.preferences,1);
        output=std::move(result);return SQLITE_OK;
    });
}
application::StorageResult<application::QwkSavedPreferences> SqliteAccountRepository::save_qwk_preferences(const application::MessageActor& a,std::int64_t revision,const application::QwkPreferences& p){
    if(revision<1 || !application::valid_qwk_preferences(p))return read_failure<application::QwkSavedPreferences>(invalid("validate explicit caller packet preferences"));
    return impl_->write_result<application::QwkSavedPreferences>("save own caller packet preferences",1024,[a,revision,p](sqlite3* db,auto& output){
        message_detail::actor(db,a);const auto prior=qwk_detail::saved_preferences(db,a.account);
        if(!prior){if(revision!=1)return SQLITE_ABORT;qwk_detail::insert_preferences(db,a.account,p,2);}
        else{
            SqliteStatement q(db,"UPDATE sfx_qwk_caller_preferences SET private_mode=?1,own_public_posts=?2,encoding=?3,preserve_colors=?4,word_wrap=?5,indexes=?6,full_headers=?7,revision=revision+1 WHERE account_id=?8 AND revision=?9 AND revision<9223372036854775807","CAS only own packet preferences");
            q.bind(1,static_cast<std::int64_t>(p.private_mail));q.bind(2,p.own_public_posts);q.bind(3,static_cast<std::int64_t>(p.encoding));q.bind(4,p.preserve_colors);q.bind(5,p.word_wrap.value());q.bind(6,p.indexes);q.bind(7,p.full_headers);q.bind(8,a.account);q.bind(9,revision);q.step();if(sqlite3_changes(db)!=1)return SQLITE_ABORT;
        }
        qwk_detail::clear_continuation(db,a.account);output=*qwk_detail::saved_preferences(db,a.account);return SQLITE_OK;
    });
}
namespace qwk_detail {
struct ReferenceMaterial {std::vector<std::uint8_t> operation;std::optional<std::uint32_t> reference;};
ReferenceMaterial reference_material(sqlite3* db,const application::MessageActor& a,application::MessageId message){
    const auto authority=message_detail::actor(db,a);const auto m=message_detail::load(db,a,authority,message,false);
    if(m.header.target.kind==application::MessageKind::private_ftn || (m.header.target.kind==application::MessageKind::private_local && (m.header.target.recipient!=a.account || m.header.inbox_removed)))throw StorageFailure(StorageErrorCode::not_found,"QWK reference target unavailable",SQLITE_NOTFOUND);
    SqliteStatement receipt(db,"SELECT operation_id FROM sfx_message_commit_receipts WHERE message_id=?1 UNION ALL SELECT r.import_id FROM sfx_ftn_import_receipts r JOIN sfx_ftn_message_metadata m ON m.import_id=r.import_id AND m.message_id=r.original_message_id WHERE r.original_message_id=?1","bind immutable local posting or trusted FTN import creation identity");receipt.bind(1,message.value);
    if(!receipt.row())throw StorageFailure(StorageErrorCode::not_found,"QWK creation identity unavailable",SQLITE_NOTFOUND);
    ReferenceMaterial result;result.operation=receipt.blob(0);if(receipt.row())throw StorageFailure(StorageErrorCode::corrupt,"QWK creation identity collision",SQLITE_CORRUPT);
    SqliteStatement previous(db,"SELECT reference FROM sfx_qwk_message_reference WHERE message_id=?1 AND creation_operation=?2","reuse same stable QWK reference");previous.bind(1,message.value);previous.bind(2,result.operation);
    if(previous.row()){const auto ref=previous.int64(0);if(ref<1||ref>application::kQwkMaximumReference)throw StorageFailure(StorageErrorCode::corrupt,"invalid QWK reference",SQLITE_CORRUPT);result.reference=static_cast<std::uint32_t>(ref);}
    return result;
}
std::uint32_t allocate_reference(sqlite3* db,application::MessageId message,const ReferenceMaterial& material){
    if(material.reference)return *material.reference;
    SqliteStatement number(db,"SELECT coalesce(max(reference),0)+1 FROM sfx_qwk_message_reference","allocate never-reused classic reference");
    if(!number.row()||number.int64(0)>application::kQwkMaximumReference)throw StorageFailure(StorageErrorCode::full,"QWK reference space exhausted",SQLITE_FULL);
    const auto ref=number.int64(0);SqliteStatement save(db,"INSERT INTO sfx_qwk_message_reference VALUES(?1,?2,?3)","retain immutable QWK reference without private body");save.bind(1,ref);save.bind(2,message.value);save.bind(3,material.operation);save.step();return static_cast<std::uint32_t>(ref);
}
}
application::StorageResult<std::uint32_t> SqliteAccountRepository::qwk_reference(const application::MessageActor& a,application::MessageId message){
    if(message.value<1)return read_failure<std::uint32_t>(invalid("validate QWK reference target"));
    const auto existing=impl_->readers_.read<std::optional<std::uint32_t>>("read currently authorized QWK reference",[a,message](sqlite3* db){message_detail::ReadSnapshot snapshot(db);return qwk_detail::reference_material(db,a,message).reference;});
    if(!existing||!existing.value)return {existing.status,{}};
    if(*existing.value)return {application::StorageStatus::success(),**existing.value};
    // Only a genuinely absent reference enters the writer. Recheck authority
    // and creation identity there, including a concurrent allocation race.
    return impl_->write_result<std::uint32_t>("allocate authorized stable content-free QWK reference",1024,[a,message](sqlite3* db,auto& output){output=qwk_detail::allocate_reference(db,message,qwk_detail::reference_material(db,a,message));return SQLITE_OK;});
}
application::StorageResult<std::vector<std::uint32_t>> SqliteAccountRepository::qwk_references(const application::MessageActor& a,const std::vector<application::MessageId>& ids){
    if(ids.empty()||ids.size()>application::kQwkReferenceBatchMaximum||std::any_of(ids.begin(),ids.end(),[](const auto id){return id.value<1;}))return read_failure<std::vector<std::uint32_t>>(invalid("validate bounded QWK references"));
    const auto existing=impl_->readers_.read<std::optional<std::vector<std::uint32_t>>>("read currently authorized QWK references",[a,ids](sqlite3* db)->std::optional<std::vector<std::uint32_t>>{
        message_detail::ReadSnapshot snapshot(db);std::vector<std::uint32_t> refs;refs.reserve(ids.size());bool complete=true;
        for(const auto id:ids){const auto value=qwk_detail::reference_material(db,a,id);if(!value.reference)complete=false;refs.push_back(value.reference.value_or(0));}
        if(!complete)return {};
        return refs;
    });
    if(!existing||!existing.value)return {existing.status,{}};
    if(*existing.value)return {application::StorageStatus::success(),std::move(**existing.value)};
    return impl_->write_result<std::vector<std::uint32_t>>("allocate bounded authorized QWK references",1024+ids.size()*1024,[a,ids](sqlite3* db,auto& output){
        std::vector<std::uint32_t> refs;refs.reserve(ids.size());
        for(const auto id:ids)refs.push_back(qwk_detail::allocate_reference(db,id,qwk_detail::reference_material(db,a,id)));
        output=std::move(refs);return SQLITE_OK;
    });
}
