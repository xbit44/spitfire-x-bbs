// Isolated tests for the separate GPL helper, October 8 2026.
// GPL-2.0-or-later; this test binary is not an SFX product component.
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <iostream>
#include <algorithm>
#include <stdexcept>
#include <atomic>
#include <future>
#include "host-completion-review.hpp"
extern "C" {
#include "zmodem.h"
int zmodem_send_data_subpkt(zmodem_t*, unsigned char, unsigned char*, size_t);
int zmodem_recv_header(zmodem_t*);
}

// Exercise the actual helper callback clock code without waiting five minutes.
// This test-only time function is not compiled into the helper executable.
static ULONGLONG simulated_tick=0;
static ULONGLONG sfx_test_tick() { return simulated_tick; }
#define GetTickCount64 sfx_test_tick
#include "sfx_message_receive.inc"
#undef GetTickCount64

static std::atomic<unsigned> checks{0}, units{0}, processes{0};
static std::string root;
static void require(bool b,const char* text) {
    ++checks;
    if (!b) throw std::runtime_error(text);
}
struct Memory {
    std::vector<unsigned char> in,out;
    size_t pos=0;
    bool cancel=false;
};
static int put(void* p,unsigned char b,unsigned) {
    auto& c=*static_cast<Memory*>(p);
    if(c.out.size()>1048576) return -1;
    c.out.push_back(b); return 0;
}
static int get(void* p,unsigned) {
    auto& c=*static_cast<Memory*>(p);
    return c.pos<c.in.size()?c.in[c.pos++]:-1;
}
static BOOL on(void*) {return TRUE;}
static BOOL cancelled(void* p) {return static_cast<Memory*>(p)->cancel?TRUE:FALSE;}
static BOOL waiting(void* p,unsigned) {
    auto& c=*static_cast<Memory*>(p); return c.pos<c.in.size();
}
static zmodem_t init(Memory& m,bool strict=false) {
    zmodem_t z;
    zmodem_init(&z,&m,nullptr,nullptr,put,get,on,cancelled,waiting,nullptr);
    z.sfx_message=strict; z.max_errors=2; z.max_file_size=262144;
    z.can_fcs_32=FALSE; z.escape_telnet_iac=FALSE;
    return z;
}
static std::vector<unsigned char> metadata(const std::string& name,const std::string& fields) {
    std::vector<unsigned char> m(name.begin(),name.end());
    m.push_back(0); m.insert(m.end(),fields.begin(),fields.end()); m.push_back(0); return m;
}
static void frame(zmodem_t& z,int kind,unsigned pos=0) {
    require(zmodem_send_pos_header(&z,kind,pos,FALSE)==0,"encode header");
}
static void packet(zmodem_t& z,std::vector<unsigned char> bytes,unsigned char kind=ZCRCE) {
    require(zmodem_send_data_subpkt(&z,kind,bytes.data(),bytes.size())==0,"encode packet");
}
static std::vector<unsigned char> stream(size_t bytes,const std::string& name="art.ans",
    const std::string& fields="", bool eof=true, bool finish=true, bool extra=false,
    bool crc32=false, int offset=-1, bool streaming=false, unsigned file_flags=0, unsigned data_offset=0) {
    Memory m; auto z=init(m); z.can_fcs_32=crc32;
    frame(z,ZFILE,file_flags); packet(z,metadata(name,fields.empty()?std::to_string(bytes)+" 0 0 0 1 "+std::to_string(bytes)+" 0":fields),ZCRCW);
    frame(z,ZDATA,data_offset);
    size_t pos=0;
    while(pos<bytes) {
        auto n=std::min<size_t>(1024,bytes-pos);
        std::vector<unsigned char> data(n);
        for(size_t i=0;i<n;++i) data[i]=static_cast<unsigned char>((pos+i)%256);
        packet(z,data,(streaming && pos+n<bytes)?ZCRCG:ZCRCE);
        pos+=n;
        if(!streaming && pos<bytes) frame(z,ZDATA,static_cast<unsigned>(pos));
    }
    if(eof) frame(z,ZEOF,offset<0?static_cast<unsigned>(bytes):static_cast<unsigned>(offset));
    if(extra) { frame(z,ZFILE); packet(z,metadata("second.txt","1"),ZCRCW); }
    if(finish) { frame(z,ZFIN); m.out.push_back('O'); m.out.push_back('O'); }
    return m.out;
}
static void meta_case(const std::vector<unsigned char>& bytes,bool valid,const char* label) {
    Memory m; auto z=init(m,true);
    z.rx_data_subpacket_len=static_cast<unsigned>(bytes.size());
    require(bytes.size()<=sizeof(z.rx_data_subpacket),"metadata bound");
    memcpy(z.rx_data_subpacket,bytes.data(),bytes.size());
    zmodem_parse_zfile_subpacket(&z);
    require((z.sfx_failure==0)==valid,label);
    ++units;
}
static void data_case(std::vector<unsigned char> wire,bool valid,const char* label,size_t expectedMax=262144) {
    Memory m; m.in=std::move(wire); auto z=init(m,true);
    int type=zmodem_recv_init(&z);
    std::string name=root+"/unit-"+std::to_string(units)+".bin";
    FILE* f=fopen(name.c_str(),"w+b"); require(f!=nullptr,"unit output creation");
    unsigned errors=z.max_errors+1;
    if(type==ZFILE && !z.sfx_failure && !z.sfx_refusal) errors=zmodem_recv_file_data(&z,f,0);
    bool good=errors<=z.max_errors && z.sfx_zeof && !z.sfx_failure;
    require(good==valid,label);
    require(ftell(f)>=0 && static_cast<size_t>(ftell(f))<=expectedMax,"no cap overshoot on disk");
    if(valid) {
        require(zmodem_recv_init(&z)==ZFIN && !z.sfx_failure,"normal batch finish");
        rewind(f);
        for(unsigned i=0;i<static_cast<unsigned>(z.current_file_size);++i)
            require(fgetc(f)==static_cast<int>(i%256),"exact data round trip");
    }
    fclose(f); ++units;
}
static void disk_failure_case() {
    auto wire=stream(4);
    Memory m; m.in=std::move(wire); auto z=init(m,true);
    require(zmodem_recv_init(&z)==ZFILE,"disk-fault initial metadata");
    std::string name=root+"/readonly-unit.bin";
    FILE* f=fopen(name.c_str(),"wb"); require(f!=nullptr,"create disk-fault fixture"); fclose(f);
    f=fopen(name.c_str(),"rb"); require(f!=nullptr,"open read-only stream");
    auto errors=zmodem_recv_file_data(&z,f,0);
    require(z.sfx_failure==5 && !z.sfx_zeof && errors>z.max_errors,"persistent failed data write");
    fclose(f); ++units;
}
static std::string read(const std::string& file) {
    FILE* f=fopen(file.c_str(),"rb"); if(!f)return {};
    std::string out; char b[512]; size_t n;
    while((n=fread(b,1,sizeof(b),f))) out.append(b,n);
    fclose(f); return out;
}

