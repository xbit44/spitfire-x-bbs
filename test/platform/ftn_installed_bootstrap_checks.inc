{
    const auto initial=runtime.status();
    check(initial&&initial->state==app::AlphaRuntimePresentationState::setup_required&&!initial->bootstrap_complete,
        "fresh installed shipping Runtime reports actual first-run state");
    app::AlphaBootstrapRequest request;request.configuration.bbs_name="Section14 Installed Fixture";
    request.configuration.bbs_time_zone="Pacific Standard Time";request.configuration.logical_nodes=4;
    request.configuration.telnet.bind_address="127.0.0.1";request.configuration.telnet.port=26431;
    request.configuration.telnet.bot_gate_enabled=false;request.configuration.botgate.enabled=false;
    request.configuration.ssh.enabled=true;request.configuration.ssh.bind_address="127.0.0.1";request.configuration.ssh.port=26432;
    request.sysop_name="SSH Fixture";request.sysop_password="Sfx-test-password-123";request.system_password="Sfx-system-password-456";
    check(runtime.bootstrap(request).code==app::LocalControlResponseCode::ok,
        "normal first-run bootstrap through actual current shipping owner IPC");
    app::erase_bootstrap_secrets(request);
    const auto configured=runtime.status();
    check(configured&&configured->bootstrap_complete&&configured->configuration_loaded,
        "installed Runtime confirms durable bootstrap");
}
