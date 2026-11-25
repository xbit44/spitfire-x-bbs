// Reuse owned NTFS/SQLite fixture builders, not the prior manager test runner.
#define main retained_file_manager_main
#include "../integration/file_manager_tests.cpp"
#undef main
#include "alpha_file_viewer.hpp"
#include <deque>
namespace runtime=sfx::apps::runtime;
namespace {
struct ViewerTerminal final:app::ICallerTerminal{
    std::deque<std::string> input;std::string output;unsigned content_calls{};
    app::TerminalOutcome set_text_style(app::TerminalTextStyle)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome clear_screen()override{return app::TerminalOutcome::completed;}
    app::TerminalInputResult read_visible(const app::TerminalInputRequest& request)override{check(app::valid_terminal_input_request(request),"viewer prompt valid");if(input.empty())return {app::TerminalOutcome::disconnected,{}};auto text=input.front();input.pop_front();return {text.empty()?app::TerminalOutcome::empty:app::TerminalOutcome::completed,std::move(text)};}
    app::TerminalInputResult read_secret(const app::TerminalInputRequest&)override{return {};}
    app::TerminalKeyResult read_key(const app::TerminalKeyRequest&)override{return {};}
    app::TerminalOutcome present(const app::TerminalPresentationRequest&)override{return app::TerminalOutcome::completed;}
    app::TerminalOutcome present_display(const app::TerminalDisplayPresentationRequest& request)override{check(app::valid_terminal_display_presentation_request(request),"viewer output valid");for(const auto& op:request.program->operations)if(op.kind==app::TerminalDisplayOperationKind::cp437_text){check(op.bytes.find('\x1b')==std::string::npos,"untrusted original escapes absent");output+=op.bytes;}return app::TerminalOutcome::completed;}
    app::TerminalOutcome pause(const app::TerminalPauseRequest&)override{return app::TerminalOutcome::completed;}
};
std::string member_zip(){
    struct Free{void operator()(struct archive* a)const{archive_write_free(a);}};std::unique_ptr<struct archive,Free> writer(archive_write_new());std::string bytes;
    check(archive_write_set_format_zip(writer.get())==ARCHIVE_OK&&archive_write_set_bytes_per_block(writer.get(),0)==ARCHIVE_OK&&archive_write_open(writer.get(),&bytes,nullptr,zip_write,nullptr)==ARCHIVE_OK,"ZIP viewer fixture begins");
    for(const auto& item:std::vector<std::pair<std::string,std::string>>{{"readme.txt","ZIP text @FILEAREA@"},{"art.ans","\x1b[31mZIP ART\x1b[0m"},{"binary.bin",std::string("MZ\0data",7)}}){auto* e=archive_entry_new();archive_entry_set_pathname(e,item.first.c_str());archive_entry_set_filetype(e,AE_IFREG);archive_entry_set_size(e,static_cast<la_int64_t>(item.second.size()));check(archive_write_header(writer.get(),e)==ARCHIVE_OK,"ZIP member header");archive_entry_free(e);check(archive_write_data(writer.get(),item.second.data(),item.second.size())==static_cast<la_ssize_t>(item.second.size()),"ZIP member bytes");}
    check(archive_write_close(writer.get())==ARCHIVE_OK,"ZIP fixture ends");return bytes;
}
}
int main(int argc,char** argv){try{
    check(argc==2,"fresh native viewer root required");const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"retain every prior viewer fixture");const auto sid=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(sid);
    for(const auto& path:{root,root/L"Data",root/L"Files",root/L"Files"/L"General",root/L"Files"/L"Uploads",root/L"Files"/L"UploadToSysop"})ins::create_directory(path,descriptor.get());
    sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorageSettings settings;settings.data_directory=root/L"Data";settings.utc_now=[]{return std::int64_t{1792248000};};storage::SqliteStorage db(settings,paths);check(db.start().succeeded()&&db.shutdown().succeeded(),"fresh production schema");const auto database=root/L"Data"/L"SFX.DB";seed(database);check(db.start().succeeded(),"synthetic caller starts");
    const app::FileActor actor{id(2),2,1,1};const app::FileConfigurationActor owner{true,{}};
    const auto make=[&](std::int64_t area,std::string name,const std::string& body){
        const auto config=db.file_configuration(owner);const auto selected=std::find_if(config.value->areas.begin(),config.value->areas.end(),[&](const auto& a){return a.area.id.value==area;});
        std::vector<std::filesystem::path> others;for(const auto& a:config.value->areas)if(a.area.id.value!=area)others.push_back(std::filesystem::u8path(a.storage_path));
        auto directory=native::FilePayloadDirectory::area(root,std::filesystem::u8path(selected->storage_path),others,{},sid);auto payload=directory.create(name,1048576);
        for(std::size_t at=0;at<body.size();){const auto count=std::min<std::size_t>(65536,body.size()-at);payload.append(reinterpret_cast<const std::uint8_t*>(body.data()+at),count);at+=count;}
        app::FileRegistration registration;registration.operation=id(operations++);registration.area={area};registration.expected_area_revision=selected->area.revision;registration.filename=name;registration.descriptions={name,"Viewer fixture"};registration.public_uploader="Sysop";registration.payload=payload.identity();registration.allow_duplicate_content=true;const auto result=db.register_file(owner,registration);check(result&&result.value,"actual held payload registered");return *result.value;
    };
    const auto text=make(1,"large.txt","HEAD\n"+std::string(70000,'x')+"\nTAIL-MARKER\n");std::string art;for(unsigned n=0;n<130;++n)art+="\x1b[31mRow"+std::to_string(n)+"\r\n";
    const auto ansi=make(1,"art.ans",art);const auto binary=make(1,"binary.txt",std::string("MZ\0exe",6));const auto zip=make(1,"archive.zip",member_zip());const auto restricted=make(2,"review.txt","REVIEW-BODY-PRIVATE");const auto hidden=make(3,"private.txt","PRIVATE-SECRET-BODY");
    ViewerTerminal terminal;bool ansi_mode{};std::string frames;app::FileListPorts list{terminal};list.dimensions=[&]{return app::MessageEditorDimensions{ansi_mode,80,24};};list.live=[]{return true;};list.write=[&](std::string_view text){frames+=text;return true;};list.byte=[]{return app::StreamCallerTerminal::EditorByteResult{app::TerminalOutcome::completed,'Q'};};
    runtime::CallerFileViewerPorts ports{db,root,sid,[&]{return std::optional<app::FileActor>{actor};},list};ports.authority=[]{return app::FileAuthority{10,999};};
    bool revoke{},blocked_write{};std::string active_name;
    ports.display=[&](const app::TerminalDisplayProgram& program,const std::function<bool()>& guard){++terminal.content_calls;check(guard(),"current authority before native view output");
        if(!active_name.empty()){ins::Handle writer(CreateFileW((root/L"Files"/L"General"/std::filesystem::u8path(active_name)).c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));blocked_write=writer.get()==INVALID_HANDLE_VALUE;}
        if(revoke){const auto config=db.file_configuration(owner);app::FileConfigurationChange change;change.operation=id(operations++);change.action=app::FileConfigurationAction::area;change.expected_catalog_revision=config.value->policy.catalog_revision;change.area=config.value->areas[0];change.area.area.permissions.download=20;check(db.change_file_configuration(owner,change).succeeded(),"revoke during displayed content boundary");check(!guard(),"read guard rejects changed live configuration");return app::TerminalOutcome::cancelled;}
        return terminal.present_display({"viewer.test.content",&program,{true,24}});
    };
    runtime::AlphaFileViewer viewer(ports);
    active_name="large.txt";check(viewer.view(text.id,false)&&terminal.output.find("TAIL-MARKER")!=std::string::npos&&blocked_write,"actual multi-chunk payload rendered completely under write exclusion");
    terminal.output.clear();active_name="art.ans";check(viewer.view(ansi.id,false)&&terminal.output.find("Row129")!=std::string::npos,"actual >100-row ANSI parsed and safely paged");
    terminal.output.clear();const auto before=terminal.content_calls;check(viewer.view(binary.id,false)&&terminal.content_calls==before&&terminal.output.find("binary, unsupported")!=std::string::npos,"actual content detection refuses binary with text extension");
    terminal.output.clear();active_name.clear();check(viewer.view(restricted.id,false)&&terminal.content_calls==before&&terminal.output.find("REVIEW-BODY")==std::string::npos,"review-download restriction also denies online payload view");
    terminal.output.clear();check(viewer.view(hidden.id,false)&&terminal.content_calls==before&&terminal.output.find("private.txt")==std::string::npos&&terminal.output.find("PRIVATE-SECRET")==std::string::npos,"private metadata and content absent");
    terminal.output.clear();terminal.input={"1","2","3","Q"};check(viewer.view(zip.id,true)&&terminal.output.find("ZIP text @FILEAREA@")!=std::string::npos&&terminal.output.find("ZIP ART")!=std::string::npos&&terminal.output.find("binary, unsupported")!=std::string::npos,"real held ZIP members read safely; binary list-only");check(terminal.input.empty(),"all archive choices consumed explicitly");
    {auto state=db.file_caller_state(actor);auto desired=*state.value;desired.lightbar=false;check(db.save_file_caller_state(actor,desired).succeeded(),"saved caller lightbar Off");ansi_mode=true;terminal.input={"Q"};frames.clear();check(viewer.view(zip.id,true)&&terminal.input.empty()&&frames.find("\x1b[2J")==std::string::npos,"ANSI archive selector honors saved caller text-list preference");ansi_mode=false;}
    terminal.output.clear();revoke=true;check(viewer.view(text.id,false)&&terminal.output.find("TAIL-MARKER")==std::string::npos,"mid-view revocation contains output without file mutation");
    check(scalar(database,"SELECT count(*) FROM sfx_file_statistics")==0&&scalar(database,"SELECT sum(download_count) FROM sfx_file_catalog")==0,"online reads never fabricate download counters");
    check(!std::filesystem::exists(root/L"Work"),"ZIP preview extracted no filesystem members or scratch");check(db.shutdown().succeeded(),"native viewer DB stopped normally");
    std::cout<<"PASS native viewer "<<checks<<" checks; actual NTFS/SQLite/ZIP/locks, synthetic actors/terminal; no installed Runtime or client UI claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