// Actual external helper over inherited private loopback; no live SFX session.
// Explicit allowlist and kill-on-close Job keep test processes contained.
static bool old_empty_control=false;
static void process_case(const std::string& exe,const std::vector<unsigned char>& wire,
    bool success,const char* label,size_t cap=262144,bool preset=false,bool disconnect=false,
    bool kill=false, bool idle=false,size_t tail=0,bool handoff=true,
    const std::string* supplied=nullptr,size_t fragment=4096,unsigned tail_delay=0,unsigned preset_kind=0,unsigned refusal=0,size_t offer_end=0,bool await_zero_pos=false) {
    std::string dir=root+"/process-"+std::to_string(processes++);
    require(CreateDirectoryA(dir.c_str(),nullptr)!=0,"fresh process fixture");
    if(preset) {
        FILE* f=fopen((dir+"/payload.bin").c_str(),"wb"); require(f!=nullptr,"preset file");
        fputs("retained",f); fclose(f);
    }
    const auto preset_name=dir+(preset_kind==1?"/result.txt":preset_kind==2?"/dsz.log":"/payload.bin");
    if(preset_kind==1 || preset_kind==2){FILE* f=fopen(preset_name.c_str(),"wb");require(f!=nullptr,"preset result/log");fputs("retained",f);fclose(f);}
    if(preset_kind==3){FILE* f=fopen((dir+"/victim.bin").c_str(),"wb");require(f!=nullptr,"hardlink victim");fputs("retained",f);fclose(f);require(CreateHardLinkA(preset_name.c_str(),(dir+"/victim.bin").c_str(),nullptr)!=0,"hardlinked existing target");}
    if(preset_kind==4)require(CreateDirectoryA(preset_name.c_str(),nullptr)!=0,"existing target directory");
    SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    sockaddr_in addr{}; addr.sin_family=AF_INET; addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    require(bind(listener,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))==0,"bind loopback");
    require(listen(listener,1)==0,"listen loopback");
    int n=sizeof(addr); require(getsockname(listener,reinterpret_cast<sockaddr*>(&addr),&n)==0,"bound port");
    SOCKET caller=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
    if(connect(caller,reinterpret_cast<sockaddr*>(&addr),sizeof(addr))) {
        int error=WSAGetLastError(); closesocket(caller); closesocket(listener);
        throw std::runtime_error("connect relay WSA="+std::to_string(error)+" port="+std::to_string(ntohs(addr.sin_port)));
    }
    require(true,"connect relay");
    SOCKET child=accept(listener,nullptr,nullptr); closesocket(listener);
    require(child!=INVALID_SOCKET,"accept helper endpoint");
    require(SetHandleInformation(reinterpret_cast<HANDLE>(caller),HANDLE_FLAG_INHERIT,0)!=0,"caller not inherited");
    require(SetHandleInformation(reinterpret_cast<HANDLE>(child),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT)!=0,"child inheritable");
    SIZE_T size=0; InitializeProcThreadAttributeList(nullptr,1,0,&size);
    std::vector<unsigned char> attrs(size);
    STARTUPINFOEXA si{}; si.StartupInfo.cb=sizeof(si);
    si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attrs.data());
    require(InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&size)!=0,"handle list init");
    HANDLE allowed=reinterpret_cast<HANDLE>(child);
    require(UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&allowed,sizeof(allowed),nullptr,nullptr)!=0,"allowlisted endpoint");
    HANDLE job=CreateJobObjectA(nullptr,nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{}; limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    require(job && SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"test job");
    PROCESS_INFORMATION pi{};
    std::string command="\""+exe+"\" --sfx-message-v1 "+std::to_string(child);
    require(CreateProcessA(exe.c_str(),command.data(),nullptr,nullptr,TRUE,
        EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,dir.c_str(),&si.StartupInfo,&pi)!=0,"launch strict helper");
    if(!AssignProcessToJobObject(job,pi.hProcess)) {
        TerminateProcess(pi.hProcess,8); WaitForSingleObject(pi.hProcess,2000);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess); CloseHandle(job);
        throw std::runtime_error("job assignment refused before resume");
    }
    require(ResumeThread(pi.hThread)!=static_cast<DWORD>(-1),"resume after job assignment");
    CloseHandle(pi.hThread); DeleteProcThreadAttributeList(si.lpAttributeList);
    u_long nonblocking=1; ioctlsocket(caller,FIONBIO,&nonblocking);
    size_t sent=0; ULONGLONG start=GetTickCount64(),tail_start=0;
    Memory skip_encoding,finish_encoding,pos_encoding,peer_finish;
    auto skip_z=init(skip_encoding),finish_z=init(finish_encoding),pos_z=init(pos_encoding),peer_z=init(peer_finish);
    zmodem_send_pos_header(&skip_z,ZSKIP,0,TRUE);zmodem_send_zfin(&finish_z);zmodem_send_pos_header(&pos_z,ZRPOS,0,TRUE);
    frame(peer_z,ZFIN);
    const std::string skip_header(skip_encoding.out.begin(),skip_encoding.out.end());
    const std::string finish_header(finish_encoding.out.begin(),finish_encoding.out.end());
    const std::string pos_header(pos_encoding.out.begin(),pos_encoding.out.end());
    std::string output;
    completion_review::Ledger ledger;
    while(WaitForSingleObject(pi.hProcess,0)==WAIT_TIMEOUT && GetTickCount64()-start<(idle?70000:15000)) {
        if(sent<wire.size()) {
            const bool at_tail=tail && sent>=wire.size()-tail;
            if(at_tail && !tail_start)tail_start=GetTickCount64();
            if(!at_tail || GetTickCount64()-tail_start>=tail_delay){
                auto send_limit=wire.size();
                if(await_zero_pos && output.find(pos_header)==std::string::npos)send_limit=offer_end;
                else if(refusal && refusal!=3 && output.find(skip_header)==std::string::npos)send_limit=offer_end?offer_end:wire.size()-tail-2-peer_finish.out.size();
                else if(refusal && output.find(finish_header)==std::string::npos)send_limit=wire.size()-tail-2;
                // Fragment the offer, then send ordinary payload blocks after
                // the receiver has explicitly requested fresh offset zero.
                const auto send_fragment=await_zero_pos && output.find(pos_header)!=std::string::npos?size_t(4096):fragment;
                auto offered=sent<send_limit?std::min<size_t>(send_fragment,send_limit-sent):0;
                if(tail_delay && !at_tail)offered=std::min<size_t>(offered,wire.size()-tail-sent);
                int wrote=offered?send(caller,reinterpret_cast<const char*>(wire.data()+sent),static_cast<int>(offered),0):0;
                if(wrote>0){require(ledger.append({reinterpret_cast<const char*>(wire.data()+sent),static_cast<size_t>(wrote)}),"bounded forwarding counter");sent+=wrote;}
            }
        } else if(disconnect) shutdown(caller,SD_SEND);
        char observed[4096];int received=recv(caller,observed,sizeof(observed),0);if(received>0)output.append(observed,received);
        if(kill && GetTickCount64()-start>=100) {
            require(TerminateJobObject(job,7)!=0,"explicit job cancellation");
            require(WaitForSingleObject(pi.hProcess,2000)==WAIT_OBJECT_0,"cancelled helper reaped within two seconds");
            break;
        }
        Sleep(1);
    }
    bool ended=WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0;
    if(!ended) { TerminateJobObject(job,8); WaitForSingleObject(pi.hProcess,2000); }
    DWORD code=8; GetExitCodeProcess(pi.hProcess,&code);
    shutdown(child,SD_SEND);
    for(;;){char observed[4096];const int received=recv(caller,observed,sizeof(observed),0);if(received<=0)break;output.append(observed,received);}
    closesocket(child);
    CloseHandle(pi.hProcess); CloseHandle(job); closesocket(caller);
    if(!ended){
        FILE* trace=fopen((dir+"/observed-output.bin").c_str(),"wb");
        if(trace){fwrite(output.data(),1,output.size(),trace);fclose(trace);}
        std::cerr<<"Process timeout: "<<label<<" sent="<<sent<<" wire="<<wire.size()<<" offer_end="<<offer_end<<" output="<<output.size()<<"\n";
    }
    require(ended,"bounded child process exit");
    require((code==0)==success,label);
    if(kill) { require(code==7,"no success after forced cancellation"); return; }
    if(preset_kind){
        if(preset_kind==4)require((GetFileAttributesA(preset_name.c_str())&FILE_ATTRIBUTE_DIRECTORY)!=0,"existing directory never replaced");
        else require(read(preset_name)=="retained","preexisting result/log/hardlink bytes never overwritten");
        if(preset_kind==3)require(read(dir+"/victim.bin")=="retained","hardlink victim unchanged");
        return;
    }
    if(idle) require(GetTickCount64()-start>=60000 && GetTickCount64()-start<65000,"real 60s receive inactivity bound");
    auto result=read(dir+"/result.txt");
    require(result.size()<256 && result.find(old_empty_control?"SFX-MESSAGE-RECEIVE 3\n":"SFX-MESSAGE-RECEIVE 4\n")==0,"bounded exact-profile private result");
    require((result.find("failure 0\n")!=std::string::npos)==success,"structured failure agrees");
    auto body=read(dir+"/payload.bin"); require(body.size()<=cap,"external helper cannot overshoot");
    if(preset) require(body=="retained","no overwrite");
    if(success) {
        if(await_zero_pos)require(output.find(pos_header)!=std::string::npos,"resume-capable fresh transfer explicitly requests byte zero before any data");
        if(refusal){
            require(GetFileAttributesA((dir+"/payload.bin").c_str())==INVALID_FILE_ATTRIBUTES && GetLastError()==ERROR_FILE_NOT_FOUND,"refused offer creates no payload");
            require(result.find("completed 0\n")!=std::string::npos && result.find("bytes 0\n")!=std::string::npos && result.find("zeof 0\n")!=std::string::npos,"no file received or ZEOF fabricated on refusal");
            if(refusal==3){
                require(result.find("attempted 0\n")!=std::string::npos,"empty cancellation cannot fabricate offered file");
                require(output.find(skip_header)==std::string::npos && output.find(pos_header)==std::string::npos,"empty cancellation sends neither ZSKIP nor ZRPOS");
            } else require(output.find(skip_header)!=std::string::npos && output.find(pos_header)==std::string::npos,"policy refusal sends ZSKIP and never requests file data");
            require(output.size()>=finish_header.size() && output.substr(output.size()-finish_header.size())==finish_header,"policy completion final ZFIN fully observed");
        }else require(result.find("completed 1\n")!=std::string::npos && result.find("zeof 1\n")!=std::string::npos
            && result.find("batch 1\n")!=std::string::npos,"completed file AND batch");
        if(supplied)require(body==*supplied,"exact supplied inert bytes round trip");
        else for(size_t i=0;i<body.size();++i) require(static_cast<unsigned char>(body[i])==i%256,"process byte round trip");
        const auto consumed=completion_review::receipt_input(result,body.size(),refusal);
        require(consumed && *consumed==wire.size()-tail,"helper counts exact raw input through OO only");
        const std::string_view pending(reinterpret_cast<const char*>(wire.data()+sent),wire.size()-sent);
        require(completion_review::accept(code==0,true,read(dir+"/dsz.log")==(refusal?"S ":"Z ")+std::to_string(body.size())+" 0 0 0 0 0 payload.bin\n",true,body.size(),result,ledger,pending,refusal)==handoff,"bounded host handoff predicate, not exit/reset alone");
        std::cout<<"ACCOUNT process="<<processes.load()<<" consumed="<<*consumed<<" forwarded="<<sent<<" tail="<<tail<<" accept="<<handoff<<"\n";
    }else{
        require(!completion_review::receipt_input(result,body.size()),"failed helper receipt never passes host completion parser");
        if(old_empty_control){
            Memory abort_encoding;auto abort_z=init(abort_encoding);zmodem_send_pos_header(&abort_z,ZABORT,0,TRUE);
            const std::string abort_header(abort_encoding.out.begin(),abort_encoding.out.end());
            require(output.find(finish_header)!=std::string::npos && output.find(abort_header)!=std::string::npos,"old helper acknowledges empty end then aborts");
            require(result.find("attempted 0\n")!=std::string::npos && body.empty(),"old control offered no file and produced no payload");
        }
    }
}

