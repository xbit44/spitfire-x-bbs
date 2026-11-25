// Explicit disposable beta.4 fixture only. Production message commits and
// ordinary existing credentials; never discover/adopt an owner board.
#include "fixture_paths.hpp"
#include "sfx/application/menu_session.hpp"
#define main retained_qwk_storage_main
#include "../integration/qwk_storage_tests.cpp"
#undef main
#include <fstream>
#ifdef _WIN32
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/platform/win32/alpha_runtime_platform.hpp"
#include "sfx/platform/win32/offline_account_administration.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#else
#include <unistd.h>
#endif
int main(int argc,char** argv)try{
    check(argc==2,"explicit beta4 disposable board required");const auto root=std::filesystem::absolute(argv[1]);
#ifdef _WIN32
    namespace win=sfx::platform::win32;namespace ins=win::installation;
    check(root==std::filesystem::path(L"C:\\Program Files\\SFX-Beta4-Test-20261113-r1"),"exact Windows synthetic fixture");const auto owner=ins::operator_sid();ins::verify_runtime_installation(root,owner);win::OfflineInstallationGuard guard;check(guard.acquire(root,owner).succeeded(),"exclusive stopped normal installed fixture");win::WindowsLocalStoragePathPolicy paths(owner);
#else
    check(geteuid()==1001&&root==(sfx::test::fixture_account_home() / "sfx-tests/beta4-20261113-r1/installed-fresh-r1/board"),"exact dedicated-account native synthetic fixture");sfx::test::FakeStoragePathPolicy paths;
#endif
    const auto file=root/"Data"/"SFX.DB";
    check(scalar(file,"SELECT count(*) FROM sfx_accounts")==1&&scalar(file,"SELECT count(*) FROM sfx_local_messages")==0,"fresh one-account empty synthetic board only");
    {
        internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(1000),false);
        c.exec("INSERT INTO sfx_accounts(account_id,display_name,name_key,security_level,account_state,revision,purge_allowed,created_utc_seconds,modified_utc_seconds) VALUES(2,'Beta4 Author','beta4 author',10,'active',1,1,1793970000,1793970000);INSERT INTO sfx_account_statistics(account_id) VALUES(2);INSERT INTO sfx_account_profiles(account_id) VALUES(2);INSERT INTO sfx_account_name_reservations VALUES('beta4 author',2,1,'Beta4 Author',1793970000,NULL)","closed synthetic author without login credentials");
        for(unsigned account=1;account<=2;++account){internal::SqliteStatement q(c.get(),"INSERT INTO sfx_account_sessions VALUES(?1,?2,?2,1793970000,0,1793970000,NULL,NULL,?1)","trusted synthetic offline seed session");q.bind(1,id(50000+account));q.bind(2,static_cast<std::int64_t>(account));q.step();}
    }
    storage::SqliteStorageSettings settings;settings.data_directory=root/"Data";storage::SqliteStorage db(settings,paths);check(db.start().succeeded(),"normal production writer opens stopped fixture");
    const app::MessageActor author{id(50002),2,1,scalar(file,"SELECT revision FROM sfx_security_policy WHERE singleton_id=1")};
    for(unsigned n=0;n<256;++n){
        const auto context=db.begin_message(author,{app::MessageKind::public_echo,{2},{},{}});check(bool(context),"production public compose context");
        std::string body;std::uint32_t random=n+1;
        for(unsigned i=0;i<4096;++i){random^=random<<13;random^=random>>17;random^=random<<5;body.push_back(static_cast<char>('!'+random%90));if(i%64==63)body+='\n';}
        if(n==200)body+=std::string("Classic pi ")+static_cast<char>(0xe3)+" stays on this line\nNext line\n";
        check(bool(db.commit_message({author,id(51000+n),*context.value,"Beta4 Public "+std::to_string(n),body})),"production synthetic message commit");
    }
    check(db.shutdown().succeeded(),"synthetic seed writer closes");
    {internal::SqliteConnection c(file,SQLITE_OPEN_READWRITE,std::chrono::milliseconds(1000),false);c.exec("UPDATE sfx_account_sessions SET completed_utc_seconds=1793970001,outcome_code='normal' WHERE account_id IN(1,2) AND started_utc_seconds=1793970000","close only synthetic seed sessions");}
    std::cout<<"PASS beta4 synthetic source seed "<<checks<<" checks; 256 public messages, content pi retained, credentials unchanged\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL beta4 source seed after "<<checks<<": "<<e.what()<<'\n';return 1;}
