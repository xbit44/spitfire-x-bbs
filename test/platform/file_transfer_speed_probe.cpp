// Test-only actual native pump/helper/NTFS timing. Never a shipped program.
#define SFX_FILE_HOST_SUPPORT_ONLY
#include "file_transfer_host_tests.cpp"
#include <chrono>
#include <iomanip>
namespace {
bool coarse_idle{};
bool no_timer{}, timeout_once{};
std::uint64_t sleeps{}, sleep_us{}, waits{}, wait_us{};
using ProbeClock=std::chrono::steady_clock;
void probe_sleep(DWORD milliseconds){
    const auto start=ProbeClock::now();
    ::Sleep(milliseconds==1&&coarse_idle?16:milliseconds);
    ++sleeps;sleep_us+=std::chrono::duration_cast<std::chrono::microseconds>(ProbeClock::now()-start).count();
}
DWORD probe_wait(HANDLE handle,DWORD milliseconds){
    const auto start=ProbeClock::now();const auto result=::WaitForSingleObject(handle,milliseconds);
    if(milliseconds>0&&milliseconds<=2){++waits;wait_us+=std::chrono::duration_cast<std::chrono::microseconds>(ProbeClock::now()-start).count();}
    if(milliseconds==2&&timeout_once){timeout_once=false;return WAIT_TIMEOUT;}return result;
}
HANDLE probe_timer(LPSECURITY_ATTRIBUTES security,LPCWSTR name,DWORD flags,DWORD access){return no_timer ? nullptr : ::CreateWaitableTimerExW(security,name,flags,access);}
}
#define Sleep probe_sleep
#define WaitForSingleObject probe_wait
#define CreateWaitableTimerExW probe_timer
#if defined(SFX_SPEED_BASELINE)
#include "baseline_pump.inc"
#else
#include "../../src/apps/runtime/alpha_file_transfer.cpp"
#endif
#undef Sleep
#undef WaitForSingleObject
#undef CreateWaitableTimerExW

int wmain(int argc,wchar_t** argv)try{
    check(argc==5,"fresh fixture, helper, MiB and coarse flag required");
    const auto root=inst::canonical_root(argv[1]);
    check(root.parent_path()==inst::Path(L"C:/sfxtst/runs")&&root.filename().native().rfind(L"section9-speed-probe-",0)==0&&!std::filesystem::exists(root),"fresh bounded probe fixture");
    const auto mib=std::stoul(argv[3]);check(mib>=1&&mib<=100,"bounded timing input");const std::wstring mode=argv[4];coarse_idle=mode==L"coarse";no_timer=mode==L"fallback";timeout_once=mode==L"timeout";
    const auto owner=inst::operator_sid();const auto sd=inst::mutable_descriptor(owner),structural=inst::structural_descriptor(owner);
    inst::create_directory(root,sd.get());
    for(const auto& path:{root/L"Bin",root/L"Bin"/L"Tools",root/L"Work",root/L"Payload"})inst::create_directory(path,sd.get());
    const auto helper=inst::read_file(argv[2],4*1024*1024);check(inst::sha256(helper)=="0EA6C97136964F3AC4AAA9006E1D1AE51B982BA41C864DE82D2F86E895D90A0E","unchanged exact helper");
    inst::create_file(root/L"Bin"/L"Tools"/L"SFXFileTransfer.exe",helper,structural.get());protect_file_fixture(root/L"Bin"/L"Tools",structural.get());protect_file_fixture(root/L"Bin",structural.get());protect_file_fixture(root,structural.get());
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock");struct Cleanup{~Cleanup(){WSACleanup();}} cleanup;
    auto directory=win::FilePayloadDirectory::private_store(root/L"Payload",owner);
    auto lease=directory.create("bulk.bin",mib*1024ULL*1024ULL);
    const std::string body(mib*1024U*1024U,'S');
    Peer peer;frame(peer.c.wire,4);packet(peer.c.wire,std::string("bulk.zip")+'\0'+std::to_string(body.size())+'\0','k');peer.gate([&]{return peer.header(1);});
    std::size_t frames{};
    for(std::size_t at=0;at<body.size();at+=8192){
        frame(peer.c.wire,10,static_cast<std::uint32_t>(at));const auto count=std::min<std::size_t>(8192,body.size()-at);
        for(std::size_t offset=0;offset<count;offset+=1024)packet(peer.c.wire,std::string_view(body).substr(at+offset,std::min<std::size_t>(1024,count-offset)),offset+1024<count?'i':'k');
        const auto previous=frames++;peer.gate([&,previous]{return previous?peer.header(3,previous):peer.header(9);});
    }
    frame(peer.c.wire,11,static_cast<std::uint32_t>(body.size()));peer.gate([&]{return peer.header(3,frames);});peer.finish_receive(2);
    auto ports=peer.ports();ports.write=[&](std::string_view bytes){peer.c.emitted.append(bytes);return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,bytes.size()};};
    native::AlphaFileTransfer transfer(root,owner,std::move(ports));
    sleeps=sleep_us=waits=wait_us=0;const auto start=ProbeClock::now();
    const auto result=transfer.run(filewire::Direction::receive,{item(lease,true,"bulk.zip",body.size())});
    const auto us=std::chrono::duration_cast<std::chrono::microseconds>(ProbeClock::now()-start).count();
    check(result.code==native::NativeFileTransferCode::completed&&peer.c.synchronized&&result.files.size()==1&&result.files[0].completed&&result.files[0].bytes==body.size(),"actual bounded-window transfer completed with native receipt");
    check(lease.identity().bytes==body.size(),"native payload length");
    const auto expected=inst::sha256(std::vector<std::uint8_t>(body.begin(),body.end()));
    const char hex[]="0123456789ABCDEF";std::string actual;for(const auto byte:lease.identity().sha256){actual+=hex[byte>>4];actual+=hex[byte&15];}check(actual==expected,"exact SHA256 body");
    check(std::filesystem::is_empty(root/L"Work"),"owned helper scratch cleaned");
    std::cout<<"SPEED {\"bytes\":"<<body.size()<<",\"microseconds\":"<<us<<",\"bytes_per_second\":"<<(body.size()*1000000ULL/us)<<",\"sleep_calls\":"<<sleeps<<",\"sleep_us\":"<<sleep_us<<",\"wait_calls\":"<<waits<<",\"wait_us\":"<<wait_us<<",\"coarse_idle\":"<<(coarse_idle?"true":"false")<<",\"frames\":"<<frames<<"}"<<std::endl;
#if !defined(SFX_SPEED_BASELINE)
    if(no_timer)check(sleeps>0&&waits==0,"older-host timer failure preserves original bounded fallback");
    else check(sleeps==0&&waits>0,"high-resolution idle waits remain active, including bounded timeout");
    DWORD handles_before{},handles_after{};check(GetProcessHandleCount(GetCurrentProcess(),&handles_before)!=FALSE,"handle count before timer lifetime loop");
    for(unsigned n=0;n<100;++n){native::TransferIdleWait idle;idle.pause();}
    check(GetProcessHandleCount(GetCurrentProcess(),&handles_after)&&handles_before==handles_after,"all transfer timers closed; no process handle leak");
#endif
    std::cout<<"PASS exact helper, bounded8192 credit, CRC/EOF/receipt, SHA256 and scratch closure"<<std::endl;return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<std::endl;return 1;}
