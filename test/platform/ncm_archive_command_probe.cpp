// Test-only Windows CRT system() boundary. Does NOT execute NCM, curses,
// keystrokes, console-input injection, a reader editor or a product program.
// Mirrors the pinned v0.54 documented archive command construction only.
#include "fixture_paths.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
int main(int argc,char** argv)try{
    if(argc!=3)return 64;
    const std::string action=argv[1];
    const auto folder=std::filesystem::absolute(argv[2]).lexically_normal();
    const std::string prefix=(sfx::test::fixture_checkout() / "build/section8d-reader-command-r1/cases").make_preferred().string() + '\\';
    const auto directory=folder.string();
    if(directory.rfind(prefix,0)!=0||!std::filesystem::is_directory(folder)||directory.find_first_of("%&|<>^\"\r\n")!=std::string::npos)return 65;
    std::filesystem::current_path(folder);
    const bool old=action=="extract-original"||action=="add-original";
    const bool extract=action=="extract-original"||action=="extract-call";
    if(!extract&&action!="add-original"&&action!="add-call"&&action!="update-call")return 66;
    const std::string archiver="\"C:\\Program Files\\7-Zip\\7z.exe\"";
    // On Windows v0.54 quotespace() quotes the appended packet/REP filename.
    // No change to installed archiver, PATH, COMSPEC or shell/security policy.
    const auto target=folder/(extract?"Source Packet.QWK":"Reply Packet.REP");
    const auto command=(old?std::string{}:std::string("call "))+archiver+(extract?" e -y ":" a -tzip -y ")+"\""+target.string()+"\""+(extract?"":action=="update-call"?" HEADERS.DAT":" SFX863DF.MSG");
    std::cout<<"COMMAND "<<command<<std::endl;
    const auto result=std::system(command.c_str());std::cout<<"SYSTEM_RESULT "<<result<<std::endl;return result;
}catch(const std::exception& e){std::cerr<<"PROBE_FAILURE "<<e.what()<<std::endl;return 67;}
