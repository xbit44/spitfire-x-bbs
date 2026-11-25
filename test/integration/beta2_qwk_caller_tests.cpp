// Actual shared caller D routing + production SQLite/builder/ZIP. Transport
// helper delivery is qualified separately; this test does not fabricate it.
#include "sfx/application/menu_session.hpp"
#define main retained_qwk_storage_main
#include "qwk_storage_tests.cpp"
#undef main
#include "sfx/application/menu_session.hpp"
#include "../../src/apps/runtime/qwk_download_preparation.hpp"
#include <fstream>
int main(int argc,char** argv)try{
    check(argc==2,"fresh isolated QWK caller fixture required");
    const auto root=std::filesystem::absolute(argv[1]);check(!std::filesystem::exists(root),"preserve prior caller evidence");std::filesystem::create_directories(root);
    storage::SqliteStorageSettings settings;settings.data_directory=root;settings.utc_now=[]{return std::int64_t{1793970000};};sfx::test::FakeStoragePathPolicy paths;storage::SqliteStorage db(settings,paths);
    check(db.start().succeeded()&&db.shutdown().succeeded(),"create current production schema then stop for synthetic actors");
    const auto file=root/"SFX.DB";seed(file);check(db.start().succeeded(),"seeded production storage opens");
    const app::MessageActor author{id(2),2,1,1},caller{id(3),3,1,1};
    const auto context=db.begin_message(author,{app::MessageKind::public_echo,{2},{},{}});check(bool(context),"ordinary message compose authority");
    const std::string original=std::string("Before ")+static_cast<char>(0xe3)+" after\nNext line\n";
    const auto posted=db.commit_message({author,id(9000),*context.value,"Classic caller pi",original});check(bool(posted),"ordinary stored source contains actual CP437 pi");
    for(const bool utf8:{false,true}){
        auto saved=db.qwk_preferences(caller);check(bool(saved),"authenticated caller settings");auto choices=saved.value->preferences;choices.encoding=utf8?app::QwkEncoding::utf8:app::QwkEncoding::cp437;choices.full_headers=utf8;
        check(bool(db.save_qwk_preferences(caller,saved.value->revision,choices)),"caller explicitly selects supported packet encoding");
        app::MenuSessionSnapshot state;state.access={10,false,1};state.configuration_revision=1;state.preferences={3,{},1,1793970000};state.graphics=app::GraphicsMode::ascii;
        app::MenuSessionPorts ports;ports.snapshot=[&]{return std::optional<app::MenuSessionSnapshot>{state};};std::string output;ports.write=[&](std::string_view bytes){output+=bytes;return true;};
        ports.display=[](const app::TerminalDisplayProgram&){return true;};ports.save_preferences=[](const app::UpdateMenuPreferenceRequest&){return false;};ports.utc_now=[]{return std::int64_t{1793970000};};
        ports.artwork=[](const auto&){return app::TrustedDisplaySelectionResult{app::TrustedDisplaySelectionCode::not_found,{}};};
        std::string input="D\r";std::size_t next{};unsigned downloads{};
        ports.read=[&](app::MenuInputDecoder& decoder,const std::function<bool()>&)->std::optional<app::MenuInput>{std::optional<app::MenuInput> result;while(next<input.size()){const auto event=decoder.feed(static_cast<unsigned char>(input[next++]));if(event.kind!=app::MenuInputKind::pending){result.emplace();result->kind=event.kind;result->value=event.value;break;}}return result;};
        ports.action=[&](app::MenuAction action)->std::optional<std::string>{
            check(action==app::MenuAction::qwk_download,"actual QWK D dispatches download only");++downloads;
            app::QwkPacketMetadata meta;meta.board_name="Beta2 isolated caller";meta.sysop="Sysop";meta.caller_name="Bob";meta.created="11-12-2026,12:00:00";
            const auto packet=sfx::apps::runtime::prepare_caller_qwk_download(db,caller,id(utf8?9002:9001),meta,[](std::int64_t){return sfx::archive::QwkRecordDate{"11-12-26","12:00","1793970000"};});
            check(packet.result.ok()&&packet.storage&&packet.progress&&packet.progress->delivered.size()==1,"caller D produces complete usable packet instead of refusal");
            const auto members=download_members(packet.zip);const auto& dat=members.at("MESSAGES.DAT");const std::string expected=utf8?"Before \xcf\x80 after\nNext line\n":"Before * after\xe3Next line\xe3";
            check(dat.size()==384&&dat[250]==static_cast<char>(0xe1)&&dat.substr(256,expected.size())==expected,"actual classic/UTF8 ZIP has correct header framing, content and real line breaks");
            check(db.quote_source(caller,posted.value->id).value->body.canonical_lf==original&&scalar(file,"SELECT count(*) FROM sfx_qwk_continuation")==0&&scalar(file,"SELECT count(*) FROM sfx_caller_echo_state")==0,"D preparation preserves stored text and read/delivery progress");
            std::ofstream out(root/(utf8?"caller-utf8.qwk":"caller-classic.qwk"),std::ios::binary);out.write(packet.zip.data(),static_cast<std::streamsize>(packet.zip.size()));check(bool(out),"packet retained for independent review");return "Packet prepared for transfer.";
        };
        (void)app::run_menu_session(ports,app::MenuId::qwk);check(downloads==1&&output.find("Packet prepared for transfer.")!=std::string::npos,"caller receives successful Download result without visiting Settings menu");
    }
    check(db.shutdown().succeeded(),"caller fixture closes normally");std::cout<<"PASS beta2 caller D "<<checks<<" checks; actual menu/SQLite/builder ZIP, no native transfer claim\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL beta2 caller D after "<<checks<<": "<<e.what()<<'\n';return 1;}
