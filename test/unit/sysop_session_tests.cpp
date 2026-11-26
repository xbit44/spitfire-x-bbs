#include "sfx/application/sysop_session.hpp"
#include "sfx/application/terminal_stream.hpp"
#include "sfx/application/chat_session.hpp"
#include <algorithm>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <set>
namespace app=sfx::application;
unsigned checks{};
void check(bool value,const char* label){++checks;if(!value)throw std::runtime_error(label);}
struct Wire:app::IInteractiveSessionByteStream {
    std::string input,output,frame;std::size_t position{};bool sampled{};
    std::function<void(const std::string&)> capture;
    app::InteractiveStreamReadResult read_some(std::size_t n) override {
        if(!sampled){sampled=true;if(capture)capture(frame);}
        if(position>=input.size())return {app::InteractiveStreamOutcome::disconnected,{}};
        const auto count=std::min(n,input.size()-position);std::vector<std::uint8_t> bytes(input.begin()+position,input.begin()+position+count);position+=count;return {app::InteractiveStreamOutcome::completed,std::move(bytes)};
    }
    app::InteractiveStreamWriteResult write_some(std::string_view bytes) override {output+=bytes;frame+=bytes;return {app::InteractiveStreamOutcome::completed,bytes.size()};}
    void feed(std::string s){input=std::move(s);position=0;sampled=false;}
};
struct Terminal:app::ICallerTerminal {
    Wire wire;app::StreamCallerTerminal native{wire,{app::SecretInputEchoMode::masked,7,7}};
    std::deque<std::string> script;unsigned confirmations{};
    std::string take(){if(script.empty())throw std::runtime_error("script exhausted");auto s=script.front();script.pop_front();return s;}
    app::TerminalOutcome set_text_style(app::TerminalTextStyle s) override{return native.set_text_style(s);}
    app::TerminalOutcome clear_screen() override{wire.frame.clear();return native.clear_screen();}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& r) override {wire.feed(take()+"\r");return native.read_visible(r);}
    app::TerminalInputResult read_secret(const app::TerminalInputRequest& r) override {wire.feed("SfxSynthetic!83\r");return native.read_secret(r);}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest& r) override {if(r.identity.find("confirm")!=r.identity.npos)++confirmations;wire.feed(take()+"\r");return native.read_key(r);}
    app::TerminalOutcome present(const app::TerminalPresentationRequest& r) override{return native.present(r);}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& r) override{return native.present_display(r);}
    app::TerminalOutcome pause(const app::TerminalPauseRequest& r) override{wire.feed("\r");return native.pause(r);}
};
struct Fixture {
    Terminal terminal;app::SysopSessionPorts ports{terminal};bool authorized{true},ansi{};unsigned mutations{},queries{},polls{},jobs{},rows{25},columns{80};
    app::LocalAccountSnapshot account;app::FileConfiguration config;std::vector<app::ManagedFile> files;
    std::vector<app::FileManagementTask> tasks;std::vector<std::string> frames;
    Fixture(bool color):ansi(color){
        terminal.set_text_style(color?app::TerminalTextStyle::ogsf_ansi:app::TerminalTextStyle::plain);
        terminal.wire.capture=[&](const std::string& s){frames.push_back(s);};
        ports.authorized=[&]{return authorized;};ports.ansi=[&]{return ansi;};ports.hotkeys=[]{return false;};ports.rows=[&]{return rows;};ports.columns=[&]{return columns;};ports.write=[&](std::string_view s){terminal.wire.write_some(s);return true;};ports.artwork=[]{return false;};
        account.code=app::LocalAccountResultCode::ok;account.policy.revision=1;account.policy.new_user_level=10;account.policy.sysop_threshold=999;
        app::AccountRecord a;a.account_id=42;a.display_name="TESTCALLER";a.revision=1;a.security_level=std::uint16_t{10};a.purge_allowed=true;a.profile.location="Portland, OR";a.profile.sex='X';account.accounts.push_back(a);
        ports.accounts=[&](const app::LocalAccountQuery& q){++queries;if(!authorized)return app::LocalAccountSnapshot{app::LocalAccountResultCode::unauthorized};auto result=account;if(q.account_id&&q.account_id!=42)result.accounts.clear();return result;};
        ports.account_change=[&](app::LocalAccountRequest& r){++mutations;if(r.action==app::LocalAccountAction::save_user){if(r.expected_revision!=account.accounts[0].revision)return app::LocalAccountResult{app::LocalAccountResultCode::conflict};account.accounts[0].profile=r.profile;account.accounts[0].security_level=r.security_level;++account.accounts[0].revision;}app::erase_local_account_secrets(r);return app::LocalAccountResult{app::LocalAccountResultCode::ok,true,true,account.accounts[0].revision};};
        ports.online=[&]{
            app::ChatHub hub;app::ChatParticipant me;me.identity.session.bytes[0]=1;me.identity.account=1;me.identity.node=1;me.name="Fixture Sysop";me.sysop=true;me.level=999;me.sysop_threshold=999;me.safe=true;
            auto other=me;other.identity.session.bytes[0]=2;other.identity.account=2;other.identity.node=2;other.name="Test Caller";other.sysop=false;other.level=10;other.activity="Main Menu";
            check(hub.attach(me)&&hub.attach(other),"synthetic online assignments");app::ChatSessionPorts cp{terminal};cp.write=ports.write;cp.refresh=ports.authorized;cp.ansi=ports.ansi;cp.rows=ports.rows;cp.columns=ports.columns;cp.now=[]{return std::uint64_t{1000};};cp.node_count=[]{return std::uint16_t{4};};cp.text_color=[]{return std::uint8_t{7};};cp.hotkeys=ports.hotkeys;cp.can_disconnect=ports.authorized;
            cp.disconnect=[&](const app::ChatParticipant& target){check(target.identity==other.identity,"disconnect captures exact selected assignment");++mutations;return std::string("Disconnect requested for Test Caller on node 2. Normal cleanup is in progress.");};
            return app::ChatCallerSession(hub,me.identity,std::move(cp)).online();
        };ports.messages=[]()->bool{throw std::runtime_error("message previews require actual storage reader fixture");};
        app::FileGroup g;g.id={1};g.name="General Files";config.groups.push_back(g);app::FileAreaConfiguration area;area.area.id={1};area.area.group={1};area.area.name="Downloads";area.area.revision=1;config.areas.push_back(area);
        for(int i=1;i<=42;++i){app::ManagedFile f;f.file.id={i};f.file.area={1};f.file.revision=1;f.file.name="sample"+std::to_string(i)+".zip";f.file.bytes=1200;f.file.public_uploader="TESTCALLER";f.file.descriptions.short_text="Synthetic review file";files.push_back(f);}
        ports.file_configuration=[&]{return std::optional<app::FileConfiguration>{config};};
        ports.files=[&](const app::FileManagementReadRequest& r){app::FileManagementReadSnapshot page;page.code=app::LocalAccountResultCode::ok;if(r.kind==app::FileManagementQueryKind::detail){for(const auto& f:files)if(f.file.id==r.selection)page.records.push_back(f);return page;}for(const auto& f:files)if(f.file.id.value>r.after.value&&page.records.size()<40)page.records.push_back(f);if(!page.records.empty()&&page.records.back().file.id.value<42)page.next=page.records.back().file.id;return page;};
        ports.select_all_files=[&](const app::FileManagementQuery&){return std::optional<std::vector<app::ManagedFile>>{files};};
        ports.identifier=[&]{app::Identifier128 id;id.bytes[0]=static_cast<std::uint8_t>(++jobs);id.bytes[1]=static_cast<std::uint8_t>(jobs>>8);return id;};
        ports.file_start=[&](const app::FileManagementTask& task){check(app::valid_file_management_task(task),"controller emits valid native task");tasks.push_back(task);app::FileManagementJobStatus r;r.code=app::LocalAccountResultCode::ok;r.job=task.job;r.kind=task.kind;r.state=app::FileManagementJobState::completed;r.journal_confirmed=true;r.detail="Completed synthetic operation.";return r;};ports.file_status=[](app::Identifier128){return app::FileManagementJobStatus{};};ports.file_cancel=ports.file_status;ports.wait=[](const auto& done){return done();};
        ports.networks=[]{return std::vector<app::SysopNamedChoice>{{"fidonet","FidoNet",true},{"xleague","X-League",true}};};ports.links=[](std::string_view network){check(network=="fidonet","selected network preserved");return std::vector<app::SysopNamedChoice>{{"hub","FidoNet Hub",true}};};ports.poll=[&](std::string_view network,std::string_view link){check(network=="fidonet"&&link=="hub","poll selection exact");++polls;return std::string("FidoNet / FidoNet Hub: Poll requested; delivery is not confirmed.");};
        ports.ftn_view=[](std::string_view network,app::SysopFtnView view,std::uint64_t page){check(network=="fidonet","inspection network preserved");app::SysopTextPage p;p.title=view==app::SysopFtnView::queues?"Queues - FidoNet":view==app::SysopFtnView::held?"Held Mail - FidoNet":"BinkP Activity - FidoNet";p.page=page;p.lines={"FidoNet Hub: synthetic read-only entry"};return p;};
        ports.log=[](app::SysopLog source,std::optional<std::uint64_t> page){app::SysopTextPage p;const char* names[]={"HEYSYSOP.LOG","CALLERS.LOG","bink.log","freshclam.log","BotGate Activity"};p.title=names[static_cast<unsigned>(source)];p.page=page.value_or(1);p.previous=p.page>0;p.next=p.page<1;p.lines={"2026-11-04 14:00:00 - Synthetic review entry"};p.notice="Current log";return p;};
    }
    bool run(std::initializer_list<const char*> script){for(const auto* s:script)terminal.script.push_back(s);return app::SysopCallerSession(ports).run();}
    void save(const std::filesystem::path& root,std::string_view name){if(root.empty())return;std::filesystem::create_directories(root);unsigned i{};for(const auto& frame:frames){std::ofstream f(root/(std::string(name)+"-"+std::to_string(++i)+(ansi?".ans":".txt")),std::ios::binary);f<<frame;}}
};
struct StockSysopReader:app::ITrustedDisplayAssetReader {
    app::TrustedDisplayAssetReadResult read_asset(std::string_view name,std::size_t limit)override{
        if(name!="SYSOP.ANS"&&name!="SYSOP.ASC")return {app::TrustedDisplayAssetReadCode::not_found,{},{},{}};
        const auto path=std::filesystem::path("distribution/Display")/(name=="SYSOP.ANS"?"sysop.ans":"sysop.asc");std::ifstream f(path,std::ios::binary);const std::string bytes((std::istreambuf_iterator<char>(f)),{});check(bool(f)&&bytes.size()<=limit,"actual stock Sysop asset read");return {app::TrustedDisplayAssetReadCode::available,std::string(name),std::string(name),bytes};
    }
};
int main(int argc,char** argv){try{
    const std::filesystem::path evidence=argc>1?std::filesystem::path(argv[1]):std::filesystem::path{};
    for(const bool ansi:{false,true}){
        Fixture stock(ansi);
        stock.ports.artwork=[&]{
            StockSysopReader reader;
            app::TrustedDisplaySelector selector(reader);
            app::TrustedDisplaySelectionRequest request;
            request.identity=app::TrustedDisplayIdentity::menu_sysop;
            request.graphics=ansi?app::GraphicsMode::ansi:app::GraphicsMode::ascii;
            request.authenticated_security_level=std::uint16_t{999};
            const auto result=selector.select(request);
            check(result.available()&&result.program.has_value(),"actual stock Sysop artwork uses trusted selection");
            const auto program=app::trim_menu_artwork_padding(result.program->terminal_program);
            return stock.terminal.clear_screen()==app::TerminalOutcome::completed&&
                stock.terminal.present_display({"sysop.artwork",&program,{true,std::uint16_t{14}}})==app::TerminalOutcome::completed&&
                stock.ports.write(ansi?"\x1b[0m\r\n":"\r\n");
        };
        check(stock.run({"Q"}),"stock artwork reaches actual command controller without a padding pause");
        const auto& stock_wire=stock.terminal.wire.output;
        const auto bottom=stock_wire.rfind('\xbe'); // CP437 lower-right frame corner.
        const auto prompt=stock_wire.find("Command: ");
        check(bottom!=std::string::npos&&prompt!=std::string::npos&&prompt>bottom&&
            std::count(stock_wire.begin()+bottom,stock_wire.begin()+prompt,'\n')==1,
            "stock Sysop prompt appears on the row immediately after the frame");
        stock.save(evidence,"stock-top");
        Fixture denied(ansi);denied.authorized=false;check(denied.run({}),"denied closes safely");check(denied.queries==0&&denied.mutations==0&&denied.terminal.wire.output.empty(),"denied entry has no disclosure or action");
        Fixture edit(ansi);check(edit.run({"C","A","1","R","Rob Example","Q","S","Q","Q","Q"}),"save and continue completes");check(edit.mutations==1&&edit.account.accounts[0].profile.real_name=="Rob Example","draft saves exact fields once");edit.save(evidence,"accounts");
        Fixture discard(ansi);check(discard.run({"C","A","1","R","Unsaved","Q","C","Q","D","Q","Q","Q"}),"cancel navigation then discard");check(discard.mutations==0,"discard never mutates");
        Fixture escaped(ansi);check(escaped.run({"C","A","1","L","\x1b","S","Q","Q","Q","Q"}),"Escape cancels field edit");check(escaped.account.accounts[0].profile.location=="Portland, OR","cancelled field retains saved value");
        Fixture retry(ansi);const auto commit=retry.ports.account_change;unsigned attempts{};retry.ports.account_change=[&](app::LocalAccountRequest& r){if(!attempts++)return app::LocalAccountResult{app::LocalAccountResultCode::policy_rejected};return commit(r);};check(retry.run({"C","A","1","R","Corrected Name","S","S","Q","Q","Q","Q"}),"failed save retains correctable draft for retry");check(retry.mutations==1&&retry.account.accounts[0].profile.real_name=="Corrected Name","corrected draft committed once");retry.save(evidence,"save-error-retry");
        Fixture state(ansi);check(state.run({"C","A","1","T","E","N","Q","Q","Q","Q"}),"deactivation default confirmation cancel");check(state.mutations==0,"cancelled state change never mutates");state.save(evidence,"account-confirm");
        Fixture online(ansi);check(online.run({"C","W","D","2","N","D","2","Y","Q","Q","Q"}),"actual Who's Online disconnect confirmation");check(online.mutations==1,"one explicit confirmed synthetic disconnect");online.save(evidence,"online");
        Fixture security(ansi);check(security.run({"C","A","1","T","L","500","S","Q","Q","Q","Q"}),"security edit saves");check(security.account.accounts[0].security_level==500,"security level persisted");security.save(evidence,"security");
        Fixture create(ansi);check(create.run({"C","A","N","N","NEWCALLER","S","Q","Q","Q"}),"new caller flow");check(create.mutations==1,"new caller committed once");check(create.terminal.wire.output.find("SfxSynthetic!83")==std::string::npos,"secrets never displayed");create.save(evidence,"new-caller");
        Fixture ftn(ansi);check(ftn.run({"T","1","P","1","Q","Q","H","Q","A","Q","B","Q","Q"}),"FTN navigation");check(ftn.polls==1,"only explicit poll mutates");ftn.save(evidence,"ftn");
        Fixture logs(ansi);check(logs.run({"L","H","Q","C","Q","B","Q","F","Q","A","P","N","R","Q","Q","Q"}),"all five log sources and navigation");check(logs.mutations==0&&logs.polls==0,"logs read only");logs.save(evidence,"logs");
        Fixture files(ansi);check(files.run({"F","B","1","1","A","X","Y","Q","Q","Q"}),"area-wide selection and removal");check(files.tasks.size()==42,"select all includes off-screen records");std::set<std::int64_t> ids;for(const auto& t:files.tasks){check(t.operation.action==app::FileOperationAction::purge&&t.operation.expected_revision==1,"exact permanent removal request");ids.insert(t.operation.file.value);}check(ids.size()==42,"each selected record attempted exactly once");files.save(evidence,"files");
        Fixture actions(ansi);check(actions.run({"F","B","1","1","D","1","R","1","renamed.zip","Q","F","*.zip","M","1","1","1","Q","P","O","Q","Q"}),"details rename move and both scans");check(actions.tasks.size()==4&&actions.tasks[0].operation.destination.value==1&&actions.tasks[0].operation.expected_destination_revision==1,"rename binds current destination revision");actions.save(evidence,"file-actions");
        Fixture archive(ansi);const auto start=archive.ports.file_start;archive.ports.file_start=[&](const app::FileManagementTask& task){auto r=start(task);if(task.kind!=app::FileManagementTaskKind::close_view){app::FileManagementViewPage v;v.cursor.bytes[0]=77;v.filename="sample1.zip";v.archive=task.kind!=app::FileManagementTaskKind::read_view_member;v.total=1;if(v.archive)v.entries.push_back({0,"README.TXT",48,false,false,true});else{v.member="README.TXT";for(unsigned char c:std::string("Safe inert text preview. No extraction or execution."))v.cells.push_back({c,7});}r.view=v;}return r;};check(archive.run({"F","B","1","1","Z","1","M","1","I","Q","Q","Q","Q"}),"ZIP directory member index and close navigation");check(archive.tasks.size()==4&&archive.tasks.back().kind==app::FileManagementTaskKind::close_view,"preview explicitly closes its exact held cursor");archive.save(evidence,"zip-preview");
        Fixture held(ansi);app::Identifier128 original;app::FileManagementJobStatus status;held.ports.file_start=[&](const app::FileManagementTask& task){check(app::valid_file_management_task(task),"valid held recovery task");held.tasks.push_back(task);status.code=app::LocalAccountResultCode::ok;status.job=task.job;status.kind=task.kind;status.journal_confirmed=true;if(task.kind==app::FileManagementTaskKind::operation){original=task.operation.operation;status.state=app::FileManagementJobState::held;status.detail="Rename needs reconciliation. Original operation retained.";}else{check(task.kind==app::FileManagementTaskKind::reconcile_operation&&task.target==original,"recovery targets original operation");status.state=app::FileManagementJobState::completed;status.detail="Original operation reconciled.";}return status;};held.ports.file_status=[&](app::Identifier128){return status;};check(held.run({"F","B","1","1","R","1","held.zip","R","1","another.zip","Q","J","R","S","Q","Q"}),"uncertain native change blocks new intent until same-operation reconciliation");check(held.tasks.size()==2,"blocked repeated action never reaches backend");held.save(evidence,"file-recovery");
        Fixture conflict(ansi);conflict.ports.account_change=[](app::LocalAccountRequest&){return app::LocalAccountResult{app::LocalAccountResultCode::conflict};};check(conflict.run({"C","A","1","R","Draft Name","S","O","Y","Q","Q","Q","Q"}),"conflict retains draft until explicit reload confirmation");conflict.save(evidence,"account-conflict");
        Fixture reset(ansi);check(reset.run({"C","A","1","T","P","N","D","N","Q","Q","Q","Q"}),"password and account deletion confirmations default safely");check(reset.mutations==0,"declined destructive confirmations do not mutate");reset.save(evidence,"account-reset-delete");
        Fixture missing(ansi);missing.ports.log=[](app::SysopLog,std::optional<std::uint64_t>){app::SysopTextPage p;p.title="CALLERS.LOG";p.notice="Current log is missing.";return p;};check(missing.run({"L","C","R","Q","Q","Q"}),"missing log explicit refresh remains read only");missing.save(evidence,"log-unavailable");
        Fixture hotkey(ansi);hotkey.ports.hotkeys=[]{return true;};check(hotkey.run({"C","W","Q","Q","L","Q","Q"}),"hotkey navigation uses the same controllers");hotkey.save(evidence,"hotkeys");
        Fixture shortscreen(ansi);shortscreen.rows=15;shortscreen.columns=80;check(shortscreen.run({"C","A","1","T","Q","Q","Q","Q"}),"short terminal pages personal and security forms");shortscreen.save(evidence/"80x15","short-screen");
        if(!ansi)check(files.terminal.wire.output.find('\x1b')==std::string::npos,"plain mode contains no ANSI");
    }
    std::cout<<"PASS Sysop session: "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL Sysop session: "<<e.what()<<"\n";return 1;}}
