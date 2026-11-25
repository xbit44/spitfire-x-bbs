// Separate GPL-2.0-or-later packet helper peer tests. No SFX product linkage.
// Actual child/socket/file transfers, not installed Telnet/SSH/NCM evidence.
#include <winsock2.h>
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>
#include <filesystem>
#include <map>
#include <sstream>
#include <array>
extern "C" {
#include "zmodem.h"
int zmodem_recv_header(zmodem_t*);
}
namespace {
unsigned checks{},cases{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
std::string read_file(const std::filesystem::path& path){FILE* f=std::fopen(path.string().c_str(),"rb");if(!f)return {};std::string result;char buffer[8192];std::size_t n;while((n=std::fread(buffer,1,sizeof(buffer),f)))result.append(buffer,n);const bool okay=!std::ferror(f)&&std::fclose(f)==0;check(okay,"read actual helper fixture bytes");return result;}
void write_file(const std::filesystem::path& path,const std::string& bytes){FILE* f=std::fopen(path.string().c_str(),"wb");check(f!=nullptr,"create owned test payload");const auto n=std::fwrite(bytes.data(),1,bytes.size(),f);check(std::fclose(f)==0&&n==bytes.size(),"exact owned payload written");}
struct Child {
    SOCKET peer{INVALID_SOCKET};HANDLE process{},job{};
    std::filesystem::path directory;
    std::uint64_t input{},output{};
    std::array<unsigned char,4096> send_buffer{};std::size_t send_buffer_size{};bool buffer_sends{};
    std::array<unsigned char,4096> read_buffer{};std::size_t read_buffer_at{},read_buffer_size{};bool buffer_reads{};
    ULONGLONG start{GetTickCount64()};bool aborted{};
    Child(const std::string& exe,const std::filesystem::path& root,bool send,const std::string& bytes,const char* offer="TEST.REP"){
        directory=root/((send?"send-":"receive-")+std::to_string(++cases));check(std::filesystem::create_directory(directory),"fresh per-process test folder");
        if(send)write_file(directory/"payload.qwk",bytes);else write_file(directory/offer,bytes);
        SOCKET listener=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(listener!=INVALID_SOCKET,"private listener");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0 && listen(listener,1)==0,"private ephemeral bind/listen");int size=sizeof(address);check(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)==0,"private endpoint identity");
        peer=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(peer!=INVALID_SOCKET,"owned peer socket");
        if(connect(peer,reinterpret_cast<sockaddr*>(&address),sizeof(address))!=0){const auto error=WSAGetLastError();closesocket(peer);peer=INVALID_SOCKET;closesocket(listener);throw std::runtime_error("owned loopback connect WSA="+std::to_string(error)+" port="+std::to_string(ntohs(address.sin_port)));}
        check(true,"owned peer connect");SOCKET child=accept(listener,nullptr,nullptr);closesocket(listener);check(child!=INVALID_SOCKET,"owned child endpoint");
        check(SetHandleInformation(reinterpret_cast<HANDLE>(peer),HANDLE_FLAG_INHERIT,0)!=0 && SetHandleInformation(reinterpret_cast<HANDLE>(child),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT)!=0,"only child endpoint inheritable");
        SIZE_T attribute_size{};InitializeProcThreadAttributeList(nullptr,1,0,&attribute_size);std::vector<unsigned char> attributes(attribute_size);STARTUPINFOEXA si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());check(InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attribute_size)!=0,"attribute allowlist initialized");HANDLE allowed=reinterpret_cast<HANDLE>(child);
        check(UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,&allowed,sizeof(allowed),nullptr,nullptr)!=0,"one endpoint handle allowlist");
        job=CreateJobObjectA(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;check(job && SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"owned kill-on-close test job");
        const std::string mode=send?"--sfx-packet-send-v1 ":"--sfx-packet-receive-v1 ";std::string command='"'+exe+"\" "+mode+std::to_string(child)+(send?" SFXTEST":"");PROCESS_INFORMATION pi{};
        const bool launched=CreateProcessA(exe.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,directory.string().c_str(),&si.StartupInfo,&pi)!=0;DeleteProcThreadAttributeList(si.lpAttributeList);closesocket(child);check(launched,"launch exact purpose-only helper");process=pi.hProcess;
        if(!AssignProcessToJobObject(job,process)){TerminateProcess(process,99);WaitForSingleObject(process,2000);CloseHandle(pi.hThread);throw std::runtime_error("contain before resume failed");}
        check(ResumeThread(pi.hThread)!=static_cast<DWORD>(-1),"resume only owned contained helper");CloseHandle(pi.hThread);u_long nonblocking=1;check(ioctlsocket(peer,FIONBIO,&nonblocking)==0,"bounded nonblocking peer");
    }
    ~Child(){if(process){if(WaitForSingleObject(process,0)==WAIT_TIMEOUT){TerminateJobObject(job,99);WaitForSingleObject(process,2000);}CloseHandle(process);}if(job)CloseHandle(job);if(peer!=INVALID_SOCKET)closesocket(peer);}
    bool alive(){return !aborted && GetTickCount64()-start<300000;}
    bool flush(unsigned seconds){
        // Test PEER only: ordinary client-sized writes, not16 million select /
        // send syscalls. No future acknowledgement is pre-fed. Flush before
        // reads/waits/final child proof; ledger counts actual successful bytes.
        const auto end=GetTickCount64()+static_cast<ULONGLONG>(seconds)*1000;std::size_t at{};
        while(at<send_buffer_size){
            if(!alive()){std::cerr<<"DIAG peer flush deadline elapsed="<<GetTickCount64()-start<<" output="<<output<<std::endl;return false;}
            fd_set set;FD_ZERO(&set);FD_SET(peer,&set);timeval wait{0,seconds?100000:0};const auto ready=select(0,nullptr,&set,nullptr,&wait);
            if(ready==SOCKET_ERROR){aborted=true;return false;}
            if(ready){const auto n=send(peer,reinterpret_cast<const char*>(send_buffer.data()+at),static_cast<int>(send_buffer_size-at),0);if(n>0){at+=static_cast<std::size_t>(n);output+=static_cast<std::uint64_t>(n);continue;}if(!n||WSAGetLastError()!=WSAEWOULDBLOCK){aborted=true;return false;}}
            if(GetTickCount64()>=end)return false;
        }
        send_buffer_size=0;return true;
    }
    int queued_send(unsigned char value,unsigned seconds){if(!buffer_sends)return byte(&value,true,seconds);send_buffer[send_buffer_size++]=value;return send_buffer_size==send_buffer.size()&&!flush(seconds)?-1:0;}
    int byte(unsigned char* value,bool sending,unsigned seconds){const auto end=GetTickCount64()+static_cast<ULONGLONG>(seconds)*1000;for(;;){if(!alive())return -1;
        if(!sending&&buffer_reads&&read_buffer_at<read_buffer_size){*value=read_buffer[read_buffer_at++];++input;return *value;}
        fd_set set;FD_ZERO(&set);FD_SET(peer,&set);timeval wait{0,seconds?100000:0};const int ready=select(0,sending?nullptr:&set,sending?&set:nullptr,nullptr,&wait);if(ready==SOCKET_ERROR){aborted=true;return -1;}if(ready){
            if(!sending&&buffer_reads){const auto n=recv(peer,reinterpret_cast<char*>(read_buffer.data()),static_cast<int>(read_buffer.size()),0);if(n>0){read_buffer_size=static_cast<std::size_t>(n);read_buffer_at=0;continue;}if(!n||WSAGetLastError()!=WSAEWOULDBLOCK){aborted=true;return -1;}}
            else{const int n=sending?send(peer,reinterpret_cast<const char*>(value),1,0):recv(peer,reinterpret_cast<char*>(value),1,0);if(n==1){if(sending)++output;else ++input;return sending?0:*value;}if(!n || WSAGetLastError()!=WSAEWOULDBLOCK){aborted=true;return -1;}}
        }if(GetTickCount64()>=end)return -1;}}
    DWORD finish(){check(flush(2),"all actual peer output flushed before completion proof");check(WaitForSingleObject(process,5000)==WAIT_OBJECT_0,"helper terminated after final protocol boundary");DWORD code{};check(GetExitCodeProcess(process,&code)!=0,"actual helper exit observed");return code;}
};
int send_byte(void* data,unsigned char value,unsigned timeout){return static_cast<Child*>(data)->queued_send(value,timeout);}
int recv_byte(void* data,unsigned timeout){auto* c=static_cast<Child*>(data);if(!c->flush(2))return -1;unsigned char value{};return c->byte(&value,false,timeout);}
BOOL connected(void* data){return static_cast<Child*>(data)->alive()?TRUE:FALSE;}
BOOL cancelled(void* data){return static_cast<Child*>(data)->aborted?TRUE:FALSE;}
BOOL waiting(void* data,unsigned timeout){auto* c=static_cast<Child*>(data);if(!c->flush(2))return FALSE;if(c->alive()&&c->buffer_reads&&c->read_buffer_at<c->read_buffer_size)return TRUE;fd_set set;FD_ZERO(&set);FD_SET(c->peer,&set);timeval wait{static_cast<long>(timeout),0};return c->alive()&&select(0,&set,nullptr,nullptr,&wait)>0?TRUE:FALSE;}
int peer_log(void* data,int level,const char* text){auto* c=static_cast<Child*>(data);FILE* log=std::fopen((c->directory/"peer-events.log").string().c_str(),"ab");if(log){std::fprintf(log,"%d %s\n",level,text);std::fclose(log);}return 0;}
zmodem_t peer(Child& c,bool crc32){zmodem_t z{};zmodem_init(&z,&c,peer_log,nullptr,send_byte,recv_byte,connected,cancelled,waiting,nullptr);z.escape_telnet_iac=FALSE;z.can_fcs_32=crc32?TRUE:FALSE;z.want_fcs_16=crc32?FALSE:TRUE;z.can_overlap_io=TRUE;z.max_errors=8;z.recv_timeout=10;z.send_timeout=2;
    // Generic peer treats this as exclusive at frame boundaries. This is NOT
    // the helper's inclusive packet cap, which remains exactly 16777216.
    z.max_file_size=16777217;return z;}
