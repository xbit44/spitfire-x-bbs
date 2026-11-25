// Inside compose_local_message. Upfront confirmation authorizes one validated
// upload-to-post operation, not a transfer-only publication or a new permission.
if(command.command==MessageEditorCommand::upload){
    const auto selected=fresh_context();actor=p.actor();
    if(!selected || !actor || !p.source_receiver || !p.source_receiver->available({*actor,*selected})){
        if(!say(p,"Message-file receiving is unavailable; draft unchanged."))return MessageComposeOutcome::failed;
        continue;
    }
    if(validate_message_subject(editor.draft().subject(),true)!=MessageValidation::valid){
        if(!say(p,"The subject contains invalid characters or exceeds the message limit."))return MessageComposeOutcome::failed;
        continue;
    }
    const auto& old_body=editor.draft().body().canonical_lf;
    const auto confirmed_actor=*actor;
    const auto same_actor=[&](const MessageActor& a){return a.session==confirmed_actor.session && a.account==confirmed_actor.account &&
        a.account_revision==confirmed_actor.account_revision && a.policy_revision==confirmed_actor.policy_revision;};
    if(std::any_of(old_body.begin(),old_body.end(),[](char c){return c!=' ' && c!='\n';}) &&
        !say(p,"A valid upload will replace\nthis message body and post it."))return MessageComposeOutcome::failed;
    if(!say(p,"Allowed: .ans, .asc, .txt; maximum 256 KiB (262,144 bytes)."))return MessageComposeOutcome::failed;
    const auto confirm=p.terminal.read_key({"message.upload.confirm","Upload and post this message? [Y/n] ",{'Y','N'},'Y'});
    if(confirm.outcome==TerminalOutcome::cancelled || confirm.key=='N')continue;
    if(!confirm.has_key())return MessageComposeOutcome::failed;
    const auto checked=fresh_context();actor=p.actor();
    if(!actor || !same_actor(*actor) || !checked || !same_message_receive_context(*selected,*checked)){
        if(!say(p,"Context changed; upload again.\nCurrent draft unchanged."))return MessageComposeOutcome::failed;
        continue;
    }
    // Reserve a clean protocol row. Only the bounded binary lease may write
    // during receive; no helper output reaches ordinary input/NodeView decoding.
    if(!p.write("\r\n"))return MessageComposeOutcome::failed;
    const auto received=p.source_receiver->receive({*actor,*checked});decoder.reset();live_command_row.reset();
    if(received.code==MessageReceiveCode::disconnected){
        // Unsynchronized binary failure closes the transport. Do not attempt
        // the ordinary editor-mode restoration on that closed/uncertain lease.
        input_mode.enabled=false;return MessageComposeOutcome::failed;
    }
    // SyncTERM can expose part of its initial ZRINIT negotiation behind its
    // transfer popup. Clear only that current row after the synchronized handoff,
    // never send terminal controls into protocol or erase the user's whole screen.
    if(!p.write(p.dimensions().ansi?"\r\x1b[2K\x1b[?7h\x1b[0m":"\r\n"))return MessageComposeOutcome::failed;
    if(received.code!=MessageReceiveCode::completed || !received.source){
        const char* feedback="File was not accepted;\ncurrent draft unchanged. Nothing posted.";
        if(received.code==MessageReceiveCode::cancelled)
            feedback="No file was uploaded.\nCurrent draft unchanged. Nothing posted.";
        else if(received.code==MessageReceiveCode::refused && received.refusal==MessageReceiveRefusal::extension)
            feedback="File type not allowed. Use .ans, .asc or .txt.\nCurrent draft unchanged. Nothing posted.";
        else if(received.code==MessageReceiveCode::refused && received.refusal==MessageReceiveRefusal::too_large)
            feedback="File too large. Maximum: 256 KiB (262,144 bytes).\nCurrent draft unchanged. Nothing posted.";
        if(!acknowledge_upload_feedback(p,feedback))return MessageComposeOutcome::failed;
        continue;
    }
    const auto current=fresh_context();actor=p.actor();
    if(received.refusal!=MessageReceiveRefusal::none || !actor || !same_actor(*actor) || !current || !same_message_receive_context(*checked,*current) ||
        !validate_message_source(*received.source,current->maximum_lines)){
        if(!acknowledge_upload_feedback(p,"File validation or access changed.\nCurrent draft unchanged; not posted."))return MessageComposeOutcome::failed;
        continue;
    }
    CommitMessageRequest request{*actor,p.operation(),*current,editor.draft().subject(),{}};
    request.original_source=*received.source;
    const auto saved=p.storage.commit_message(request);
    if(saved && saved.value){say(p,"Message saved.");return MessageComposeOutcome::saved;}
    const auto known=p.storage.message_commit_outcome(request);
    if(known && known.value && known.value->committed){say(p,"Message saved.");return MessageComposeOutcome::saved;}
    if(!known || !known.value)uncertain=request;
    else refresh_context=true;
    // Keep the prior editor intact until the confirmed atomic commit. Unknown
    // outcomes use the existing reconciliation path and the SAME operation.
    if(!say(p,uncertain?"Post outcome uncertain.\nReconcile before another operation.":"Message was not posted.\nCurrent draft unchanged."))return MessageComposeOutcome::failed;
    continue;
}
