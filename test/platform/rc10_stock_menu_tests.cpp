#include "setup_engine.cpp"
#include <fstream>
#include <iostream>
#include <algorithm>
namespace ins=sfx::platform::win32::installation;
namespace setup=sfx::apps::setup;
namespace {
unsigned checks{};
void check(bool value,const char* label){++checks;ins::require(value,label);}
void write(const ins::Path& p,const std::vector<std::uint8_t>& data){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(data.data()),static_cast<std::streamsize>(data.size()));f.close();check(bool(f),"owned fixture write");}
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==3,"fresh root and source checkout required");
    const auto root=ins::canonical_root(argv[1]);const auto source=ins::canonical_root(argv[2]);
    check(!std::filesystem::exists(root),"new fixture only");const auto owner=ins::operator_sid();auto descriptor=ins::mutable_descriptor(owner);
    ins::create_directory(root,descriptor.get());ins::create_directory(root/L"Display",descriptor.get());
    std::vector<std::wstring> notices;const setup::Progress progress=[&](const auto& text){notices.push_back(text);};
    const auto run=[&]{setup::seed_missing_stock_menus(root,owner,progress);};run();
    const auto ans=root/L"Display"/L"main.ans",asc=root/L"Display"/L"main.asc";
    const auto new_ans=ins::read_file(source/L"distribution"/L"Display"/L"main.ans",262144),new_asc=ins::read_file(source/L"distribution"/L"Display"/L"main.asc",262144);
    const auto old_ans=ins::read_file(source/L"test"/L"fixtures"/L"rc9-stock"/L"main.ans",262144),old_asc=ins::read_file(source/L"test"/L"fixtures"/L"rc9-stock"/L"main.asc",262144);
    check(ins::read_file(ans,262144)==new_ans&&ins::read_file(asc,262144)==new_asc,"fresh current matching pair");
    const auto predecessor=[&]{write(ans,old_ans);write(asc,old_asc);};predecessor();run();
    check(ins::read_file(ans,262144)==new_ans&&ins::read_file(asc,262144)==new_asc,"exact old pair refreshed together");
    check(std::any_of(notices.begin(),notices.end(),[](const auto& n){return n.find(L"Updated matching stock menu pair: main")!=n.npos;}),"updated pair reported");
    predecessor();const std::vector<std::uint8_t> custom{'O','w','n','e','r'};write(asc,custom);
    check(SetFileAttributesW(ans.c_str(),FILE_ATTRIBUTE_READONLY)&&SetFileAttributesW(asc.c_str(),FILE_ATTRIBUTE_READONLY),"read-only custom mixed fixture");
    auto editor=ins::open_object(asc,false,GENERIC_READ,FILE_SHARE_READ);run();
    check(ins::read_file(ans,262144)==old_ans&&ins::read_file(asc,262144)==custom,"read-only custom pair and owner reader preserved");editor.reset();
    check(SetFileAttributesW(ans.c_str(),FILE_ATTRIBUTE_NORMAL)&&SetFileAttributesW(asc.c_str(),FILE_ATTRIBUTE_NORMAL),"restore only fixture attributes");
    check(DeleteFileW(asc.c_str())!=FALSE,"remove only fixture companion");run();
    check(!std::filesystem::exists(asc)&&ins::read_file(ans,262144)==old_ans,"missing partner stays absent and existing art stays exact");
    predecessor();auto reader=ins::open_object(asc,false,GENERIC_READ,FILE_SHARE_READ);bool refused=false;
    try{run();}catch(...){refused=true;}
    check(refused&&ins::read_file(ans,262144)==old_ans&&ins::read_file(asc,262144)==old_asc,"busy exact pair refuses before either write");reader.reset();run();
    check(ins::read_file(ans,262144)==new_ans&&ins::read_file(asc,262144)==new_asc,"retry after owner reader closes updates both");
    {
        const auto ba=root/L"Display"/L"bulletin.ans",bp=root/L"Display"/L"bulletin.asc";
        const auto oa=ins::read_file(source/L"test"/L"fixtures"/L"rc12-stock"/L"bulletin.ans",262144),op=ins::read_file(source/L"test"/L"fixtures"/L"rc12-stock"/L"bulletin.asc",262144);
        const auto na=ins::read_file(source/L"distribution"/L"Display"/L"bulletin.ans",262144),np=ins::read_file(source/L"distribution"/L"Display"/L"bulletin.asc",262144);
        write(ba,oa);write(bp,op);run();check(ins::read_file(ba,262144)==na&&ins::read_file(bp,262144)==np,"exact rc12 bulletin pair refreshes both corrected columns");
        write(ba,oa);write(bp,custom);run();check(ins::read_file(ba,262144)==oa&&ins::read_file(bp,262144)==custom,"custom rc12 bulletin companion preserves both files");
        write(bp,op);auto busy=ins::open_object(bp,false,GENERIC_READ,FILE_SHARE_READ);bool blocked=false;try{run();}catch(...){blocked=true;}
        check(blocked&&ins::read_file(ba,262144)==oa&&ins::read_file(bp,262144)==op,"busy rc12 bulletin pair refuses before writing either member");busy.reset();run();
        check(ins::read_file(ba,262144)==na&&ins::read_file(bp,262144)==np,"rc12 bulletin retry safely refreshes both");
    }
    for(const auto& p:std::filesystem::directory_iterator(root/L"Display"))check(p.path().filename().wstring().find(L".sfx-menu-")!=0,"no successful-update recovery debris");
    std::cout<<"PASS Windows stock menu preservation "<<checks<<" checks\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL Windows stock menus after "<<checks<<" checks: "<<e.what()<<'\n';return 1;}