static std::string complete_receipt(uint64_t count){return "SFX-MESSAGE-RECEIVE 4\nattempted 1\ncompleted 1\nbytes 4\nzeof 1\nbatch 1\nfailure 0\nrefusal 0\ninput "+std::to_string(count)+"\n";}
static void handoff_cases(){
    for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("\n\n"),std::string("\r\n\r"),std::string("Q"),std::string(" "),std::string("\x18"),std::string("\0",1),std::string("\x1b[A")}){
        const bool expected=tail.empty() || tail=="\r" || tail=="\n" || tail=="\r\n";
        for(size_t split=0;split<=tail.size();++split){
            completion_review::Ledger ledger;require(ledger.append("protocol-OO"),"ledger prefix");require(ledger.append(std::string_view(tail).substr(0,split)),"ledger tail");
            require(completion_review::accept(true,true,true,true,4,complete_receipt(11),ledger,std::string_view(tail).substr(split))==expected,"every forwarded/pending split accepts only one logical Enter");
        }
    }
    completion_review::Ledger ledger;require(ledger.append("protocol-OO"),"known protocol ledger");
    for(uint64_t count:{0ULL,1ULL,8ULL,12ULL,UINT64_MAX})require(!completion_review::accept(true,true,true,true,4,complete_receipt(count),ledger,{}),"missing/forged/unconsumed/overflow count rejected");
    auto good=complete_receipt(11);
    for(size_t i=0;i<good.size();++i){auto bad=good;bad[i]='!';require(!completion_review::accept(true,true,true,true,4,bad,ledger,{}),"every receipt-field corruption rejected");}
    for(auto suffix:{"0\n","011\n","-1\n","18446744073709551616\n","11\nextra\n","11 "}){
        auto bad=good.substr(0,good.find("input ")+6)+suffix;require(!completion_review::accept(true,true,true,true,4,bad,ledger,{}),"noncanonical and overflow/suffix receipt rejected");
    }
    for(unsigned denied=0;denied<4;++denied)require(!completion_review::accept(denied!=0,denied!=1,denied!=2,denied!=3,4,good,ledger,{}),"exit/files/DSZ/current authority independently mandatory");
    auto wrapped=ledger;wrapped.forwarded=UINT64_MAX;require(!wrapped.append("X"),"forwarding count cannot wrap");
    require(!completion_review::accept(true,true,true,true,0,good,ledger,{}) && !completion_review::accept(true,true,true,true,262145,good,ledger,{}),"payload cap remains mandatory");
}
static std::vector<unsigned char> supplied_stream(const std::string& body,bool crc32){
    Memory m;auto z=init(m);z.can_fcs_32=crc32;
    frame(z,ZFILE);packet(z,metadata("owned.ans",std::to_string(body.size())),ZCRCW);frame(z,ZDATA);
    for(size_t pos=0;pos<body.size();pos+=1024){const auto n=std::min<size_t>(1024,body.size()-pos);packet(z,{body.begin()+pos,body.begin()+pos+n},pos+n<body.size()?ZCRCG:ZCRCE);}
    frame(z,ZEOF,static_cast<unsigned>(body.size()));frame(z,ZFIN);m.out.push_back('O');m.out.push_back('O');return m.out;
}

