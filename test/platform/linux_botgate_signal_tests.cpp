// Link wrappers inject real broken-pipe writes in the downloader's curl calls.
// No wrapper or fault switch is linked into a production image.
#include "botgate_https.hpp"
#include <curl/curl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <cerrno>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
thread_local unsigned performed{},cleaned{},pipe_failures{};
void broken_pipe() {
    int descriptors[2]{};
    if(pipe(descriptors)!=0) { ++pipe_failures;return; }
    close(descriptors[0]);
    const auto result=write(descriptors[1],"x",1);
    if(result!=-1 || errno!=EPIPE) ++pipe_failures;
    close(descriptors[1]);
}
void require(bool value,const char* text) { if(!value) throw std::runtime_error(text); }
bool pending(int number) { sigset_t value{};require(sigpending(&value)==0,"read pending signals");return sigismember(&value,number)==1; }
void drain(int number) { sigset_t value{};sigemptyset(&value);sigaddset(&value,number);timespec zero{};require(sigtimedwait(&value,nullptr,&zero)==number,"drain fixture signal"); }
void exercise(bool blocked,bool already_pending) {
    sigset_t original{},wanted{};
    require(pthread_sigmask(SIG_SETMASK,nullptr,&original)==0,"read original mask");
    wanted=original;sigaddset(&wanted,SIGUSR1);
    if(blocked) sigaddset(&wanted,SIGPIPE);else sigdelset(&wanted,SIGPIPE);
    require(pthread_sigmask(SIG_SETMASK,&wanted,nullptr)==0,"set fixture mask");
    require(pthread_kill(pthread_self(),SIGUSR1)==0,"queue unrelated signal");
    if(already_pending) require(pthread_kill(pthread_self(),SIGPIPE)==0,"queue caller signal");
    performed=cleaned=pipe_failures=0;std::atomic<bool> stop{};
    const auto result=sfx::apps::runtime::https::get("https://127.0.0.1:1/never",stop,{});
    require(!result.bytes && result.transport_result==CURLE_SEND_ERROR,"transfer failure remains visible");
    require(performed==1 && cleaned==1 && !pipe_failures,"both transfer and cleanup generate real EPIPE");
    require(pending(SIGPIPE)==already_pending,"preserve caller pending SIGPIPE only");
    require(pending(SIGUSR1),"preserve unrelated pending signal");
    sigset_t after{};require(pthread_sigmask(SIG_SETMASK,nullptr,&after)==0,"read restored mask");
    for(int number=1;number<NSIG;++number)
        require(sigismember(&after,number)==sigismember(&wanted,number),"preserve complete caller mask");
    if(already_pending) drain(SIGPIPE);
    drain(SIGUSR1);
    require(pthread_sigmask(SIG_SETMASK,&original,nullptr)==0,"restore fixture mask");
}
}
extern "C" CURLcode __wrap_curl_easy_perform(CURL*) { ++performed;broken_pipe();return CURLE_SEND_ERROR; }
extern "C" void __real_curl_easy_cleanup(CURL*);
extern "C" void __wrap_curl_easy_cleanup(CURL* handle) { ++cleaned;broken_pipe();__real_curl_easy_cleanup(handle); }
int main() { try {
    require(geteuid()!=0,"unprivileged test required");
    struct sigaction before{};require(sigaction(SIGPIPE,nullptr,&before)==0 && before.sa_handler==SIG_DFL,"default fatal SIGPIPE disposition required");
    exercise(false,false);exercise(true,false);exercise(true,true);
    std::exception_ptr failure;
    std::thread worker([&]{try{exercise(false,false);exercise(true,true);}catch(...){failure=std::current_exception();}});
    worker.join();if(failure)std::rethrow_exception(failure);
    struct sigaction after{};require(sigaction(SIGPIPE,nullptr,&after)==0 && after.sa_handler==before.sa_handler,"global disposition unchanged");
    std::cout<<"PASS 5 downloader signal scenarios: real EPIPE during transfer and cleanup, default fatal handler survival, exact caller masks/pending signals, worker isolation\n";
    return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;} }
