// Exact installed product, nonce/PID-bound owner pipe and production publisher.
// The outer coordinator permits only its disposable Program Files fixtures.
void event_sound_checks(const ins::Path& root){
    const auto get=[](Runtime& runtime){
        const auto snapshot=app::decode_configuration_snapshot(runtime.exchange(app::LocalControlMessageType::configuration_snapshot_request,app::LocalControlMessageType::configuration_snapshot_response));
        check(snapshot.has_value(),"installed configuration query decoded");return *snapshot;
    };
    const auto save=[](Runtime& runtime,const app::OperationalConfiguration& candidate,std::uint64_t expected){
        const auto result=app::decode_configuration_update_result(runtime.exchange(app::LocalControlMessageType::configuration_update_request,app::LocalControlMessageType::configuration_update_response,app::encode_configuration_update_request({expected,candidate})));
        check(result.has_value(),"installed configuration save result decoded");return *result;
    };
    app::EventSoundConfiguration expected;
    // Deliberately absent files qualify persistence and nonfatal missing-WAV
    // behavior without distributing test audio or mutating real sound files.
    const auto base=root/L"Data"/L"section13-absent.wav";const auto bytes=base.u8string();expected.paths.fill(bytes);
    {
        Runtime runtime(root);auto snapshot=get(runtime);auto candidate=snapshot.saved;
        ++candidate.configuration_revision;candidate.event_sounds=expected;
        const auto result=save(runtime,candidate,snapshot.saved.configuration_revision);
        check(result.code==app::LocalControlResponseCode::ok&&result.activation==app::AlphaConfigurationActivation::applied,"ten mappings apply through actual protected Runtime IPC");
        snapshot=get(runtime);check(snapshot.active.event_sounds==expected&&snapshot.saved.event_sounds==expected,"active and durable mappings match; no listener restart requested");
        const auto conflict=save(runtime,candidate,candidate.configuration_revision-1);
        check(conflict.code!=app::LocalControlResponseCode::ok,"stale sound save refused");
        win::WindowsSecureIdentifierGenerator ids;app::SoundPollRequest cursor{ids.generate(),{},0,true};
        const auto poll=[&](const app::SoundPollRequest& request){return app::decode_sound_poll_snapshot(runtime.exchange(app::LocalControlMessageType::sound_poll_request,app::LocalControlMessageType::sound_poll_response,app::encode_sound_poll_request(request)));};
        auto sounds=poll(cursor);check(sounds&&sounds->owner&&sounds->events.empty()&&sounds->configuration==expected,"installed event owner binds with authoritative mappings and no history");
        cursor.epoch=sounds->epoch;cursor.after=sounds->through;
        auto other=cursor;other.owner=ids.generate();sounds=poll(other);check(sounds&&!sounds->owner&&sounds->events.empty(),"installed second audio owner cannot duplicate playback");
        sounds=poll(cursor);check(sounds&&sounds->owner&&sounds->events.empty(),"configuration save/status/start success remain silent");
        runtime.stop();
    }
    {
        Runtime restarted(root);const auto snapshot=get(restarted);
        check(snapshot.active.event_sounds==expected&&snapshot.saved.event_sounds==expected,"all ten mappings survive actual installed Runtime restart");
        auto silent=snapshot.saved;++silent.configuration_revision;silent.event_sounds={};
        const auto result=save(restarted,silent,snapshot.saved.configuration_revision);check(result.code==app::LocalControlResponseCode::ok,"Clear all mappings saves silent defaults");
        const auto read=get(restarted);check(read.active.event_sounds==app::EventSoundConfiguration{},"clear roundtrip returns empty assignments");
        auto restore=read.saved;++restore.configuration_revision;restore.event_sounds=expected;
        check(save(restarted,restore,read.saved.configuration_revision).code==app::LocalControlResponseCode::ok,"restore synthetic mappings for normal-update preservation check");
        restarted.stop();
    }
}
