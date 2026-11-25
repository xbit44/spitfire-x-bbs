// Reuse only the isolated native fixture, not an installed/live owner BBS.
#define main rc9_previous_engine_main
#include "ftn_engine_tests.cpp"
#undef main
#include "sfx/platform/win32/ftn_stored_scan.hpp"
#include "sfx/application/ftn_stored_message.hpp"
#include "sfx/archive/qwk_packet_builder.hpp"
#include "alpha_qwk_reply_spool.hpp"
#include <archive.h>
#include <archive_entry.h>
#include <fstream>
#include <sodium.h>
#include <map>
namespace {
std::filesystem::path actual_reader;
#include "qwk_reply_source_fake.inc"
std::map<std::string,std::string> members(std::string_view zip){
    auto* a=archive_read_new();check(a&&archive_read_support_format_zip(a)==ARCHIVE_OK&&archive_read_open_memory(a,zip.data(),zip.size())==ARCHIVE_OK,"open actual QWK ZIP");
    std::map<std::string,std::string> out;archive_entry* e{};for(;;){const auto next=archive_read_next_header(a,&e);if(next==ARCHIVE_EOF)break;check(next==ARCHIVE_OK,"actual ZIP entry");auto& bytes=out[archive_entry_pathname(e)];char b[8192];for(;;){const auto n=archive_read_data(a,b,sizeof b);check(n>=0,"actual ZIP CRC/data");if(!n)break;bytes.append(b,static_cast<std::size_t>(n));}}
    check(archive_read_free(a)==ARCHIVE_OK,"close actual QWK archive");return out;
}
std::string msg(std::string_view subject,std::uint16_t attr=0x191,std::string flags="KFS"){
    std::string b(190,'\0');b.replace(0,10,"BRE System");b.replace(36,10,"BRE System");b.replace(72,subject.size(),subject);
    const auto word=[&](std::size_t at,unsigned n){b[at]=static_cast<char>(n);b[at+1]=static_cast<char>(n>>8);};
    word(166,1);word(168,2);word(172,100);word(174,100);word(186,attr);
    return b+"\1INTL 2:100/1 2:100/2\r\1FLAGS "+flags+"\r\n"+'\0';
}
void save_bytes(const std::filesystem::path& path,std::string_view bytes){std::ofstream f(path,std::ios::binary);f.write(bytes.data(),bytes.size());check(bool(f),"write isolated synthetic source");}
std::string load_bytes(const std::filesystem::path& path){std::ifstream f(path,std::ios::binary);check(bool(f),"read actual independent-reader output");return {std::istreambuf_iterator<char>(f),{}};}
std::string terminal_visible(std::string_view raw){std::string out;for(std::size_t at=0;at<raw.size();){if(raw[at]=='\x1b'&&at+1<raw.size()&&raw[at+1]=='['){at+=2;while(at<raw.size()){const auto byte=static_cast<unsigned char>(raw[at++]);if(byte>=0x40&&byte<=0x7e)break;}}else out+=raw[at++];}return out;}
struct PauseObservedTerminal:app::ICallerTerminal{app::ICallerTerminal& inner;unsigned pauses{};explicit PauseObservedTerminal(app::ICallerTerminal& v):inner(v){}app::TerminalOutcome set_text_style(app::TerminalTextStyle s)override{return inner.set_text_style(s);}app::TerminalOutcome clear_screen()override{return inner.clear_screen();}app::TerminalInputResult read_visible(const app::TerminalInputRequest& r)override{return inner.read_visible(r);}app::TerminalInputResult read_secret(const app::TerminalInputRequest& r)override{return inner.read_secret(r);}app::TerminalKeyResult read_key(const app::TerminalKeyRequest& r)override{return inner.read_key(r);}app::TerminalOutcome present(const app::TerminalPresentationRequest& r)override{return inner.present(r);}app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& r)override{return inner.present_display(r);}app::TerminalOutcome pause(const app::TerminalPauseRequest& r)override{++pauses;return inner.pause(r);}};
std::shared_ptr<app::IQwkReplySource> reader_reply(Fixture& server,const std::filesystem::path& folder,std::string_view board){
    const auto out=server.root/L"ActualReaderReply";std::filesystem::create_directory(out);
    std::wstring command=L"\""+actual_reader.native()+L"\" \""+folder.native()+L"\" \""+out.native()+L"\" "+std::filesystem::u8path(board).native();
    STARTUPINFOW startup{sizeof(startup)};PROCESS_INFORMATION process{};
    check(CreateProcessW(actual_reader.c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)!=FALSE,"launch isolated actual upstream NoCarrierMail API probe");
    ins::Handle thread(process.hThread),child(process.hProcess);const auto wait=WaitForSingleObject(child.get(),30000);if(wait!=WAIT_OBJECT_0)TerminateProcess(child.get(),99);check(wait==WAIT_OBJECT_0,"independent reader exits within bound");DWORD code{};check(GetExitCodeProcess(child.get(),&code)&&!code,"actual reader recognized FTN and serialized addressed reply");
    std::vector<sfx::archive::QwkArchiveFile> files;files.push_back({std::string(board)+".MSG",load_bytes(out/(std::filesystem::u8path(board).native()+L".MSG"))});
    if(std::filesystem::exists(out/L"HEADERS.DAT"))files.push_back({"HEADERS.DAT",load_bytes(out/L"HEADERS.DAT")});
    if(std::filesystem::exists(out/L"TODOOR.EXT"))files.push_back({"TODOOR.EXT",load_bytes(out/L"TODOOR.EXT")});
    std::string zip(65536,'\0');std::size_t used{};auto* writer=archive_write_new();check(writer&&archive_write_set_format_zip(writer)==ARCHIVE_OK&&archive_write_open_memory(writer,zip.data(),zip.size(),&used)==ARCHIVE_OK,"real ZIP envelope for independent reader's exact bytes");
    std::uint64_t expanded{};for(const auto& file:files){auto* entry=archive_entry_new();archive_entry_set_pathname(entry,file.name.c_str());archive_entry_set_filetype(entry,AE_IFREG);archive_entry_set_perm(entry,0600);archive_entry_set_size(entry,file.bytes.size());check(archive_write_header(writer,entry)==ARCHIVE_OK&&archive_write_data(writer,file.bytes.data(),file.bytes.size())==static_cast<la_ssize_t>(file.bytes.size()),"pack independent reader member without alterations");expanded+=file.bytes.size();archive_entry_free(entry);}
    check(archive_write_close(writer)==ARCHIVE_OK&&archive_write_free(writer)==ARCHIVE_OK,"close real reader reply envelope");zip.resize(used);save_bytes(server.root/L"ActualReaderReply.REP",zip);
    const auto envelope=sfx::archive::read_qwk_reply_zip(zip,board);check(envelope.result.ok(),"production hostile REP ZIP validation accepts independent reader");std::vector<app::QwkFile> decoded_files;for(const auto& f:envelope.files)decoded_files.push_back({f.name,f.bytes});
    const auto decoded=app::decode_qwk_reply(board,decoded_files);if(!decoded)std::cerr<<"NCM decode: "<<decoded.detail<<'\n';check(bool(decoded)&&decoded.records.size()==3&&files.size()==3,"production codec accepts actual three-member mixed NCM reply and QWKE Drop");
    const auto job=server.root/L"Work"/L"ActualReaderSpool";const auto sd=ins::mutable_descriptor(server.receipt.owner);ins::create_directory(job,sd.get());
    const auto sealed=runtime::seal_qwk_reply_spool(job,server.receipt.owner,decoded,zip.size(),expanded);check(bool(sealed)&&sealed.value,"actual independent reply sealed using shipping Runtime spool");return *sealed.value;
}
std::int64_t scalar(Fixture& f,const char* sql){namespace i=sfx::adapters::storage::internal;i::SqliteConnection db(f.root/L"Data"/L"SFX.DB",SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return db.scalar_int64(sql,"RC9 independent invariant");}
void stored_checks(Fixture& server,Fixture& client){
    const auto netmail=client.root/L"GameNetMail",outbound=client.root/L"GameOutbound";auto sd=ins::mutable_descriptor(client.receipt.owner);ins::create_directory(netmail,sd.get());ins::create_directory(outbound,sd.get());
    const auto payload=netmail/L"777B0104.164";const std::string content("real inter-BBS payload bytes\0end",31);save_bytes(payload,content);
    const auto raw=msg("777B0104.164");check(raw.size()>190&&bool(app::parse_ftn_stored_message(raw)),"zero stored zones resolved by INTL");
    auto unsafe=client.config;unsafe.settings.msg_netmail_enabled=true;unsafe.settings.msg_netmail_directory=(client.root/L"FTN"/L"Inbound").u8string();std::string unsafe_after;
    const auto refused=native::scan_ftn_stored_messages(*client.db,unsafe,client.receipt,unsafe_after,[]{return true;});check(refused.failed&&!refused.queued,"network-controlled Inbound cannot become a trusted local MSG request directory");
    auto wrong=raw;wrong[176]=3;check(!app::parse_ftn_stored_message(wrong),"conflicting header/INTL zone rejected");check(!app::parse_ftn_stored_message(raw.substr(0,180)),"short binary stored header refused");
    save_bytes(netmail/L"1.MSG",raw);client.config.settings.msg_netmail_enabled=true;client.config.settings.msg_netmail_directory=netmail.u8string();client.save(1100);client.apply();client.ready();
    await([&]{return scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='777B0104.164'")==1;},"automatic native engine scans numbered MSG and queues real payload");
    check(std::filesystem::exists(payload)&&std::filesystem::exists(netmail/L"1.MSG"),"both originals survive scanning without remote ACK");
    check(client.engine->scan(),"explicit Scan Outbound accepts stored-message integration");Sleep(1200);check(scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='777B0104.164'")==1,"repeat scan reconciles original immutable request");
    client.engine.reset();auto read_request=raw;read_request[164]=1;read_request[186]=static_cast<char>(static_cast<unsigned char>(read_request[186])|4U);save_bytes(netmail/L"1.MSG",read_request);
    check(client.db->shutdown().succeeded()&&client.db->start().succeeded(),"stored MSG source and queue survive managed storage restart");
    client.config=*client.db->ftn_configuration({true,{}}).value;check(client.config.settings.msg_netmail_enabled&&client.config.settings.msg_netmail_directory==netmail.u8string(),"actual stored MSG configuration survives restart");
    client.engine=std::make_unique<runtime::AlphaFtnEngine>(client.receipt,*client.db,*client.db,id(62000),client.sounds);client.apply();client.ready();Sleep(1200);
    check(scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='777B0104.164'")==1,"restart cannot duplicate a queued stored MSG request");
    check(client.engine->poll("TEST","PEER"),"actual BinkP sends classic attach payload");
    try{await([&]{return !std::filesystem::exists(payload)&&!std::filesystem::exists(netmail/L"1.MSG");},"only committed GOT permits KFS payload cleanup and KillSent MSG cleanup");}
    catch(...){const auto s=server.engine->snapshot(),c=client.engine->snapshot();std::cerr<<"Retained transfer diagnostics: server="<<s.diagnostic<<" listening="<<s.classic_listening<<" port="<<s.classic_port<<" processing="<<s.processing_error<<"; client="<<c.diagnostic<<" target="<<client.config.links.front().port<<" processing="<<c.processing_error<<'\n';throw;}
    std::ifstream received(server.root/L"FTN"/L"Inbound"/L"777B0104.164",std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(received)),{});check(bytes==content,"real peer receives original payload filename and every byte");
    const auto kept=netmail/L"keep.bin";save_bytes(kept,"keep payload");save_bytes(netmail/L"2.MSG",msg("keep.bin",0x111,""));
    await([&]{return scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='keep.bin'")==1;},"second stored request queued");check(client.engine->poll("TEST","PEER"),"send non-KillSent stored request");
    await([&]{std::ifstream in(netmail/L"2.MSG",std::ios::binary);std::string b((std::istreambuf_iterator<char>(in)),{});return b.size()>186&&(static_cast<unsigned char>(b[186])&8);},"non-KillSent MSG is marked Sent after ACK");check(std::filesystem::exists(kept),"no KFS retains attachment");
    const auto changed=netmail/L"changed.bin";save_bytes(changed,"original bytes");save_bytes(netmail/L"3.MSG",msg("changed.bin"));
    await([&]{return scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='changed.bin'")==1;},"changed-source fixture queued snapshot");
    save_bytes(changed,"new producer bytes");check(client.engine->poll("TEST","PEER"),"send retained original staged snapshot");
    await([&]{return scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='changed.bin' AND state=3")==1;},"changed source cannot change staged transmitted content");Sleep(1200);
    check(std::filesystem::exists(changed)&&std::filesystem::exists(netmail/L"3.MSG"),"changed source prevents destructive KFS cleanup and retains request");
    save_bytes(netmail/L"4.MSG",msg("missing.bin"));check(client.engine->scan(),"scan missing attach request");Sleep(1200);check(std::filesystem::exists(netmail/L"4.MSG")&&scalar(client,"SELECT count(*) FROM sfx_ftn_outbound WHERE wire_name='missing.bin'")==0,"missing payload retained, never queued as delivered");
}
void qwk_checks(Fixture& server,Fixture& client){
    app::MessageActor bob{id(3),3,1,1},alice{id(2),2,1,1};
    app::MessageTarget target;target.kind=app::MessageKind::private_ftn;target.remote=app::MessageRemoteTarget{"TEST","2:100/2@test","2:100/1@test","Alice"};
    const auto context=client.db->begin_message(bob,target);check(bool(context),"online NetMail context shares authorization with QWK");
    const auto sent=client.db->commit_message({bob,id(1200),*context.value,"Offline NetMail","Original incoming NetMail\n"});check(bool(sent),"post native NetMail for real inbound export");
    await([&]{return client.db->pending_ftn_scan({true,{}},"TEST",0,16).value->empty();},"engine stages original NetMail packet");check(client.engine->poll("TEST","PEER"),"deliver real inbound NetMail before QWK download");
    await([&]{return scalar(server,"SELECT count(*) FROM sfx_local_messages WHERE kind=1 AND recipient_account_id=2")==1;},"native incoming remote mail in intended local Inbox");
    const auto prefs=server.db->qwk_preferences(alice);check(bool(prefs),"initialize Alice QWK settings");
    app::QwkPacketMetadata metadata;metadata.board_name="RC9 Synthetic";metadata.caller_name="Alice";metadata.created="11-01-2026,12:00:00";
    const auto packet=sfx::archive::prepare_qwk_download(*server.db,alice,id(1201),metadata,[](std::int64_t){return sfx::archive::QwkRecordDate{"11-01-26","12:00","20261101120000+0000"};});
    check(packet.result.ok()&&packet.progress,"prepare real private FTN QWK without changing Read");auto files=members(packet.zip);
    check(files.at("DOOR.ID").find("VERSION = 1.0.0-alpha.2")!=std::string::npos,"DOOR.ID uses actual shared product version");
    check(files.at("TOREADER.EXT").find("AREA 65535 PN")!=std::string::npos,"reader sees separate private NetMail conference");
    const auto& data=files.at("MESSAGES.DAT");check(data.size()>=256&&static_cast<unsigned char>(data[128+123])==255&&static_cast<unsigned char>(data[128+124])==255&&data.substr(128+71,25).find("@2:100/2")==0,"QWKE FTN sender address in fixed subject and proper conference");
    check(data.find("Subject: Offline NetMail")!=std::string::npos&&scalar(server,"SELECT opened FROM sfx_message_recipient_state WHERE recipient_account_id=2")==0,"real subject retained and preparation does not mark mail read");
    const auto folder=server.root/L"ReaderExchange";std::filesystem::create_directory(folder);for(const auto& f:files)save_bytes(folder/std::filesystem::u8path(f.first),f.second);save_bytes(server.root/L"ReaderExchange.QWK",packet.zip);
    check(bool(server.db->commit_qwk_delivery(alice,*packet.progress))&&scalar(server,"SELECT opened FROM sfx_message_recipient_state WHERE recipient_account_id=2")==1,"actual delivered private packet commits owned mail Read only before later offline preference edits");
    auto reply=reply_record(65535,"Bob","@2:100/2","Subject: Offline reply\n\nReply from offline reader\n");reply.message.from="Alice";reply.message.private_mail=true;reply.message.reference=static_cast<std::uint32_t>(std::stoul(data.substr(129,7)));
    const auto source=std::make_shared<ReplySource>(packet.progress->initial.policy.board_id,std::vector<app::QwkReplyRecord>{reply});
    const auto imported=server.db->import_qwk_reply(alice,source);if(!imported)std::cerr<<"REP: "<<imported.status.operation<<'\n';check(imported&&imported.value->messages==1,"authorized offline FTN reply atomically creates normal message and scan intent");
    const auto replay=server.db->import_qwk_reply(alice,source);check(replay&&replay.value->replayed,"identical uploaded REP receipt cannot duplicate outbound NetMail");
    const auto invalid=server.db->import_qwk_reply(bob,source);check(!invalid,"forged caller cannot send another user's offline reply");
    await([&]{return server.db->pending_ftn_scan({true,{}},"TEST",0,16).value->empty();},"offline reply follows ordinary outbound scanner");check(client.engine->poll("TEST","PEER"),"peer retrieves real QWK-origin NetMail packet");
    await([&]{return scalar(client,"SELECT count(*) FROM sfx_local_messages WHERE kind=1 AND recipient_account_id=3")==1;},"offline reply travels through native BinkP and reaches Bob Inbox");
    if(!actual_reader.empty()){
        const auto actual=reader_reply(server,folder,source->metadata.board_id);const auto accepted=server.db->import_qwk_reply(alice,actual);if(!accepted)std::cerr<<"NCM import: "<<accepted.status.operation<<'\n';check(accepted&&accepted.value->messages==2&&accepted.value->commands==1,"actual NoCarrierMail private/public replies and Drop import atomically through sealed source");
        check(scalar(server,"SELECT count(*) FROM sfx_local_messages WHERE kind=0 AND CAST(subject AS TEXT)='Public alongside FTN NetMail reply'")==1,"actual mixed reader upload keeps public content separate from private NetMail");
        const auto dropped=server.db->message_scan_preference(alice,{2});check(dropped&&!dropped.value->unread&&!dropped.value->to_you,"QWKE Drop changes only authenticated caller's public scan flags");
        await([&]{return server.db->pending_ftn_scan({true,{}},"TEST",0,16).value->empty();},"actual NCM reply staged by ordinary native scanner");check(client.engine->poll("TEST","PEER"),"retrieve actual NCM-origin reply over BinkP");
        await([&]{return scalar(client,"SELECT count(*) FROM sfx_local_messages WHERE kind=1 AND recipient_account_id=3")==2;},"actual NCM reply reaches peer Inbox through complete native path");
        const std::vector<app::QwkFile> only_control{{source->metadata.board_id+".MSG",data.substr(0,128)},{"TODOOR.EXT","AREA 2 a\r\n"}};
        auto command_files=only_control;command_files[0].bytes=source->metadata.board_id+std::string(128-source->metadata.board_id.size(),' ');
        const auto control=app::decode_qwk_reply(source->metadata.board_id,command_files);check(control&&control.records.size()==1,"control-only QWKE upload has no fabricated message record");
        const auto private_job=server.root/L"Work"/L"ControlOnlySpool";auto sd=ins::mutable_descriptor(server.receipt.owner);ins::create_directory(private_job,sd.get());
        const auto control_source=runtime::seal_qwk_reply_spool(private_job,server.receipt.owner,control,100,command_files[0].bytes.size()+command_files[1].bytes.size());check(bool(control_source),"small genuine control-only envelope seals without artificial padding");
        const auto control_result=server.db->import_qwk_reply(alice,*control_source.value);check(control_result&&control_result.value->commands==1&&!control_result.value->messages,"control-only upload applies own public Add without posting content");
        const auto added=server.db->message_scan_preference(alice,{2});check(added&&added.value->unread&&added.value->to_you,"QWKE Add restores authenticated caller's scan flags");
        command_files[1].bytes="EXEC program\r\n";check(!app::decode_qwk_reply(source->metadata.board_id,command_files),"offline control input never executes arbitrary instructions");
        command_files[1].bytes="AREA 2 a\r\nAREA 2 D\r\n";check(!app::decode_qwk_reply(source->metadata.board_id,command_files),"ambiguous duplicate offline area controls rejected");
    }
    auto blank=reply;blank.message.reference=0;blank.message.body="Subject: \n\nBlank subject offline body\n";check(bool(server.db->import_qwk_reply(alice,std::make_shared<ReplySource>(source->metadata.board_id,std::vector<app::QwkReplyRecord>{blank}))),"blank subject remains blank through FTN offline compose");
    auto forged=reply;forged.message.from="Sysop";check(!server.db->import_qwk_reply(alice,std::make_shared<ReplySource>(source->metadata.board_id,std::vector<app::QwkReplyRecord>{forged})),"offline From cannot claim Sysop authority");
    auto wrong=reply;wrong.message.conference=65534;check(!server.db->import_qwk_reply(alice,std::make_shared<ReplySource>(source->metadata.board_id,std::vector<app::QwkReplyRecord>{wrong})),"private NetMail cannot be posted to a public or unknown conference");
}
void paging_checks(Fixture& f){
    {app::MessageActor sysop{id(1),1,1,1};sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("T");stream.script_non_secret_text("N");stream.script_non_secret_text("\r");app::StreamCallerTerminal terminal(stream);PauseObservedTerminal observed(terminal);app::MessageQuestionTerminal framed(observed,[]{return true;},[&](std::string_view s){return stream.write_some(s).bytes_written==s.size();});app::MessageComposePorts compose{*f.db,framed,[&]{return std::optional<app::MessageActor>{sysop};},[]{return id(1390);},[]{return app::MessageEditorDimensions{true,80,24};},[&]{return terminal.read_editor_byte();},[&](std::string_view s){return stream.write_some(s).bytes_written==s.size();},{}};app::MessageCallerSession session({compose,[]{return std::string("Sysop");},[](auto){return std::string("test date");},[]{return true;},{}});check(session.initialize()&&session.selection()->echo.id.value==1,"empty conference fixture selects first echo");const auto action=session.action(app::MenuAction::message_read);check(action&&action->empty(),"empty conference action completes after Enter acknowledgment");const auto output=terminal_visible(stream.output());check(output.find("No available messages in this conference.")!=output.npos&&observed.pauses==1,"empty conference notice waits at a real terminal pause before returning");save_bytes(f.root/"empty-conference.txt",stream.output());}
    app::MessageActor alice{id(2),2,1,1};app::MessageTarget target{app::MessageKind::public_echo,{2},{},{}};const auto context=f.db->begin_message(alice,target);check(bool(context),"paged message context");
    std::string body;for(unsigned i=0;i<45;++i)body+="Row "+std::to_string(i)+std::string(65,'x')+'\n';const auto saved=f.db->commit_message({alice,id(1300),*context.value,"Page header must remain visible",body});check(bool(saved),"long wrapped public message committed");
    for(const bool ansi:{false,true})for(const auto columns:{40U,80U}){
        sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text(std::string(100,'C')+"Q");
        app::StreamCallerTerminal terminal(stream);terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        app::MessageQuestionTerminal framed(terminal,[=]{return ansi;},[&](std::string_view s){return stream.write_some(s).bytes_written==s.size();});
        app::MessageReaderPorts ports{*f.db,framed,[&]{return std::optional<app::MessageActor>{alice};},{},[](auto){return std::string("test date");},[]{return true;},24,true,{},[]{return id(1301);},[=]{return app::MessageEditorDimensions{ansi,columns,24};},[]{return std::uint8_t{6};}};
        app::MessageQueryOptions query;query.mode=app::MessageQueryMode::echo;query.echo={2};const auto outcome=app::read_local_messages(ports,query,false,saved.value->area_number);
        check(outcome==app::MessageReaderOutcome::cancelled,"long message read and navigation complete with actual terminal");const auto raw_output=stream.output();save_bytes(f.root/("reader-"+std::to_string(columns)+(ansi?"-ansi.txt":"-plain.txt")),raw_output);
        const auto output=terminal_visible(raw_output);
        const auto pause=output.find("<Pause>");check(pause!=output.npos&&output.find("Subject:")<pause&&output.find("Row 0")<pause&&pause<output.find("Row 20"),"compact four-line header plus first body page pauses before screen overflow");
        check(output.find("Network:")==output.npos&&output.find("From address:")==output.npos,"compact header spends no rows on duplicate FTN metadata");
        check(output.find("Row 44")!=output.npos,"all continued message rows remain visible");
    }
    for(unsigned n=0;n<20;++n){app::MessageActor sysop{id(1),1,1,1};const auto local=f.db->begin_message(sysop,{app::MessageKind::private_local,{},2,{}});check(local&&bool(f.db->commit_message({sysop,id(1400+n),*local.value,std::string(70,'x'),"Inbox layout test\n"})),"long-subject inbox fixture message");}
    for(const auto columns:{40U,80U}){sfx::test::DeterministicInteractiveByteStreamFake stream;stream.script_non_secret_text("Q\r");app::StreamCallerTerminal terminal(stream);app::MessageQuestionTerminal framed(terminal,[]{return true;},[&](std::string_view s){return stream.write_some(s).bytes_written==s.size();});app::MessageReaderPorts ports{*f.db,framed,[&]{return std::optional<app::MessageActor>{alice};},{},{},[]{return true;},24,true,{},[]{return id(1450);},[=]{return app::MessageEditorDimensions{true,columns,24};},{}};app::MessageQueryOptions query;query.mode=app::MessageQueryMode::inbox;check(app::read_local_messages(ports,query,true)==app::MessageReaderOutcome::cancelled,"inbox list returns through its normal prompt");const auto raw=stream.output(),visible=terminal_visible(raw);check(raw.find("\x1b[2J\x1b[H")!=raw.npos&&visible.rfind("MESSAGE HEADERS",0)==0,"ANSI inbox clears directly without the plain-mode blank-line gap");check(visible.find("...")!=visible.npos,"oversized inbox headers are visibly clipped");std::size_t begin{};unsigned lines{};for(auto end=visible.find('\n');end!=visible.npos;end=visible.find('\n',begin)){const auto size=end-begin-(end>begin&&visible[end-1]=='\r'?1:0);check(size<columns,"every inbox row fits terminal width without wrapping");++lines;begin=end+1;}check(lines<=24,"inbox header page fits the available screen height");save_bytes(f.root/("inbox-"+std::to_string(columns)+".txt"),raw);}
}
}
int main(int argc,char** argv){try{
    check(argc==2||argc==3,"new isolated RC9 fixture root required");if(argc==3)actual_reader=std::filesystem::absolute(argv[2]);const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve earlier RC9 evidence");std::filesystem::create_directories(root);
    Fixture server(root/L"Server",1,2),client(root/L"Client",2,1);server.ready();client.ready();client.config.links.front().port=server.engine->snapshot().classic_port;client.save(1000);client.apply();client.ready();
    stored_checks(server,client);qwk_checks(server,client);paging_checks(server);
    server.engine->stop();client.engine->stop();runtime::close_alpha_rlogin_process();std::cout<<"PASS "<<checks<<" RC9 native .MSG/ACK/cleanup, QWK FTN delivery/ownership/replay/version and header/wrapped-screen paging checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}}
