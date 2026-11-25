#include <cstdio>
#define SFX_NEWSLETTER_FILE_TEST_DIAGNOSTICS 1
#include "newsletter_file.hpp"
#include "alpha_trusted_displays.hpp"
#include "sfx/application/classic_information.hpp"
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
namespace fs=std::filesystem;
#if defined(_WIN32)
constexpr const char* display_folder="Display";
constexpr const char* new_newsletter="SFNWSLTR.ASC";
#else
constexpr const char* display_folder="display";
constexpr const char* new_newsletter="sfnwsltr.asc";
#endif
namespace {unsigned checks{};void check(bool value,const char* message){++checks;if(!value)throw std::runtime_error(message);}
void write(const fs::path& path,std::string_view text){std::ofstream f(path,std::ios::binary);f.write(text.data(),static_cast<std::streamsize>(text.size()));f.close();check(bool(f),"fixture write");
#if !defined(_WIN32)
fs::permissions(path,fs::perms::owner_read|fs::perms::owner_write);
#endif
}
std::string bytes(const fs::path& path){std::ifstream f(path,std::ios::binary);return std::string((std::istreambuf_iterator<char>(f)),{});}}
int main(int argc,char** argv){try{
    check(argc==3,"fresh test root and owner fixture required");const auto root=fs::absolute(argv[1]);check(!fs::exists(root),"fresh fixture only");fs::create_directories(root/display_folder);
#if !defined(_WIN32)
    fs::permissions(root,fs::perms::owner_all);fs::permissions(root/display_folder,fs::perms::owner_all);
#endif
    const auto original=bytes(argv[2]);check(original.size()==6698,"actual owner fixture");
    const auto asc=root/display_folder/"sfnwsltr.asc",ans=root/display_folder/"sfnwsltr.ans";write(asc,original);
    sfx::apps::runtime::NewsletterFileStore file(root);const auto before=file.load();check(before.available&&before.exists&&before.bytes==original,"load actual entire fixture");
    const auto stamp=fs::last_write_time(asc);check(file.save(before,original,[]{return true;}).saved&&bytes(asc)==original&&fs::last_write_time(asc)==stamp,"unchanged Save preserves bytes and mtime");
    auto changed=original;changed.replace(0,1,"!");const auto edited=file.save(before,changed,[]{return true;});
    if(!edited.saved){std::cerr<<edited.notice;
#if defined(_WIN32)
        std::cerr<<" Win32="<<GetLastError();
#endif
        std::cerr<<'\n';}
    check(edited.saved&&bytes(asc)==changed,"native bounded edit publishes exact document");
    check(!file.save(before,original,[]{return true;}).saved&&bytes(asc)==changed,"stale snapshot refuses without replacing current content");
    auto current=file.load();write(ans,"\x1b[31mOwner ANSI");check(!file.load().available&&!file.save(current,original,[]{return true;}).saved&&bytes(asc)==changed&&bytes(ans)=="\x1b[31mOwner ANSI","ANSI appearing after load preserves both files");fs::remove(ans);
    check(!file.save(current,original,[]{return false;}).saved&&bytes(asc)==changed,"revoked access refuses publication");
    unsigned calls{};const auto cancelled=file.save(current,original,[&]{return ++calls<3;});check(!cancelled.saved&&bytes(asc)==changed,"revocation at publication restores original and retains draft");
    const auto fresh=root/"fresh";fs::create_directories(fresh/display_folder);
#if !defined(_WIN32)
    fs::permissions(fresh,fs::perms::owner_all);fs::permissions(fresh/display_folder,fs::perms::owner_all);
#endif
    sfx::apps::runtime::NewsletterFileStore missing(fresh);const auto absent=missing.load();check(absent.available&&!absent.exists&&fs::is_empty(fresh/display_folder),"missing load and Cancel create no newsletter or lock");
    check(missing.save(absent,"New newsletter\t  ",[]{return true;}).saved&&bytes(fresh/display_folder/new_newsletter)=="New newsletter\t  ","only explicit Save creates missing newsletter preserving no final newline");
#if !defined(_WIN32)
    const auto unsafe=root/"unsafe";fs::create_directories(unsafe/display_folder);fs::permissions(unsafe,fs::perms::owner_all);fs::permissions(unsafe/display_folder,fs::perms::owner_all);
    fs::create_symlink(asc,unsafe/display_folder/"SFNWSLTR.ASC");sfx::apps::runtime::NewsletterFileStore linked(unsafe);check(!linked.load().available&&bytes(asc)==changed,"symlink newsletter refuses and target stays unchanged");
#endif
    namespace app=sfx::application;
    sfx::apps::runtime::AlphaTrustedDisplays displays(root);
    for(const auto& content:std::vector<std::string>{"Short newsletter.",original,"Edited\r\n"+original,"Text\twith tabs  \r\n\r\n\r\n","First@PAUSE@Second"}){
        write(asc,content);
        for(auto graphics:{app::GraphicsMode::ascii,app::GraphicsMode::ansi}){
            app::TrustedDisplaySelectionRequest request;request.identity=app::TrustedDisplayIdentity::sfnewsletter;request.graphics=graphics;
            auto selected=displays.select(request);check(selected.available(),"real native newsletter selector reads fixture");
            const auto authored=content.find("@PAUSE@")!=std::string::npos?1:0;
            const auto pauses=[](const auto& program){return std::count_if(program.operations.begin(),program.operations.end(),[](const auto& op){return op.kind==app::TerminalDisplayOperationKind::pause;});};
            check(pauses(selected.program->terminal_program)==authored,"native adapter does not add a hidden newsletter acknowledgement");
            check(app::prepare_acknowledged_display(selected.program->terminal_program,graphics)&&pauses(selected.program->terminal_program)==authored+1,"caller adds exactly one final pause and retains authored pause");
            app::MenuSessionPorts ports;app::MenuSessionSnapshot state;state.graphics=graphics;state.access={999,true,1};
            ports.snapshot=[&]{return std::optional<app::MenuSessionSnapshot>{state};};
            ports.artwork=[&](const auto& r){return displays.select(r);};ports.write=[](std::string_view){return true;};
            ports.read=[](auto&,const auto&)->std::optional<app::MenuInput>{return app::MenuInput{app::MenuInputKind::command,'Y'};};
            unsigned views{};
            ports.display=[&](const auto& program){++views;check(pauses(program)==authored+1,"login offer has one completion acknowledgement");return true;};
            check(app::offer_login_newsletter(ports,{})&&views==1,"actual native selection through shared login-offer path");
        }
    }
    std::cout<<"PASS native newsletter files "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native newsletter files: "<<e.what()<<'\n';return 1;}}