static std::vector<unsigned char> offered(const std::string& name,const std::string& fields,bool crc32=false,unsigned file_flags=0){
    Memory m;auto z=init(m);z.can_fcs_32=crc32;
    frame(z,ZFILE,file_flags);packet(z,metadata(name,fields),ZCRCW);return m.out;
}
static std::vector<unsigned char> policy_wire(const std::string& name,const std::string& fields,bool crc32=false,unsigned file_flags=0){
    auto wire=offered(name,fields,crc32,file_flags);Memory ending;auto z=init(ending);
    frame(z,ZFIN);ending.out.push_back('O');ending.out.push_back('O');
    wire.insert(wire.end(),ending.out.begin(),ending.out.end());return wire;
}
static void policy_cases(const std::string& exe){
    for(const auto* ext:{"ans","ANS","AnS","asc","ASC","aSc","txt","TXT","TxT"}){
        const auto name=std::string("file with spaces.")+ext;
        Memory m;auto z=init(m,true);auto meta=metadata(name,"262144");
        memcpy(z.rx_data_subpacket,meta.data(),meta.size());z.rx_data_subpacket_len=static_cast<unsigned>(meta.size());zmodem_parse_zfile_subpacket(&z);
        require(!z.sfx_failure && !z.sfx_refusal && z.current_file_size==262144,"allowed final extension case variants at exact limit");++units;
        process_case(exe,stream(4,name),true,"allowed extension real process");
    }
    for(const auto* name:{"file.exe","file.zip","file","file.ans.exe","file.txt.bak","file.tXtX","file.ansi"}){
        for(bool crc32:{false,true}){
            auto wire=policy_wire(name,"55402",crc32);
            process_case(exe,wire,true,"disallowed extension skipped before payload",0,false,false,false,false,0,true,nullptr,7,0,0,SFX_REFUSE_EXTENSION);
        }
    }
    for(const auto* size:{"262145","1099511627776","18446744073709551615"}){
        for(bool crc32:{false,true}){
            auto wire=policy_wire("large.txt",size,crc32);
            process_case(exe,wire,true,"oversized announcement skipped without payload",0,false,false,false,false,0,true,nullptr,1,0,0,SFX_REFUSE_SIZE);
        }
    }
    process_case(exe,policy_wire("large.EXE","262145"),true,"oversize reason takes precedence",0,false,false,false,false,0,true,nullptr,4096,0,0,SFX_REFUSE_SIZE);
    for(unsigned reason:{SFX_REFUSE_EXTENSION,SFX_REFUSE_SIZE})for(const auto& tail:{std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("Q"),std::string("\0",1),std::string("\x18"),std::string("\x1b[A")}){
        auto wire=policy_wire(reason==SFX_REFUSE_SIZE?"large.txt":"bad.exe",reason==SFX_REFUSE_SIZE?"262145":"55402");wire.insert(wire.end(),tail.begin(),tail.end());
        process_case(exe,wire,true,"refusal completion tail never silently drained",0,false,false,false,false,tail.size(),tail=="\r" || tail=="\n" || tail=="\r\n",nullptr,4096,0,0,reason);
    }
    auto missing=policy_wire("bad.exe","55402");missing.resize(missing.size()-2);
    process_case(exe,missing,false,"skip without OO cannot restore editor",0,false,true);
    auto partial=policy_wire("bad.exe","55402");partial.pop_back();
    process_case(exe,partial,false,"skip with partial OO cannot restore editor",0,false,true);
    process_case(exe,offered("bad.exe","55402"),false,"skip without ZFIN cannot restore editor",0,false,true);
    process_case(exe,stream(4,"bad.exe"),false,"peer sends unrequested data after skip",0);
    auto extra=offered("bad.exe","55402");const auto next=policy_wire("another.txt","4");extra.insert(extra.end(),next.begin(),next.end());
    process_case(exe,extra,false,"second offered file after skip remains fatal",0);
    for(const auto* fields:{"262145 0 0 0 2 524290","262145 0 0 0 1 1","18446744073709551616","262145 8"})
        process_case(exe,policy_wire("bad.exe",fields),false,"malformed or multi-file oversized offer not downgraded to soft refusal",0);
    auto wrongpos=offered("bad.exe","4");Memory ending;auto z=init(ending);frame(z,ZFIN,1);ending.out.push_back('O');ending.out.push_back('O');wrongpos.insert(wrongpos.end(),ending.out.begin(),ending.out.end());
    process_case(exe,wrongpos,false,"nonzero ZFIN position rejected on skip",0);
    for(unsigned reason:{SFX_REFUSE_EXTENSION,SFX_REFUSE_SIZE}){
        const auto receipt=std::string("SFX-MESSAGE-RECEIVE 4\nattempted 1\ncompleted 0\nbytes 0\nzeof 0\nbatch 1\nfailure 0\nrefusal ")+std::to_string(reason)+"\ninput 11\n";
        completion_review::Ledger ledger;ledger.append("protocol-OO");
        require(completion_review::accept(true,true,true,true,0,receipt,ledger,{},reason),"exact refusal receipt accepts only as policy refusal");
        require(!completion_review::receipt_input(receipt,4) && !completion_review::receipt_input(receipt,0,3),"refusal cannot become a message or unknown reason");
        for(size_t i=0;i<receipt.size();++i){auto bad=receipt;bad[i]='!';require(!completion_review::receipt_input(bad,0,reason),"every refusal receipt field is mandatory");}
        for(unsigned denied=0;denied<4;++denied)require(!completion_review::accept(denied!=0,denied!=1,denied!=2,denied!=3,0,receipt,ledger,{},reason),"refusal still requires normal exit owned files DSZ fresh authority");
    }
}
static void strict_policy_cases(const std::string& exe){
    for(unsigned reason:{SFX_REFUSE_EXTENSION,SFX_REFUSE_SIZE}){
        const auto offer=offered(reason==SFX_REFUSE_SIZE?"large.txt":"bad.exe",reason==SFX_REFUSE_SIZE?"262145":"4");
        const auto run=[&](const std::vector<unsigned char>& ending,bool ok,size_t fragment,const char* label){
            auto wire=offer;wire.insert(wire.end(),ending.begin(),ending.end());wire.push_back('O');wire.push_back('O');
            process_case(exe,wire,ok,label,0,false,false,false,false,0,ok,nullptr,fragment,0,0,reason,offer.size());
        };
        for(int style:{ZBIN,ZBIN32,ZHEX}){
            Memory m;auto z=init(m);z.can_fcs_32=style==ZBIN32;
            require(zmodem_send_pos_header(&z,ZFIN,0,style==ZHEX)==0,"encode strict completion style");
            for(size_t fragment:{1U,7U,4096U})run(m.out,true,fragment,"strict cooperative refusal header fragmentation");
            for(const auto& prefix:{std::string("Q"),std::string("raw payload"),std::string("*Q"),std::string("**\x18Z"),std::string("***"),std::string("\0",1),std::string("\x18\x18\x18\x18\x18"),std::string("\r\n")}){
                auto bad=std::vector<unsigned char>(prefix.begin(),prefix.end());bad.insert(bad.end(),m.out.begin(),m.out.end());
                run(bad,false,1,"DR-POLICY-001 prefixes cannot resynchronize to later valid ZFIN");
            }
            auto corrupt=m.out;corrupt[4]^=1;run(corrupt,false,7,"bad completion CRC or type is sticky failure");
            auto unknown=m.out;unknown[3]='Z';run(unknown,false,4096,"unknown style cannot be skipped");
            auto flow=m.out;flow.insert(flow.begin(),XON);flow.insert(flow.begin()+3,XOFF|0x80);flow.push_back(XON);
            // A post-header XON is NOT allowed: literal OO immediately follows.
            run(flow,false,1,"flow after ZFIN cannot be silently discarded before OO");
            flow.pop_back();run(flow,true,1,"bounded legal XON and parity XOFF outside escape pair");
            auto excessive=m.out;excessive.insert(excessive.begin(),4,XON);
            run(excessive,false,7,"metadata XON plus four extra exceeds fixed flow budget");
            auto single=m.out;single.erase(single.begin());run(single,true,1,"one ZPAD is valid framing");
            if(style==ZHEX){
                auto parity=m.out;parity[parity.size()-2]|=0x80;parity.back()|=0x80;run(parity,true,1,"hex parity CR LF framing");
                auto lf=m.out;lf.erase(lf.end()-2);run(lf,true,7,"hex LF-only framing");
                auto bad=m.out;bad.back()='Q';run(bad,false,1,"hex requires exact LF terminator");
            }
        }
    }
}
static void client_compat_cases(const std::string& exe){
    // Captured NetRunner ZFILE: wire-order flags 00 00 05 03, CRC32;
    // metadata netrunner.txt NUL 8280 14005557642 NUL. IcyTerm's published
    // sender uses the same flags, but that is not actual-client qualification.
    constexpr unsigned resume=0x03050000U;
    for(bool crc32:{false,true}){
        for(unsigned flags:{0U,0x01050000U,resume}){
            const auto offer=offered("netrunner.txt","8280 14005557642",crc32,flags);
            auto wire=stream(8280,"netrunner.txt","8280 14005557642",true,true,false,crc32,-1,true,flags);
            data_case(wire,true,"fresh-file conversion capability preserves exact bytes");
            process_case(exe,wire,true,"client capability receives fresh file from byte zero",262144,false,false,false,false,0,true,nullptr,7,0,0,0,offer.size(),true);
        }
        for(const auto* name:{"small.ANS","small.aSc","small.TxT"})
            process_case(exe,stream(4,name,"",true,true,false,crc32,-1,false,resume),true,"resume capability keeps allowed extension variants");
        data_case(stream(262144,"art.ans","",true,true,false,crc32,-1,true,resume),true,"resume capability exact cap");
        process_case(exe,stream(262144,"art.ans","",true,true,false,crc32,-1,true,resume),true,"resume-capable actual cap");
        process_case(exe,policy_wire("bad.exe","55402",crc32,resume),true,"resume capability cannot bypass extension policy",0,false,false,false,false,0,true,nullptr,7,0,0,SFX_REFUSE_EXTENSION);
        process_case(exe,policy_wire("huge.txt","1099511627776",crc32,resume),true,"resume capability cannot bypass size policy",0,false,false,false,false,0,true,nullptr,1,0,0,SFX_REFUSE_SIZE);
        for(unsigned flags:{0x02050000U,0x04050000U,0xff050000U,0x03050100U,0x03050001U}){
            auto wire=stream(4,"art.ans","",true,true,false,crc32,-1,false,flags);
            data_case(wire,false,"conversion transform or transport/reserved bits remain rejected");
            process_case(exe,wire,false,"unsupported flag cannot become a successful receive");
        }
        process_case(exe,stream(4,"art.ans","",true,true,false,crc32,-1,false,resume,2),false,"nonzero initial sender offset is not resumed");
        process_case(exe,stream(4,"art.ans","",true,true,false,crc32,-1,false,resume),false,"resume offer cannot overwrite existing payload",262144,true);
        process_case(exe,stream(4,"art.ans","",true,true,true,crc32,-1,false,resume),false,"resume offer cannot admit a second file");
        auto missing=stream(4,"art.ans","",true,true,false,crc32,-1,false,resume);missing.resize(missing.size()-2);
        process_case(exe,missing,false,"resume offer still requires literal OO completion",262144,false,true);
        auto wrong=stream(4,"art.ans","",true,true,false,crc32,3,false,resume);
        process_case(exe,wrong,false,"resume offer still requires exact ZEOF");
    }
}

