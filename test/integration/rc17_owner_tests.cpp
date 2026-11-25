// All data and native objects are synthetic, in a new isolated fixture.
#define main rc17_retained_engine_main
#include "ftn_engine_tests.cpp"
#undef main
#include "sfx/platform/win32/ftn_native_custody.hpp"
#include "sfx/platform/win32/ftn_processed_expiry.hpp"
#include "sfx/platform/win32/ftn_native_tic.hpp"
#include <fstream>
namespace {
using Key=app::TerminalNavigationKey;
void key_checks(){
    for(const auto& prefix:{std::string("\x1b["),std::string("\x1bO"),std::string("\x1b[1;")}){
        sfx::test::DeterministicInteractiveByteStreamFake bytes;
        for(char c:std::string("ABDC"))for(char part:prefix+std::string(1,c))bytes.script_non_secret_text(std::string(1,part));
        bytes.script_non_secret_text("Q");app::StreamCallerTerminal terminal(bytes);
        for(auto key:{Key::up,Key::down,Key::left,Key::right}){
            const app::TerminalKeyRequest request{"message.reader.command"," ",{'Q'},'Q',false,true,true};
            const auto result=terminal.read_key(request);
            check(result.navigation==key&&app::valid_terminal_key_result(request,result),"fragmented CSI/SS3 arrow decoded without Enter");
        }
        check(terminal.read_key({"message.reader.command"," ",{'Q'},'Q'}).key=='Q',"arrows leave next ordinary command intact");
    }
    sfx::test::DeterministicInteractiveByteStreamFake bytes;bytes.script_non_secret_text("\x1b");bytes.script_read_outcome(app::InteractiveStreamOutcome::timed_out);
    app::StreamCallerTerminal terminal(bytes);check(terminal.read_key({"message.reader.command"," ",{'Q'},'Q',false,true,true}).outcome==app::TerminalOutcome::cancelled,"bare Escape remains cancellation at transport boundary");
}
struct Observed final:app::ICallerTerminal{
    app::ICallerTerminal& terminal;unsigned clears{};std::vector<std::string> frames;std::vector<std::size_t> footer_lines;std::string frame;std::function<void()> observe_key;
    std::string cursor_position;
    std::vector<std::pair<std::string,std::size_t>> pauses;
    explicit Observed(app::ICallerTerminal& t):terminal(t){}
    app::TerminalOutcome set_text_style(app::TerminalTextStyle s)override{return terminal.set_text_style(s);}
    app::TerminalOutcome clear_screen()override{++clears;frame.clear();cursor_position.clear();return terminal.clear_screen();}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& r)override{return terminal.read_visible(r);}
    app::TerminalInputResult read_secret(const app::TerminalInputRequest& r)override{return terminal.read_secret(r);}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest& r)override{
        frames.push_back(frame);
        if(observe_key)observe_key();
        const auto lines=std::count(frame.begin(),frame.end(),'\n');
        if(r.identity=="message.reader.command")footer_lines.push_back(1+lines);
        if(r.identity=="message.reader.more")pauses.emplace_back(cursor_position,lines);
        return terminal.read_key(r);
    }
    app::TerminalOutcome present(const app::TerminalPresentationRequest& r)override{return terminal.present(r);}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& r)override{
        for(const auto& op:r.program->operations){
            if(op.kind==app::TerminalDisplayOperationKind::ansi_sequence&&!op.bytes.empty()&&op.bytes.back()=='H'){
                cursor_position=op.bytes;
                if(op.bytes=="\x1b[H")frame.clear();
            }else if(op.kind==app::TerminalDisplayOperationKind::cp437_text)frame+=op.bytes;
            else if(op.kind==app::TerminalDisplayOperationKind::logical_newline)frame+='\n';
        }
        return terminal.present_display(r);
    }
    app::TerminalOutcome pause(const app::TerminalPauseRequest& r)override{return terminal.pause(r);}
};
void reader_checks(Fixture& f){
    app::MessageActor alice{id(2),2,1,1};auto context=f.db->begin_message(alice,{app::MessageKind::public_echo,{2},{},{}});check(bool(context),"reader fixture context");
    std::string body;for(unsigned i=0;i<55;++i)body+="ROW "+std::to_string(i)+" - reader navigation\n";
    auto first=f.db->commit_message({alice,id(17001),*context.value,"First navigation message",body});
    context=f.db->begin_message(alice,{app::MessageKind::public_echo,{2},{},{}});
    auto second=f.db->commit_message({alice,id(17002),*context.value,"Second navigation message",body});check(first&&second,"two long messages committed");
    for(bool ansi:{false,true})for(unsigned columns:{40U,80U,132U})for(unsigned height:{18U,24U}){
        sfx::test::DeterministicInteractiveByteStreamFake bytes;bytes.script_non_secret_text("\x1b[B\x1b[A\x1b[C\x1b[D"+std::string(10,'C')+"\x1b[A\x1b[BCQ");
        app::StreamCallerTerminal terminal(bytes);terminal.set_text_style(ansi?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        Observed observed(terminal);app::MessageQuestionTerminal framed(observed,[=]{return ansi;},[&](std::string_view s){return bytes.write_some(s).bytes_written==s.size();});
        app::MessageReaderPorts ports{*f.db,framed,[&]{return std::optional<app::MessageActor>{alice};},{},[](auto){return std::string("test date");},[]{return true;},24,true,{},[]{return id(17003);},[=]{return app::MessageEditorDimensions{ansi,columns,height};},[]{return std::uint8_t{6};}};
        app::MessageQueryOptions query;query.echo={2};query.mode=app::MessageQueryMode::echo;
        check(app::read_local_messages(ports,query,false,first.value->area_number)==app::MessageReaderOutcome::cancelled,"reader arrows work at pause and footer with ordinary commands retained");
        check(observed.frames.size()>5&&observed.frames[0].rfind("Subject: First",0)==0,"first page begins at compact header");
        check(observed.frames[1].rfind("To: ",0)==0&&observed.frames[2].rfind("Subject: First",0)==0,"Down moves one physical row and Up restores it");
        check(observed.frames[3].find("Subject: Second navigation")!=std::string::npos&&observed.frames[4].find("Subject: First navigation")!=std::string::npos,"Right/Left change messages immediately at a pause");
        const auto count=static_cast<unsigned>(std::count(observed.frames[0].begin(),observed.frames[0].end(),'\n'));
        check(count==height-1,"pause fills available rows and reserves exactly one prompt row");
        if(ansi){
            const auto expected="\x1b["+std::to_string(height)+";1H";
            check(std::all_of(observed.pauses.begin(),observed.pauses.end(),[&](const auto& pause){return pause.first==expected;}),
                "ANSI pause remains on the reserved terminal row");
            check(std::any_of(observed.pauses.begin(),observed.pauses.end(),[&](const auto& pause){return pause.second<height-1;}),
                "ANSI pause remains anchored while scrolling a short final viewport");
        }
        check(!ansi||observed.clears==3,"ANSI row navigation avoids clear-screen flashes; only new messages clear");
        check(!observed.footer_lines.empty()&&std::all_of(observed.footer_lines.begin(),observed.footer_lines.end(),[&](auto rows){return rows<=height;}),"actual generated wrapped footer fits with the final viewport without terminal scroll");
        check(ansi||bytes.output().find('\x1b')==std::string::npos,"plain clients receive no ANSI framing");
        std::ofstream output(f.root/("reader-"+std::to_string(columns)+"-"+std::to_string(height)+(ansi?"-ansi.txt":"-plain.txt")),std::ios::binary);output<<bytes.output();
    }
    const auto pref=f.db->message_scan_preference(alice,{2});check(pref&&pref.value->highest_opened==second.value->area_number,"row navigation and older revisits do not rewind last read");
}
void location_checks(Fixture& f){
    app::MessageActor alice{id(2),2,1,1},bob{id(3),3,1,1};
    check(f.db->remember_message_area(alice,{2}).succeeded(),"successful accessible conference selection persisted");
    check(f.db->saved_message_area(alice).value->value().echo.id.value==2,"saved location is stable echo identity");
    check(!f.db->saved_message_area(bob).value->has_value(),"other callers do not inherit location");
    sfx::test::DeterministicInteractiveByteStreamFake bytes;app::StreamCallerTerminal terminal(bytes);
    app::MessageComposePorts compose{*f.db,terminal,[&]{return std::optional<app::MessageActor>{alice};},[]{return id(17020);},[]{return app::MessageEditorDimensions{};},{},{},{}};
    app::MessageCallerSession session({compose,[]{return std::string("Alice");},{},{},{}});
    check(session.initialize()&&session.selection()->echo.id.value==2,"new caller session restores selected conference");
    check(f.db->shutdown().succeeded()&&f.db->start().succeeded()&&f.db->saved_message_area(alice).value->value().echo.id.value==2,"saved location survives database reopen/disconnect");
    {sfx::adapters::storage::internal::SqliteConnection db(f.root/L"Data/SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);db.exec("UPDATE sfx_local_echoes SET enabled=0 WHERE echo_id=2","synthetic access loss");}
    check(!f.db->saved_message_area(alice).value->has_value(),"disabled saved echo is never exposed");
    app::MessageCallerSession fallback({compose,[]{return std::string("Alice");},{},{},{}});check(fallback.initialize()&&fallback.selection()->echo.id.value!=2,"unavailable location falls back to an accessible echo");
    check(!f.db->remember_message_area(alice,{2}),"disabled selection cannot be saved");
    {sfx::adapters::storage::internal::SqliteConnection db(f.root/L"Data/SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);db.exec("UPDATE sfx_local_echoes SET enabled=1 WHERE echo_id=2","restore synthetic echo");}
}
void artwork_checks(Fixture& f){
    app::MessageActor alice{id(2),2,1,1};const auto context=f.db->begin_message(alice,{app::MessageKind::public_echo,{2},{},{}});check(bool(context),"colored navigation context");
    app::MessageSource art{app::MessageSourceFormat::ansi_canvas,1,{}};for(unsigned n=0;n<50;++n){auto row="RED ART "+std::to_string(n);row+=std::string(79-row.size(),' ');art.bytes+="\x1b[31m"+row+"Z\r\n";}
    app::CommitMessageRequest request{alice,id(21000),*context.value,"Colored scrolling",{}};request.original_source=art;const auto saved=f.db->commit_message(request);check(bool(saved),"authored ANSI source committed without changing its bytes");
    sfx::test::DeterministicInteractiveByteStreamFake bytes;bytes.script_non_secret_text("\x1b[B\x1b[A"+std::string(20,'C')+"Q");app::StreamCallerTerminal terminal(bytes);terminal.set_text_style(app::TerminalTextStyle::ogsf_ansi);
    Observed observed(terminal);unsigned snapshots{};observed.observe_key=[&]{if(snapshots<3){std::ofstream snapshot(f.root/("artwork-view-"+std::to_string(snapshots)+".txt"),std::ios::binary);snapshot<<bytes.output();}++snapshots;};
    app::MessageQuestionTerminal framed(observed,[]{return true;},[&](std::string_view s){return bytes.write_some(s).bytes_written==s.size();});
    app::MessageReaderPorts ports{*f.db,framed,[&]{return std::optional<app::MessageActor>{alice};},{},[](auto){return std::string("test date");},[]{return true;},24,true,{},[]{return id(21001);},[]{return app::MessageEditorDimensions{true,80,24};},[]{return std::uint8_t{6};}};
    app::MessageQueryOptions query;query.mode=app::MessageQueryMode::echo;query.echo={2};check(app::read_local_messages(ports,query,false,saved.value->area_number)==app::MessageReaderOutcome::cancelled,"colored artwork scrolls and pages using actual terminal");
    const auto& output=bytes.output();std::string visible;std::vector<unsigned> colors;unsigned color=7;
    for(std::size_t at=0;at<output.size();){const auto c=output[at++];if(c!='\x1b'){visible+=c;colors.push_back(color);continue;}check(at<output.size()&&output[at++]=='[',"bounded artwork CSI");const auto begin=at;while(at<output.size()&&(output[at]<'@'||output[at]>'~'))++at;check(at<output.size(),"complete artwork CSI");const auto final=output[at++];if(final!='m')continue;auto args=output.substr(begin,at-begin-1);for(std::size_t pos=0;pos<=args.size();){const auto end=args.find(';',pos);const auto field=args.substr(pos,end==args.npos?end:end-pos);const auto n=field.empty()?0:std::stoi(field);if(!n)color=7;else if(n>=30&&n<=37){constexpr unsigned map[]={0,4,2,6,1,5,3,7};color=map[n-30];}if(end==args.npos)break;pos=end+1;}}
    unsigned found{};for(auto at=visible.find("RED ART");at!=visible.npos;at=visible.find("RED ART",at+7)){++found;check(colors[at]==4,"authored red survives every viewport repaint and pause; caller gold cannot override");}check(found>=50,"all colored artwork rows remain reachable");
    std::ofstream evidence(f.root/L"artwork-navigation-ansi.txt",std::ios::binary);evidence<<output;
}
void engine_status_checks(const std::filesystem::path& root){
    Fixture f(root,1,2);f.ready();app::FtnMessageEcho echo;echo.network="TEST";echo.tag="STATUS.TEST";echo.local_echo=2;echo.description="Synthetic status";f.config.message_echoes.push_back(echo);f.config.permissions.push_back({"TEST","PEER","STATUS.TEST",false,true,true,false});f.save(22000);
    app::MessageActor alice{id(2),2,1,1};const auto context=f.db->begin_message(alice,{app::MessageKind::public_echo,{2},{},{}});check(bool(context),"status test message context");const auto saved=f.db->commit_message({alice,id(22001),*context.value,"Synthetic status","Retained outbound body\n"});check(bool(saved),"retain a post written before policy changed");
    f.config.message_echoes.back().require_real_name=true;f.save(22002);f.apply();f.ready();
    check(f.engine->scan(),"outbound failure is exercised by actual background processor");
    const auto label="Outbound message #"+std::to_string(saved.value->id.value);
    const auto occurrences=[&]{const auto state=f.engine->snapshot();return std::count_if(state.activity.begin(),state.activity.end(),[&](const auto& event){return event.message.rfind(label+": Set a valid Real Name",0)==0;});};
    await([&]{return occurrences()==1&&!f.engine->snapshot().processing_error.empty();},"actual engine logs exact outbound message and corrective action");
    check(f.engine->scan(),"repeat unchanged failing source");Sleep(4200);check(occurrences()==1,"unchanged source failure does not repeatedly flood activity log");
    {sfx::adapters::storage::internal::SqliteConnection db(f.root/L"Data/SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);db.exec("UPDATE sfx_account_profiles SET real_name='Alice Real' WHERE account_id=2","correct synthetic caller profile");}
    check(f.engine->scan(),"retry corrected original message");await([&]{const auto state=f.engine->snapshot();return state.scanned_messages==1&&state.processing_error.empty();},"correction resumes same message and clears processing attention summary");f.engine->stop();
}
app::FtnIngressReceipt receive(Fixture& f,unsigned number,std::string name,std::string bytes){
    const auto flat=native::FilePayloadDirectory::private_store(f.root/L"FTN/Inbound",f.receipt.owner);
    app::FtnIngressReceipt r;r.id=id(number);r.session=id(number+100);r.network="TEST";r.link="PEER";r.proof=app::FtnIngressProof::inbound_cram;r.configuration_revision=f.config.revision;r.file={name,bytes.size(),1700000000+number};
    {auto receiving=native::FtnNativeReceive::open(flat,*f.db,r,[]{return true;},&flat);receiving.append(bytes);receiving.finish();r=receiving.receipt();}
    return r;
}
void inbound_checks(Fixture& f){
    auto loose=receive(f,18000,"20meg.zip","ordinary game/test payload");
    const auto pending=f.db->pending_ftn_ingress_after({true,{}},16,{});check(pending&&std::none_of(pending.value->begin(),pending.value->end(),[&](const auto& r){return r.id==loose.id;}),"loose ZIP is not polled as message toss work");
    std::filesystem::remove(f.root/L"FTN/Inbound/20meg.zip");const auto unused=native::toss_ftn_native_receipt(*f.db,f.config,f.receipt,loose,[]{return true;});check(unused.reason.empty()&&!unused.held,"Sysop removal of loose file causes no missing packet retry");
    auto missing=receive(f,18100,"missing.pkt","synthetic packet removed before toss");std::filesystem::remove(f.root/L"FTN/Inbound/missing.pkt");
    const auto held=native::toss_ftn_native_receipt(*f.db,f.config,f.receipt,missing,[]{return true;});check(held.held&&!held.processed&&held.reason=="ftn_inbound_file_missing_or_changed","missing packet is held truthfully once without false import");
    const auto next=f.db->pending_ftn_ingress_after({true,{}},16,{});check(next&&std::none_of(next.value->begin(),next.value->end(),[&](const auto& r){return r.id==missing.id;}),"missing packet no longer loops in automatic processing");
    auto tic=receive(f,18200,"removed.tic","synthetic removed TIC");std::filesystem::remove(f.root/L"FTN/Inbound/removed.tic");
    const auto lost_tic=native::publish_ftn_native_tic(*f.db,*f.db,f.config,f.receipt,tic,[]{return true;});check(lost_tic.held&&!lost_tic.processed,"missing unjournaled TIC also stops automatic retry without false publication");
}
void scan_checks(Fixture& f){
    app::FtnMessageEcho echo;echo.network="TEST";echo.tag="RC17.TEST";echo.description="Synthetic";echo.local_echo=2;f.config.message_echoes.push_back(echo);f.config.permissions.push_back({"TEST","PEER","RC17.TEST",false,true,true,false});f.save(19000);
    app::MessageActor alice{id(2),2,1,1};auto context=f.db->begin_message(alice,{app::MessageKind::public_echo,{2},{},{}});check(bool(context),"FTN public compose context");
    auto saved=f.db->commit_message({alice,id(19001),*context.value,"Synthetic outbound","Outbound message body\n"});check(bool(saved),"outbound source retained");
    f.config.message_echoes.back().require_real_name=true;f.save(19003);
    auto refused=f.db->begin_message(alice,{app::MessageKind::public_echo,{2},{},{}});
    check(!refused&&refused.status.operation==app::kEchoRealNameRequired,"missing real name refused before opening editor");
    const auto refused_commit=f.db->commit_message({alice,id(19004),*context.value,"Already composing","Draft must not bypass current policy\n"});
    check(!refused_commit,"stale compose context cannot bypass real-name rule at final save");
    auto scan=f.db->pending_ftn_scan({true,{}},"TEST",0,4);check(scan&&scan.value->size()==1&&scan.value->front().reason=="ftn_scan_real_name_required","missing required real name gets exact actionable diagnosis");
    {sfx::adapters::storage::internal::SqliteConnection db(f.root/L"Data/SFX.DB",SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);db.exec("UPDATE sfx_account_profiles SET real_name='Alice Real' WHERE account_id=2","synthetic owner correction");}
    check(bool(f.db->begin_message(alice,{app::MessageKind::public_echo,{2},{},{}})),"correct profile allows a new compose");
    scan=f.db->pending_ftn_scan({true,{}},"TEST",0,4);check(scan&&scan.value->size()==1&&scan.value->front().reason.empty(),"corrected source retries same retained message");
    const auto result=native::scan_ftn_native_intent(*f.db,f.config,f.receipt,scan.value->front(),[]{return true;});check(result.processed,"corrected outgoing source publishes packet");
    auto jobs=f.db->claim_ftn_jobs({true,{}},"TEST","PEER",id(60000),id(19002),4,false);check(jobs&&jobs.value->size()==1,"one immutable packet job queued");
    const auto path=f.root/L"FTN/Outbound/TEST"/std::filesystem::u8path(jobs.value->front().file.name);check(std::filesystem::exists(path)&&path.extension()==L".pkt","new outbound packet is an actual .pkt with its wire filename");
}
void processed_checks(Fixture& f){
    for(unsigned n=0;n<2;++n){
        app::FtnPacket packet;packet.header.origin=*app::parse_ftn_address("2:100/2@test");packet.header.destination=*app::parse_ftn_address("2:100/1@test");packet.header.year=2026;packet.header.month=11;packet.header.day=3;
        app::FtnPackedMessage m;m.origin_net=m.destination_net=100;m.origin_node=2;m.destination_node=1;m.date="03 Nov 26  12:00:00";m.from="Synthetic Remote";m.to="All";m.subject="Recognizable original";m.text="AREA:RC17.TEST\r\1MSGID: 2:100/2 rc17-name-"+std::to_string(n)+"\r\1CHRS: CP437 2\rSynthetic content\r";packet.messages.push_back(m);
        const auto wire=app::encode_ftn_packet(packet);check(wire.has_value(),"native synthetic packet encoded");auto received=receive(f,20000+n,"original.pkt",*wire);
        const auto tossed=native::toss_ftn_native_receipt(*f.db,f.config,f.receipt,received,[]{return true;});check(tossed.processed&&tossed.imported==1,"actual packet imports into configured echo");
        app::FtnRetainedOriginal original;original.ingress=received.id;original.network="TEST";original.wire_name=received.file.name;std::string resolved;
        {auto source=native::open_ftn_received(*f.db,f.receipt,received,[]{return true;});check(source.path().extension()==L".pkt"&&source.path().parent_path().filename()==L"TEST","processed source keeps recognizable filename/type in network custody");
        check((n==0&&source.name()=="original.pkt")||(n==1&&source.name().rfind("original-",0)==0),"same-name originals are disambiguated without overwriting");original.native=source.identity();resolved=source.name();}
        auto cancelled=std::make_shared<std::atomic_bool>(false);auto expiry=native::FtnProcessedExpiry::inspect(f.receipt,original,cancelled);check(expiry->observation()==native::FtnProcessedExpiry::Observation::exact,"retention finds exact native identity under recognizable/collision filename");expiry.reset();
        if(n){const auto directory=native::FilePayloadDirectory::private_store(f.root/L"FTN/Processed/TEST",f.receipt.owner);{auto legacy=directory.read(resolved,true);legacy.rename_to(directory,native::ftn_native_artifact_name(received.id));}auto reopened=native::open_ftn_received(*f.db,f.receipt,received,[]{return true;});check(reopened.identity().sha256==original.native.sha256,"legacy .dat original remains byte/identity compatible");}
    }
}
}
int main(int argc,char** argv)try{
    check(argc==2,"fresh isolated RC17 evidence root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve all prior fixtures");std::filesystem::create_directories(root);
    key_checks();Fixture f(root/L"Server",1,2);f.ready();f.engine->stop();
    reader_checks(f);artwork_checks(f);location_checks(f);inbound_checks(f);scan_checks(f);processed_checks(f);engine_status_checks(root/L"Status");
    runtime::close_alpha_rlogin_process();std::cout<<"PASS "<<checks<<" RC17 reader/location/inbound/scan/native naming checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
