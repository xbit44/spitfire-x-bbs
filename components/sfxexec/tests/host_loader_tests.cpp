// Test production helper functions against fresh owned files/socket only.
// Never invoke the native host main, serve(), an installed board or a DOS game.
#include <fstream>
#define main SfxHostMainNotInvoked
#include "bridge.cpp"
#undef main
static unsigned checks;
static void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> static void rejects(F f){bool caught=false;try{f();}catch(const std::exception&){caught=true;}check(caught,"expected safe refusal");}
int main(int argc,char** argv){try{
    check(argc==3,"fresh fixture root and real candidate BIN directory");auto root=safe_path(fs::path(argv[1]));check(!fs::exists(root),"fresh files only");fs::create_directories(root);
    auto binaries=safe_path(fs::path(argv[2]));
    for(int kind=0;kind<7;++kind){auto base=root/fs::path("pair"+std::to_string(kind));fs::create_directories(base/L"BIN");
        const wchar_t* names[]={L"SFXEXEC.DLL",L"SFXDOOR.EXE",L"SFXLDR.DLL"};
        for(unsigned i=0;i<3;++i){if(kind>=1&&kind<=3&&i==unsigned(kind-1))continue;auto input=read_file(binaries/names[i],16*1024*1024);if(kind>=4&&i==unsigned(kind-4))input.back()^=1;
            std::ofstream file(base/L"BIN"/names[i],std::ios::binary);file.write(input.data(),input.size());}
        if(kind==0){PairPins pins(base);check(true,"exact hash-bound triplet pinned");Handle write(CreateFileW((base/L"BIN"/L"SFXLDR.DLL").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr));check(write.h==INVALID_HANDLE_VALUE,"pinned diagnostic cannot be replaced");}
        else rejects([&]{PairPins pins(base);});
    }
    auto base=root/L"layout";fs::create_directories(base/L"GAMES"/L"guess");
    check(private_work(base,"GAMES\\guess")==safe_path(base/L"GAMES"/L"guess"),"relative guess path follows candidate drive/root");
    check(private_work(base,(base/L"GAMES"/L"guess").string())==safe_path(base/L"GAMES"/L"guess"),"absolute existing profile supported");
    rejects([&]{private_work(base,"..\\outside");});rejects([&]{private_work(base,"GAMES-other\\guess");});
    Acl acl;Context current;current.node=12;current.account=17;current.caller="Fixture";current.rows=25;
    Drop drop{current,10,5,"C:\\fixture\\NODE\\12\\","Sysop","Fixture","10/24/26","15:00",54000};
    auto work=base/L"GAMES"/L"guess",node=base/L"NODE"/L"12";fs::create_directories(node);
    {OwnedFiles files(acl);files.create(work/L"SFDOORS.DAT",sfdoors(drop));check(read_file(work/L"SFDOORS.DAT",8192)==sfdoors(drop),"fresh SFDOORS in game working directory");check(!fs::exists(node/L"SFDOORS.DAT"),"SFDOORS not misplaced in node directory");}
    check(!fs::exists(work/L"SFDOORS.DAT"),"owned transient lifecycle cleanup on failed startup");
    {OwnedFiles files(acl);files.create(node/L"DOOR.SYS",doorsys(drop));check(read_file(node/L"DOOR.SYS",8192)==doorsys(drop),"52-line DOOR.SYS in actual-node session directory");}
    {std::ofstream existing(work/L"SFDOORS.DAT");existing<<"prior independent test file";}
    {OwnedFiles files(acl);rejects([&]{files.create(work/L"SFDOORS.DAT",sfdoors(drop));});}
    check(read_file(work/L"SFDOORS.DAT",8192)=="prior independent test file","existing dropfile preserved on collision");
    WSADATA wsa{};check(WSAStartup(MAKEWORD(2,2),&wsa)==0,"owned loopback startup");Socket server,client,accepted;
    server.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    check(bind(server.s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(server.s,1)==0,"owned ephemeral listener");int size=sizeof(address);getsockname(server.s,reinterpret_cast<sockaddr*>(&address),&size);
    client.s=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(connect(client.s,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"owned client connect");accepted.s=accept(server.s,nullptr,nullptr);
    rejects([&]{runtime_peer(accepted.s,base,acl.sid);});check(true,"non-Runtime peer refused before log/board access");
    std::cout<<"PASS "<<checks<<" actual helper/Win32 file/socket checks; no native VDM/game\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<" win32="<<GetLastError()<<"\n";return 1;}}
