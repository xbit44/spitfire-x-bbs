void followup_secret_checks(){
    app::CallerLoginSecret secret;
    const auto credential=app::make_argon2id_credential_envelope({},std::vector<std::uint8_t>(16,1),std::vector<std::uint8_t>(32,2),100,false);
    secret.capture(7,credential,"SyntheticCallerOnly");check(!secret.empty(),"successful caller proof retained in connection memory");
    auto copy=secret.for_account(7,credential);check(copy&&copy->view()=="SyntheticCallerOnly","matching exact current verifier supplies launch credential without a prompt");
    auto changed=credential;changed.verifier[0]^=1;check(!secret.for_account(7,changed)&&secret.empty(),"changed verifier destroys stale caller secret");
    secret.capture(7,credential,"SyntheticCallerOnly");check(!secret.for_account(8,credential)&&secret.empty(),"another account cannot consume a session secret");
    changed=credential;changed.created_utc_seconds++;secret.capture(7,credential,"SyntheticCallerOnly");check(!secret.for_account(7,changed)&&secret.empty(),"full credential proof includes creation identity");
    changed=credential;changed.password_change_required=true;secret.capture(7,changed,"TemporaryOnly");check(secret.empty(),"temporary login password never becomes a Door credential");
    secret.capture(7,credential,std::string(129,'a'));check(secret.empty(),"bounded caller credential memory");
    secret.capture(7,credential,"NewPermanentOnly");secret.clear();check(secret.empty()&&!secret.for_account(7,credential),"session cleanup removes replayable credential");
}
