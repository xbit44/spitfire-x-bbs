void test_live_account_editor_storage(const std::filesystem::path& root) {
    test::FakeStoragePathPolicy paths;
    auto ready=create_ready_storage(root/"live-editor",paths);
    auto& db=*ready.database;
    // Exercise the pre-existing last-usable-Sysop guard independently of the
    // separate Section 8A designation guard. An upgraded installation may
    // legitimately require an explicit, initially absent assignment.
    require(db.shutdown().succeeded(),"stop new independent last-Sysop fixture");
    {internal::SqliteConnection fixture(db.database_path(),SQLITE_OPEN_READWRITE,100ms,false);
        fixture.exec("DELETE FROM sfx_system_sysop; INSERT INTO sfx_system_sysop VALUES(1,NULL,1)","synthetic unconfigured designation");}
    require(db.start().succeeded() && !db.message_configuration({true,{}},{},{}).value->system_sysop,
        "unconfigured upgrade fixture remains explicit, never guessed");
    std::uint32_t sequence=5000;
    const auto context=[&]{++sequence;return audit(sequence,base_utc+sequence,app::AuditReasonCode::sysop_action,ready.sysop_id);};
    const auto caller=create_account(db,"Editor Caller",4000,base_utc+10);
    require(caller.succeeded(),"editor caller fixture created");
    const auto id=*caller.account_id;
    app::SaveAccountRequest save{id,1,1,"Renamed Editor",profile(),10,false,context()};
    save.profile.location="Saved together";
    const auto saved=db.save_account(save);
    require(saved && saved.revision==2,"one Save atomically changes name/profile/access/purge");
    auto record=*db.find_account_by_id(id).value;
    require(record.display_name==save.display_name && record.profile.location==save.profile.location &&
        record.purge_allowed==false && record.revision==2,"complete saved record matches");
    save.audit=context();save.profile.location="Stale draft";
    require(!db.save_account(save) && db.find_account_by_id(id).value->profile.location=="Saved together","stale complete Save preserves newer record");
    save.expected_revision=2;save.expected_policy_revision=2;save.audit=context();
    require(!db.save_account(save),"stale policy binding prevents Save");
    save.expected_policy_revision=1;save.display_name="Initial Sysop";save.audit=context();
    require(!db.save_account(save) && db.find_account_by_id(id).value->revision==2,"name conflict rolls back every field");
    save.display_name="Rollback Name";save.security_level=500;save.audit=context();
    require(!db.save_account(save) && db.find_account_by_id(id).value->display_name=="Renamed Editor","undefined level rolls back rename and profile");
    const auto reclaim=create_account(db,"Rollback Name",4001,base_utc+11);
    require(reclaim.succeeded(),"failed Save does not leak name reservation");
    auto sysop=*db.find_account_by_id(ready.sysop_id).value;
    require(db.change_account_state({sysop.account_id,sysop.revision,app::AccountState::locked,{},context()}).last_sysop_protected,"last Sysop deactivation returns precise reason");
    require(db.tombstone_account({sysop.account_id,sysop.revision,app::AuditReasonCode::sysop_action,context()}).last_sysop_protected,"last Sysop deletion returns precise reason");
    require(db.assign_security_level({sysop.account_id,sysop.revision,1,10,context()}).last_sysop_protected,"last Sysop assignment returns precise reason");
    app::SaveAccountRequest demote{sysop.account_id,sysop.revision,1,*sysop.display_name,sysop.profile,10,false,context()};
    require(db.save_account(demote).last_sysop_protected,"last Sysop complete Save returns precise reason");
    demote.expected_revision+=1; demote.audit=context();
    const auto stale_sysop=db.save_account(demote);
    require(!stale_sysop && !stale_sysop.last_sysop_protected,"stale revision is not misreported as last Sysop");
    require(db.find_account_by_id(sysop.account_id).value->revision==sysop.revision,"all last-Sysop refusals leave revision exact");
    save.display_name="Renamed Editor";save.security_level=999;save.audit=context();
    require(db.save_account(save).succeeded(),"second usable Sysop can be created through Save");
    require(db.change_account_state({sysop.account_id,sysop.revision,app::AccountState::locked,{},context()}).succeeded(),"one Sysop may deactivate when another remains usable");
    const auto caller_revision=db.find_account_by_id(id).value->revision;
    require(!db.change_account_state({id,caller_revision,app::AccountState::locked,{},context()}),"inactive Sysop does not count as usable fallback");
    require(db.change_account_state({sysop.account_id,sysop.revision+1,app::AccountState::active,{},context()}).succeeded(),"reactivation restores eligibility");
    app::PublishSecurityPolicyRequest policy;
    policy.expected_revision=1;policy.levels=*db.security_levels().value;
    policy.levels.push_back({500,"New Sysop Threshold",{},app::SecurityLevelState::active});
    policy.new_user_level=10;policy.sysop_threshold=500;policy.policy_hash=policy.published_policy_hash=std::string(64,'c');policy.audit=context();
    policy.audit.reason=app::AuditReasonCode::security_policy;
    require(db.publish_security_policy(policy).succeeded(),"live policy fixture adds configured threshold");
    auto current=*db.find_account_by_id(sysop.account_id).value;
    require(db.assign_security_level({sysop.account_id,current.revision,2,500,context()}).succeeded(),"first Sysop assigned new threshold");
    require(db.assign_security_level({id,caller_revision,2,500,context()}).succeeded(),"second Sysop assigned new threshold");
    policy.expected_revision=2;policy.sysop_threshold=999;policy.audit=context();
    policy.audit.reason=app::AuditReasonCode::security_policy;
    require(!db.publish_security_policy(policy) && db.security_policy().value->revision==2,"raising shared threshold cannot remove all Sysops");
    // Removal compares the exact reviewed membership inside the same writer transaction.
    policy.sysop_threshold=500;policy.new_user_level=500;
    for(auto& level:policy.levels) if(level.level==10) level.state=app::SecurityLevelState::retired;
    policy.reassignments={{10,500}};policy.expected_reassigned_accounts=std::vector<std::pair<app::AccountId,std::int64_t>>{};policy.audit=context();
    policy.audit.reason=app::AuditReasonCode::security_policy;
    require(!db.publish_security_policy(policy),"new assigned user after removal preview refuses entire transaction");
    policy.expected_reassigned_accounts=std::vector<std::pair<app::AccountId,std::int64_t>>{{*reclaim.account_id,1}};policy.audit=context();
    policy.audit.reason=app::AuditReasonCode::security_policy;
    require(db.publish_security_policy(policy).succeeded(),"exact reviewed removal reassigns users and defaults atomically");
    require(db.find_account_by_id(*reclaim.account_id).value->security_level==500,"replacement applied");
    require(db.shutdown().succeeded() && db.start().succeeded(),"complete user and shared policy changes survive reopen");
    require(db.find_account_by_id(id).value->display_name=="Renamed Editor" && db.security_policy().value->revision==3,"reopened editor state exact");
    require(db.shutdown().succeeded(),"live editor fixture closed");
}
