#include "message_upload_outcome_fake.inc"
struct FakeMessageReceiver final:app::IMessageSourceReceiver {
    app::MessageReceiveResult result{app::MessageReceiveCode::completed,app::MessageSource{app::MessageSourceFormat::ansi_canvas,1,"\x1b[2J\x1b[10;55H|12Artwork"}};
    bool enabled=true;unsigned calls{};std::function<void()> before_result;
    bool available(const app::MessageReceiveRequest&)override{return enabled;}
    app::MessageReceiveResult receive(const app::MessageReceiveRequest&)override{++calls;if(before_result)before_result();return result;}
};
void source_compose_checks(const std::filesystem::path& root,sfx::test::FakeStoragePathPolicy& policy){
    auto s=options(root);db::SqliteStorage store(s,policy);check(store.start().succeeded() && store.shutdown().succeeded(),"upload fixture bootstrapped");
    const auto file=root/"SFX.DB";seed(file);check(store.start().succeeded(),"upload actors ready");
    app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};app::MessageTarget target{app::MessageKind::public_echo,{2},{},{}};
    unsigned operation=2300;
    const auto run=[&](std::string script,bool ansi,FakeMessageReceiver& receiver,app::MessageTarget destination,std::string* transcript=nullptr,app::IMessageStorage* injected=nullptr){
        sfx::test::DeterministicInteractiveByteStreamFake stream;
        // Escape cancels a pending read and erases its prefetched fragment.
        // Script later real Enter/commands as later reads, not discarded input.
        std::size_t scripted=0;
        for(std::size_t pos=0;pos<script.size();++pos)if(script[pos]==27){
            if(pos>scripted)stream.script_non_secret_text(std::string_view(script).substr(scripted,pos-scripted));
            stream.script_non_secret_text(std::string_view(script).substr(pos,1));
            // Bare Escape is distinguished from a fragmented CSI/SS3 key by
            // an idle tail poll. The later real Enter must not be prefetched
            // and erased together with the cancelled input fragment.
            stream.script_read_outcome(app::InteractiveStreamOutcome::timed_out);scripted=pos+1;
        }
        if(scripted<script.size())stream.script_non_secret_text(std::string_view(script).substr(scripted));
        app::StreamCallerTerminal terminal(stream);
        app::MessageComposePorts ports{injected?*injected:store,terminal,[&]{return std::optional<app::MessageActor>{alice};},[&]{return id(operation++);},[ansi]{return app::MessageEditorDimensions{ansi,80,24};},[&]{return terminal.read_editor_byte();},[&](std::string_view text){return stream.write_some(text).outcome==app::InteractiveStreamOutcome::completed;}};
        ports.source_receiver=&receiver;
        ports.accepted_key=[&](bool cr){terminal.finish_editor_key(cr);};
        const auto outcome=app::compose_local_message(ports,destination,"Destination","Alice");
        if(transcript)*transcript=stream.output();
        check(stream.output().find("\x1b[10;55H")==std::string::npos,"uploaded cursor command never forwarded to compose");
        check(stream.output().find("Replace current draft")==std::string::npos && stream.output().find("Uploaded draft:")==std::string::npos && stream.output().find("MESSAGE FILE PREVIEW")==std::string::npos,"obsolete mandatory preview/replacement/draft-menu absent");
        if(!ansi)check(stream.output().find('\x1b')==std::string::npos,"plain composition escape-free");
        return outcome;
    };
    check(app::message_line_command("/uPlOaD").command==app::MessageEditorCommand::upload && app::message_line_command("/ul").command==app::MessageEditorCommand::upload,"standalone upload aliases case insensitive");
    check(app::message_line_command("/UL other").command==app::MessageEditorCommand::none,"upload does not dispatch embedded/replacement text");
    for(bool ansi:{false,true}){
        FakeMessageReceiver receiver;const auto before=scalar(file,"SELECT total_committed FROM sfx_message_counters");
        std::string success;
        check(run("Uploaded\r\nold draft\r\n/UL\r\nY\r\n",ansi,receiver,target,&success)==app::MessageComposeOutcome::saved && receiver.calls==1,"both editors upload and post after just one confirmation");
        check(success.find("Save this message?")==std::string::npos && success.find("A valid upload will replace")!=std::string::npos,"nonempty-body replacement explained before the single posting question, no extra Save question");
        check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==before+1,"upload publishes exactly once after authorized valid receive");
        const auto latest=scalar(file,"SELECT max(message_id) FROM sfx_local_messages");const auto opened=store.quote_source(bob,{latest});
        check(opened && opened.value->original_source && opened.value->original_source->bytes==receiver.result.source->bytes,"accepted original preserved byte-exact");
        receiver={};
        check(run("Decline\r\nold draft\r\n/UL\r\nN\r\n/S\r\nY\r\n",ansi,receiver,target)==app::MessageComposeOutcome::saved && !receiver.calls,"decline before transfer; ordinary prior draft remains saveable");
        const auto old=store.quote_source(bob,{scalar(file,"SELECT max(message_id) FROM sfx_local_messages")});
        check(old && !old.value->original_source && old.value->body.canonical_lf.find("old draft")!=std::string::npos,"declined source never replaces prior draft");
        for(const auto code:{app::MessageReceiveCode::cancelled,app::MessageReceiveCode::timed_out,app::MessageReceiveCode::failed}){
            receiver={};receiver.result={code,{}};
            std::string refusal;
            const auto failure_outcome=run("Failure\r\nretained\r\n/UPLOAD\r\nY\r\nx\r\n\x1b\r\n/S\r\nY\r\n",ansi,receiver,target,&refusal);
            if(failure_outcome!=app::MessageComposeOutcome::saved){std::cerr<<"upload refusal fixture ansi="<<ansi<<" receive="<<static_cast<unsigned>(code)<<" outcome="<<static_cast<unsigned>(failure_outcome)<<" tail=";for(std::size_t i=refusal.size()>2400?refusal.size()-2400:0;i<refusal.size();++i){const auto c=static_cast<unsigned char>(refusal[i]);if(c>=32&&c<127)std::cerr<<c;else std::cerr<<"["<<static_cast<unsigned>(c)<<"]";}std::cerr<<'\n';}
            check(failure_outcome==app::MessageComposeOutcome::saved,"receive failure waits for Enter, ignores non-Enter/Escape, preserves old draft in both modes");
            const auto explanation=code==app::MessageReceiveCode::cancelled?"No file was uploaded.":"File was not accepted";
            check(refusal.find(explanation)!=std::string::npos && refusal.find("Nothing posted.")!=std::string::npos && refusal.find("<enter>")>refusal.find(explanation),"exact cancellation/refusal is explained before acknowledgment");
            const auto unchanged=store.quote_source(bob,{scalar(file,"SELECT max(message_id) FROM sfx_local_messages")});
            check(unchanged && !unchanged.value->original_source && unchanged.value->body.canonical_lf=="retained\n","separate ordinary Save retains exact old text after no-file/failure notice");
        }
        receiver={};receiver.result={app::MessageReceiveCode::cancelled,{}};
        const auto before_empty=scalar(file,"SELECT total_committed FROM sfx_message_counters");std::string empty_notice;
        check(run("Empty batch\r\nKeep my draft\r\n/UL\r\nY\r\n",ansi,receiver,target,&empty_notice)==app::MessageComposeOutcome::failed && scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_empty,"empty cancellation cannot post or redraw without Enter acknowledgment");
        check(empty_notice.find("No file was uploaded.")!=std::string::npos && empty_notice.find("<enter>")>empty_notice.find("Nothing posted."),"empty cancellation has clear held no-file notice in both editors");
        receiver={};receiver.result.source->bytes="\x1b]0;hostile\a";
        check(run("Invalid\r\nretained\r\n/UL\r\nY\r\n\r\n/S\r\nY\r\n",ansi,receiver,target)==app::MessageComposeOutcome::saved,"unsupported source refused and acknowledged without losing draft");
        for(const auto reason:{app::MessageReceiveRefusal::extension,app::MessageReceiveRefusal::too_large}){
            receiver={};receiver.result={app::MessageReceiveCode::refused,{},reason};
            const auto before_policy=scalar(file,"SELECT total_committed FROM sfx_message_counters");std::string policy_notice;
            check(run("Policy refusal\r\nKeep my draft\r\n/UL\r\nY\r\nx\r\n\x1b\r\n/S\r\nY\r\n",ansi,receiver,target,&policy_notice)==app::MessageComposeOutcome::saved,"both editors restore preserved text after Enter-only policy notice");
            const auto expected=reason==app::MessageReceiveRefusal::extension?"File type not allowed. Use .ans, .asc or .txt.":"File too large. Maximum: 256 KiB (262,144 bytes).";
            check(policy_notice.find(expected)!=std::string::npos && policy_notice.find("<enter>")>policy_notice.find(expected),"specific extension/size explanation remains visible until Enter");
            check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_policy+1,"only separate ordinary Save posts after refusal");
            const auto saved_old=store.quote_source(bob,{scalar(file,"SELECT max(message_id) FROM sfx_local_messages")});
            check(saved_old && !saved_old.value->original_source && saved_old.value->body.canonical_lf=="Keep my draft\n","prior text exact and no uploaded source after refusal");
            const auto before_held=scalar(file,"SELECT total_committed FROM sfx_message_counters");
            check(run("Held policy\r\nKeep my draft\r\n/UL\r\nY\r\n",ansi,receiver,target,&policy_notice)==app::MessageComposeOutcome::failed && scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_held,"no Enter means no editor redraw or post on policy refusal");
        }
        receiver={};receiver.result={app::MessageReceiveCode::failed,{}};
        const auto before_notice=scalar(file,"SELECT total_committed FROM sfx_message_counters");std::string notice;
        check(run("Held refusal\r\nretained\r\n/UL\r\nY\r\n",ansi,receiver,target,&notice)==app::MessageComposeOutcome::failed,"refusal cannot resume editing without an acknowledgment");
        check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_notice && notice.find("<enter>")!=std::string::npos && (!ansi || notice.find("\x1b[2J",notice.find("<enter>"))==std::string::npos),"unacknowledged refusal has no post or full-screen redraw");
        receiver={};receiver.enabled=false;
        check(run("Unavailable\r\nretained\r\n/UL\r\n/S\r\nY\r\n",ansi,receiver,target)==app::MessageComposeOutcome::saved && !receiver.calls,"unavailable capability does not start receiving");
        receiver={};receiver.before_result=[&]{
            const auto snapshot=store.message_configuration({true,{}},app::MessageGroupId{1},{});
            check(bool(snapshot),"fresh policy during receive fixture");
            app::MessageConfigurationChange change;change.operation=id(operation++);change.action=app::MessageConfigurationAction::echo;
            change.expected_configuration_revision=snapshot.value->revision;change.presentation_mask=1;
            change.echo=*std::find_if(snapshot.value->echoes.begin(),snapshot.value->echoes.end(),[](const auto& e){return e.id.value==2;});
            change.echo.renegade_pipe_colors=!change.echo.renegade_pipe_colors;
            check(bool(store.change_message_configuration({true,{}},change)),"actual storage policy changes while receive result is pending");
        };
        const auto before_stale=scalar(file,"SELECT total_committed FROM sfx_message_counters");std::string transcript;
        check(run("Stale receive\r\nretained\r\n/UL\r\nY\r\n\r\n/S\r\nY\r\n",ansi,receiver,target,&transcript)==app::MessageComposeOutcome::failed,"stale context cannot silently renew an unreviewed Save; input exhaustion fails");
        check(transcript.find("Current draft unchanged; not posted.")!=std::string::npos && transcript.find("connected draft is retained")!=std::string::npos && scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_stale,"stale receive retains connected prior draft and produces no source/post/counter");
        receiver={};
        check(run("Default upload\r\n/UPLOAD\r\n\r\n",ansi,receiver,target,&success)==app::MessageComposeOutcome::saved && receiver.calls==1,"approved default Enter consent posts valid upload in both editors");
        check(success.find("A valid upload will replace")==std::string::npos,"empty body needs no replacement explanation");
        receiver={};receiver.result={app::MessageReceiveCode::disconnected,{}};
        const auto before_disconnect=scalar(file,"SELECT total_committed FROM sfx_message_counters");
        check(run("Disconnected\r\nretained\r\n/UL\r\nY",ansi,receiver,target,&transcript)==app::MessageComposeOutcome::failed,"disconnected binary lease never resumes ordinary editing");
        check(transcript.find("File was not accepted")==std::string::npos && transcript.find("\x1b[?67h")==std::string::npos && scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_disconnect,"no normal feedback/editor restoration or post after disconnected binary lease");
        receiver={};receiver.before_result=[&]{alice=bob;};
        const auto before_actor=scalar(file,"SELECT total_committed FROM sfx_message_counters");
        check(run("Changed actor\r\n/UL\r\nY\r\n\r\n/A\r\nY",ansi,receiver,target,&transcript)==app::MessageComposeOutcome::cancelled && transcript.find("not posted")!=std::string::npos,"consent cannot silently migrate to a different caller session");
        check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_actor,"actor switch does not publish");alice={id(2),2,1,1};
        receiver={};
        // A late source insertion failure follows receipt/body allocation in
        // this same transaction; ordinary retained-text Save remains valid.
        receiver.before_result=[&]{edit(file,"CREATE TRIGGER upload_commit_failure BEFORE INSERT ON sfx_message_sources BEGIN SELECT RAISE(ABORT,'upload commit fault'); END");};
        check(run("Commit refused\r\nretained\r\n/UL\r\nY\r\n/S\r\nY\r\n",ansi,receiver,target,&transcript)==app::MessageComposeOutcome::saved && transcript.find("Message was not posted.")!=std::string::npos,"late atomic source commit refusal retains old draft for separate ordinary Save");
        edit(file,"DROP TRIGGER upload_commit_failure");
        const auto retained=store.quote_source(bob,{scalar(file,"SELECT max(message_id) FROM sfx_local_messages")});
        check(retained && !retained.value->original_source && retained.value->body.canonical_lf.find("retained")!=std::string::npos,"failed source/receipt transaction cannot replace old editor body");
        receiver={};const auto before_subject=scalar(file,"SELECT total_committed FROM sfx_message_counters");
        check(run("\r\n/UL\r\nY\r\n",ansi,receiver,target,&transcript)==app::MessageComposeOutcome::saved && receiver.calls==1,"blank subject permits an explicitly approved upload in both editors");
        check(scalar(file,"SELECT total_committed FROM sfx_message_counters")==before_subject+1,"blank-subject upload posts once");
    }
    FakeMessageReceiver receiver;
    check(scalar(file,"SELECT count(*) FROM sfx_message_sources")==6,"six approved public uploads including blank subjects produced original source rows");
    for(bool committed:{true,false}){
        receiver={};UploadOutcomeStorage injected(store);std::string transcript;
        if(!committed)receiver.before_result=[&]{edit(file,"CREATE TRIGGER upload_uncertain_failure BEFORE INSERT ON sfx_message_sources BEGIN SELECT RAISE(ABORT,'uncertain fault'); END");};
        const auto before=scalar(file,"SELECT total_committed FROM sfx_message_counters");
        check(run("Uncertain upload\r\nretained old text\r\n/UL\r\nY\r\nR\r\n/A\r\nY",false,receiver,target,&transcript,&injected)==(committed?app::MessageComposeOutcome::saved:app::MessageComposeOutcome::cancelled),"unknown outcome reconciles committed source or resumes old editor without automatic post");
        if(!committed)edit(file,"DROP TRIGGER upload_uncertain_failure");
        check(injected.commits==1 && injected.outcomes==2 && scalar(file,"SELECT total_committed FROM sfx_message_counters")==before+int(committed),"lost result never allocates a second operation or publishes old draft");
        check(transcript.find("Pending save outcome:")!=std::string::npos && transcript.find("Save this message?")==std::string::npos,"reconciliation is only exceptional control flow, never a second save consent");
    }
    receiver={};target={app::MessageKind::private_local,{},3,{}};
    check(run("Private upload\r\n/UL\r\nY\r\n",false,receiver,target)==app::MessageComposeOutcome::saved,"eligible ordinary caller private source upload-to-post");
    const auto private_id=app::MessageId{scalar(file,"SELECT max(message_id) FROM sfx_local_messages")};
    check(store.quote_source(bob,private_id) && !store.quote_source(app::MessageActor{id(4),4,1,1},private_id),"uploaded private source isolation unchanged");
    check(scalar(file,"SELECT count(*) FROM sfx_message_source_write_scope")==0,"upload Save leaves no privileged source scope");
    check(store.shutdown().succeeded(),"upload compose fixture stopped");
}
