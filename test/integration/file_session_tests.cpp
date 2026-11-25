#include "sfx/application/file_session.hpp"
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/test/storage_fakes.hpp"
#include "sqlite_internal.hpp"
#include "sqlite_statement.hpp"
#include <algorithm>
#include <deque>
#include <iostream>
#include <fstream>
#include <stdexcept>

namespace app=sfx::application;
namespace storage=sfx::adapters::storage;
namespace internal=storage::internal;
namespace {
unsigned checks{},operations=100;
void check(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
app::Identifier128 id(unsigned n){app::Identifier128 v;v.bytes[0]=9;v.bytes[12]=static_cast<std::uint8_t>(n>>24);v.bytes[13]=static_cast<std::uint8_t>(n>>16);v.bytes[14]=static_cast<std::uint8_t>(n>>8);v.bytes[15]=static_cast<std::uint8_t>(n);return v;}
void seed(const std::filesystem::path& file){
    internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(100),false);
    c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(1,'Sysop','sysop',999,'active',1,1,1700000000,1700000000),(2,'Alice','alice',10,'active',1,1,1700000000,1700000000);INSERT INTO sfx_account_statistics(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_profiles(account_id) SELECT account_id FROM sfx_accounts;INSERT INTO sfx_account_name_reservations SELECT name_key,account_id,1,display_name,1700000000,NULL FROM sfx_accounts;UPDATE sfx_security_policy SET published_revision=revision,published_policy_hash=policy_hash;UPDATE sfx_installation_state SET staged_sysop_account_id=1,setup_state='complete',completed_utc_seconds=1700000000","synthetic FileSession actors; not authentication");
    for(unsigned n=1;n<=2;++n){internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1700000000,0,1700000000,NULL,NULL,?1)","test-only native session");q.bind(1,id(n));q.bind(2,static_cast<std::int64_t>(n));q.step();}
}
std::int64_t scalar(const std::filesystem::path& file,const char* sql){internal::SqliteConnection c(file,SQLITE_OPEN_READONLY,std::chrono::milliseconds(100),false);return c.scalar_int64(sql,"owned fixture invariant");}
struct Terminal final:app::ICallerTerminal{
    std::deque<std::string> inputs;std::deque<char> keys;std::string output,wire;std::function<void()> before_input,before_key;bool connected{true};unsigned pauses{},transfer_reports{};
    app::TerminalOutcome set_text_style(app::TerminalTextStyle)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome clear_screen()override{return app::TerminalOutcome::completed;}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& request)override{
        wire+=request.prompt;
        check(app::valid_terminal_input_request(request),"valid file text prompt");if(before_input){auto f=std::move(before_input);before_input={};f();}if(inputs.empty()){connected=false;return {app::TerminalOutcome::disconnected,{}};}auto text=inputs.front();inputs.pop_front();return {text.empty()?app::TerminalOutcome::empty:app::TerminalOutcome::completed,std::move(text)};
    }
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{return {};}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest& request)override{
        if(!keys.empty()){wire+=request.prompt;wire+=keys.front();if(request.advance_line)wire+="\r\n";}
        if(before_key){auto callback=std::move(before_key);before_key={};callback();}
        check(app::valid_terminal_key_request(request),"valid file key prompt");if(keys.empty()){connected=false;return {app::TerminalOutcome::disconnected,{}};}auto c=keys.front();keys.pop_front();if(c=='\r'){check(request.default_key.has_value(),"Enter has explicit default");return {app::TerminalOutcome::empty,request.default_key};}check(std::find(request.allowed_keys.begin(),request.allowed_keys.end(),c)!=request.allowed_keys.end(),"scripted key actually offered");return {app::TerminalOutcome::completed,c};
    }
    app::TerminalOutcome present(const app::TerminalPresentationRequest&)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& request)override{
        if(request.identity=="files.transfer.result"){
            check(request.paging.more_prompt_enabled,"transfer results page by rendered rows");++transfer_reports;
        }
        check(app::valid_terminal_display_presentation_request(request),"valid file display program");for(const auto& op:request.program->operations){if(op.kind==app::TerminalDisplayOperationKind::cp437_text){check(op.bytes.find('\x1b')==std::string::npos,"metadata contains no terminal escape");output+=op.bytes;}else if(op.kind==app::TerminalDisplayOperationKind::paging_newline||op.kind==app::TerminalDisplayOperationKind::logical_newline)output+='\n';}return connected?app::TerminalOutcome::completed:app::TerminalOutcome::disconnected;
    }
    app::TerminalOutcome pause(const app::TerminalPauseRequest&)override{check(output.find("<enter>")!=std::string::npos,"transfer pause has a visible Enter instruction");++pauses;return app::TerminalOutcome::completed;}
    void script(std::initializer_list<std::string> text,std::string_view choices){if(!inputs.empty()||!keys.empty())std::cerr<<"UNCONSUMED text="<<inputs.size()<<" keys="<<keys.size()<<" output="<<output.substr(0,1600)<<'\n';check(inputs.empty()&&keys.empty(),"previous script fully consumed");inputs=text;for(char c:choices)keys.push_back(c);output.clear();wire.clear();}
};
}
int main(int argc,char** argv){try{
    check(argc==2,"new fixture root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"never overwrite fixture");std::filesystem::create_directories(root);
    storage::SqliteStorageSettings settings;settings.data_directory=root;settings.utc_now=[]{return std::int64_t{1792248000};};sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(settings,paths);
    check(db.start().succeeded(),"production migration starts");check(db.shutdown().succeeded(),"stopped for synthetic seed");const auto database=root/"SFX.DB";seed(database);check(db.start().succeeded(),"seeded file fixture starts");
    const app::FileActor alice{id(2),2,1,1};const app::FileConfigurationActor owner{true,{}};
    const auto import=[&](std::int64_t area,std::string name,unsigned content){const auto config=db.file_configuration(owner);check(config.succeeded(),"native test configuration");app::FileRegistration r;r.operation=id(operations++);r.area={area};r.expected_area_revision=config.value->areas[static_cast<std::size_t>(area-1)].area.revision;r.filename=std::move(name);r.descriptions={"Safe short @SYSOP@","Long inert description"};r.public_uploader="Sysop";r.payload={10,{},"synthetic-volume","synthetic-file-"+std::to_string(content)};r.payload.sha256[0]=static_cast<std::uint8_t>(content);r.original_upload_utc=1700000000;const auto added=db.register_file(owner,r);check(added&&added.value,"native catalog fixture registration, no physical payload claim");return *added.value;};
    std::vector<app::FileEntry> library;for(unsigned n=0;n<30;++n)library.push_back(import(1,"file"+std::string(n<10?"0":"")+std::to_string(n)+".txt",n+1));
    const auto hidden=import(3,"private-secret.txt",50);(void)import(2,"review.txt",51);
    app::FileQuery totals;totals.scope=app::FileSearchScope::all;const auto first_page=db.file_page(alice,totals,{},5);
    check(first_page&&first_page.value->total_files==31&&first_page.value->total_bytes==310,"authorized scope totals exclude private file");
    const auto next_page=db.file_page(alice,totals,*first_page.value->next,5);check(next_page&&next_page.value->total_files==31&&next_page.value->total_bytes==310,"complete totals independent of page cursor");
    check(first_page.value->entries.front().new_to_caller,"join-date publication starts new marker");
    Terminal terminal;app::FileSessionPorts ports{db,{terminal}};ports.list.dimensions=[]{return app::MessageEditorDimensions{false,80,24};};ports.list.live=[&]{return terminal.connected;};ports.actor=[&]{return std::optional<app::FileActor>{alice};};ports.authority=[]{return app::FileAuthority{10,999};};ports.operation=[]{return id(operations++);};ports.date=[](std::int64_t){return "2026-10-17 12:00:00";};ports.scan_date=[](std::string_view value)->std::optional<std::int64_t>{return value=="2020-01-01"?std::optional<std::int64_t>{1577836800}:std::nullopt;};ports.caller_name=[]{return "Alice";};app::TransferProtocolPreference protocol=app::TransferProtocolPreference::zmodem;ports.protocol=[&]{return std::optional<app::TransferProtocolPreference>{protocol};};ports.upload_space=[](app::FileAreaId){return std::optional<std::uint64_t>{104857600};};
    unsigned upload_calls{},download_calls{};bool download_success{},private_seen{},upload_completed=true,upload_held{},mixed_upload{},host_av=true;auto upload_verdict=app::FileScanVerdict::not_scanned;
    ports.malware_scanning_available=[&]{return host_av;};
    std::optional<app::CallerFileBatchResult> injected_upload;
    bool x_enabled=false;ports.protocol_available=[&](auto value){return value!=app::TransferProtocolPreference::xmodem||x_enabled;};
    ports.upload=[&](const std::vector<app::FileUploadItem>& queue,bool private_submission,app::TransferProtocolPreference){++upload_calls;private_seen=private_submission;
        if(injected_upload)return *injected_upload;
        if(mixed_upload){check(!private_submission&&queue.size()==2,"mixed ordinary batch remains in selected Uploads area");app::CallerFileReport failed;failed.name=queue[0].name;failed.held=true;failed.detail="No file data requested: name lookup failed.";app::CallerFileReport added;added.name=queue[1].name;added.bytes=1274880;added.received=true;added.completed=true;added.file={92};added.scan.verdict=app::FileScanVerdict::clean;added.detail="Scan complete - clean; file added";return app::CallerFileBatchResult{true,{failed,added}};}
        check(queue.size()==1&&queue.front().name=="secret.zip","private transfer receives its separate queue");app::CallerFileReport report;report.name=queue.front().name;report.detail="Synthetic transfer outcome";report.completed=upload_completed;report.held=upload_held;report.scan.verdict=upload_verdict;report.file={91};report.bytes=12;return app::CallerFileBatchResult{true,{report}};};
    ports.download=[&](const std::vector<app::FileId>& queue,app::TransferProtocolPreference){++download_calls;check(queue.size()==1,"download queue port bounded");app::CallerFileReport report;report.name="file00.txt";report.file=queue.front();report.completed=download_success;report.skipped=!download_success;report.detail=download_success?"Synthetic completion":"Synthetic peer skip";report.bytes=10;return app::CallerFileBatchResult{true,{report}};};
    app::FileCallerSession session(ports);const auto context=session.context();check(context&&context->area=="General Files"&&context->number==1,"restores first accessible area");check(db.file_caller_state(alice).value->current_area.value==1,"first current area persisted");
    {const auto before=upload_calls;const auto pauses=terminal.pauses;terminal.script({},"");
        check(session.action(app::MenuAction::file_upload_add,app::MenuId::files).has_value()&&terminal.pauses==pauses+1,
            "denied upload visibly pauses before menu redraw");
        check(terminal.output.find("does not allow your uploads")!=std::string::npos&&terminal.output.find("<enter>")!=std::string::npos&&upload_calls==before,
            "denial is readable and never starts a transfer");}
    terminal.script({"file00.txt","Q"},"Q");check(session.action(app::MenuAction::file_list,app::MenuId::files).has_value(),"list -> File Information -> list -> return");check(terminal.output.find("Long inert description")!=std::string::npos&&terminal.output.find("Sysop")!=std::string::npos,"information contains public metadata");
    {const auto first=terminal.output.find("File Information"),last=terminal.output.find("[Q] Return:",first);check(first!=terminal.output.npos&&last!=terminal.output.npos,"clean detail and compact footer appear");const auto detail=terminal.output.substr(first,last-first);check(detail.find("Safe short")==detail.npos&&detail.find("Long inert description")==detail.rfind("Long inert description"),"long DIZ shown once, short duplicate omitted");check(detail.find("Uploaded:")==detail.npos&&detail.find("Published:")==detail.npos&&detail.find("Added:    2026-10-17")!=detail.npos,"published date only in caller compact view");check(detail.find("[E]")==detail.npos&&detail.find("[T] Tag  [R] Read Text  [V] View Archive")!=detail.npos,"ordinary caller footer retains keys and edit restriction");}
    check(terminal.output.rfind("  Filename        Size     Description\n",0)==0&&terminal.output.find("Updated:")==terminal.output.npos&&terminal.output.find("file(s),")==terminal.output.npos,"actual caller listing omits summary/context/timestamp and matches compact header");
    check(terminal.output.find(">   file00.txt"+std::string(8,' ')+"0.0K   Safe short @SYSOP@")!=terminal.output.npos,"common filenames retain compact columns with separate selection and queue marker cells");
    terminal.script({"private*","Q"},"A");check(session.action(app::MenuAction::file_find,app::MenuId::files).has_value(),"global private-name search safe");check(terminal.output.find("private-secret")==std::string::npos,"private entry absent from public search output");
    terminal.script({"3","Q"},"");check(session.action(app::MenuAction::file_change,app::MenuId::files).has_value()&&session.context()->number==1,"guessed private area rejected by direct selector");check(terminal.output.find("Upload to Sysop")==std::string::npos,"denied private area name not disclosed");
    check(terminal.output.find("File Areas")!=std::string::npos&&terminal.output.find("File Area number, G for File Groups")==std::string::npos,"Change immediately displays selector without preliminary question");
    terminal.script({"2"},"N");check(session.action(app::MenuAction::file_change,app::MenuId::files).has_value()&&session.context()->number==2,"direct accessible stable area selection");
    terminal.script({"ordinary.zip"},"YNN");check(session.action(app::MenuAction::file_upload_add,app::MenuId::files).has_value()&&session.uploads().items().size()==1,"direct upload queues without starting when No chosen");check(session.uploads().items()[0].destination.value==2&&session.uploads().items()[0].prefer_diz,"queued destination metadata exact");
    terminal.script({},"");check(session.leave(app::MenuId::batch_transfer,app::MenuId::files)==true&&session.uploads().items().size()==1,"return preserves queue without asking to discard it");
    terminal.script({"secret.zip"},"YNN");const auto private_result=session.action(app::MenuAction::file_private,app::MenuId::files);check(private_result&&private_seen&&upload_calls==1&&session.uploads().items().size()==1,"separate private submission preserves ordinary queue");check(private_result->destination==app::MenuId::files&&terminal.pauses==2,"private completion acknowledged then returns File Menu");check(session.uploads().items()[0].name=="ordinary.zip"&&session.uploads().items()[0].destination.value==2,"normal filename/destination unchanged");
    terminal.script({},"Y");check(session.action(app::MenuAction::file_upload_clear,app::MenuId::batch_transfer).has_value()&&session.uploads().items().empty(),"explicit Clear removes metadata queue only");
    terminal.script({std::string(100,'n')+".zip"},"YNN");check(session.action(app::MenuAction::file_upload_add,app::MenuId::files).has_value()&&session.uploads().items().size()==1&&session.uploads().items()[0].descriptions.short_text.size()<=80,"long valid filename queues with bounded provisional description");
    terminal.script({},"Y");check(session.action(app::MenuAction::file_upload_clear,app::MenuId::batch_transfer).has_value()&&session.uploads().items().empty(),"clear long-name metadata queue explicitly");
    terminal.script({"one.zip","two.zip"},"UYYN");check(session.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value()&&session.uploads().items().size()==2&&upload_calls==1,"Batch Add supports multiple uploads without starting them");
    terminal.script({},"");check(session.leave(app::MenuId::batch_transfer,app::MenuId::files)==true&&session.uploads().items().size()==2,"combined Batch return preserves uploads");
    terminal.script({"file00.txt","",""},"N");const auto added=session.action(app::MenuAction::file_download_add,app::MenuId::files);check(added&&added->destination==app::MenuId::files&&session.downloads().items().size()==1,"filename selection tags explicit authorized record");
    terminal.script({},"");check(session.leave(app::MenuId::batch_transfer,app::MenuId::files)==true&&session.uploads().items().size()==2&&session.downloads().items().size()==1,"all returns preserve both queues");
    protocol=app::TransferProtocolPreference::xmodem;terminal.script({},"");check(session.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer).has_value()&&download_calls==0,"unavailable saved protocol never silently substituted");protocol=app::TransferProtocolPreference::zmodem;
    terminal.script({},"N");check(session.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer)->destination==app::MenuId::files&&session.downloads().items().size()==1,"skipped download returns safely with queue retained");download_success=true;
    terminal.script({},"N");const auto downloaded=session.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer);
    check(downloaded&&downloaded->destination==app::MenuId::files&&downloaded->notice.empty()&&session.downloads().items().empty(),"proven download completes once and removes the queued identity");
    terminal.script({"file00.txt","",""},"D");check(session.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value()&&session.downloads().items().size()==1,"tag from Batch without initiating transfer");
    terminal.script({""},"YN");terminal.before_input=[&]{check(terminal.output.find("Download Queue:")!=std::string::npos&&terminal.output.find("file00.txt")!=std::string::npos&&terminal.output.find("Download files: 1; total bytes: 10")!=std::string::npos,"direct D shows queued filename/count/bytes before asking for additional filename");check(terminal.output.find("one.zip")==std::string::npos,"direct D displays downloads only");};
    const auto direct_queued=session.action(app::MenuAction::file_download_add,app::MenuId::files);check(direct_queued&&session.downloads().items().empty()&&download_calls==3,"blank optional filename starts normal confirmation/transfer of existing tags");
    terminal.script({""},"");const auto empty_download=session.action(app::MenuAction::file_download_add,app::MenuId::files);check(empty_download&&empty_download->notice=="Download queue is empty."&&terminal.output.find("Download files: 0")!=std::string::npos&&download_calls==3,"empty queue is explicit and Enter never starts an empty transfer");
    check(scalar(database,"SELECT count(*) FROM sfx_file_statistics")==0,"synthetic transfer port does not fabricate real accounting evidence");
    mixed_upload=true;terminal.script({},"N");const auto previous_reports=terminal.transfer_reports;
    check(session.action(app::MenuAction::file_upload_begin,app::MenuId::batch_transfer).has_value(),"mixed result returns normally");
    check(session.uploads().items().size()==1&&session.uploads().items().front().name=="one.zip","remove only clean accepted file immediately, without public-area move");
    check(terminal.output.find("Received: 1 complete file(s), 1274880 bytes.")!=std::string::npos&&terminal.output.find("Added: 1 file(s), 1274880 bytes.")!=std::string::npos&&terminal.output.find("Completed: 0")==std::string::npos,"received and accepted accounting stays truthful for mixed rejected/clean batch");
    check(terminal.transfer_reports==previous_reports+1,"mixed batch uses one paged report including its totals");
    mixed_upload=false;terminal.script({},"Y");check(session.action(app::MenuAction::file_upload_clear,app::MenuId::batch_transfer).has_value(),"explicitly clear only failed synthetic queue item");
    // Complete-receive rejection consumes queue metadata, never success credit.
    for(unsigned trial=0;trial<7;++trial){
        const auto name="rejected-"+std::to_string(trial)+".zip";
        terminal.script({name},"YNN");check(session.action(app::MenuAction::file_upload_add,app::MenuId::files).has_value()&&session.uploads().items().size()==1,"queue terminal-result fixture");
        app::CallerFileReport f;f.name=name;f.bytes=100;f.received=trial<5;f.held=trial!=0;f.deleted=trial==0;f.scan.verdict=trial==0?app::FileScanVerdict::detected:trial==1?app::FileScanVerdict::error:trial==2?app::FileScanVerdict::incomplete:app::FileScanVerdict::clean;f.detail=trial==0?"fail - deleted":"Rejected/held without acceptance";
        if(trial==0)f.scan.detections={"Synthetic-Test-Signature"};
        if(trial==1)f.detail="File not added: Scanner unavailable or failed; retained privately Private copy retained.";
        // An inconsistent completed/error report must not acquire success credit.
        if(trial==4){f.completed=true;f.scan.verdict=app::FileScanVerdict::error;}
        injected_upload=app::CallerFileBatchResult{true,{f}};terminal.script({},"N");check(session.action(app::MenuAction::file_upload_begin,app::MenuId::batch_transfer).has_value(),"rejected receive reports normally");
        check(session.uploads().items().empty()==f.received,"only complete received rejection is removed; interrupted/preflight remains retryable");
        check(terminal.output.find("Added: 0 file(s), 0 bytes.")!=std::string::npos&&terminal.output.find("file added")==std::string::npos,"rejected/deleted upload never counted or announced added");
        if(trial==0)check(terminal.output.find("Synthetic-Test-Signature")!=std::string::npos&&terminal.output.find("Received: 1 complete file(s), 100 bytes.")!=std::string::npos,"detection/deletion remains visible with truthful received/added totals");
        if(trial==1)check(terminal.output.find(name+"\n  File not added: Scanner unavailable or failed. Private copy retained.")!=std::string::npos,"held result puts filename on its own row and explains retention once");
        check(scalar(database,"SELECT count(*) FROM sfx_file_statistics")==0,"queue cleanup never fabricates storage success statistics");
        if(!f.received){terminal.script({},"Y");check(session.action(app::MenuAction::file_upload_clear,app::MenuId::batch_transfer).has_value(),"explicit cleanup of retryable test entry");}
    }
    injected_upload.reset();
    terminal.script({"up-a.zip","up-b.zip"},"UYYN");check(session.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value(),"seed both-queues removal uploads");
    terminal.script({"file00.txt","",""},"D");check(session.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value(),"seed both-queues removal download");
    const auto catalog_before=scalar(database,"SELECT count(*) FROM sfx_file_catalog");
    for(const auto choices:{"Q","AN","A\r"}){terminal.script({},choices);check(session.action(app::MenuAction::file_both_remove,app::MenuId::batch_transfer).has_value()&&session.uploads().items().size()==2&&session.downloads().items().size()==1,"All/No/default-No/cancel preserve both queues");}
    terminal.script({},"AY");check(session.action(app::MenuAction::file_both_remove,app::MenuId::batch_transfer).has_value()&&session.uploads().items().empty()&&session.downloads().items().empty(),"confirmed All removes both metadata queues");
    check(terminal.output.find("No physical file deleted.")!=std::string::npos&&scalar(database,"SELECT count(*) FROM sfx_file_catalog")==catalog_before,"All is not a catalog or physical-file delete");
    terminal.script({},"A");check(session.action(app::MenuAction::file_both_remove,app::MenuId::batch_transfer).has_value()&&session.uploads().items().empty()&&session.downloads().items().empty(),"All on already empty queues is a harmless no-op");
    terminal.script({"file00.txt","",""},"D");check(session.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value(),"tag initial download for optional additional filename");
    terminal.script({"file01.txt","",""},"N");check(session.action(app::MenuAction::file_download_add,app::MenuId::files).has_value()&&session.downloads().items().size()==2&&terminal.output.find("Download files: 2; total bytes: 20")!=std::string::npos,"direct D accepts an additional authorized match and refreshes displayed tags");
    terminal.script({},"AY");check(session.action(app::MenuAction::file_both_remove,app::MenuId::batch_transfer).has_value()&&session.downloads().items().empty(),"All also clears a download-only queue after confirmation");
    upload_verdict=app::FileScanVerdict::clean;terminal.script({"secret.zip"},"YNN");const auto clean=session.action(app::MenuAction::file_private,app::MenuId::files);
    check(clean&&clean->notice.empty()&&terminal.output.find("Scan complete - clean; file added\n<enter>")!=std::string::npos&&terminal.output.find("Upload report:")==std::string::npos&&terminal.output.find("Completed:")==std::string::npos,"committed clean success is explicit and concise with visible Enter and no redundant notice");
    upload_verdict=app::FileScanVerdict::not_scanned;terminal.script({"secret.zip"},"YNN");check(session.action(app::MenuAction::file_private,app::MenuId::files).has_value()&&terminal.output.find("malware scanning Off")!=std::string::npos&&terminal.output.find("Scan complete")==std::string::npos,"scanning Off never claims a clean scan");
    host_av=false;terminal.script({"secret.zip"},"YNN");check(session.action(app::MenuAction::file_private,app::MenuId::files).has_value()&&terminal.output.find("Upload complete - file added\n<enter>")!=std::string::npos&&terminal.output.find("malware")==std::string::npos&&terminal.output.find("Scan complete")==std::string::npos,"no-AV host adds upload without any malware-scanning message or clean claim");host_av=true;
    upload_completed=false;upload_held=true;upload_verdict=app::FileScanVerdict::clean;terminal.script({"secret.zip"},"YNN");check(session.action(app::MenuAction::file_private,app::MenuId::files).has_value()&&terminal.output.find("Upload report:")!=std::string::npos&&terminal.output.find("Added: 0")!=std::string::npos&&terminal.output.find("Received:")!=std::string::npos&&terminal.output.find("Completed:")==std::string::npos&&terminal.output.find("file added")==std::string::npos,"clean scan alone cannot announce a held file added; received and added totals are separate");
    upload_completed=true;upload_held=false;upload_verdict=app::FileScanVerdict::error;terminal.script({"secret.zip"},"YNN");check(session.action(app::MenuAction::file_private,app::MenuId::files).has_value()&&terminal.output.find("Upload report:")!=std::string::npos&&terminal.output.find("file added")==std::string::npos,"unexpected scan verdict cannot use success-only screen");
    app::FileUploadItem review{"review.zip",{2},{"queued",""},false,true};terminal.script({},"");const auto diz=session.review_upload(review,app::FileDescriptions{"\\DIZ is text","@SYSOP@ remains inert"},100);check(diz&&!diz->sysop_only&&diz->descriptions.short_text=="\\DIZ is text"&&terminal.output.empty(),"valid untrusted DIZ is inert and needs no redundant confirmation");
    terminal.script({"\\private description"},"N");const auto manual=session.review_upload(review,{},100);check(manual&&manual->sysop_only&&manual->descriptions.short_text=="private description","only caller live description supports historical backslash flag");
    terminal.script({"My description"},"N");const auto no_diz=session.review_upload(review,{},100);check(no_diz&&no_diz->descriptions.short_text=="My description","no DIZ requires caller description before acceptance");
    review.prefer_diz=false;review.descriptions={"already supplied","body"};terminal.script({},"");const auto supplied=session.review_upload(review,{},100);check(supplied&&supplied->descriptions.short_text=="already supplied"&&terminal.output.empty(),"pre-transfer caller metadata needs no second confirmation");
    review.prefer_diz=true;terminal.script({""},"");check(!session.review_upload(review,{},100),"blank required description holds rather than inventing acceptance");
    terminal.script({"1"},"N");check(session.action(app::MenuAction::file_change,app::MenuId::files).has_value(),"return General for scan");
    terminal.script({"","Q"},"C");check(session.action(app::MenuAction::file_new,app::MenuId::files).has_value(),"interrupted multi-page scan returns safely");check(scalar(database,"SELECT count(*) FROM sfx_file_scan_seen WHERE account_id=2")==20&&!db.file_caller_state(alice).value->scan_completed_utc,"only emitted first page acknowledged; incomplete scope not completed");
    terminal.script({"","Q"},"C");check(session.action(app::MenuAction::file_new,app::MenuId::files).has_value(),"resume scan exposes remaining unseen publications");check(scalar(database,"SELECT count(*) FROM sfx_file_scan_seen WHERE account_id=2")==30&&db.file_caller_state(alice).value->scan_completed_utc.has_value(),"complete exactly current area, no unseen cutoff");
    check(scalar(database,"SELECT count(*) FROM sfx_file_scan_seen s JOIN sfx_file_catalog f USING(file_id) WHERE f.area_id<>1")==0,"other and private areas not acknowledged");
    app::FileQuery marker;marker.area={1};check(!db.file_page(alice,marker,{},1).value->entries.front().new_to_caller,"completed publication marker cleared only for actual seen row");marker.area={2};check(db.file_page(alice,marker,{},1).value->entries.front().new_to_caller,"other area's unseen marker retained");
    terminal.script({},"NQ");check(session.action(app::MenuAction::file_settings,app::MenuId::settings).has_value()&&!db.file_caller_state(alice).value->new_file_scan,"new file login scan preference persists");terminal.script({},"");check(session.login_scan()&&terminal.output.empty(),"saved Off suppresses login scan");
    for(bool ansi:{false,true}){
        const auto original_lightbar=db.file_caller_state(alice).value->lightbar;
        std::string frames;auto settings_ports=ports;
        settings_ports.list.lightbars=true;settings_ports.list.dimensions=[ansi]{return app::MessageEditorDimensions{ansi,80,25};};
        settings_ports.list.write=[&](std::string_view text){frames+=text;return true;};
        terminal.script({},"NNLLQ");app::FileCallerSession preferences(settings_ports);
        check(preferences.action(app::MenuAction::file_settings,app::MenuId::settings).has_value(),"two N/L toggles retain caller action/input behavior");
        const auto text=frames+terminal.output;const auto heading=text.find("File Scan Preferences");
        check(heading!=text.npos&&text.find("File Scan Preferences",heading+1)==text.npos,"N/L toggles do not stack whole preference screens");
        check(text.find("File preference saved.")==text.npos,"N/L toggle success does not append a saved notice");
        if(ansi)check(frames.find("\x1b[3;29H")!=frames.npos&&frames.find("\x1b[4;22H")!=frames.npos,"ANSI repaints only reserved On/Off value positions");
        else check(terminal.output.find("New File Scan at Logon: On")!=terminal.output.npos&&terminal.output.find("File Light Bars: Off")!=terminal.output.npos,"plain mode emits compact truthful changed-value lines");
        check(!db.file_caller_state(alice).value->new_file_scan&&db.file_caller_state(alice).value->lightbar==original_lightbar,"paired toggles restore original durable preferences");
    }
    for(bool ansi:{false,true}){
        const auto original=*db.file_caller_state(alice).value;
        auto date_ports=ports;date_ports.list.dimensions=[ansi]{return app::MessageEditorDimensions{ansi,80,25};};
        date_ports.list.write=[&](std::string_view text){terminal.wire+=text;return true;};
        terminal.script({"invalid","","2020-01-01"},"DDDQ");
        app::FileCallerSession date_settings(date_ports);
        check(date_settings.action(app::MenuAction::file_settings,app::MenuId::settings).has_value(),"date invalid, Enter cancel, valid input and return complete");
        check(terminal.wire.find("D\r\nNew File Scan Date YYYY-MM-DD (Enter cancels): ")!=terminal.wire.npos,"date entry begins on fresh line after echoed D in ANSI and plain modes");
        check(terminal.output.find("Enter a valid earlier date.")!=terminal.output.npos,"invalid date preserves existing validation");
        check(db.file_caller_state(alice).value->since_utc==1577836800,"valid earlier scan date persists unchanged semantics");
        auto restore=*db.file_caller_state(alice).value;restore.since_utc=original.since_utc;
        check(db.save_file_caller_state(alice,restore).succeeded(),"date fixture restores original scan boundary");
    }
    {
        std::string frames;auto settings_ports=ports;settings_ports.list.lightbars=true;
        settings_ports.list.dimensions=[]{return app::MessageEditorDimensions{true,80,25};};
        settings_ports.list.write=[&](std::string_view text){frames+=text;return true;};
        terminal.script({},"NQ");terminal.before_key=[&]{auto newer=*db.file_caller_state(alice).value;newer.new_file_scan=!newer.new_file_scan;check(db.save_file_caller_state(alice,newer).succeeded(),"concurrent preference writer advances displayed revision");};
        app::FileCallerSession preferences(settings_ports);
        check(preferences.action(app::MenuAction::file_settings,app::MenuId::settings).has_value()&&frames.find("Preference not saved; please try again.")!=frames.npos,"inline revision conflict remains visible and never claims success");
        auto restore=*db.file_caller_state(alice).value;restore.new_file_scan=false;check(db.save_file_caller_state(alice,restore).succeeded(),"test restores its own scan choice with current revision");
    }
    for(bool ansi:{false,true}){
        std::string frames;std::deque<unsigned char> bytes;
        auto marker_ports=ports;marker_ports.list.lightbars=ansi;
        marker_ports.list.dimensions=[ansi]{return app::MessageEditorDimensions{ansi,80,25};};
        marker_ports.list.write=[&](std::string_view text){frames+=text;return true;};
        marker_ports.list.byte=[&]{if(bytes.empty())return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::disconnected,{}};const auto c=bytes.front();bytes.pop_front();return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,c};};
        marker_ports.download=[](const std::vector<app::FileId>& items,app::TransferProtocolPreference){app::CallerFileReport done;done.file=items.front();done.name="file00.txt";done.completed=true;return app::CallerFileBatchResult{true,{done}};};
        app::FileCallerSession marked(marker_ports);
        if(ansi){terminal.script({},"");bytes={'t','q'};}
        else terminal.script({"T","N","P","S","Q"},"F");
        check(marked.action(app::MenuAction::file_list,app::MenuId::files).has_value()&&marked.downloads().items().size()==1,"listing tags exact queued identity through real caller action");
        const auto text=frames+terminal.output;
        std::ofstream(root/(ansi?"queue-marker-ansi.ans":"queue-marker-plain.asc"),std::ios::binary)<<text;
        check(text.find("* file00.txt")!=text.npos&&text.find("  file01.txt")!=text.npos,"queued star and blank reserve the same pre-filename columns");
        terminal.script({},"N");
        check(marked.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer).has_value()&&marked.downloads().items().empty(),"proven completion removes actual queued identity");
        frames.clear();if(ansi){terminal.script({},"");bytes={'q'};}else terminal.script({"Q"},"");
        check(marked.action(app::MenuAction::file_list,app::MenuId::files).has_value()&&(frames+terminal.output).find("* file00.txt")==std::string::npos,"redrawn listing clears marker after queue removal without shifting filename");
    }
    terminal.script({"T","Q"},"");terminal.before_input=[&]{const auto cfg=db.file_configuration(owner);app::FileConfigurationChange change;change.operation=id(operations++);change.action=app::FileConfigurationAction::area;change.expected_catalog_revision=cfg.value->policy.catalog_revision;change.area=cfg.value->areas[0];change.area.area.permissions.download=20;check(db.change_file_configuration(owner,change).succeeded(),"live download permission revoked after frame");};
    check(session.action(app::MenuAction::file_list,app::MenuId::files).has_value()&&session.downloads().items().empty(),"tag reauthorizes current permission after displayed list");
    for(bool populated:{false,true}){
        app::FileCallerSession view_session(ports);
        terminal.script({"2"},"N");check(view_session.action(app::MenuAction::file_change,app::MenuId::files).has_value(),"queue-view fixture selects authorized upload area through public action");
        if(populated){terminal.script({"view-only.zip"},"YNN");check(view_session.action(app::MenuAction::file_upload_add,app::MenuId::files).has_value()&&view_session.uploads().items().size()==1,"queue fixture added through public caller action");}
        for(auto action:{app::MenuAction::file_upload_view,app::MenuAction::file_download_view,app::MenuAction::file_both_view}){
            terminal.script({},"");const auto before=terminal.pauses;const auto size=view_session.uploads().items().size();
            check(view_session.action(action,app::MenuId::batch_transfer).has_value()&&terminal.pauses==before+1,"every standalone queue listing acknowledges empty/populated results exactly once");
            check(terminal.output.find("<enter>")!=std::string::npos&&view_session.uploads().items().size()==size,"listing is readable and does not alter queues");
        }
    }
    terminal.script({"1"},"N");check(session.action(app::MenuAction::file_change,app::MenuId::files).has_value(),"compact fixture selects public area normally");
    for(const unsigned height:{24U,50U}){
        std::string frames;auto wide_ports=ports;wide_ports.list.dimensions=[height]{return app::MessageEditorDimensions{true,80,height};};wide_ports.list.write=[&](std::string_view s){frames+=s;return true;};wide_ports.list.byte=[]{return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,'q'};};
        app::FileCallerSession wide(wide_ports);terminal.script({},"");
        check(wide.action(app::MenuAction::file_list,app::MenuId::files).has_value(),"actual storage-backed compact ANSI list");check((frames.find("file29.txt")!=frames.npos)==(height==50),"tall screen requests a larger bounded storage page, not the historical twenty-file cap");
        check(frames.find("\x1b[1;1H\x1b[1;37;40m  Filename")!=frames.npos&&frames.find("Updated:")==frames.npos,"production session selects compact renderer with reserved queue marker column");
    }
    (void)import(1,"a-very-long-filename.zip",71);terminal.script({"Q"},"");check(session.action(app::MenuAction::file_list,app::MenuId::files).has_value(),"long filename in compact list");
    check(terminal.output.find("a-very-long-filename.zip")!=terminal.output.npos&&terminal.output.find("Filename"+std::string(16,' ')+"Size     Description")!=terminal.output.npos,"long filename widens common columns up to the previous allowance, not truncated to sixteen");
    {
        using P=app::TransferProtocolPreference;auto protocol_ports=ports;x_enabled=true;unsigned started{};
        // The preceding revocation test intentionally left General download at
        // level20. Restore this fixture through the same configuration API;
        // keep that earlier denial assertion and product authorization intact.
        const auto cfg=db.file_configuration(owner);app::FileConfigurationChange restore;
        restore.operation=id(operations++);restore.action=app::FileConfigurationAction::area;
        restore.expected_catalog_revision=cfg.value->policy.catalog_revision;restore.area=cfg.value->areas[0];restore.area.area.permissions.download=10;
        check(db.change_file_configuration(owner,restore).succeeded(),"restore authorized General download fixture for independent protocol tests");
        protocol_ports.protocol_available=[](P p){return p==P::xmodem||p==P::ymodem||p==P::ymodem_g||p==P::zmodem;};
        P used=P::select_at_transfer;std::vector<app::FileId> delivered;
        protocol_ports.download=[&](const auto& queue,P p){++started;used=p;delivered=queue;app::CallerFileBatchResult result;
            for(auto file:queue){app::CallerFileReport report;report.file=file;report.name="review.txt";report.completed=true;report.bytes=10;report.detail="Synthetic completion";result.files.push_back(report);}return result;};
        for(auto saved:{P::xmodem,P::ymodem,P::ymodem_g,P::zmodem,P::select_at_transfer}){
            protocol=saved;app::FileCallerSession transfer(protocol_ports);
            terminal.script({"1"},"N");check(transfer.action(app::MenuAction::file_change,app::MenuId::files).has_value(),"protocol fixture selects allowed area");
            terminal.script({"file00.txt","",""},"D");check(transfer.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value()&&transfer.downloads().items().size()==1&&terminal.connected,"protocol fixture queues exact file");
            terminal.script({},saved==P::select_at_transfer?"GN":"N");
            check(transfer.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer).has_value()&&transfer.downloads().items().empty(),"selected protocol finishes exact queue item");
            check(used==(saved==P::select_at_transfer?P::ymodem_g:saved)&&delivered.size()==1,"saved choice runs directly; selector passes explicit choice");
            if(saved==P::xmodem)check(terminal.output.find("one file at a time")!=std::string::npos,"XMODEM single-file behavior explained");
        }
        check(started==5,"one transfer per explicit action");
        protocol=P::xmodem;app::FileCallerSession single(protocol_ports);
        terminal.script({"1"},"N");check(single.action(app::MenuAction::file_change,app::MenuId::files).has_value(),"X queue fixture area");
        terminal.script({"file00.txt","","file01.txt","",""},"D");check(single.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value()&&single.downloads().items().size()==2,"two exact queued download identities");
        const auto waiting=single.downloads().items()[1];terminal.script({},"N");
        check(single.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer).has_value()&&delivered.size()==1&&single.downloads().items().size()==1&&single.downloads().items()[0]==waiting,"XMODEM sends only first selected identity; rest stay queued");
        protocol=P::select_at_transfer;terminal.script({},"Q");const auto before=started;
        check(single.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer).has_value()&&started==before&&single.downloads().items().size()==1,"selector cancellation retains queue and starts no transfer");
        check(terminal.output.find("\n[X] XMODEM")!=terminal.output.npos&&terminal.output.find("[Q] Cancel\n\n")!=terminal.output.npos,"shared transfer selector has a blank line before first option and after Cancel");
        protocol=P::zmodem;
    }
    {
        app::FileCallerSession detail(ports);terminal.script({"file01.txt","Q"},"TQ");
        check(detail.action(app::MenuAction::file_list,app::MenuId::files).has_value()&&detail.downloads().items().size()==1&&detail.downloads().items().front()==library[1].id,"detail Tag queues exact shown identity and Q returns to listing");
        check(terminal.output.find("File is in the download queue.")!=terminal.output.npos,"Tag feedback survives clean detail redraw");
        const auto last=terminal.output.rfind("Filename");if(last==terminal.output.npos||terminal.output.find("> * file01.txt",last)==terminal.output.npos)std::cerr<<"DETAIL RETURN "<<terminal.output.substr(last==terminal.output.npos?0:last)<<'\n';check(last!=terminal.output.npos&&terminal.output.find("> * file01.txt",last)!=terminal.output.npos,"Q preserves selected file identity in refreshed listing");
        auto owner_ports=ports;owner_ports.actor=[]{return std::optional<app::FileActor>{{id(1),1,1,1}};};owner_ports.authority=[]{return app::FileAuthority{999,999};};app::FileCallerSession privileged(owner_ports);
        terminal.script({"file01.txt","Q"},"Q");terminal.before_key=[&]{
            check(terminal.output.find("File Information")!=terminal.output.npos&&terminal.output.find("[E] Edit Description")==terminal.output.npos,"imported file retains existing own-upload edit restriction even for Sysop caller");
            const auto current=db.managed_file(owner,library[1].id);check(bool(current),"owner reads exact managed file");
            app::FileOperationRequest edit;edit.operation=id(operations++);edit.file=library[1].id;edit.expected_revision=current.value->file.revision;edit.descriptions={"Current refreshed description","Updated long DIZ"};edit.public_uploader=current.value->file.public_uploader;
            check(db.prepare_file_operation(owner,edit)&&db.commit_file_operation(owner,edit.operation,{current.value->payload,current.value->payload,false,true}),"separate authorized owner metadata edit occurs while caller detail is open");
        };
        check(privileged.action(app::MenuAction::file_list,app::MenuId::files).has_value(),"Q refreshes actual changed file metadata");
        const auto refreshed=terminal.output.rfind("Filename");check(refreshed!=terminal.output.npos&&terminal.output.find("Current refreshed description",refreshed)!=terminal.output.npos&&terminal.output.find(">   file01.txt",refreshed)!=terminal.output.npos,"changed revision refreshes text without switching selected file identity");
    }
    // Connected reports retain success/skip accounting while Y bypasses pause.
    for(const bool success:{false,true})for(const bool disconnect:{false,true}){
        auto countdown_ports=ports;unsigned calls{};
        countdown_ports.transfer_logoff=[&]()->std::optional<bool>{
            ++calls;check(terminal.output.find("Completed: "+std::to_string(success?1:0)+" file(s)")!=std::string::npos,"report is presented before countdown, including failed/skip outcome");return disconnect;
        };
        app::FileCallerSession transfer(countdown_ports);download_success=success;
        terminal.script({"file00.txt","",""},"D");check(transfer.action(app::MenuAction::file_both_add,app::MenuId::batch_transfer).has_value(),"queue through public caller action before Y countdown");
        const auto pauses=terminal.pauses;terminal.script({},disconnect?"\r":"Y");
        const auto effect=transfer.action(app::MenuAction::file_download_begin,app::MenuId::batch_transfer);
        check(effect&&effect->goodbye==disconnect&&calls==1&&terminal.pauses==pauses&&terminal.output.find("<enter>")==std::string::npos,"Y and default Y automatically invoke countdown without Enter pause");
        check(transfer.downloads().items().empty()==success,"countdown does not change transfer proof/queue preservation");
        if(!disconnect)check(effect->destination==app::MenuId::files,"Abort keeps caller online and returns File menu");
    }
    check(!db.file_information(alice,hidden.id),"private storage boundary still enforced");check(db.shutdown().succeeded(),"normal fixture shutdown");
    std::cout<<"PASS "<<checks<<" file-session checks; real SQLite with synthetic actors/terminal/transfer ports; no installed/native transfer claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
