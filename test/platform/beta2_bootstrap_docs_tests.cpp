#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <iostream>
namespace win=sfx::platform::win32;namespace ins=win::installation;
int wmain(int argc,wchar_t** argv)try{
    ins::require(argc==2&&!ins::elevated(),"ordinary isolated metadata fixture required");
    const auto parent=ins::canonical_root(argv[1]);ins::require(!std::filesystem::exists(parent),"preserve prior evidence");
    const auto owner=ins::operator_sid();auto descriptor=ins::mutable_descriptor(owner);ins::create_directory(parent,descriptor.get());unsigned checks{},sequence{};
    const auto check=[&](bool okay,const char* why){++checks;ins::require(okay,why);};
    const std::string valid="SFX-DOCS-1\nDocs/Sysop/wiki.html|"+std::string(64,'A')+"\nREADME.txt|"+std::string(64,'B')+"\n";
    const auto trial=[&](std::string text,bool recognized,bool real_state=false,bool legacy=false,bool wrong_digest=false,bool hardlink=false){
        const auto root=parent/std::to_wstring(++sequence);ins::create_directory(root,descriptor.get());ins::create_directory(root/L"Config",descriptor.get());ins::create_directory(root/L"Data",descriptor.get());
        const std::vector<std::uint8_t> bytes(text.begin(),text.end());const auto digest=wrong_digest?std::string(64,'0'):ins::sha256(bytes);
        const auto leaf=legacy?L"stock-docs-legacy-"+std::wstring(digest.begin(),digest.end())+L".v1":std::wstring(L"stock-docs.v1");
        const auto file=root/L"Config"/leaf;ins::create_file(file,bytes,descriptor.get());
        if(real_state)ins::create_file(root/L"Data"/L"SFX.DB",{},descriptor.get());
        if(hardlink)check(CreateHardLinkW((root/L"Config"/L"unknown-link").c_str(),file.c_str(),nullptr)!=FALSE,"new synthetic hardlink only");
        check(win::stock_docs_bootstrap_metadata(root,file,owner)==recognized,"exact receipt content/identity classification");
        if(!hardlink)check(ins::read_file(file,20000)==bytes,"inspection preserves exact receipt bytes");
    };
    trial(valid,true);trial(valid,true,true);trial(valid,true,false,true);trial(valid,false,false,true,true);trial(valid,false,false,false,false,true);
    for(const auto& text:{std::string{},std::string("OWNER DATA\n"),std::string("SFX-DOCS-1\n"),valid.substr(0,valid.size()-1),valid+"garbage\n",valid+valid.substr(11),std::string(16385,'x')})trial(text,false);
    for(const auto* path:{"../outside","Config/SFX.conf","Docs/../outside","Docs//bad","Docs\\bad","Docs/name.","Docs/name ","Docs/a|b","Docs/a:b"})trial("SFX-DOCS-1\n"+std::string(path)+"|"+std::string(64,'A')+"\n",false);
    trial("SFX-DOCS-1\nDocs/Sysop/wiki.html|"+std::string(63,'A')+"Z\n",false);
    trial(valid+"Docs/sysop/WIKI.HTML|"+std::string(64,'A')+"\n",false);
    // The real Setup receipt/structure is required for classification. Use the
    // exact still-unconfigured installed fixture, preserving its original files.
    const auto installed=ins::canonical_root(L"C:\\Program Files\\SFX-Beta2-Test-20261112-r1");
    ins::verify_runtime_installation(installed,owner);
    check(!std::filesystem::exists(installed/L"Config"/L"SFX.conf")&&!std::filesystem::exists(installed/L"Data"/L"SFX.DB"),"actual fixture remains uninitialized");
    const auto original=ins::read_file(installed/L"Config"/L"stock-docs.v1",16384);
    const auto classify=[&](bool expected){const auto state=win::alpha_installation_has_persistent_state(installed,owner,[&](const auto& candidate){return win::stock_docs_bootstrap_metadata(installed,candidate,owner);});check(state&&state.value&&*state.value==expected,"actual Setup structure keeps genuine state guarded");};
    classify(false);
    const auto introduced=[&](const ins::Path& path,const std::vector<std::uint8_t>& bytes,bool expected){
        check(!std::filesystem::exists(path),"new exact temporary guard fixture only");ins::create_file(path,bytes,descriptor.get());
        struct Retire {ins::Path path;~Retire(){DeleteFileW(path.c_str());}} retire{path};
        classify(expected);check(DeleteFileW(path.c_str())!=FALSE,"retire only this newly created guard fixture");
    };
    introduced(installed/L"Config"/L"unknown-beta2-test-state",{},true);
    introduced(installed/L"Data"/L"SFX.DB",{},true);
    introduced(installed/L"Config"/(L"stock-docs-legacy-"+std::wstring(64,L'0')+L".v1"),original,true);
    const auto hash=ins::sha256(original);
    introduced(installed/L"Config"/(L"stock-docs-legacy-"+std::wstring(hash.begin(),hash.end())+L".v1"),original,false);
    check(ins::read_file(installed/L"Config"/L"stock-docs.v1",16384)==original,"original failed-bootstrap receipt stays byte exact");classify(false);
    std::cout<<"PASS beta2 bootstrap Docs metadata "<<checks<<" checks; actual private file/classifier, no owner board\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL bootstrap Docs metadata: "<<e.what()<<'\n';return 1;}
