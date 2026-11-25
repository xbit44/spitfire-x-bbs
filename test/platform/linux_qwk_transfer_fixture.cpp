#include "linux_qwk_transfer.hpp"
#include <chrono>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace app=sfx::application;
namespace rt=sfx::apps::runtime;
namespace fs=std::filesystem;
int main(int argc,char** argv){try{
    if(argc!=5||geteuid()==0)return 2;
    const fs::path root=argv[1];const int wire=std::stoi(argv[2]);const bool sending=std::string_view(argv[3])=="send";const std::string mode=argv[4];
    if(fcntl(wire,F_SETFL,fcntl(wire,F_GETFL)|O_NONBLOCK))throw std::runtime_error("fixture nonblocking");
    const auto now=[](){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());};
    const auto began=now();bool acquired{},released{},synchronized{},changed{};unsigned turns{},records{},notices{};std::uint64_t input{},output{};
    app::DecodedQwkTransferChannel channel;channel.acquire=[&]{if(acquired)return false;acquired=true;return true;};
    channel.release=[&](bool sync){released=true;synchronized=sync;};channel.now=now;
    app::QwkTransferRequest request;request.board_id="SFXTEST";request.board_revision=1;
    channel.authorized=[&](const app::QwkTransferRequest& r){return r.board_id==request.board_id&&r.board_revision==request.board_revision&&(mode!="revoke"||now()-began<150);};
    channel.absolute_deadline=[&]{
        if(mode=="late"||(mode=="tamper"&&!changed))for(const auto& entry:fs::directory_iterator(root/"work")){
            std::ifstream file(entry.path()/"result.txt",std::ios::binary);const std::string receipt{std::istreambuf_iterator<char>(file),{}};
            if(receipt.find("batch 1\nfailure 0\n")!=std::string::npos){
                changed=true;if(mode=="late")return began;
                std::ofstream edit(entry.path()/"result.txt",std::ios::binary|std::ios::app);edit<<"unexpected\n";
            }
        }
        return began+30000;
    };
    channel.read=[&](std::size_t maximum){
        app::InteractiveStreamReadResult result;result.bytes.resize(std::min<std::size_t>(maximum,251));const auto n=recv(wire,result.bytes.data(),result.bytes.size(),0);
        if(n>0){result.bytes.resize(static_cast<std::size_t>(n));result.outcome=app::InteractiveStreamOutcome::completed;input+=static_cast<std::uint64_t>(n);}
        else{result.bytes.clear();result.outcome=n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)?app::InteractiveStreamOutcome::timed_out:app::InteractiveStreamOutcome::disconnected;}
        return result;
    };
    channel.write=[&](std::string_view bytes){
        app::InteractiveStreamWriteResult result;
        if(mode=="extra"&&!changed)for(const auto& entry:fs::directory_iterator(root/"work")){
            std::ofstream file(entry.path()/"unexpected.txt",std::ios::binary|std::ios::app);file<<"unexpected\n";file.close();changed=true;
        }
        if(++turns%11==0){result.outcome=app::InteractiveStreamOutcome::timed_out;return result;}
        const auto n=::send(wire,bytes.data(),std::min<std::size_t>(bytes.size(),137),MSG_NOSIGNAL);
        if(n>0){result.bytes_written=static_cast<std::size_t>(n);result.outcome=app::InteractiveStreamOutcome::completed;output+=static_cast<std::uint64_t>(n);}
        else result.outcome=n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)?app::InteractiveStreamOutcome::timed_out:app::InteractiveStreamOutcome::disconnected;
        return result;
    };
    rt::LinuxQwkPacketTransfer transfer(root,std::move(channel),[](std::string_view why){std::cerr<<why<<'\n';},[&](std::string resource,std::string why){
        if(resource.size()!=36||resource.substr(0,4)!="qwk-"||why.substr(0,12)!="packet_work_")throw std::runtime_error("notice structure");
        ++notices;
    });
    if(!transfer.available(request))throw std::runtime_error("fixture helper unavailable");
    app::QwkTransferCode code{};app::QwkTransferRefusal refusal{};std::size_t before{};
    std::shared_ptr<app::IQwkReplySource> retained;
    const auto break_cleanup=[&]{if(mode=="cleanup")for(const auto& entry:fs::directory_iterator(root/"work")){std::ofstream unexpected(entry.path()/"retained.txt");unexpected<<"KEEP";}};
    {
        if(sending){
            std::ifstream file(root/"prepared.zip",std::ios::binary);const std::string bytes{std::istreambuf_iterator<char>(file),{}};
            auto result=transfer.send(request,bytes);code=result.code;refusal=result.refusal;
            before=static_cast<std::size_t>(std::distance(fs::directory_iterator(root/"work"),fs::directory_iterator{}));
            if(code==app::QwkTransferCode::completed&&mode!="unconfirmed"){if(!result.confirm_completed)throw std::runtime_error("missing confirmation");result.confirm_completed();result.confirm_completed();}
            break_cleanup();
        }else{
            auto result=transfer.receive(request);code=result.code;refusal=result.refusal;
            before=static_cast<std::size_t>(std::distance(fs::directory_iterator(root/"work"),fs::directory_iterator{}));
            if(code==app::QwkTransferCode::completed){
                if(!result.source||!result.source->rewind())throw std::runtime_error("missing sealed reply");
                for(;;){const auto r=result.source->next();if(!r||!r.value)throw std::runtime_error("spool read failed");if(!*r.value)break;++records;}
                if(records!=2||result.source->info().board_id!="SFXTEST")throw std::runtime_error("spool contents");
                if(mode=="unconfirmed")retained=result.source;else result.source->confirm_completed();
            }
            break_cleanup();
        }
    }
    const bool token_cancelled=retained&&retained->cancelled();retained.reset();
    const auto after=std::distance(fs::directory_iterator(root/"work"),fs::directory_iterator{});int child_status{};
    const bool reaped=waitpid(-1,&child_status,WNOHANG)==-1&&errno==ECHILD;
    std::cout<<"{\"code\":"<<static_cast<unsigned>(code)<<",\"refusal\":"<<static_cast<unsigned>(refusal)<<",\"acquired\":"<<acquired<<",\"released\":"<<released<<",\"synchronized\":"<<synchronized
        <<",\"input\":"<<input<<",\"output\":"<<output<<",\"records\":"<<records<<",\"work_before\":"<<before<<",\"work_after\":"<<after<<",\"token_cancelled\":"<<token_cancelled<<",\"reaped\":"<<reaped<<",\"notices\":"<<notices<<"}\n";
    if(!reaped||!acquired||!released||notices!=(after?1U:0U))return 1;
    if(mode=="cleanup")return code==app::QwkTransferCode::completed&&synchronized&&after==1?0:1;
    if(mode=="revoke"||mode=="late"||mode=="extra"||mode=="tamper"||mode=="bad-tail")return code==app::QwkTransferCode::disconnected&&!synchronized&&after==1?0:1;
    if(mode=="invalid"||mode=="extension"||mode=="skip")return code==app::QwkTransferCode::refused&&synchronized&&after==1?0:1;
    if(mode=="empty")return code==app::QwkTransferCode::cancelled&&synchronized&&after==0?0:1;
    if(mode=="unconfirmed")return code==app::QwkTransferCode::completed&&synchronized&&after==1&&(sending||token_cancelled)?0:1;
    return code==app::QwkTransferCode::completed&&synchronized&&before==1&&after==0?0:1;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