static std::vector<unsigned char> empty_wire(int style,unsigned queries,const std::string& startup=""){
    Memory m;auto z=init(m);z.can_fcs_32=style==ZBIN32;
    m.out.insert(m.out.end(),startup.begin(),startup.end());
    for(unsigned n=0;n<queries;++n)require(zmodem_send_pos_header(&z,ZRQINIT,0,style==ZHEX)==0,"encode empty query");
    require(zmodem_send_pos_header(&z,ZFIN,0,style==ZHEX)==0,"encode empty finish");
    m.out.push_back('O');m.out.push_back('O');return m.out;
}
static void empty_prefix_case(const std::vector<unsigned char>& prefix,bool ok){
    Memory m;auto z=init(m,true);sfx_channel ch{};
    ch.initial_size=static_cast<unsigned>(std::min<size_t>(prefix.size(),sizeof(ch.initial)));
    ch.initial_overflow=prefix.size()>sizeof(ch.initial);
    std::copy_n(prefix.begin(),ch.initial_size,ch.initial);
    require(bool(sfx_empty_prefix(&z,&ch))==ok,"strict initial bytes reparsed without scan or I/O");
    require((z.sfx_failure==0)==ok,"empty prefix error cannot be hidden");++units;
}
static void empty_cases(const std::string& exe){
    for(int style:{ZBIN,ZBIN32,ZHEX})for(unsigned queries:{0U,1U,8U})for(const auto& startup:{std::string{},std::string("rz\r"),std::string("rz\r\n")}){
        auto wire=empty_wire(style,queries,startup);auto prefix=wire;prefix.resize(prefix.size()-2);empty_prefix_case(prefix,true);
        for(size_t fragment:{size_t(1),size_t(7),size_t(4096)})
            process_case(exe,wire,true,"verified empty cancellation without payload",0,false,false,false,false,0,true,nullptr,fragment,0,0,3);
    }
    // Exact NetRunner captured startup and parity CR/LF hex framing.
    auto captured=empty_wire(ZHEX,1,"rz\r");
    for(size_t i=0;i<captured.size();++i)if(captured[i]=='\n')captured[i]|=0x80;
    process_case(exe,captured,true,"NetRunner empty trace completes only through OO",0,false,false,false,false,0,true,nullptr,1,0,0,3);
    for(const auto& tail:{std::string{},std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("Q"),std::string("\0",1),std::string("\x1b[A")}){
        auto wire=captured;wire.insert(wire.end(),tail.begin(),tail.end());
        const bool safe=tail.empty() || tail=="\r" || tail=="\n" || tail=="\r\n";
        process_case(exe,wire,true,"empty completion never drains arbitrary tail",0,false,false,false,false,tail.size(),safe,nullptr,4096,0,0,3);
    }
    auto prefix=captured;prefix.resize(prefix.size()-2);
    for(size_t i=0;i<prefix.size();++i){auto bad=prefix;bad[i]='!';empty_prefix_case(bad,false);}
    empty_prefix_case(std::vector<unsigned char>(257,'Q'),false);
    for(const auto& junk:{std::string("Q"),std::string("\r"),std::string(" "),std::string("\0",1),std::string("rzX"),std::string("rz\rX"),std::string("\x1b[A")}){
        auto bad=captured;bad.insert(bad.begin(),junk.begin(),junk.end());
        process_case(exe,bad,false,"empty end cannot resynchronize past junk",0,false,true);
        bad=captured;bad.insert(bad.begin()+3,junk.begin(),junk.end());
        process_case(exe,bad,false,"junk after exact startup cannot disappear",0,false,true);
    }
    for(int type:{ZACK,ZNAK,ZEOF,ZSINIT,ZCOMMAND}){
        Memory m;auto z=init(m);frame(z,type);auto end=empty_wire(ZBIN,0);m.out.insert(m.out.end(),end.begin(),end.end());
        process_case(exe,m.out,false,"non-query frame cannot be empty cancellation",0,false,true);
    }
    auto missing=captured;missing.resize(missing.size()-2);process_case(exe,missing,false,"empty ZFIN without OO fails",0,false,true);
    auto partial=captured;partial.pop_back();process_case(exe,partial,false,"empty partial OO fails",0,false,true);
    auto wrong=captured;wrong[wrong.size()-2]='Q';process_case(exe,wrong,false,"empty nonliteral OO fails",0,false,true);
    process_case(exe,empty_wire(ZHEX,9),false,"bounded query count cannot be extended",0,false,true);
    {Memory m;auto z=init(m);frame(z,ZFIN,1);m.out.push_back('O');m.out.push_back('O');process_case(exe,m.out,false,"empty end nonzero fields forbidden",0,false,true);}
    {auto offered_file=offered("art.ans","4");auto end=empty_wire(ZHEX,0);offered_file.insert(offered_file.end(),end.begin(),end.end());process_case(exe,offered_file,false,"file attempt cannot downgrade to empty cancellation",0,false,true);}
    const auto receipt=std::string("SFX-MESSAGE-RECEIVE 4\nattempted 0\ncompleted 0\nbytes 0\nzeof 0\nbatch 1\nfailure 0\nrefusal 3\ninput 11\n");
    completion_review::Ledger ledger;ledger.append("protocol-OO");
    require(completion_review::accept(true,true,true,true,0,receipt,ledger,{},3),"empty receipt safe only as no-file completion");
    for(unsigned reason:{0U,1U,2U,4U})require(!completion_review::receipt_input(receipt,0,reason),"empty receipt cannot masquerade as file/policy result");
    for(size_t i=0;i<receipt.size();++i){auto bad=receipt;bad[i]='!';require(!completion_review::receipt_input(bad,0,3),"every empty completion receipt field mandatory");}
    for(unsigned denied=0;denied<4;++denied)require(!completion_review::accept(denied!=0,denied!=1,denied!=2,denied!=3,0,receipt,ledger,{},3),"empty still requires normal exit exact files DSZ fresh authority");
    auto old=receipt;old[20]='3';require(!completion_review::receipt_input(old,0,3),"profile3 cannot authorize profile4 empty result");
}

