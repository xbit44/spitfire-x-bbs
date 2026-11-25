namespace ftn_setup_queue_detail {void create(sqlite3*,const application::FtnTransferToken&,const application::FtnConfiguration&,std::int64_t);}
namespace ftn_retention_detail {
void apply(sqlite3*,const application::FtnTransferToken&,const application::FtnConfiguration&,const application::FtnConfiguration&,std::int64_t);
void record_processed(sqlite3*,const application::Identifier128&,std::int64_t);
}
namespace ftn_detail {
application::FtnConfiguration stored_config(sqlite3* db){
    SqliteStatement q(db,"SELECT revision,definition FROM sfx_ftn_configuration WHERE singleton_id=1","read FTN configuration");
    if(!q.row())file_detail::corrupt();
    const auto bytes=q.blob(1);
    auto c=bytes.empty()?std::optional<application::FtnConfiguration>(application::FtnConfiguration{}):application::decode_ftn_configuration(bytes);
    if(!c||c->revision!=q.int64(0))file_detail::corrupt();
    return std::move(*c);
}
application::FtnConfiguration config(sqlite3* db){
    auto c=stored_config(db);
    c.message_catalog_revision=message_detail::catalog_revision(db);c.file_catalog_revision=file_detail::policy(db).catalog_revision;
    for(auto& e:c.message_echoes)if(e.local_echo){SqliteStatement target(db,"SELECT e.name,e.access_minimum,e.reading_minimum,e.posting_minimum,e.renegade_pipe_colors,r.maximum_count,r.maximum_age_days,r.echo_id FROM sfx_local_echoes e LEFT JOIN sfx_echo_retention r ON r.echo_id=e.echo_id WHERE e.echo_id=?1","read mapped FTN echo presentation and retention");target.bind(1,e.local_echo);if(target.row()){if(target.is_null(7))file_detail::corrupt();e.local_name=target.text(0);e.access_level=static_cast<std::uint16_t>(target.int64(1));e.read_level=static_cast<std::uint16_t>(target.int64(2));e.post_level=static_cast<std::uint16_t>(target.int64(3));e.pipe_colors=target.int64(4)!=0;e.maximum_messages=message_detail::retention_value(target,5,application::kMaximumRetainedMessageCount);e.maximum_age_days=message_detail::retention_value(target,6,application::kMaximumMessageAgeDays);}}
    return c;
}
std::array<std::uint8_t,32> fingerprint(const std::vector<std::uint8_t>& bytes){
    std::array<std::uint8_t,32> hash{};
    if(sodium_init()<0||crypto_hash_sha256(hash.data(),bytes.data(),bytes.size())!=0)throw StorageFailure(application::StorageErrorCode::internal,"fingerprint FTN configuration",SQLITE_ERROR);
    return hash;
}
bool same(std::string a,std::string b){for(auto* s:{&a,&b})for(auto& ch:*s)if(ch>='A'&&ch<='Z')ch=static_cast<char>(ch-'A'+'a');return a==b;}
bool matches_request_hash(const std::vector<std::uint8_t>& stored,const std::array<std::uint8_t,32>& current,const application::FtnConfiguration& c){
    if(stored.size()!=current.size())return false;
    const auto matches=[&](const auto& hash){return sodium_memcmp(stored.data(),hash.data(),hash.size())==0;};if(matches(current))return true;
    // Preserve exact older operation receipts. New setup/retention commands
    // are never represented as a different historical request.
    for(std::uint8_t version=6;version>=2;--version){const auto old=application::encode_ftn_configuration_legacy(c,version);if(!old.empty()&&matches(fingerprint(old)))return true;}
    return false;
}
application::FtnNetwork& network(application::FtnConfiguration& c,const std::string& code){
    const auto it=std::find_if(c.networks.begin(),c.networks.end(),[&](const auto& n){return same(n.code,code);});
    if(it==c.networks.end())file_detail::conflict();
    return *it;
}
bool local_name(std::string_view s){if(s.empty()||s.size()>64||s.front()==' '||s.back()==' ')return false;for(unsigned char ch:s)if(ch<32||ch>126)return false;return true;}
}
application::StorageResult<application::FtnConfiguration> SqliteAccountRepository::ftn_configuration(const application::MessageConfigurationActor& a){
    return impl_->readers_.read<application::FtnConfiguration>("read authorized FTN configuration",[a](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);return ftn_detail::config(db);});
}
application::StorageResult<std::int64_t> SqliteAccountRepository::ftn_configuration_receipt(const application::MessageConfigurationActor& a,const application::FtnTransferToken& token){
    return impl_->readers_.read<std::int64_t>("reconcile authorized FTN operation",[a,token](sqlite3* db){message_detail::ReadSnapshot snapshot(db);message_detail::config_authority(db,a);SqliteStatement receipt(db,"SELECT result_revision FROM sfx_ftn_configuration_receipts WHERE operation_id=?1","read exact FTN operation receipt");receipt.bind(1,token);return receipt.row()?receipt.int64(0):std::int64_t{};});
}
application::StorageResult<application::FtnConfigurationChangeResult> SqliteAccountRepository::change_ftn_configuration(
    const application::MessageConfigurationActor& a,const application::FtnTransferToken& token,const application::FtnConfiguration& c){
    using Result=application::FtnConfigurationChangeResult;
    if(!std::any_of(token.begin(),token.end(),[](auto v){return v!=0;}))return read_failure<Result>(invalid("validate FTN operation identity"));
    const auto bytes=application::encode_ftn_configuration(c);
    if(bytes.empty())return read_failure<Result>(invalid("validate FTN configuration"));
    const auto hash=ftn_detail::fingerprint(bytes);
    // Account for both encoded/typed copies and prospective mapping work. The
    // existing writer rejects over-budget requests; its global limit is intact.
    const auto budget=bytes.size()*2+65536+c.networks.size()*512+c.message_echoes.size()*512+c.file_echoes.size()*128+c.permissions.size()*128;
    const auto now=impl_->message_utc_now_();
    return impl_->write_result<Result>("change authorized FTN configuration",budget+application::kFtnRetentionReviewMaximum*256,[a,token,c,hash,now](sqlite3* db,auto& out){
        message_detail::config_authority(db,a);
        {SqliteStatement receipt(db,"SELECT request_fingerprint,result_revision FROM sfx_ftn_configuration_receipts WHERE operation_id=?1","reconcile FTN configuration operation");receipt.bind(1,token);
            if(receipt.row()){const auto old_hash=receipt.blob(0);if(!ftn_detail::matches_request_hash(old_hash,hash,c))file_detail::conflict();out=Result{ftn_detail::config(db),receipt.int64(1),true};return SQLITE_OK;}}
        const auto previous=ftn_detail::config(db);
        if(previous.revision!=c.revision||previous.message_catalog_revision!=c.message_catalog_revision||previous.file_catalog_revision!=c.file_catalog_revision)file_detail::conflict();
        ftn_retention_detail::apply(db,token,previous,c,now);
        auto next=c;bool changed_messages=false;
        for(auto& network:next.networks){
            if(network.message_group){SqliteStatement group(db,"SELECT 1 FROM sfx_message_groups WHERE group_id=?1","validate FTN message group");group.bind(1,network.message_group);if(!group.row())file_detail::conflict();}
            else if(!network.new_group_name.empty()){
                if(!ftn_detail::local_name(network.new_group_name))return SQLITE_CONSTRAINT;
                SqliteStatement collision(db,"SELECT 1 FROM sfx_message_groups WHERE name=?1 COLLATE NOCASE","validate reviewed setup group name");collision.bind(1,network.new_group_name);if(collision.row())file_detail::conflict();
                SqliteStatement group(db,"INSERT INTO sfx_message_groups(name,display_order,access_minimum,enabled,revision) SELECT ?1,coalesce(max(display_order)+1,0),10,1,1 FROM sfx_message_groups RETURNING group_id","create setup group in final FTN transaction");group.bind(1,network.new_group_name);if(!group.row())file_detail::conflict();network.message_group=group.int64(0);group.step();changed_messages=true;
            }
        }
        std::set<std::int64_t> mapped;
        for(auto& echo:next.message_echoes){
            if(!echo.store_locally)continue;
            auto& network=ftn_detail::network(next,echo.network);
            const bool creating=!echo.local_echo;
            if(!echo.local_echo){
                if(!network.message_group){
                    const auto name=network.new_group_name.empty()?network.name:network.new_group_name;
                    if(!ftn_detail::local_name(name))return SQLITE_CONSTRAINT;
                    SqliteStatement collision(db,"SELECT 1 FROM sfx_message_groups WHERE name=?1 COLLATE NOCASE","check FTN new group name collision");collision.bind(1,name);if(collision.row())file_detail::conflict();
                    SqliteStatement group(db,"INSERT INTO sfx_message_groups(name,display_order,access_minimum,enabled,revision) SELECT ?1,coalesce(max(display_order)+1,0),10,1,1 FROM sfx_message_groups RETURNING group_id","create reviewed FTN message group");group.bind(1,name);if(!group.row())file_detail::conflict();network.message_group=group.int64(0);group.step();changed_messages=true;
                }
                const auto name=echo.local_name.empty()?echo.tag:echo.local_name;
                if(!ftn_detail::local_name(name))return SQLITE_CONSTRAINT;
                SqliteStatement collision(db,"SELECT 1 FROM sfx_local_echoes WHERE group_id=?1 AND name=?2 COLLATE NOCASE","check FTN conference name collision");collision.bind(1,network.message_group);collision.bind(2,name);if(collision.row())file_detail::conflict();
                SqliteStatement available(db,"WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<999) SELECT x FROM n WHERE NOT EXISTS(SELECT 1 FROM sfx_local_echoes WHERE conference_number=x) ORDER BY x LIMIT 1","allocate FTN conference within existing capacity");if(!available.row())file_detail::conflict();const auto number=available.int64(0);
                SqliteStatement create(db,"INSERT INTO sfx_local_echoes(group_id,name,display_order,access_minimum,reading_minimum,posting_minimum,enabled,conference_number,revision,next_message_number,renegade_pipe_colors) SELECT ?1,?2,coalesce(max(display_order)+1,0),?3,?4,?5,1,?6,1,1,?7 FROM sfx_local_echoes WHERE group_id=?1 RETURNING echo_id","create selected FTN local conference");
                create.bind(1,network.message_group);create.bind(2,name);create.bind(3,echo.access_level);create.bind(4,echo.read_level);create.bind(5,echo.post_level);create.bind(6,number);create.bind(7,echo.pipe_colors);if(!create.row())file_detail::conflict();echo.local_echo=create.int64(0);create.step();changed_messages=true;
            }else{
                SqliteStatement target(db,"SELECT group_id,name FROM sfx_local_echoes WHERE echo_id=?1","validate existing FTN local conference");target.bind(1,echo.local_echo);if(!target.row()||(network.message_group&&network.message_group!=target.int64(0)))file_detail::conflict();
                if(echo.update_local_name){
                    if(!ftn_detail::local_name(echo.local_name))return SQLITE_CONSTRAINT;
                    SqliteStatement collision(db,"SELECT 1 FROM sfx_local_echoes WHERE group_id=?1 AND name=?2 COLLATE NOCASE AND echo_id<>?3","check reviewed FTN rename collision");collision.bind(1,target.int64(0));collision.bind(2,echo.local_name);collision.bind(3,echo.local_echo);if(collision.row())file_detail::conflict();
                    SqliteStatement update(db,"UPDATE sfx_local_echoes SET name=?1,revision=revision+1 WHERE echo_id=?2","apply only reviewed FTN conference rename");update.bind(1,echo.local_name);update.bind(2,echo.local_echo);update.step();changed_messages=true;
                }
                if(echo.update_local_permissions){SqliteStatement update(db,"UPDATE sfx_local_echoes SET access_minimum=?1,reading_minimum=?2,posting_minimum=?3,renegade_pipe_colors=?4,revision=revision+1 WHERE echo_id=?5","apply reviewed FTN conference permissions");update.bind(1,echo.access_level);update.bind(2,echo.read_level);update.bind(3,echo.post_level);update.bind(4,echo.pipe_colors);update.bind(5,echo.local_echo);update.step();changed_messages=true;}
            }
            if(!mapped.insert(echo.local_echo).second)return SQLITE_CONSTRAINT;
            if(creating||echo.update_local_retention){
                message_detail::save_echo_retention(db,{echo.local_echo},{echo.maximum_messages,echo.maximum_age_days},creating?3:echo.update_local_retention);
                if(!creating&&!echo.update_local_name&&!echo.update_local_permissions){SqliteStatement revision(db,"UPDATE sfx_local_echoes SET revision=revision+1 WHERE echo_id=?1","advance explicitly edited FTN retention policy");revision.bind(1,echo.local_echo);revision.step();}
                changed_messages=true;
            }
            echo.update_local_name=false;
            echo.update_local_permissions=false;
            echo.update_local_retention=0;
        }
        for(auto& network:next.networks)if(network.message_group)network.new_group_name.clear();
        for(const auto& echo:next.file_echoes){SqliteStatement area(db,"SELECT 1 FROM sfx_file_areas WHERE area_id=?1","validate FTN file area");area.bind(1,echo.file_area);if(!area.row())file_detail::conflict();}
        if(changed_messages){SqliteStatement advance(db,"UPDATE sfx_message_configuration_state SET revision=revision+1 WHERE singleton_id=1","publish atomic FTN conference import");advance.step();}
        ++next.revision;next.message_catalog_revision=message_detail::catalog_revision(db);next.file_catalog_revision=file_detail::policy(db).catalog_revision;
        if(next.setup_subscriptions){ftn_setup_queue_detail::create(db,token,next,now);next.setup_subscriptions.reset();}
        const auto definition=application::encode_ftn_configuration(next);if(definition.empty())return SQLITE_CONSTRAINT;
        SqliteStatement save(db,"UPDATE sfx_ftn_configuration SET revision=?1,definition=?2 WHERE singleton_id=1","publish FTN configuration in mapping transaction");save.bind(1,next.revision);save.bind(2,definition);save.step();
        SqliteStatement receipt(db,"INSERT INTO sfx_ftn_configuration_receipts VALUES(?1,?2,?3)","record idempotent FTN configuration operation");receipt.bind(1,token);receipt.bind(2,hash);receipt.bind(3,next.revision);receipt.step();out=Result{std::move(next),c.revision+1,false};return SQLITE_OK;
    });
}
