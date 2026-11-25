#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#include "sfx/platform/linux/file_scanner.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

namespace host=sfx::platform::linux_host;
namespace app=sfx::application;
namespace fs=std::filesystem;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
std::string read(const fs::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),{}};}
void run(const fs::path& root,std::string_view mode){
    check(geteuid()!=0&&root.is_absolute(),"normal native owner");
    auto config=host::open_owned_directory(root/"config");const std::string intent="SFX_LINUX_FIRST_RUN=1\n";
    host::publish_private_file(config.get(),"first-run.v1",{intent.begin(),intent.end()},false);
    check(static_cast<bool>((sfx::test::mark_provisioned_linux_fixture(root),host::initialize_fresh_portable_board_key(root))),"real portable fixture key");
    auto bindings=host::LinuxFilePayloadBindings::initialize_fresh(root);
    auto staging=host::FilePayloadDirectory::private_store(root/"data/fileintake",bindings);
    host::LinuxFileScanner scanner(root);
    if(mode=="--unready"){
        auto payload=staging.create("benign-unready.txt",1024);
        const std::string body="Benign scanner readiness fixture.\n";
        payload.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());
        const auto original=payload.identity();
        const auto result=scanner.scan(payload,1024);
        check(result.process_reaped&&result.exit_code==0xffffffffU&&result.diagnostic=="scan_database_missing"&&
            result.result.verdict==app::FileScanVerdict::error,"missing definitions refuse scan before launch");
        check(payload.identity().sha256==original.sha256&&payload.identity().bytes==original.bytes,"unready scanning preserves exact private input");
        return;
    }
    const auto one=[&](const std::string& name,app::FileScanVerdict expected){
        const auto bytes=read(root/"Inputs"/name);check(!bytes.empty(),"real native fixture bytes");
        auto payload=staging.create(name,16*1024*1024);payload.append(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size());const auto original=payload.identity();
        const auto result=scanner.scan(payload,1024*1024);std::cout<<name<<" verdict="<<static_cast<unsigned>(result.result.verdict)<<" exit="<<result.exit_code<<" detail="<<result.result.detail<<" diagnostic="<<result.diagnostic<<'\n'<<std::flush;
        check(result.process_reaped,"native scanner child reaped");check(result.result.verdict==expected,"actual scanner classified expected one-file result");
        check(payload.identity().sha256==original.sha256&&payload.identity().bytes==original.bytes,"scanner never mutates/deletes original intake");
        if(expected==app::FileScanVerdict::detected)check(!result.result.detections.empty(),"real official signature evidence retained");
        else check(result.result.detections.empty(),"non-detection does not invent malware evidence");
    };
    one("clean.txt",app::FileScanVerdict::clean);
    if(mode=="--benign")return;
    one("zip64-clean.zip",app::FileScanVerdict::clean);
    one("eicar.txt",app::FileScanVerdict::detected);
    one("encrypted.zip",app::FileScanVerdict::incomplete);one("limit.zip",app::FileScanVerdict::incomplete);
    auto payload=staging.create("cancel.txt",1024);const std::string body="Unchanged native cancellation fixture";payload.append(reinterpret_cast<const std::uint8_t*>(body.data()),body.size());const auto original=payload.identity();
    auto result=scanner.scan(payload,1024*1024,[]{return true;});check(result.result.verdict==app::FileScanVerdict::incomplete&&result.process_reaped,"prelaunch cancellation holds without child");
    result=scanner.scan(payload,1024*1024,{}, {},1500);check(result.result.verdict==app::FileScanVerdict::timeout&&result.process_reaped,"real deadline reaps native scanner and holds original");
    bool cancel{};result=scanner.scan(payload,1024*1024,[&]{return cancel;},[&](std::uint64_t elapsed){if(elapsed>=1500)cancel=true;});
    check(cancel&&result.result.verdict==app::FileScanVerdict::incomplete&&result.process_reaped,"active native cancellation reaps and holds");
    bool observed_child{};
    result=scanner.scan(payload,1024*1024,[&]{
        const auto children=read(fs::path("/proc/self/task")/std::to_string(getpid())/"children");
        if(!children.empty())observed_child=true;
        return observed_child&&children.empty();
    });
    check(observed_child&&result.exit_code==0&&result.result.verdict==app::FileScanVerdict::incomplete&&result.process_reaped,"revocation after successful child exit still prevents clean acceptance");
    check(payload.identity().sha256==original.sha256,"cancel/timeout payload remains exact");
    const auto engine=host::open_owned_directory(root/"bin/clamav");host::publish_private_file(engine.get(),"libc.so.6",{'b','a','d'},false);
    result=scanner.scan(payload,1024*1024);check(result.result.verdict==app::FileScanVerdict::error&&result.diagnostic=="scan_unpinned_engine_entry"&&result.process_reaped,"unlisted side-loaded library refused before launch");
    check(unlinkat(engine.get(),"libc.so.6",0)==0,"retire only injected owned fixture file");
    std::uint64_t leftover{};for(const auto& entry:fs::directory_iterator(root/"work/clamav"))if(entry.path().filename()!="operation.lock"){
        for(const auto& member:fs::recursive_directory_iterator(entry.path()))if(member.is_regular_file()){struct stat status{};check(lstat(member.path().c_str(),&status)==0,"retained scratch stat");leftover+=static_cast<std::uint64_t>(status.st_blocks)*512;}}
    check(leftover<16*1024*1024,"resource reservations released after exact child reap");
}
}
int main(int argc,char** argv){try{check(argc==2||argc==3,"prepared private native root required");
    const std::string_view mode=argc==3?argv[2]:"";
    check(mode.empty()||mode=="--unready"||mode=="--benign","valid scanner test mode");
    run(argv[1],mode);std::cout<<"PASS "<<checks<<" real native scanner checks; intake acceptance tested separately\n";return 0;}catch(const std::exception& error){std::cerr<<"FAIL after "<<checks<<": "<<error.what()<<'\n';return 1;}}
