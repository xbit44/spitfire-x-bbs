#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <stdexcept>

namespace fs=std::filesystem;
namespace host=sfx::platform::linux_host;
namespace codec=sfx::platform::portable;
namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
void directory(const fs::path& path){check(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"new private directory");}
void put(const fs::path& path,std::string_view value,bool replace=false){
    const auto parent=host::open_owned_directory(path.parent_path());
    host::publish_private_file(parent.get(),path.filename().string(),{value.begin(),value.end()},replace);
}
void run(const fs::path& root){
    check(geteuid()!=0&&root.is_absolute()&&!fs::exists(root),"normal user and fresh fixture");
    directory(root);const auto board=root/"board";directory(board);directory(board/"config");
    check(host::load_portable_board_key(board).status==host::PortableKeyStatus::missing,"missing load does not generate identity");
    check(!(sfx::test::mark_provisioned_linux_fixture(board),host::initialize_fresh_portable_board_key(board))&&!fs::exists(board/"config/board.key"),"initialization requires explicit first-run intent");
    put(board/"config/first-run.v1","SFX_LINUX_FIRST_RUN=1\n");
    auto first=(sfx::test::mark_provisioned_linux_fixture(board),host::initialize_fresh_portable_board_key(board));
    check(bool(first)&&first.diagnostic==std::string("portable_key_ready"),"fresh private portable identity created");
    check(fs::file_size(board/"config/board.key")==88,"existing portable key format retained");
    struct stat info{};check(lstat((board/"config/board.key").c_str(),&info)==0&&(info.st_mode&0777)==0600&&info.st_nlink==1,"key is private single-link file");
    auto again=(sfx::test::mark_provisioned_linux_fixture(board),host::initialize_fresh_portable_board_key(board));
    check(again&&first.value->board==again.value->board&&first.value->key.bytes==again.value->key.bytes,"retry never replaces published key");
    const auto sealed=codec::seal_portable_credential(first.value->key,first.value->board,{'S','F','X','R'},"RLOGIN","synthetic portable password");
    check(sealed.has_value(),"shared shipping envelope seals fixture credential");
    const auto moved=root/"moved board";fs::rename(board,moved);
    auto moved_key=host::load_portable_board_key(moved);
    check(moved_key&&moved_key.value->board==first.value->board,"whole-folder move has no path lock");
    check(codec::open_portable_credential(moved_key.value->key,moved_key.value->board,{'S','F','X','R'},"RLOGIN",*sealed)->view()=="synthetic portable password","moved identity decrypts existing credential");
    const auto copy=root/"copied board";fs::copy(moved,copy,fs::copy_options::recursive);
    auto copied_key=host::load_portable_board_key(copy);
    check(copied_key&&copied_key.value->board==first.value->board&&copied_key.value->key.bytes==first.value->key.bytes,"whole-folder copy retains identity without inode/machine binding");
    const auto key=copy/"config/board.key",saved=copy/"config/original-key-preserved";
    fs::rename(key,saved);
    check(!(sfx::test::mark_provisioned_linux_fixture(copy),host::initialize_fresh_portable_board_key(copy))&&!fs::exists(key),"established marker forbids missing-key regeneration");
    check(mkfifo(key.c_str(),0600)==0,"FIFO key fixture without writer");
    check(!host::load_portable_board_key(copy)&&!(sfx::test::mark_provisioned_linux_fixture(copy),host::initialize_fresh_portable_board_key(copy))&&fs::is_fifo(key),"FIFO key promptly refused and preserved");
    check(unlink(key.c_str())==0,"remove only added FIFO");
    check(symlink("original-key-preserved",key.c_str())==0,"key symlink fixture");
    check(!host::load_portable_board_key(copy),"symlink key refused");check(unlink(key.c_str())==0,"remove only added symlink");
    fs::rename(saved,key);
    check(chmod(key.c_str(),0644)==0&&!host::load_portable_board_key(copy),"shared key permissions refused without repair");
    check(chmod(key.c_str(),0600)==0,"restore private fixture permissions");
    const auto metadata=copy/"config/portable-credentials.v1";
    fs::rename(metadata,copy/"config/original-marker-preserved");
    check(!host::load_portable_board_key(copy),"ordinary load cannot repair incomplete identity metadata");
    auto resumed=(sfx::test::mark_provisioned_linux_fixture(copy),host::initialize_fresh_portable_board_key(copy));
    check(resumed&&resumed.value->key.bytes==first.value->key.bytes,"explicit interrupted fresh setup finishes marker with unchanged key");
    put(metadata,"damaged marker\n",true);
    check(!(sfx::test::mark_provisioned_linux_fixture(copy),host::initialize_fresh_portable_board_key(copy))&&!host::load_portable_board_key(copy),"damaged existing marker is not replaced");
    const auto config=host::open_owned_directory(copy/"config");
    const auto text=host::read_private_file(config.get(),"portable-credentials.v1",256);
    check(std::string(text.begin(),text.end())=="damaged marker\n","failed recovery preserves diagnostic bytes");
    auto unaffected=host::load_portable_board_key(moved);
    check(unaffected&&unaffected.value->key.bytes==first.value->key.bytes,"independent original remains intact");
}
}
int main(int argc,char** argv){try{check(argc==2,"explicit fixture path");run(fs::path(argv[1]));
    std::cout<<"PASS "<<checks<<" Linux portable key custody checks; no key bytes logged\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
