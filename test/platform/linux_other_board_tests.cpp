#include "sfx/platform/linux/other_board_text_file.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
namespace app=sfx::application;
namespace host=sfx::platform::linux_host;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
}
int main(int argc,char** argv){try{
    check(argc==2&&geteuid()!=0,"fresh private fixture and normal user required");const fs::path root=argv[1];
    check(root.is_absolute()&&!fs::exists(root)&&fs::create_directory(root)&&chmod(root.c_str(),0700)==0,"new owned fixture root");
    check(fs::create_directory(root/"config")&&chmod((root/"config").c_str(),0700)==0,"new private config");
    host::initialize_other_board_text(root);const auto directory=host::open_owned_directory(root/"config");
    std::vector<app::OtherBoardLogRecord> logs;
    host::LinuxOtherBoardTextStorage store(root,[&](const auto& event){logs.push_back(event);return true;});
    const auto empty=store.read(0);check(empty&&empty.value&&empty.value->entries.empty(),"initial catalog is empty and available");
    check(!store.read(-1),"invalid cursor rejected");
    host::LinuxSecureIdentifierGenerator ids;app::ClassicInformationActor actor{true,0,0,1794000000,ids.generate()};
    app::ClassicInformationChange change;change.values={"Native board","example.test:2323","example.test:2222"};
    const auto added=store.change(change,actor,"Fixture owner");check(added&&added.value,"add durable row");
    check(logs.empty(),"adapter does not publish journal inside authority transaction");
    check(store.publish_log(*added.value)&&logs.size()==1&&logs.front().board_name==change.values.name,"post-transaction journal receives exact committed row");
    auto before=store.read(0);check(before&&before.value&&before.value->entries.size()==1,"read added row");
    change.action=app::ClassicInformationAction::edit_board;change.id=1;change.expected_revision=before.value->entries[0].revision;change.values.name="Edited board";
    check(store.change(change,actor,"Fixture owner").succeeded(),"matching whole-file revision edits row");
    check(store.change(change,actor,"Fixture owner").status.code==app::StorageErrorCode::conflict,"stale selection cannot overwrite later edit");
    before=store.read(0);check(before&&before.value&&before.value->entries[0].values.name=="Edited board","stale refusal preserves accepted bytes");
    auto rows=std::vector<app::OtherBoardValues>{{"Manual owner edit","manual.test:2323",""}};
    auto bytes=app::encode_other_board_text(rows);host::publish_private_file(directory.get(),"otherbbs.txt",{bytes.begin(),bytes.end()},true);
    change.expected_revision=before.value->entries[0].revision;
    check(store.change(change,actor,"Fixture owner").status.code==app::StorageErrorCode::conflict,"manual owner replacement invalidates old selection");
    before=store.read(0);check(before&&before.value&&before.value->entries[0].values.name=="Manual owner edit","manual edit becomes authoritative immediately");
    host::FileDescriptor lock(openat(directory.get(),".otherbbs.lock",O_RDWR|O_NOFOLLOW|O_CLOEXEC));
    check(bool(lock)&&flock(lock.get(),LOCK_EX|LOCK_NB)==0,"hold independent writer lock");
    change.expected_revision=before.value->entries[0].revision;
    check(!store.change(change,actor,"Fixture owner"),"another writer lock prevents replacement");lock=host::FileDescriptor{};
    const auto file=root/"config/otherbbs.txt";
    check(chmod(file.c_str(),0644)==0&&!store.read(0),"shared-readable catalog refuses access");check(chmod(file.c_str(),0600)==0,"restore fixture private mode");
    fs::rename(file,root/"config/original.txt");check(symlink("original.txt",file.c_str())==0&&!store.read(0),"catalog symlink refused");fs::rename(file,root/"config/rejected-link");
    check(mkfifo(file.c_str(),0600)==0&&!store.read(0),"unexpected FIFO refused without blocking");fs::rename(file,root/"config/rejected-fifo");
    check(link((root/"config/original.txt").c_str(),file.c_str())==0&&!store.read(0),"multiply linked catalog refused");fs::rename(file,root/"config/rejected-hardlink");
    // Keep the hardlinked evidence; publish a new independent canonical object.
    host::publish_private_file(directory.get(),"otherbbs.txt",{bytes.begin(),bytes.end()},false);
    host::publish_private_file(directory.get(),"otherbbs.txt",{0},true);check(!store.read(0),"malformed text does not become an empty successful catalog");
    host::initialize_other_board_text(root);check(!store.read(0),"ordinary initialization preserves malformed owner text");
    host::publish_private_file(directory.get(),"otherbbs.txt",{bytes.begin(),bytes.end()},true);
    before=store.read(0);check(before&&before.value,"restore valid fixture text");
    change.action=app::ClassicInformationAction::remove_board;change.values={};change.expected_revision=before.value->entries[0].revision;
    check(store.change(change,actor,"Fixture owner").succeeded(),"remove exact current row");
    check(store.read(0).value->entries.empty(),"completed deletion persists");
    std::cout<<"PASS "<<checks<<" Linux private Other BBS text/authority-bound adapter checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
