#include "setup_engine.cpp"
#include <iostream>
namespace ins=sfx::platform::win32::installation;
namespace setup=sfx::apps::setup;
namespace {
unsigned checks{};
void check(bool okay,const char* message){++checks;ins::require(okay,message);}
std::vector<std::uint8_t> bytes(std::string text){return {text.begin(),text.end()};}
std::string text(const std::vector<std::uint8_t>& value){return {value.begin(),value.end()};}
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2,"fresh contained docs fixture required");
    const auto root=ins::canonical_root(argv[1]);check(!std::filesystem::exists(root),"preserve every prior fixture");
    const auto owner=ins::operator_sid();auto descriptor=ins::mutable_descriptor(owner);
    ins::create_directory(root,descriptor.get());
    const auto fresh=[&](const wchar_t* name){auto path=root/name;ins::create_directory(path,descriptor.get());setup::seed_missing_sysop_docs(path,owner);return path;};
    const auto path=fresh(L"fresh");
    const auto internal=path/L"Config"/L"stock-docs.v1",legacy=path/L"Docs"/L".sfx-stock-docs.txt";
    const auto wiki=path/L"Docs"/L"Sysop"/L"wiki.html";
    check(std::filesystem::exists(internal)&&!std::filesystem::exists(legacy),"fresh tracking is private configuration metadata");
    check(std::filesystem::exists(path/L"Docs/ThirdParty/THIRD_PARTY.txt")&&std::filesystem::exists(path/L"Docs/ThirdParty/SFX-LICENSE.txt"),"core notice and original product license installed together");
    check(std::filesystem::exists(path/L"Docs/Doors/SFXGuess32/sysop.txt")&&std::filesystem::exists(path/L"Docs/Doors/NewsWire-X-RSS/sysop.txt"),"both Door guides installed under Docs/Doors");
    check(!std::filesystem::exists(path/L"Doors"),"document update does not install or change runtime Door programs/config");
    const auto original=ins::read_file(wiki,1024*1024);
    // A browser/owner reader must not block an unchanged documentation update.
    {auto reader=ins::open_object(wiki,false,GENERIC_READ,FILE_SHARE_READ);setup::seed_missing_sysop_docs(path,owner);}
    check(ins::read_file(wiki,1024*1024)==original,"unchanged readable stock document requires no write lease");
    auto custom=original;const auto tail=bytes("\r\nOWNER CUSTOM DOCUMENT\r\n");custom.insert(custom.end(),tail.begin(),tail.end());
    {auto handle=ins::open_object(wiki,false,GENERIC_WRITE,0);LARGE_INTEGER zero{};DWORD count{};
     check(SetFilePointerEx(handle.get(),zero,nullptr,FILE_BEGIN)&&WriteFile(handle.get(),custom.data(),static_cast<DWORD>(custom.size()),&count,nullptr)&&count==custom.size()&&SetEndOfFile(handle.get())&&FlushFileBuffers(handle.get()),"owned custom guide fixture saved");}
    setup::seed_missing_sysop_docs(path,owner);
    check(ins::read_file(wiki,1024*1024)==custom,"custom guide stays byte exact");
    const auto previous=ins::read_file(internal,16384);
    check(DeleteFileW(internal.c_str()),"retire only new fixture internal receipt to model old installation");
    ins::create_file(legacy,previous,descriptor.get());setup::seed_missing_sysop_docs(path,owner);
    const auto hash=ins::sha256(previous);const auto retained=path/L"Config"/(L"stock-docs-legacy-"+std::wstring(hash.begin(),hash.end())+L".v1");
    check(!std::filesystem::exists(legacy)&&ins::read_file(retained,16384)==previous,"recognized legacy receipt retained exactly before visible metadata retirement");
    check(ins::read_file(internal,16384)==previous&&ins::read_file(wiki,1024*1024)==custom,"legacy migration preserves prior stock bindings and custom bytes");
    // Model interruption after new receipt publication but before legacy cleanup.
    const auto conflicting=bytes("SFX-DOCS-1\nDocs/Sysop/wiki.html|"+std::string(64,'A')+"\n");
    ins::create_file(legacy,conflicting,descriptor.get());setup::seed_missing_sysop_docs(path,owner);
    check(ins::read_file(internal,16384)==previous&&ins::read_file(wiki,1024*1024)==custom,"internal receipt wins safe interrupted-migration retry");
    const auto conflicting_hash=ins::sha256(conflicting);
    check(ins::read_file(path/L"Config"/(L"stock-docs-legacy-"+std::wstring(conflicting_hash.begin(),conflicting_hash.end())+L".v1"),16384)==conflicting,"earlier conflicting legacy binding remains available as evidence");
    const auto unknown=bytes("OWNER METADATA - KEEP EXACT\r\n");ins::create_file(legacy,unknown,descriptor.get());
    setup::seed_missing_sysop_docs(path,owner);
    check(ins::read_file(legacy,16384)==unknown&&ins::read_file(wiki,1024*1024)==custom,"unknown visible metadata and custom guide are never retired or adopted");
    const auto isolated=fresh(L"unknown-internal");const auto receipt=isolated/L"Config"/L"stock-docs.v1";
    check(DeleteFileW(receipt.c_str()),"only fixture receipt removed for unknown metadata case");
    ins::create_file(receipt,unknown,descriptor.get());setup::seed_missing_sysop_docs(isolated,owner);
    check(ins::read_file(receipt,16384)==unknown,"unknown internal metadata preserved with no rewrite");
    std::cout<<"PASS "<<checks<<" native Setup documentation preservation/migration checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}
