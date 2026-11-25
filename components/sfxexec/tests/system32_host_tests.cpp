// Execute production selection/identity helpers with owned temporary files.
// Never call serve(), host main, system_driver(), or a native DOS/game process.
#include <fstream>
#define main SfxHostMainNotInvoked
#include "bridge.cpp"
#undef main
extern "C" __declspec(dllimport) BYTE* __stdcall TestBuffer(void);
static unsigned checks;
static void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F>static void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,"expected safe refusal");}
static void copy_fixture(const fs::path& p,const fs::path& q){fs::create_directories(q.parent_path());check(CopyFileW(p.c_str(),q.c_str(),TRUE),"create owned fixture without overwrite");}
int main(int argc,char** argv){try{
    check(argc==3,"fresh fixture root and candidate files required");check(TestBuffer()!=nullptr,"private fake NTVDM loaded for ordinary DLL fixture");
    auto root=safe_path(fs::path(argv[1])),bins=safe_path(fs::path(argv[2]));check(!fs::exists(root),"fresh fixture root");fs::create_directories(root);
    for(unsigned kind=0;kind<7;++kind){auto base=root/fs::path("case"+std::to_string(kind));fs::create_directories(base/L"BIN");fs::create_directories(base/L"WindowsFixture");
        auto target=base/L"WindowsFixture"/L"SFXEXEC.DLL";
        if(kind!=1)copy_fixture(bins/L"SFXEXEC.DLL",target);if(kind!=2)copy_fixture(bins/L"SFXDOOR.EXE",base/L"BIN"/L"SFXDOOR.EXE");
        if(kind==3||kind==4){auto wrong=kind==3?target:base/L"BIN"/L"SFXDOOR.EXE";std::ofstream f(wrong,std::ios::binary|std::ios::app);f<<'!';}
        if(kind==5)copy_fixture(bins/L"SFXEXEC.DLL",base/L"BIN"/L"SFXEXEC.DLL");
        if(kind==6)check(CreateHardLinkW((base/L"other-link.dll").c_str(),target.c_str(),nullptr),"owned hardlink fixture");
        if(kind==0){PairPins pins(base,target);check(true,"exact System32 fixture plus paired launcher accepted");
            Handle write(CreateFileW(target.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));check(write.h==INVALID_HANDLE_VALUE,"readonly driver pin prevents replacement");
            check(!DeleteFileW(target.c_str()),"readonly pin refuses deletion");}
        else rejects([&]{PairPins pins(base,target);});
    }
    auto base=root/L"identity";fs::create_directories(base/L"BIN");auto target=base/L"WindowsFixture"/L"SFXEXEC.DLL",other=base/L"DifferentDirectory"/L"SFXEXEC.DLL";
    copy_fixture(bins/L"SFXDOOR.EXE",base/L"BIN"/L"SFXDOOR.EXE");copy_fixture(bins/L"SFXEXEC.DLL",target);copy_fixture(bins/L"SFXEXEC.DLL",other);
    PairPins pins(base,target);const std::string session(32,'a');auto report="SFXPAIR1|SFXEXEC-20261024-01|"+session+"|1|"+target.string();
    auto approve=[&](const std::string& r,const std::string& s,unsigned n){return startup_approval(r,s,n,GetCurrentProcessId(),pins.driver.h);};
    rejects([&]{approve(report,session,1);}); // Valid report is insufficient: module not loaded.
    rejects([&]{driver_path("SFXPAIR1|wrong-pair|"+session+"|1|"+target.string(),pins.driver.h);});
    rejects([&]{same_pinned_file(other,pins.driver.h,SFX_DLL_SHA256);}); // equal bytes, wrong file/path
    auto wrong=LoadLibraryExW(other.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(wrong!=nullptr,"actual same-byte wrong-path DLL loaded only in fixture");
    rejects([&]{approve(report,session,1);});check(FreeLibrary(wrong),"fixture wrong-path module released");
    auto module=LoadLibraryExW(target.c_str(),nullptr,LOAD_WITH_ALTERED_SEARCH_PATH);check(module!=nullptr,"actual pinned module loads with fixture VDM imports");
    check(approve(report,session,1)=="SFXSTART2|"+session+"|1","identity-bound approval only after actual module verification");
    rejects([&]{approve(report,std::string(32,'b'),1);});rejects([&]{approve(report,session,2);});
    rejects([&]{approve("SFXPAIR1|SFXEXEC-20261024-01|"+session+"|1|"+other.string(),session,1);});
    DWORD before{},after{};GetProcessHandleCount(GetCurrentProcess(),&before);
    for(unsigned i=0;i<6;++i){check(approve(report,session,1)=="SFXSTART2|"+session+"|1","repeat identity gate");rejects([&]{same_pinned_file(other,pins.driver.h,SFX_DLL_SHA256);});}
    GetProcessHandleCount(GetCurrentProcess(),&after);check(before==after,"success/refusal module inventories retain no handles");check(FreeLibrary(module),"fixture exact module released");
    auto game=base/L"GAMES"/L"guess";fs::create_directories(game);
    check(private_work(base,"GAMES\\guess")==safe_path(game),"relative existing game layout");rejects([&]{private_work(base,"..\\outside");});
    Acl acl;Context context;context.node=1;context.account=7;context.caller="Fixture";context.rows=25;
    Drop drop{context,10,5,"D:\\SFXDOS\\NODE\\1\\","Sysop","Fixture","10/24/26","17:00",61200};
    {OwnedFiles files(acl);files.create(game/L"SFDOORS.DAT",sfdoors(drop));check(read_file(game/L"SFDOORS.DAT",8192)==sfdoors(drop),"fresh game-working-directory SFDOORS retained");}
    check(!fs::exists(game/L"SFDOORS.DAT"),"owned transient unwound");
    auto fresh=session_id();Handle output(CreateMailslotA(name(fresh,1,"from_dos").c_str(),4000,0,nullptr));
    Handle writer(CreateFileA(name(fresh,1,"from_dos").c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr));
    Handle finished(CreateEventW(nullptr,TRUE,TRUE,nullptr)),credit(CreateSemaphoreW(nullptr,63,64,nullptr));
    check(output.h!=INVALID_HANDLE_VALUE&&writer.h!=INVALID_HANDLE_VALUE&&finished.h&&credit.h,"owned startup ack fixture");
    const std::string ack="SFXSTARTED2|"+fresh+"|1";DWORD sent{};
    check(WriteFile(writer.h,ack.data(),DWORD(ack.size()),&sent,nullptr)&&sent==ack.size(),"queue actual startup acknowledgment");
    startup_acknowledged(output.h,credit.h,finished.h,ack);check(true,"queued ack accepted even if game already exited");
    rejects([&]{startup_acknowledged(output.h,credit.h,finished.h,ack);});
    check(WaitForSingleObject(credit.h,0)==WAIT_OBJECT_0,"reserve mismatched ack credit");
    check(WriteFile(writer.h,"wrong",5,&sent,nullptr),"queue malformed ack");rejects([&]{startup_acknowledged(output.h,credit.h,finished.h,ack);});
    std::cout<<"PASS "<<checks<<" actual System32-contract helper/module/file checks; NO real System32/native VDM/game\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" win32="<<GetLastError()<<"\n";return 1;}}