std::map<std::string,std::uint64_t> receipt(const Child& c,const char* expected){std::istringstream in(read_file(c.directory/"result.txt"));std::string header;std::getline(in,header);check(header==expected,"exact purpose/direction receipt version");std::map<std::string,std::uint64_t> result;std::string key;std::uint64_t value;while(in>>key>>value)check(result.emplace(key,value).second,"no duplicate receipt fields");check(result.size()==9,"exact receipt field count");return result;}
std::string payload(std::size_t size){std::string result(size,' ');for(std::size_t n=0;n<size;++n)result[n]=static_cast<char>((n*17+n/1031)%256);return result;}
void receive_case(const std::string& exe,const std::filesystem::path& root,std::size_t size,bool crc32,const char* offer="TEST.REP",unsigned refusal=0){
    const auto bytes=payload(size);Child c(exe,root,false,bytes,offer);c.buffer_sends=true;auto z=peer(c,crc32);z.files_remaining=z.total_files=1;z.bytes_remaining=z.total_bytes=static_cast<std::int64_t>(size);FILE* input=std::fopen((c.directory/offer).string().c_str(),"rb");check(input!=nullptr,"actual client input file");char name[129]{};strncpy_s(name,offer,_TRUNCATE);std::uint64_t sent{};
    check(zmodem_get_zrinit(&z)==ZRINIT,"peer negotiated ready receiver");z.can_fcs_32=crc32?TRUE:FALSE; // receiver permits both; exercise the actual selected wire CRC
    const bool transferred=zmodem_send_file(&z,name,input,FALSE,nullptr,&sent)!=0;check(std::fclose(input)==0,"client file close");check(transferred,"peer actual transfer or explicit skip");check(zmodem_get_zfin(&z)==ZFIN,"peer actual finish");check(c.finish()==0,"helper exact receive exit0 only after completion");const auto r=receipt(c,"SFX-PACKET-RECEIVE 1");check(r.at("failure")==0&&r.at("batch")==1&&r.at("refusal")==refusal,"completed receive/refusal classification");
    check(r.at("input")==c.output && r.at("output")==c.input,"actual socket byte ledgers match helper receipt");
    if(refusal){check(z.file_skipped && r.at("completed")==0 && r.at("bytes")==0 && !std::filesystem::exists(c.directory/"payload.rep"),"unsupported/oversize offer creates no payload or false delivery");}
    else {check(!z.file_skipped && r.at("completed")==1&&r.at("zeof")==1&&r.at("bytes")==size,"full CRC/EOF receive proof");check(read_file(c.directory/"payload.rep")==bytes,"actual received packet bytes exact");}
    std::cout<<"PASS receive size="<<size<<" CRC"<<(crc32?32:16)<<" refusal="<<refusal<<'\n';
}
void send_case(const std::string& exe,const std::filesystem::path& root,std::size_t size,bool crc32,bool late_ack=false,unsigned startup_delay=0){
    const auto bytes=payload(size);Child c(exe,root,true,bytes);c.buffer_reads=true;auto z=peer(c,crc32);
    if(startup_delay){
        // Explicit latency fixture: NO initial receiver bytes before this
        // measured boundary. It exposes a zero-poll callback that exhausts
        // ZRQINIT before a real client has time to acknowledge. Not a product
        // sleep, deadline/window change or pre-fed future acknowledgement.
        const auto started=GetTickCount64();Sleep(startup_delay);check(GetTickCount64()-started>=startup_delay&&WaitForSingleObject(c.process,0)==WAIT_TIMEOUT,"sender remains alive while initial peer response is deliberately delayed");
    }
    check(zmodem_recv_init(&z)==ZFILE,"peer receives actual helper ZFILE");check(std::string(z.current_file_name)=="SFXTEST.QWK"&&z.current_file_size==static_cast<std::int64_t>(size),"fixed payload offered with safe Board ID metadata");FILE* output=std::fopen((c.directory/"peer.qwk").string().c_str(),"wb");check(output!=nullptr,"owned peer output file");const unsigned errors=zmodem_recv_file_data(&z,output,0);check(std::fclose(output)==0,"peer file flushed/closed");check(errors<=z.max_errors&&static_cast<std::size_t>(z.current_file_pos)==size,"peer actual full payload receive");
    if(late_ack)check(zmodem_send_pos_header(&z,ZACK,static_cast<std::uint32_t>(size),TRUE)==0,"deterministic valid late ZACK before EOF ZRINIT");
    check(zmodem_recv_init(&z)==ZFIN,"peer actual final ZFIN and acknowledgement");check(recv_byte(&c,2)=='O'&&recv_byte(&c,2)=='O',"literal sender OO finish received");check(c.finish()==0,"helper exact send exit after completion");const auto r=receipt(c,"SFX-PACKET-SEND 1");
    check(r.at("failure")==0&&r.at("completed")==1&&r.at("batch")==1&&r.at("zeof")==1&&r.at("bytes")==size&&r.at("refusal")==0,"full acknowledged helper send proof");check(r.at("input")==c.output&&r.at("output")==c.input,"actual send socket ledgers match helper receipt");check(read_file(c.directory/"peer.qwk")==bytes,"actual downloaded bytes exact across bounded ACK windows");std::cout<<"PASS send size="<<size<<" CRC"<<(crc32?32:16)<<'\n';
}
void send_negative(const std::string& exe,const std::filesystem::path& root,unsigned mode){
    const auto bytes=payload(65537);Child c(exe,root,true,bytes);auto z=peer(c,true);
    check(zmodem_recv_init(&z)==ZFILE,"negative sender peer observed ZFILE");
    if(mode==1){
        check(zmodem_send_pos_header(&z,ZRPOS,1,TRUE)==0,"send forbidden initial resume");
    }else if(mode==2){
        check(zmodem_send_zabort(&z)==0,"cancel actual sender before payload");
    }else{
        FILE* out=std::fopen((c.directory/"peer.qwk").string().c_str(),"wb");
        check(out!=nullptr,"negative owned output file");
        const auto errors=zmodem_recv_file_data(&z,out,0);
        check(std::fclose(out)==0&&errors<=z.max_errors&&z.current_file_pos==65537,"negative sender payload reached EOF");
        if(mode==3){
            check(zmodem_send_pos_header(&z,ZACK,65538,TRUE)==0,"late ACK cannot acknowledge unsent byte");
        }else if(mode==4){
            unsigned offered{};
            for(unsigned n=0;n<9;++n){if(zmodem_send_pos_header(&z,ZACK,65537,TRUE)!=0)break;++offered;}
            check(offered>0,"late ACK flood offered; expected helper abort may close socket before ninth header");
        }else{
            check(zmodem_send_zrinit(&z)==0,"actual EOF receiver ready");
            check(zmodem_recv_header(&z)==ZFIN,"actual sender final boundary offered");
            check(zmodem_send_pos_header(&z,ZFIN,1,TRUE)==0,"CRC-valid but invalid nonzero final fields");
        }
    }
    check(c.finish()!=0,"negative helper exits without success");
    const auto r=receipt(c,"SFX-PACKET-SEND 1");
    check(r.at("failure")!=0&&r.at("batch")==0&&r.at("bytes")==0,"no negative acknowledged delivery receipt");
    std::cout<<"PASS send-negative mode="<<mode<<'\n';
}
void send_initial_cancel(const std::string& exe,const std::filesystem::path& root){
    Child c(exe,root,true,payload(17));Sleep(350);
    check(WaitForSingleObject(c.process,0)==WAIT_TIMEOUT,"sender still waiting before initial-response cancellation");
    for(unsigned n=0;n<5;++n)check(send_byte(&c,24,2)==0,"actual initial-wait cancellation bytes sent");
    check(c.finish()!=0,"initial-wait cancellation exits failed, not completed");const auto r=receipt(c,"SFX-PACKET-SEND 1");
    check(r.at("failure")!=0&&r.at("completed")==0&&r.at("batch")==0&&r.at("zeof")==0&&r.at("bytes")==0,"initial wait cancellation has no false delivery proof");
    std::cout<<"PASS send-initial-cancel\n";
}
void send_skip(const std::string& exe,const std::filesystem::path& root){
    Child c(exe,root,true,payload(17));auto z=peer(c,true);
    check(zmodem_recv_init(&z)==ZFILE&&zmodem_send_zskip(&z)==0,"explicit client skip without receiving payload");
    check(zmodem_recv_init(&z)==ZFIN,"skipped batch actual final handshake");
    check(recv_byte(&c,2)=='O'&&recv_byte(&c,2)=='O'&&c.finish()==0,"skipped batch actual OO and exit");
    const auto r=receipt(c,"SFX-PACKET-SEND 1");
    check(r.at("failure")==0&&r.at("refusal")==4&&r.at("batch")==1&&r.at("completed")==0&&r.at("bytes")==0&&r.at("zeof")==0,"skip is no delivery/progress, not completed file");
    check(r.at("input")==c.output&&r.at("output")==c.input,"skip exact socket ledger");
    std::cout<<"PASS send-explicit-skip\n";
}
void receive_empty(const std::string& exe,const std::filesystem::path& root,bool bad_end){
    Child c(exe,root,false,{},"EMPTY.REP");auto z=peer(c,true);
    check(zmodem_get_zrinit(&z)==ZRINIT,"empty receiver actual readiness");
    check(zmodem_send_zfin(&z)==0,"empty sender offers final once");
    int end=ZRINIT;
    for(unsigned n=0;n<8&&end==ZRINIT;++n)end=zmodem_recv_header(&z);
    check(end==ZFIN,"empty batch actual final acknowledgement after bounded ready headers");
    if(bad_end){
        check(send_byte(&c,'O',2)==0&&send_byte(&c,'X',2)==0,"invalid final literal bytes offered");
        check(c.finish()!=0,"invalid empty final is not successful");
    }else{
        check(send_byte(&c,'O',2)==0&&send_byte(&c,'O',2)==0&&c.finish()==0,"empty actual FIN/OO completion");
    }
    const auto r=receipt(c,"SFX-PACKET-RECEIVE 1");
    check(r.at("completed")==0&&r.at("bytes")==0&&!std::filesystem::exists(c.directory/"payload.rep"),"no empty payload or completed file");
    check(bad_end?(r.at("failure")!=0&&r.at("batch")==0):(r.at("failure")==0&&r.at("batch")==1&&r.at("refusal")==3&&r.at("attempted")==0),"exact empty versus invalid-end classification");
    std::cout<<"PASS receive-empty bad-end="<<bad_end<<'\n';
}
}
int main(int argc,char** argv){try{check(argc==3,"exact candidate and new owned fixture root required");const auto exe=std::filesystem::absolute(argv[1]).string();const auto root=std::filesystem::absolute(argv[2]);check(std::filesystem::create_directories(root),"new helper test root only");WSADATA w{};check(WSAStartup(MAKEWORD(2,2),&w)==0,"peer Winsock initialized");
    send_case(exe,root,17,false,false,350);send_case(exe,root,4097,true,false,350);
    send_initial_cancel(exe,root);
    for(bool crc:{false,true})for(std::size_t size:{std::size_t{17},std::size_t{4097},std::size_t{65537}}){receive_case(exe,root,size,crc);send_case(exe,root,size,crc);}
    receive_case(exe,root,17,true,"BAD.EXE",1);receive_case(exe,root,16777217,true,"BIG.REP",2);
    send_case(exe,root,65537,false,true);send_case(exe,root,65537,true,true);
    for(unsigned mode=1;mode<=5;++mode)send_negative(exe,root,mode);
    send_skip(exe,root);receive_empty(exe,root,false);receive_empty(exe,root,true);
    receive_case(exe,root,16777216,true);send_case(exe,root,16777216,true);
    WSACleanup();std::cout<<"PASS "<<checks<<" checks / "<<cases<<" actual private helper process cases; NOT Runtime/Telnet/SSH/NCM qualification\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" helper checks: "<<e.what()<<'\n';return 1;}}
