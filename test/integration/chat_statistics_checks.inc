void chat_statistics_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& paths){
    auto options=settings(root);storage::SqliteStorage db(options,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"statistics fixture initialized");
    const auto file=options.data_directory/"SFX.DB";seed(file);
    {internal::SqliteConnection connection(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
        connection.exec("INSERT INTO sfx_security_levels(security_level,label,description,level_state,created_utc_seconds,modified_utc_seconds) VALUES(50,'Staff','configured threshold test','active',1700000000,1700000000);UPDATE sfx_accounts SET security_level=50 WHERE account_id=3","synthetic configured staff identity");}
    check(db.start().succeeded(),"statistics fixture reopened");
    app::PublishSecurityPolicyRequest policy;policy.expected_revision=db.security_policy().value->revision;policy.levels=*db.security_levels().value;
    policy.new_user_level=10;policy.sysop_threshold=50;policy.policy_hash=policy.published_policy_hash=std::string(64,'b');policy.audit={id(2100),id(2101),{},1792248000,app::AuditReasonCode::security_policy};
    check(db.publish_security_policy(policy).succeeded(),"statistics uses configured level50 threshold");
    const app::MessageConfigurationActor owner{true,{}};
    auto config=*db.chat_configuration(owner).value;check(config.exclude_sysop_statistics,"missing statistics setting defaults On");
    const auto session=[&](unsigned key,app::AccountId account,std::int64_t seconds){
        app::StartSessionRequest request{id(key),id(key+100),account,static_cast<std::uint16_t>(account),1792248000};
        check(db.record_session_started(request).succeeded()&&db.record_session_started(request).replayed,"session record and start replay");
        app::FinalizeSessionRequest end{request.session_id,seconds,1792248000+seconds,app::SessionOutcomeCode::normal,id(key+200)};
        check(db.finalize_session(end).succeeded(),"genuine session finalization retained");
    };
    session(2200,3,60);session(2201,2,120);session(2202,1,180);
    const auto counters=[&](std::int64_t logons,std::int64_t seconds){
        const auto panel=storage::ControlPanelStatisticsReader::read_checked(file,1792240000,1792326400);
        const auto expected_logons=static_cast<std::uint64_t>(logons),expected_seconds=static_cast<std::uint64_t>(seconds);
        check(panel&&panel.value->successful_logons.today==expected_logons&&panel.value->successful_logons.total==expected_logons+3&&panel.value->caller_seconds.total==expected_seconds&&panel.value->caller_seconds.today==expected_seconds,"Control Panel coherent Today/Total excludes staff usage only");
        const auto information=db.system_information_counters(1792240000,1792326400);
        check(information&&information.value->total_calls==logons+3&&information.value->today_calls==logons,"caller System Information agrees with Panel");
    };
    counters(1,120);
    const auto staff=db.find_account_by_id(3);check(staff&&staff.value->statistics.successful_login_count==1&&staff.value->statistics.authenticated_session_seconds==60&&staff.value->statistics.last_successful_login_utc_seconds==1792248000,"personal staff logon/time/last-login retained");
    check(db.session_history({3,app::kMaximumUtcSeconds,100}).value->size()==2&&scalar(file,"SELECT count(*) FROM sfx_account_sessions")==6,"real session history never filtered or deleted");
    check(scalar(file,"SELECT count(*) FROM sfx_chat_usage_exclusions")==2,"both configured staff and999 excluded independently of Ghost");
    config.exclude_sysop_statistics=false;check(db.change_chat_configuration(owner,config).succeeded(),"statistics Off applies to future sessions");session(2203,3,240);counters(2,360);
    config=*db.chat_configuration(owner).value;config.exclude_sysop_statistics=true;check(db.change_chat_configuration(owner,config).succeeded(),"statistics On does not rewrite history");counters(2,360);
    check(db.shutdown().succeeded()&&db.start().succeeded(),"statistics choice and markers survive reopen");counters(2,360);check(db.shutdown().succeeded(),"statistics fixture closed");
}
