#include "../support/include/sfx/test/linux_layout_fixture.hpp"
#include "linux_file_transfer.hpp"
#include "sfx/platform/linux/portable_file_bindings.hpp"
#include "sfx/platform/linux/portable_key_store.hpp"
#include <chrono>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <sys/socket.h>
#include <unistd.h>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace host=sfx::platform::linux_host;
namespace fs=std::filesystem;
int main(int argc,char** argv){try{
    if(argc<5||argc>6||geteuid()==0)return 2;
    const fs::path root=argv[1];const int wire=std::stoi(argv[2]);const bool sending=std::string_view(argv[3])=="send";
    const char code=argv[4][0];const std::string mode=argc==6?argv[5]:"";const bool revoke=mode=="revoke";
    const bool tamper=mode=="tamper",extra=mode=="extra",late=mode=="late";
    const auto protocol=code=='X'?app::TransferProtocolPreference::xmodem:code=='Y'?app::TransferProtocolPreference::ymodem:
        code=='G'?app::TransferProtocolPreference::ymodem_g:app::TransferProtocolPreference::zmodem;
    auto config=host::open_owned_directory(root/"config");const std::string intent="SFX_LINUX_FIRST_RUN=1\n";
    host::publish_private_file(config.get(),"first-run.v1",{intent.begin(),intent.end()},false);
    if(!(sfx::test::mark_provisioned_linux_fixture(root),host::initialize_fresh_portable_board_key(root)))throw std::runtime_error("fixture key");
    auto bindings=host::LinuxFilePayloadBindings::initialize_fresh(root);
    auto spool=host::FilePayloadDirectory::private_store(root/"data/fileintake",bindings);
    auto payload=spool.create("payload",8*1024*1024);
    if(sending){
        std::array<std::uint8_t,4096> data{};for(std::size_t i=0;i<data.size();++i)data[i]=static_cast<std::uint8_t>(i);
        for(unsigned i=0;i<64;++i)payload.append(data.data(),data.size());
        payload.identity();
    }
    const auto object=payload.native_object_identity();
    auto descriptor=sending?payload.send_transfer_descriptor():payload.begin_receive_transfer();
    if(fcntl(wire,F_SETFL,fcntl(wire,F_GETFL)|O_NONBLOCK))throw std::runtime_error("fixture nonblocking");
    const auto now=[](){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
    const auto began=now();bool acquired{},released{},synchronized{},changed{};std::uint64_t input{},output{};std::size_t turns{};
    rt::LinuxFileTransferChannel channel;
    channel.acquire=[&]{if(acquired)return false;acquired=true;return true;};
    channel.release=[&](bool sync){released=true;synchronized=sync;};
    channel.authorized=[&]{return !revoke||now()-began<150;};
    channel.now=now;channel.absolute_deadline=[&]{
        if(late)for(const auto& entry:fs::directory_iterator(root/"work")){
            std::ifstream file(entry.path()/"result.txt",std::ios::binary);const std::string receipt{std::istreambuf_iterator<char>(file),{}};
            if(receipt.find("batch 1\nfailure 0\n")!=std::string::npos){changed=true;return began;}
        }
        return began+30000;
    };channel.begin_recovery=[]{return true;};
    channel.read=[&](std::size_t maximum){
        app::InteractiveStreamReadResult result;result.bytes.resize(std::min<std::size_t>(maximum,251));
        const auto n=recv(wire,result.bytes.data(),result.bytes.size(),0);
        if(n>0){result.bytes.resize(static_cast<std::size_t>(n));result.outcome=app::InteractiveStreamOutcome::completed;input+=static_cast<std::uint64_t>(n);}
        else {result.bytes.clear();result.outcome=n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)?app::InteractiveStreamOutcome::timed_out:app::InteractiveStreamOutcome::disconnected;}
        return result;
    };
    channel.write=[&](std::string_view bytes){
        app::InteractiveStreamWriteResult result;
        if((tamper||extra)&&!changed){
            for(const auto& entry:fs::directory_iterator(root/"work")){
                std::ofstream file(entry.path()/(tamper?"job.cfg":"unexpected.txt"),std::ios::binary|std::ios::app);file<<"unexpected\n";file.close();changed=true;
            }
        }
        if(++turns%11==0){result.outcome=app::InteractiveStreamOutcome::timed_out;return result;}
        const auto n=send(wire,bytes.data(),std::min<std::size_t>(bytes.size(),137),MSG_NOSIGNAL);
        if(n>0){result.bytes_written=static_cast<std::size_t>(n);result.outcome=app::InteractiveStreamOutcome::completed;output+=static_cast<std::uint64_t>(n);}
        else result.outcome=n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)?app::InteractiveStreamOutcome::timed_out:app::InteractiveStreamOutcome::disconnected;
        return result;
    };
    rt::LinuxFileTransfer bridge(root,std::move(channel));if(!bridge.available())throw std::runtime_error("fixture helper unavailable");
    const auto result=bridge.run(sending?rt::file_transfer_detail::Direction::send:rt::file_transfer_detail::Direction::receive,
        {{"test.bin",8*1024*1024,object,descriptor.get()}},protocol);
    descriptor=host::FileDescriptor();if(!sending)payload.finish_receive_transfer_after_reap();
    const auto identity=payload.identity();const auto before=std::distance(fs::directory_iterator(root/"work"),fs::directory_iterator{});
    if(result.code==rt::LinuxFileTransferCode::completed){
        if(!released||!synchronized||result.files.size()!=1||!result.files[0].completed||result.files[0].bytes!=identity.bytes||!result.confirm_completed)throw std::runtime_error("fixture complete contract");
        result.confirm_completed();result.confirm_completed();
    }
    const auto after=std::distance(fs::directory_iterator(root/"work"),fs::directory_iterator{});
    std::cout<<"{\"code\":"<<static_cast<unsigned>(result.code)<<",\"acquired\":"<<acquired<<",\"released\":"<<released<<",\"synchronized\":"<<synchronized<<",\"bytes\":"<<identity.bytes
        <<",\"input\":"<<input<<",\"output\":"<<output<<",\"work_before\":"<<before<<",\"work_after\":"<<after<<"}\n";
    if(revoke||tamper||extra||late)return result.code==rt::LinuxFileTransferCode::disconnected&&released&&!synchronized&&after==1&&(!(tamper||extra||late)||changed)?0:1;
    return result.code==rt::LinuxFileTransferCode::completed&&before==1&&after==0?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
