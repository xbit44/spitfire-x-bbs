// Actual current Runtime owner IPC47. This fresh fixture has no retained
// originals: native deletion/positive-count review is separately qualified.
{
    auto draft=read(runtime);const auto unchanged=app::encode_ftn_configuration(draft);
    check(draft.networks.size()==1&&!draft.settings.listener_enabled&&!draft.settings.tls_listener_enabled,
        "retention IPC fixture has one network and no FTN listeners");
    draft.networks[0].keep_processed=true;draft.networks[0].retention_days=1;
    const auto review_token=token();
    const auto preview=upload(runtime,draft,review_token,false,app::FtnConfigurationOperation::preview_retention);
    const auto summary=app::decode_ftn_retention_preview(preview.bytes);
    check(preview.code==app::FtnConfigurationCode::ok&&summary&&summary->token==review_token&&
        summary->configuration_revision==draft.revision&&summary->changed==0&&summary->shortened==0&&summary->networks.empty(),
        "actual current IPC returns truthful empty retained-original review, no invented totals");
    app::FtnConfigurationRequest review;review.operation=app::FtnConfigurationOperation::cancel;review.token=review_token;
    check(exchange(runtime,review).code==app::FtnConfigurationCode::ok&&app::encode_ftn_configuration(read(runtime))==unchanged,
        "Cancel leaves exact durable installed configuration unchanged");
    auto changed_draft=draft;changed_draft.networks[0].retention_days=0;
    check(upload(runtime,changed_draft,review_token,false,app::FtnConfigurationOperation::preview_retention).code==app::FtnConfigurationCode::conflict,
        "same durable review token cannot adopt changed policy bytes");
    check(exchange(runtime,review).code==app::FtnConfigurationCode::ok,"release rejected current upload");
    const auto accepted_token=token();
    check(upload(runtime,draft,accepted_token,false,app::FtnConfigurationOperation::preview_retention).code==app::FtnConfigurationCode::ok,
        "new exact installed policy review stages independently");
    review={};review.token=accepted_token;review.operation=app::FtnConfigurationOperation::commit;review.revision=draft.revision;
    check(app::encode_ftn_configuration_request(review).empty(),"missing commit size is rejected locally before owner IPC");
    review.total=static_cast<std::uint32_t>(app::encode_ftn_configuration(draft).size());
    check(exchange(runtime,review).code==app::FtnConfigurationCode::ok,"exact reviewed upload commits through actual owner pipe");
    check(runtime.restart(why),"actual Runtime restart after retention Save");
    auto retained=read(runtime);check(retained.revision==draft.revision+1&&retained.networks[0].keep_processed&&retained.networks[0].retention_days==1,
        "installed retention policy persists through process restart");
    review.operation=app::FtnConfigurationOperation::reconcile;review.revision=0;review.total=0;
    check(exchange(runtime,review).revision==retained.revision,"installed review operation reconciles after restart");
    retained.networks[0].keep_processed=false;
    check(upload(runtime,retained,token()).code==app::FtnConfigurationCode::ok&&!read(runtime).networks[0].keep_processed,
        "installed Off setting saves; no prior-original deletion claim from empty fixture");
}
