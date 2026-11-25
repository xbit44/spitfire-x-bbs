#include "linux_web_ftn_source.hpp"
#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>

namespace {
namespace web=sfx::apps::runtime::web;using Code=web::FtnEchoListDownloadCode;
unsigned checks{};void check(bool ok,const char* name){++checks;if(!ok)throw std::runtime_error(name);}
}
int main(int argc,char** argv){try{
    if(argc==3&&(std::string_view(argv[1])=="--ftn-list-url"||std::string_view(argv[1])=="--ftn-list-file"))return web::run_ftn_echo_source_worker(argv[2],std::string_view(argv[1])=="--ftn-list-url");
    if(argc!=3&&argc!=5)throw std::runtime_error("fixture directory, local HTTP URL, optional HTTPS URL and fixture certificate required");
    const auto root=std::filesystem::absolute(argv[1]);if(!std::filesystem::create_directory(root))throw std::runtime_error("fresh source fixture required");
    const auto input=root/"review.na";{std::ofstream stream(input);stream<<"TEST.CHAT Test discussion\n";}
    std::atomic<bool> cancel{false};auto result=web::read_ftn_echo_source(input.string(),false,cancel);
    check(result.code==Code::ok&&result.bytes=="TEST.CHAT Test discussion\n","ordinary explicit local source copied byte-exact through isolated worker");
    std::filesystem::create_symlink(input,root/"link.na");check(web::read_ftn_echo_source((root/"link.na").string(),false,cancel).code!=Code::ok,"final symlink source refused");
    check(mkfifo((root/"pipe.na").c_str(),0600)==0,"fifo fixture created");const auto started=std::chrono::steady_clock::now();check(web::read_ftn_echo_source((root/"pipe.na").string(),false,cancel).code!=Code::ok&&std::chrono::steady_clock::now()-started<std::chrono::seconds(2),"nonordinary FIFO refused without blocking server");
    {std::ofstream stream(root/"large.na");stream.seekp(4*1024*1024);stream.put('X');}
    check(web::read_ftn_echo_source((root/"large.na").string(),false,cancel).code==Code::too_large,"local source maximum is four MiB");
    check(web::read_ftn_echo_source("relative.na",false,cancel).code==Code::invalid_url,"local path must be explicit absolute path");
    check(web::read_ftn_echo_source("file:///etc/passwd",true,cancel).code==Code::invalid_url,"network import cannot select non-HTTP protocols");
    check(web::read_ftn_echo_source("https://user:secret@example.test/list.na",true,cancel).code==Code::invalid_url,"network source rejects embedded credentials");
    cancel.store(true);check(web::read_ftn_echo_source(input.string(),false,cancel).code==Code::cancelled,"pre-cancelled source launches no read");cancel.store(false);
    const std::string base=argv[2];result=web::read_ftn_echo_source(base+"/valid",true,cancel);
    check(result.code==Code::ok&&result.bytes=="TEST.NET Native HTTP list\n","explicit HTTP source downloaded through owned helper");
    result=web::read_ftn_echo_source(base+"/redirect",true,cancel);check(result.code==Code::ok&&result.bytes=="TEST.NET Native HTTP list\n","bounded HTTP redirect follows explicit source policy");
    check(web::read_ftn_echo_source(base+"/loop",true,cancel).code==Code::redirect,"redirect loop refused at bound");
    check(web::read_ftn_echo_source(base+"/huge",true,cancel).code==Code::too_large,"network body limit enforced");
    check(web::read_ftn_echo_source(base+"/missing",true,cancel).code==Code::http_error,"HTTP failure is not an empty successful list");
    const auto before=std::chrono::steady_clock::now();std::thread cancellation([&]{std::this_thread::sleep_for(std::chrono::milliseconds(200));cancel.store(true);});
    result=web::read_ftn_echo_source(base+"/slow",true,cancel);cancellation.join();check(result.code==Code::cancelled&&std::chrono::steady_clock::now()-before<std::chrono::seconds(2),"cancellation kills and joins stalled owned helper promptly");
    int status{};errno=0;check(waitpid(-1,&status,WNOHANG)==-1&&errno==ECHILD,"no owned source child remains after success failure or cancellation");
    if(argc==5){
        cancel.store(false);const std::string tls=argv[3];std::ifstream trust_file(argv[4]);const std::string trust((std::istreambuf_iterator<char>(trust_file)),std::istreambuf_iterator<char>());check(!trust.empty(),"isolated fixture certificate loaded without changing host trust");
        result=web::download_ftn_echo_list(tls+"/valid",cancel,&trust);check(result.code==Code::ok&&result.bytes=="TEST.NET Native HTTP list\n","HTTPS verifies selected fixture certificate and exact IP identity");
        check(web::download_ftn_echo_list(tls+"/valid",cancel).code==Code::certificate,"embedded production trust rejects unknown fixture certificate");
        check(web::read_ftn_echo_source(tls+"/valid",true,cancel).code==Code::certificate,"supervised production worker rejects unknown HTTPS certificate");
        auto wrong=tls;wrong.replace(wrong.find("127.0.0.1"),9,"localhost");check(web::download_ftn_echo_list(wrong+"/valid",cancel,&trust).code==Code::certificate,"trusted certificate with wrong host name rejected");
        result=web::download_ftn_echo_list(tls+"/downgrade",cancel,&trust);if(result.code!=Code::redirect)std::cerr<<"HTTPS downgrade result code: "<<static_cast<int>(result.code)<<'\n';check(result.code==Code::redirect,"verified HTTPS cannot redirect to plain HTTP");
        result=web::download_ftn_echo_list(tls+"/redirect",cancel,&trust);check(result.code==Code::ok&&result.bytes=="TEST.NET Native HTTP list\n","verified HTTPS relative redirect remains verified");
    }
    std::cout<<"PASS "<<checks<<" FTN source checks\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<" "<<error.what()<<'\n';return 1;}}
