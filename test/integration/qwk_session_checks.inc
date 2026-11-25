// Actual production caller controller/storage, simulated terminal inputs.
// Not native Windows UI, visible client or actual reader acceptance.
    {
        struct QwkTerminal final:app::ICallerTerminal {
            std::deque<char> keys;std::deque<std::string> input;std::string shown;unsigned reads{};
            app::TerminalOutcome set_text_style(app::TerminalTextStyle)override{return app::TerminalOutcome::completed;}
            app::TerminalOutcome clear_screen()override{return app::TerminalOutcome::completed;}
            app::TerminalInputResult read_visible(const app::TerminalInputRequest&)override{check(!input.empty(),"bounded simulated caller text available");auto v=std::move(input.front());input.pop_front();return {app::TerminalOutcome::completed,std::move(v)};}
            app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{throw std::runtime_error("QWK never requests secret");}
            app::TerminalKeyResult read_key(const app::TerminalKeyRequest& r)override{++reads;shown+=r.prompt;check(!keys.empty(),"bounded simulated caller key available");const auto key=keys.front();keys.pop_front();if(key=='\r'){check(r.default_key.has_value(),"ordinary Enter uses actual stated default");return {app::TerminalOutcome::empty,r.default_key};}check(std::find(r.allowed_keys.begin(),r.allowed_keys.end(),key)!=r.allowed_keys.end(),"simulated key permitted by actual request");return {app::TerminalOutcome::completed,key};}
            app::TerminalOutcome present(const app::TerminalPresentationRequest& r)override{shown+=r.non_secret_text;return app::TerminalOutcome::completed;}
            app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& r)override{check(r.program&&app::valid_terminal_display_program(*r.program),"actual inert controller display program valid");for(const auto& op:r.program->operations)if(op.kind==app::TerminalDisplayOperationKind::cp437_text)shown+=op.bytes;else if(op.kind==app::TerminalDisplayOperationKind::logical_newline)shown+='\n';return app::TerminalOutcome::completed;}
            app::TerminalOutcome pause(const app::TerminalPauseRequest&)override{return app::TerminalOutcome::completed;}
        } terminal;
        auto prefs_before=db.qwk_preferences(bob);check(static_cast<bool>(prefs_before),"controller caller current preferences");auto initial=prefs_before.value->preferences;initial.encoding=app::QwkEncoding::cp437;initial.full_headers=true;initial.word_wrap=true;
        check(db.save_qwk_preferences(bob,prefs_before.value->revision,initial).succeeded(),"trusted explicit controller test baseline, no product default");
        unsigned operations=8000,downloads{},uploads{};bool actor_live=true;
        app::QwkCallerSession session({db,terminal,[&]() -> std::optional<app::MessageActor>{return actor_live?std::optional<app::MessageActor>{bob}:std::nullopt;},[&]{return id(++operations);},[&]() -> std::optional<app::MessageArea>{const auto area=db.message_area(bob,{2});return area.value;},[](std::string_view text)->std::optional<std::int64_t>{return text=="2026-10-13 14:00"?std::optional<std::int64_t>{1791920000}:std::nullopt;},[&]() -> std::optional<std::string>{++downloads;return std::string(app::kQwkEmptyNotice);},[&]() -> std::optional<std::string>{++uploads;return std::string(app::kQwkReplayNotice);}});
        terminal.keys={'W','N','E','U','H','N','Q'};const auto result=session.action(app::MenuAction::qwk_settings);const auto first=db.qwk_preferences(bob);
        check(result&&first&&first.value->preferences.word_wrap==false&&first.value->preferences.encoding==app::QwkEncoding::utf8&&first.value->preferences.full_headers,"actual settings save/CAS with dependent invalid header change refused");
        check(terminal.keys.empty()&&terminal.shown.find("UTF-8 requires Full headers Yes")!=std::string::npos,"dependency refusal visible in simulated actual controller");
        check(terminal.shown.find("[H] Full headers: Yes\n[Q] Return\n")!=std::string::npos &&
            terminal.shown.find("CONTROL.DAT and DOOR.ID")==std::string::npos &&
            terminal.shown.find("Changes save immediately")==std::string::npos,
            "QWK settings Return follows Headers directly without explanatory rows");
        terminal.keys={'I','A','O','Y','C','N','N','N','E','C','H','N','Q'};check(session.action(app::MenuAction::qwk_settings).has_value(),"actual complete settings choice loop");const auto second=db.qwk_preferences(bob);
        check(second&&second.value->preferences.private_mail==app::QwkPrivateMode::all_received&&second.value->preferences.own_public_posts&&!second.value->preferences.preserve_colors&&!second.value->preferences.indexes&&second.value->preferences.encoding==app::QwkEncoding::cp437&&!second.value->preferences.full_headers,"all packet settings use caller persistence, no second subscription pool");
        terminal.shown.clear();terminal.keys={'Q'};
        const auto cancelled_pointer=db.message_scan_preference(bob,{2});
        check(session.action(app::MenuAction::qwk_pointers).has_value()&&db.message_scan_preference(bob,{2}).value->highest_opened==cancelled_pointer.value->highest_opened,"pointer scope Q returns without changes");
        check(terminal.shown=="New-message Pointers\n\n[E] Current echo\n[G] Current group\n[A] All accessible\n[Q] Cancel\nPublic pointers [E G A Q]? [E]: ","compact pointer scope option layout, no leading separator rows");
        terminal.shown.clear();terminal.keys={'\r','\r'};
        check(session.action(app::MenuAction::qwk_pointers).has_value()&&db.message_scan_preference(bob,{2}).value->highest_opened==cancelled_pointer.value->highest_opened,"scope Enter selects current echo and mode Enter safely cancels");
        check(terminal.shown.find("Set public pointers to:\n\n[A] All available\n[D] Since BBS-local date/time\n[C] Caught up\n[Q] Cancel\nPointer setting [A D C Q]? [Q]: ")!=std::string::npos,"compact second pointer menu and explicit cancel default");
        const auto flags=scalar(file,"SELECT sum(unread_enabled*2+to_you_enabled) FROM sfx_caller_echo_state WHERE account_id=3"),read_states=scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state WHERE recipient_account_id=3");
        const auto before_pointer=db.message_scan_preference(bob,{2});terminal.keys={'A','C','\r'};check(session.action(app::MenuAction::qwk_pointers).has_value()&&db.message_scan_preference(bob,{2}).value->highest_opened==before_pointer.value->highest_opened,"bulk public reset ordinary default-No does not alter positions");
        terminal.keys={'E','A','Y','\r'};check(session.action(app::MenuAction::qwk_pointers).has_value()&&db.message_scan_preference(bob,{2}).value->highest_opened==0,"confirmed actual current-echo all-available reset");
        terminal.keys={'G','D','Y','\r'};terminal.input={"2026-10-13 14:00"};check(session.action(app::MenuAction::qwk_pointers).has_value(),"actual group since-entry request uses supplied configured-zone UTC");
        terminal.keys={'A','D','\r'};terminal.input={"invalid"};const auto before_invalid=db.message_scan_preference(bob,{2});check(session.action(app::MenuAction::qwk_pointers).has_value()&&db.message_scan_preference(bob,{2}).value->highest_opened==before_invalid.value->highest_opened,"invalid date stays held/no pointer effect");
        check(scalar(file,"SELECT sum(unread_enabled*2+to_you_enabled) FROM sfx_caller_echo_state WHERE account_id=3")==flags&&scalar(file,"SELECT sum(opened) FROM sfx_message_recipient_state WHERE recipient_account_id=3")==read_states,"actual controller pointer actions preserve scan flags/private Read");
        terminal.keys={'\r'};check(session.action(app::MenuAction::qwk_download).has_value()&&downloads==1&&terminal.shown.find(app::kQwkEmptyNotice)!=std::string::npos,"empty notice held through actual ordinary Enter, no fake transfer pass");
        terminal.keys={'\r'};check(session.action(app::MenuAction::qwk_upload).has_value()&&uploads==1&&terminal.shown.find(app::kQwkReplayNotice)!=std::string::npos,"exact replay notice held through actual caller controller");
        actor_live=false;check(!session.action(app::MenuAction::qwk_settings)&&terminal.keys.empty(),"revoked actor cannot enter settings or consume more keys");
    }