int main(int argc,char** argv) try {
    if(argc!=4 && argc!=5) return 2;
    root=argv[2];
    WSADATA w{}; require(WSAStartup(MAKEWORD(2,2),&w)==0,"test Winsock");
    if(argc==5 && std::string(argv[4])=="--old-empty-control"){
        old_empty_control=true;
        for(int style:{ZBIN,ZBIN32,ZHEX})
            process_case(argv[1],empty_wire(style,1,"rz\r"),false,"old helper must fail completed empty batch",0,false,false,false,false,0,false,nullptr,7,0,0,3);
        std::cout<<"PASS old exact helper empty-batch negative control: "<<processes<<" external helper processes; "<<checks<<" assertions\n";return 0;
    }
    if(argc==5 && std::string(argv[4])=="--empty-batch"){
        empty_cases(argv[1]);
        std::cout<<"PASS "<<units<<" empty-prefix cases and "<<processes<<" external helper processes; "<<checks<<" assertions\n";return 0;
    }
    if(argc==5 && std::string(argv[4])=="--client-resume-process"){
        const auto offer=offered("netrunner.txt","8280 14005557642",true,0x03050000U);
        process_case(argv[1],stream(8280,"netrunner.txt","8280 14005557642",true,true,false,true,-1,true,0x03050000U),
            true,"external fresh resume-capable process control",262144,false,false,false,false,0,true,nullptr,7,0,0,0,offer.size(),true);
        std::cout<<"PASS resume-capable external helper process control\n";return 0;
    }
    if(argc==5 && std::string(argv[4])=="--client-compat"){
        client_compat_cases(argv[1]);
        std::cout<<"PASS "<<units<<" compatibility parser/data cases and "<<processes<<" external helper processes; "<<checks<<" assertions\n";return 0;
    }
    if(argc==5){
        const bool size=std::string(argv[4])=="--dr-size";
        require(size || std::string(argv[4])=="--dr-extension","known negative-control mode");
        auto wire=offered(size?"large.txt":"bad.exe",size?"262145":"4");const auto boundary=wire.size();
        wire.push_back('Q');Memory ending;auto z=init(ending);frame(z,ZFIN);ending.out.push_back('O');ending.out.push_back('O');wire.insert(wire.end(),ending.out.begin(),ending.out.end());
        process_case(argv[1],wire,false,"DR-POLICY-001: junk before ZFIN MUST fail",0,false,false,false,false,0,false,nullptr,1,0,0,size?SFX_REFUSE_SIZE:SFX_REFUSE_EXTENSION,boundary);
        std::cout<<"PASS DR-POLICY-001 rejection\n";return 0;
    }
    sfx_channel clock{}; clock.start=clock.inbound=1000;
    simulated_tick=60999; require(!sfx_cancelled(&clock),"inactivity not early");
    simulated_tick=61000; require(sfx_cancelled(&clock),"60s exact inactivity deadline");
    clock.cancelled=FALSE; clock.start=1000; clock.inbound=300999;
    simulated_tick=300999; require(!sfx_cancelled(&clock),"hard ceiling not early");
    simulated_tick=301000; require(sfx_cancelled(&clock),"five-minute exact ceiling despite inbound activity");
    simulated_tick=1001; require(sfx_cancelled(&clock),"deadline cancellation is sticky");
    meta_case(metadata("art.ans","1"),true,"minimal metadata");
    meta_case(metadata("file with spaces.ans","262144 0 0 0 1 262144 0"),true,"bounded complete metadata");
    for(auto name:{"../evil.ans","..\\evil.ans","/absolute","C:evil","C:\\evil","CON","con.txt",
        "NUL.ans","LPT1.txt","COM0","CON .txt","CONIN$",".hidden","dot.","end ","bad|name","bad?name","a\x1b.txt"})
        meta_case(metadata(name,"1"),false,"invalid name rejected");
    meta_case(metadata(std::string(128,'x'),"1"),true,"128-byte name");
    meta_case(metadata(std::string(129,'x'),"1"),false,"129-byte name rejected");
    meta_case(metadata("","1"),false,"empty filename");
    for(auto fields:{"0","-1","1foo","1 8","1 0 8","1 0 0 0 2 2","1 0 0 0 1 2",
        "1 0 0 0 1 1 0 0","18446744073709551616","1 177777777777 0 0 1 1"})
        meta_case(metadata("art.ans",fields),false,"invalid numeric/count metadata");
    auto missing=metadata("a","1"); missing.pop_back(); meta_case(missing,false,"missing metadata NUL");
    meta_case(std::vector<unsigned char>(8192,'x'),false,"unterminated maximum subpacket");
    auto padded=metadata("a","1"); padded.resize(128); meta_case(padded,true,"zero padding");
    padded.back()='X'; meta_case(padded,false,"nonzero suffix rejected");
    data_case(stream(1),true,"one byte");
    data_case(stream(4097,"art.ans","",true,true,false,true),true,"CRC32 multi-frame");
    data_case(stream(262144,"art.ans","",true,true,false,false,-1,true),true,"exact cap streaming CRC16");
    data_case(stream(262144,"art.ans","",true,true,false,true,-1,true),true,"exact cap streaming CRC32");
    data_case(stream(262145,"art.ans","1",true,true,false,false,-1,true),false,"streaming cap plus one");
    data_case(stream(8192,"art.ans","1"),false,"underdeclared size refuses final success");
    data_case(stream(10,"art.ans","",false,true),false,"ZFIN cannot replace ZEOF");
    data_case(stream(10,"art.ans","",true,true,false,false,9),false,"wrong EOF position");
    data_case(stream(1,"../escape.ans"),false,"wire unsafe original name");
    disk_failure_case();
    std::string exe=argv[1];
    policy_cases(exe);
    strict_policy_cases(exe);
    client_compat_cases(exe);
    empty_cases(exe);
    process_case(exe,stream(1),true,"external one-byte success");
    process_case(exe,stream(262144,"art.ans","",true,true,false,true,-1,true),true,"external cap success");
    process_case(exe,stream(262145,"art.ans","1",true,true,false,true,-1,true),false,"external cap rejection");
    process_case(exe,stream(4,"../escape.ans"),false,"external traversal rejection",0);
    process_case(exe,stream(4,"CON.ans"),false,"external device rejection",0);
    process_case(exe,stream(4,"art.ans","4 0 0 0 1 4 garbage"),false,"external malformed metadata",0);
    process_case(exe,stream(4,"art.ans","4 0 0 0 2 8"),false,"external advertised extra",0);
    process_case(exe,stream(4,"art.ans","",true,true,true),false,"extra after completed first file");
    process_case(exe,stream(4,"art.ans","",false,true),false,"premature batch finish");
    process_case(exe,stream(4,"art.ans","",true,true,false,false,3),false,"external wrong EOF position");
    process_case(exe,stream(4,"art.ans","1"),false,"external wrong declared size");
    process_case(exe,stream(4),false,"exclusive output creation",262144,true);
    auto noOO=stream(4); noOO.resize(noOO.size()-2);
    process_case(exe,noOO,false,"missing OO rejects completion",262144,false,true);
    auto disconnect=stream(4,"art.ans","",false,false);
    process_case(exe,disconnect,false,"disconnect before EOF",262144,false,true);
    auto cancelWire=stream(4,"art.ans","",false,false);
    cancelWire.insert(cancelWire.end(),5,0x18);
    process_case(exe,cancelWire,false,"protocol cancellation stays failed");
    process_case(exe,{},false,"process-tree cancel",0,false,false,true);
    process_case(exe,{},false,"actual helper inactivity timeout",0,false,false,false,true);
    for(unsigned existing=1;existing<=4;++existing)process_case(exe,stream(4),false,"preexisting result/log/hardlink/directory rejected without overwrite",262144,false,false,false,false,0,true,nullptr,4096,0,existing);
    handoff_cases();
    for(const auto& tail:{std::string("\r"),std::string("\n"),std::string("\r\n"),std::string("\r\r"),std::string("Q"),std::string("\0",1),std::string("\x18"),std::string("\x1b[A")}){
        auto wire=stream(4);wire.insert(wire.end(),tail.begin(),tail.end());
        process_case(exe,wire,true,"strict helper completion with tail accounted, never purged",262144,false,false,false,false,tail.size(),tail=="\r" || tail=="\n" || tail=="\r\n");
    }
    {auto wire=stream(4);Memory extra;auto z=init(extra);frame(z,ZFILE);packet(z,metadata("second.ans","1"),ZCRCW);const auto tail=extra.out.size();wire.insert(wire.end(),extra.out.begin(),extra.out.end());
        process_case(exe,wire,true,"post-OO extra frame is rejected by host accounting",262144,false,false,false,false,tail,false);}
    for(const auto* file:{"TEST60K.TXT","reglotf.exe"}){
        const auto body=read(std::string(argv[3])+"/"+file);require(body.size()==(std::string(file)=="TEST60K.TXT"?61440:55402),"exact owner-input length; read-only inert data");
        for(bool crc32:{false,true})for(unsigned delay:{0U,1U,2U,5U,10U}){
            auto wire=supplied_stream(body,crc32);wire.push_back('\r');
            process_case(exe,wire,true,"owner input exact transfer/early Enter accounting",262144,false,false,false,false,1,true,&body,4096,delay);
        }
    }
    auto left=std::async(std::launch::async,[&]{process_case(exe,stream(4097,"left.ans","",true,true,false,true),true,"concurrent owned helper left");});
    auto right=std::async(std::launch::async,[&]{auto wire=stream(2049);wire.push_back('\r');process_case(exe,wire,true,"concurrent owned helper right",262144,false,false,false,false,1,true);});left.get();right.get();
    std::cout<<"PASS "<<units<<" parser/data cases and "<<processes<<" external helper processes; "<<checks<<" assertions\n";
    return 0;
} catch(const std::exception& e) { std::cerr<<"FAIL after "<<checks<<" assertions: "<<e.what()<<"\n"; return 1; }
