// Test-only decoded caller channel exercising the production Windows adapter
// and the exact separate helper in a genuine, protected Setup installation.
#include <winsock2.h>
#include <windows.h>
#include "alpha_message_receive.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <algorithm>
#include <atomic>
#include <future>
#include <iostream>
#include <stdexcept>
#include <set>
#include <winioctl.h>
namespace app=sfx::application;
namespace inst=sfx::platform::win32::installation;
using Bytes=std::vector<std::uint8_t>;
unsigned checks{},cases{};
void check(bool ok,const char* label){++checks;if(!ok)throw std::runtime_error(label);}
// Independent CRC16/CCITT wire encoder. It contains no helper implementation.
std::uint16_t crc(std::uint16_t value,std::uint8_t byte){
    value^=std::uint16_t(byte)<<8;
    for(int i=0;i<8;++i)value=std::uint16_t((value<<1)^((value&0x8000)?0x1021:0));
    return value;
}
void escaped(Bytes& wire,std::uint8_t b){
    if(b==127 || b==255){wire.push_back(24);wire.push_back(b==127?'l':'m');}
    else if((b&0x60)==0){wire.push_back(24);wire.push_back(b^64);}else wire.push_back(b);
}
void frame(Bytes& wire,std::uint8_t kind,std::uint32_t position=0){
    wire.insert(wire.end(),{'*',24,'A'});std::uint16_t value{};
    const std::uint8_t fields[]{kind,std::uint8_t(position),std::uint8_t(position>>8),std::uint8_t(position>>16),std::uint8_t(position>>24)};
    for(auto b:fields){value=crc(value,b);escaped(wire,b);}escaped(wire,std::uint8_t(value>>8));escaped(wire,std::uint8_t(value));
}
void packet(Bytes& wire,std::string_view body,std::uint8_t end){
    std::uint16_t value{};for(unsigned char b:body){value=crc(value,b);escaped(wire,b);}
    value=crc(value,end);wire.push_back(24);wire.push_back(end);escaped(wire,std::uint8_t(value>>8));escaped(wire,std::uint8_t(value));
}
Bytes transfer(std::string_view source,bool finish=true,bool second=false,int declared=-1){
    Bytes wire;frame(wire,4);
    std::string metadata="owned.ans";metadata+='\0';metadata+=std::to_string(declared<0?source.size():declared);metadata+='\0';packet(wire,metadata,'k');
    frame(wire,10);for(std::size_t i=0;i<source.size();i+=1024)packet(wire,source.substr(i,1024),i+1024<source.size()?'i':'h');
    frame(wire,11,static_cast<std::uint32_t>(source.size()));
    if(second){frame(wire,4);packet(wire,std::string("second.txt\0" "1\0",13),'k');}
    if(finish){frame(wire,8);wire.insert(wire.end(),{'O','O'});}return wire;
}
struct Channel {
    Bytes wire;std::size_t offset{},chunk{4096},written{},reads{},writes{};
    bool acquired{},released{},synchronized{},permit{true},lease{true},disconnect{},stall{},tail_coalesced{};
    std::uint64_t clock{GetTickCount64()},limit{},advance{};std::function<void()> after_acquire,after_read,after_authorize;
    std::string emitted;
    std::size_t policy_offer_end{},policy_finish_end{};
    bool saw_header(std::string_view prefix)const{const auto at=emitted.find(prefix);return at!=std::string::npos && emitted.find("\r\n",at)!=std::string::npos;}
    app::DecodedMessageReceiveChannel ports(){return {
        [&]{if(!lease)return false;acquired=true;if(after_acquire)after_acquire();return true;},
        [&](bool sync){released=true;synchronized=sync;},
        [&](std::size_t maximum){++reads;if(after_read)after_read();clock+=advance;
            if(disconnect)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::disconnected,{}};
            if(stall || offset==wire.size())return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};
            auto end=wire.size();
            if(policy_offer_end && !saw_header("**\x18" "B05"))end=policy_offer_end;
            else if(policy_finish_end && !saw_header("**\x18" "B08"))end=policy_finish_end;
            if(offset>=end)return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};
            // Tail controls deliberately put OO and the already-read tail in
            // one read; they do not assert recovery of future unread input.
            const auto read_chunk=tail_coalesced && offset>=policy_finish_end?std::size_t(4096):chunk;
            auto count=std::min({maximum,read_chunk,end-offset});Bytes out(wire.begin()+offset,wire.begin()+offset+count);offset+=count;
            return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::completed,std::move(out)};},
        [&](std::string_view body){++writes;if(stall)return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::timed_out,0};
            auto count=std::min<std::size_t>(3,body.size());written+=count;emitted.append(body.substr(0,count));return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,count};},
        [&]{return advance?clock:GetTickCount64();},[&]{return limit;},[&](const auto&){if(after_authorize)after_authorize();return permit;}};}
};
app::MessageReceiveRequest request(){app::MessageReceiveRequest r;r.context.maximum_lines=1000;return r;}
app::MessageReceiveResult run(const inst::Path& root,Channel& channel){
    sfx::apps::runtime::AlphaMessageSourceReceiver receiver(root,inst::operator_sid(),channel.ports());
    check(receiver.available(request()),"exact protected helper preflight");++cases;return receiver.receive(request());
}
void policy_offer(Channel& c,std::string name,std::string size,std::string tail={}){
    frame(c.wire,4);packet(c.wire,name+'\0'+size+'\0','k');c.policy_offer_end=c.wire.size();
    frame(c.wire,8);c.policy_finish_end=c.wire.size();c.wire.insert(c.wire.end(),{'O','O'});c.wire.insert(c.wire.end(),tail.begin(),tail.end());
}
void empty_offer(Channel& c,std::string tail={}){
    c.wire.insert(c.wire.end(),{'r','z','\r'});frame(c.wire,0);frame(c.wire,8);
    c.policy_finish_end=c.wire.size();c.wire.insert(c.wire.end(),{'O','O'});c.wire.insert(c.wire.end(),tail.begin(),tail.end());
}
int wmain(int argc,wchar_t** argv)try{
    check(argc==2,"one exact fixture argument");const inst::Path root=inst::canonical_root(argv[1]);
    check(root==inst::Path(L"C:\\xtest-sfx-section8b-empty-batch-fault-r1-20261009"),"new fixed empty-batch fault fixture only; preserve previous fixtures/live installation");
    check(!inst::elevated(),"ordinary owner adapter qualification");inst::verify_runtime_installation(root,inst::operator_sid());
    check(inst::read_receipt(root,inst::operator_sid()).program_count==6,"six-program helper-bound receipt");
    WSADATA ws{};check(WSAStartup(MAKEWORD(2,2),&ws)==0,"Winsock initialized");
    struct Cleanup{~Cleanup(){WSACleanup();}} cleanup;
    const auto current_job=[&](const std::set<inst::Path>& prior){
        inst::Path found;for(const auto& item:std::filesystem::directory_iterator(root/L"Work"))
            if(item.path().filename().native().find(L"message-")==0 && !prior.count(item.path())){check(found.empty(),"one new job before helper resume");found=item.path();}
        check(!found.empty(),"new generated job found");return found;
    };
    const auto jobs=[&]{std::set<inst::Path> out;for(const auto& item:std::filesystem::directory_iterator(root/L"Work"))out.insert(item.path());return out;};
    for(auto chunk:{std::size_t(1),std::size_t(4096)})for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("Q"),std::string("\0",1),std::string("\x18"),std::string(4096,'\r')}){
        Channel c;c.chunk=chunk;c.tail_coalesced=true;empty_offer(c,tail);const auto before=jobs();const auto r=run(root,c);
        const bool safe=tail.empty() || tail=="\r" || tail=="\n" || tail=="\r\n";
        check(r.code==(safe?app::MessageReceiveCode::cancelled:app::MessageReceiveCode::disconnected) && !r.source && c.synchronized==safe,"empty completion cancellation only; unsafe ledger tail disconnects");
        check(r.refusal==app::MessageReceiveRefusal::none && jobs()==before,"empty creates no source/refusal/body and cleans owned files");
    }
    for(int mode=0;mode<5;++mode){Channel c;empty_offer(c);if(mode==0)c.wire.insert(c.wire.begin(),'Q');if(mode==1)c.wire.pop_back();if(mode==2)c.wire[c.wire.size()-2]='Q';if(mode==3){c.wire.clear();frame(c.wire,8,1);c.wire.insert(c.wire.end(),{'O','O'});}if(mode==4){c.wire.clear();frame(c.wire,4);packet(c.wire,std::string("art.ans\0" "4\0",10),'k');frame(c.wire,8);c.wire.insert(c.wire.end(),{'O','O'});}c.policy_finish_end=0;
        const auto before=jobs();const auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized && jobs()==before,"malformed incomplete or offered file cannot downgrade to empty");}
    for(int mode=0;mode<8;++mode){Channel c;empty_offer(c);const auto before=jobs();bool changed=false;
        c.after_authorize=[&]{if(changed || !c.saw_header("**\x18" "B08"))return;const auto path=current_job(before);
            if(mode==7){inst::create_file(path/L"payload.bin",{'X'},inst::mutable_descriptor(inst::operator_sid()).get());changed=true;return;}
            const auto raw=CreateFileW((path/(mode==6?L"dsz.log":L"result.txt")).c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);if(raw==INVALID_HANDLE_VALUE)return;auto file=inst::Handle(raw);
            std::string receipt="SFX-MESSAGE-RECEIVE 4\nattempted 0\ncompleted 0\nbytes 0\nzeof 0\nbatch 1\nfailure 0\nrefusal 3\ninput "+std::to_string(c.wire.size())+"\n";
            if(mode==0)receipt.replace(receipt.find("attempted 0"),11,"attempted 1");if(mode==1)receipt.replace(receipt.find("batch 1"),7,"batch 0");if(mode==2)receipt.replace(receipt.find("refusal 3"),9,"refusal 4");if(mode==3)receipt[20]='3';if(mode==4)receipt.replace(receipt.find("input ")+6,std::string::npos,"0\n");if(mode==5)receipt.replace(receipt.find("completed 0"),11,"completed 1");if(mode==6)receipt="Z 0 0 0 0 0 0 payload.bin\n";
            DWORD written{};check(WriteFile(file.get(),receipt.data(),static_cast<DWORD>(receipt.size()),&written,nullptr) && written==receipt.size() && SetEndOfFile(file.get()),"tamper empty completion evidence");changed=true;};
        const auto r=run(root,c);check(changed && r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"forged empty receipt DSZ or payload cannot restore editor");}
    for(int mode=0;mode<2;++mode){Channel c;empty_offer(c);c.after_authorize=[&]{if(c.saw_header("**\x18" "B08")){if(mode)c.limit=c.clock-1;else c.permit=false;}};const auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !c.synchronized,"empty completion cannot outlive authority/deadline");}
    for(const auto& offer:std::vector<std::pair<std::string,std::string>>{{"bad.exe","55402"},{"bad.zip","100"},{"no-extension","4"},{"large.txt","262145"},{"large.ASC","1099511627776"},{"large.ANS","18446744073709551615"}}){
        for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n")}){
            Channel c;policy_offer(c,offer.first,offer.second,tail);const auto before=jobs();const auto r=run(root,c);
            const auto expected=offer.first.find("large.")==0?app::MessageReceiveRefusal::too_large:app::MessageReceiveRefusal::extension;
            check(r.code==app::MessageReceiveCode::refused && r.refusal==expected && !r.source && c.synchronized,"production policy refusal has exact reason no source and synchronized lease");
            check(c.saw_header("**\x18" "B05") && c.saw_header("**\x18" "B08") && c.emitted.find("**\x18" "B09")==std::string::npos,"real ZSKIP/final ZFIN without requesting payload");
            check(jobs()==before,"policy refusal leaves no owned Work objects");
        }
    }
    for(const auto& offer:std::vector<std::pair<std::string,std::string>>{{"bad.exe","4"},{"large.txt","262145"}}){
        for(const auto& prefix:{std::string("Q"),std::string("*Q"),std::string("**\x18Z"),std::string("raw payload")}){
            Channel c;policy_offer(c,offer.first,offer.second);c.wire.insert(c.wire.begin()+c.policy_offer_end,prefix.begin(),prefix.end());c.policy_finish_end+=prefix.size();
            const auto before=jobs();const auto r=run(root,c);
            check(r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"DR-POLICY-001 malformed prefix must not become synchronized policy refusal");
            check(jobs()==before,"malformed refusal cleans known owned files");
        }
    }
    for(int mode=0;mode<6;++mode){
        Channel c;policy_offer(c,"bad.exe","55402");const auto before=jobs();bool changed=false;
        c.after_authorize=[&]{
            if(changed || !c.saw_header("**\x18" "B08"))return;
            const auto path=current_job(before);
            if(mode==5){inst::create_file(path/L"payload.bin",{'X'},inst::mutable_descriptor(inst::operator_sid()).get());changed=true;return;}
            const auto raw=CreateFileW((path/(mode==4?L"dsz.log":L"result.txt")).c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            if(raw==INVALID_HANDLE_VALUE)return; // Helper still owns its exclusive file handle.
            auto file=inst::Handle(raw);
            std::string receipt="SFX-MESSAGE-RECEIVE 4\nattempted 1\ncompleted 0\nbytes 0\nzeof 0\nbatch 1\nfailure 0\nrefusal "+std::string(mode==0?"4":"1")+"\ninput "+std::to_string(c.wire.size()+(mode==1?1:0))+"\n";
            if(mode==2)receipt.replace(receipt.find("completed 0"),11,"completed 1");
            if(mode==3)receipt.replace(receipt.find("bytes 0"),7,"bytes 1");
            if(mode==4)receipt="Z 0 0 0 0 0 0 payload.bin\n";
            DWORD written{};check(WriteFile(file.get(),receipt.data(),static_cast<DWORD>(receipt.size()),&written,nullptr) && written==receipt.size() && SetEndOfFile(file.get()),"tamper exact completed refusal evidence in hostile fixture");changed=true;
        };
        const auto r=run(root,c);
        std::cout<<"POLICY TAMPER mode="<<mode<<" changed="<<changed<<" code="<<static_cast<unsigned>(r.code)<<" sync="<<c.synchronized<<" checks="<<checks<<std::endl;
        check(changed && r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"unknown reason forged refusal states DSZ or extra payload cannot restore editor");
    }
    for(int mode=0;mode<2;++mode){Channel c;policy_offer(c,"bad.exe","4");c.after_authorize=[&]{if(c.saw_header("**\x18" "B08")){if(mode)c.limit=c.clock-1;else c.permit=false;}};const auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !c.synchronized,"policy refusal still requires fresh authority and unexpired deadline");}
    const std::string art="\x1b[2J\x1b[2;3H|12Owned artwork\x1b[5;34m!\r\n";
    for(auto size:{std::size_t(4096),std::size_t(1)}){
        Channel c;c.chunk=size;c.wire=transfer(art);const auto result=run(root,c);
        check(result.code==app::MessageReceiveCode::completed && result.source && result.source->bytes==art,"actual helper exact detached ANSI source");
        check(c.acquired && c.released && c.synchronized && c.offset==c.wire.size() && c.written>0,"exclusive lease, early bytes and partial outbound writes");
    }
    {Channel c;std::string bytes;bytes.reserve(262144);for(int i=0;i<65535;++i)bytes+="\x1b[0m";bytes+="CAP!";c.wire=transfer(bytes);auto r=run(root,c);
        check(r.code==app::MessageReceiveCode::completed && r.source->bytes==bytes,"host and helper exact-cap snapshot");}
    for(auto wire:{transfer(art,true,true),transfer(art,true,false,1),transfer(art,false),Bytes{24,24,24,24,24,24,24,24}}){
        Channel c;c.wire=std::move(wire);c.advance=1000;auto r=run(root,c);
        check(r.code==app::MessageReceiveCode::disconnected && !r.source && c.released && !c.synchronized,"incomplete/cancel/second-file uncertain protocol disconnects without source");
    }
    {Channel c;c.wire=transfer("\x1b]0;forbidden\a");auto r=run(root,c);check(r.code==app::MessageReceiveCode::refused && !r.source && c.synchronized,"completed forbidden source is refused without replacing draft");}
    for(const auto& tail:{std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("Q"),std::string("\0",1),std::string("\x18"),std::string("\x1b[A"),std::string(4096,'\r')}){
        Channel c;c.wire=transfer(art);c.wire.insert(c.wire.end(),tail.begin(),tail.end());
        const auto r=run(root,c);const bool allowed=tail=="\r" || tail=="\n" || tail=="\r\n";
        check(r.code==(allowed?app::MessageReceiveCode::completed:app::MessageReceiveCode::disconnected) && c.synchronized==allowed,"production helper receipt plus send ledger accepts one Enter only; malicious coalesced tails fail closed");
    }
    {Channel c;c.wire=transfer(art);frame(c.wire,4);packet(c.wire,std::string("second.txt\0" "1\0",13),'k');const auto r=run(root,c);
        check(r.code==app::MessageReceiveCode::disconnected && !c.synchronized,"extra protocol frame after OO cannot exploit private socket reset");}
    // Corrupt actual completed private receipts, not fabricated preexisting
    // files. This is an ordinary-owner test fixture only. The helper's file
    // handle must be closed and its final reply handed to the caller first.
    for(int mode=0;mode<5;++mode){
        const auto prior=jobs();Channel c;c.wire=transfer(art);bool changed=false;
        c.after_authorize=[&]{
            const std::string final="**\x18" "B0800000000022d\r\n";
            if(changed || c.emitted.size()<final.size() || c.emitted.substr(c.emitted.size()-final.size())!=final)return;
            const auto path=current_job(prior)/(mode==4?L"dsz.log":L"result.txt");
            const auto raw=CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
            if(raw==INVALID_HANDLE_VALUE)return;
            auto file=inst::Handle(raw);
            std::string receipt="SFX-MESSAGE-RECEIVE 4\nattempted 1\ncompleted 1\nbytes "+std::to_string(art.size())+"\nzeof 1\nbatch 1\nfailure 0\nrefusal 0\ninput ";
            if(mode==0)receipt+=std::to_string(c.wire.size()-1)+"\n";
            if(mode==1)receipt+=std::to_string(c.wire.size()+1)+"\n";
            if(mode==2)receipt+="0"+std::to_string(c.wire.size())+"\n";
            if(mode==3)receipt="SFX-MESSAGE-RECEIVE 1\nattempted 1\ncompleted 1\nbytes "+std::to_string(art.size())+"\nzeof 1\nbatch 1\nfailure 0\n";
            if(mode==4)receipt="Z 1 0 0 0 0 0 payload.bin\n";
            DWORD written{};check(WriteFile(file.get(),receipt.data(),static_cast<DWORD>(receipt.size()),&written,nullptr) && written==receipt.size() && SetEndOfFile(file.get()),"actual completed receipt altered in dedicated hostile fixture");changed=true;
        };
        const auto r=run(root,c);
        check(changed && r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"mismatched consumed input/canonical count/profile/DSZ cannot claim transfer success");
    }
    {Channel c;c.permit=false;sfx::apps::runtime::AlphaMessageSourceReceiver receiver(root,inst::operator_sid(),c.ports());
        check(!receiver.available(request()) && receiver.receive(request()).code==app::MessageReceiveCode::unavailable && !c.acquired,"permission denial before child/channel ownership");}
    {Channel c;c.lease=false;auto r=run(root,c);check(r.code==app::MessageReceiveCode::unavailable && !c.acquired && !c.released,"failed acquire kills suspended child before input");}
    for(int mode=0;mode<4;++mode){Channel c;c.stall=true;c.advance=1000;
        if(mode==0)c.disconnect=true;
        if(mode==1)c.after_read=[&]{if(c.reads>2)c.permit=false;};
        if(mode==2)c.limit=c.clock+2000;
        if(mode==3)c.advance=60000;
        auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && c.released && !c.synchronized,"disconnect/revocation/earlier/hard deadline closes binary session");}
    {const auto prior=jobs();Channel c;c.wire=transfer(art);c.after_acquire=[&]{const auto path=current_job(prior);
            auto moved=path;moved+=L"-moved";check(!MoveFileW(path.c_str(),moved.c_str()),"pinned job directory cannot be redirected");};
        check(run(root,c).code==app::MessageReceiveCode::completed,"path identity remains bound through child execution");}
    {const auto prior=jobs();Channel c;c.wire=transfer(art);c.after_acquire=[&]{
            inst::create_file(current_job(prior)/L"unknown.txt",{'R'},inst::mutable_descriptor(inst::operator_sid()).get());};
        auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"unknown job object rejects success and retains confined evidence");}
    {auto writable=CreateFileW((root/L"Bin/Tools/sexyz.exe").c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
        check(writable==INVALID_HANDLE_VALUE,"ordinary owner cannot replace protected helper");}
    {Channel left,right;left.wire=transfer(art);right.wire=transfer("Concurrent owned text");
        auto a=std::async(std::launch::async,[&]{sfx::apps::runtime::AlphaMessageSourceReceiver r(root,inst::operator_sid(),left.ports());return r.receive(request());});
        auto b=std::async(std::launch::async,[&]{sfx::apps::runtime::AlphaMessageSourceReceiver r(root,inst::operator_sid(),right.ports());return r.receive(request());});
        check(a.get().code==app::MessageReceiveCode::completed && b.get().code==app::MessageReceiveCode::completed,"two actual helpers have independent generated jobs/channels");cases+=2;}
    // Preexisting fixed-name objects must fail exclusive CREATE_NEW, never
    // overwrite a prior object or accept a fabricated success record.
    for(const auto* name:{L"payload.bin",L"result.txt",L"dsz.log"}){
        const auto prior=jobs();Channel c;c.wire=transfer(art);c.advance=1000;
        c.after_acquire=[&]{inst::create_file(current_job(prior)/name,{'R'},inst::mutable_descriptor(inst::operator_sid()).get());};
        const auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"exclusive file/result/log collision cannot claim success");
    }
    {const auto prior=jobs();const auto probe=root/L"Work"/(L"hardlink-probe-"+inst::unique_id());
        inst::create_file(probe,{'R'},inst::mutable_descriptor(inst::operator_sid()).get());Channel c;c.wire=transfer(art);c.advance=1000;
        c.after_acquire=[&]{check(CreateHardLinkW((current_job(prior)/L"payload.bin").c_str(),probe.c_str(),nullptr),"actual NTFS hardlink fault fixture created");};
        const auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"hardlinked fixed-name object is not accepted or followed");
        auto file=inst::Handle(CreateFileW(probe.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr));
        char value{};DWORD count{};check(file && ReadFile(file.get(),&value,1,&count,nullptr) && count==1 && value=='R',"hardlink target bytes retained; no truncation");
    }
    {const auto prior=jobs();const auto probe=root/L"Work"/(L"reparse-probe-"+inst::unique_id());
        inst::create_directory(probe,inst::mutable_descriptor(inst::operator_sid()).get());Channel c;c.wire=transfer(art);c.advance=1000;
        c.after_acquire=[&]{const auto path=current_job(prior)/L"payload.bin";
            inst::create_directory(path,inst::mutable_descriptor(inst::operator_sid()).get());
            auto file=inst::Handle(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
            const std::wstring substitute=L"\\??\\"+probe.native(),print=probe.native();
            std::vector<std::uint8_t> data(16+(substitute.size()+1+print.size()+1)*sizeof(wchar_t));
            auto* fields=reinterpret_cast<std::uint16_t*>(data.data()+8);
            *reinterpret_cast<DWORD*>(data.data())=IO_REPARSE_TAG_MOUNT_POINT;
            *reinterpret_cast<std::uint16_t*>(data.data()+4)=static_cast<std::uint16_t>(data.size()-8);
            fields[0]=0;fields[1]=static_cast<std::uint16_t>(substitute.size()*2);fields[2]=fields[1]+2;fields[3]=static_cast<std::uint16_t>(print.size()*2);
            memcpy(data.data()+16,substitute.c_str(),(substitute.size()+1)*2);memcpy(data.data()+16+fields[2],print.c_str(),(print.size()+1)*2);
            DWORD returned{};check(file && DeviceIoControl(file.get(),FSCTL_SET_REPARSE_POINT,data.data(),static_cast<DWORD>(data.size()),nullptr,0,&returned,nullptr),"actual owned NTFS reparse fault fixture created without elevation");};
        const auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !r.source && !c.synchronized,"reparse fixed-name object cannot be followed or claimed");
        check(std::filesystem::is_empty(probe),"reparse destination remains unchanged");
    }
    {const auto prior=jobs();Channel c;c.wire=transfer(art);c.advance=1000;
        c.after_acquire=[&]{inst::create_directory(current_job(prior)/L"payload.bin",inst::mutable_descriptor(inst::operator_sid()).get());};
        const auto r=run(root,c);check(r.code==app::MessageReceiveCode::disconnected && !r.source,"physical staged-file creation failure is not success");
    }
    std::cout<<"PASS host receive cases="<<cases<<" checks="<<checks<<" exact helper, detached source, fault/deadline/containment\n";return 0;
}catch(const std::exception& e){std::cerr<<"HOST RECEIVE FAIL "<<e.what()<<" checks="<<checks<<'\n';return 1;}
