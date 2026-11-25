// GPL-2.0-or-later. Separate helper/peer tests; never linked into SFX.
// Actual owned child, sockets and inherited disk handles; not installed wire/UI.
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
int zmodem_send_data_subpkt(zmodem_t*,unsigned char,unsigned char*,size_t);
int zmodem_send_from(zmodem_t*,FILE*,std::uint64_t,std::uint64_t*);
}
namespace {
unsigned checks{},cases{};
void check(bool value,const char* reason){++checks;if(!value)throw std::runtime_error(reason);}
std::string read_file(const std::filesystem::path& path){FILE* f=std::fopen(path.string().c_str(),"rb");check(f!=nullptr,"read owned fixture");std::string result;char b[8192];std::size_t n;while((n=std::fread(b,1,sizeof(b),f)))result.append(b,n);const bool good=!std::ferror(f);check(std::fclose(f)==0&&good,"read/close exact fixture");return result;}
void write_file(const std::filesystem::path& path,const std::string& bytes){FILE* f=std::fopen(path.string().c_str(),"wb");check(f!=nullptr,"new owned fixture");const auto n=std::fwrite(bytes.data(),1,bytes.size(),f);check(std::fclose(f)==0&&n==bytes.size(),"exact fixture bytes written");}
std::string payload(std::size_t n){std::string s(n,' ');for(std::size_t i=0;i<n;++i)s[i]=static_cast<char>((i*17+i/1031)%256);return s;}
std::string hex(const std::string& s){std::string r;for(unsigned char c:s){r+="0123456789ABCDEF"[c>>4];r+="0123456789ABCDEF"[c&15];}return r;}
struct File {std::string name,bytes;std::uint64_t cap{104857600};};
struct Child {
    SOCKET socket{INVALID_SOCKET};HANDLE process{},job{};std::vector<HANDLE> handles;
    std::filesystem::path directory;std::uint64_t input{},output{};
    std::array<unsigned char,4096> out{},in{};std::size_t used{},at{},available{};
    ULONGLONG start{GetTickCount64()};bool aborted{};
    std::filesystem::path slot(std::size_t n)const{return directory/("host-"+std::to_string(n)+".bin");}
    Child(const std::string& exe,const std::filesystem::path& root,bool sending,const std::vector<File>& files,unsigned invalid=0){
        directory=root/((sending?"send-":"receive-")+std::to_string(++cases));check(std::filesystem::create_directory(directory),"new per-process directory");
        std::ostringstream cfg;cfg<<(sending?"SFX-FILE-SEND 1\n":"SFX-FILE-RECEIVE 1\n")<<files.size()<<'\n';
        for(std::size_t i=0;i<files.size();++i){
            write_file(slot(i),sending?files[i].bytes:std::string{});
            HANDLE h=CreateFileW(slot(i).c_str(),sending?GENERIC_READ:(GENERIC_READ|GENERIC_WRITE),FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);
            check(h!=INVALID_HANDLE_VALUE&&SetHandleInformation(h,HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"locked inherited host-created payload");handles.push_back(h);
            const auto chosen=invalid==2&&i?handles[0]:h;
            cfg<<reinterpret_cast<std::uintptr_t>(chosen)<<' '<<files[i].cap<<' '<<hex(files[i].name)<<'\n';
        }
        if(invalid==1)cfg<<"UNEXPECTED\n";
        write_file(directory/"job.cfg",cfg.str());
        SOCKET listener=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(listener!=INVALID_SOCKET,"owned listener");sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        check(bind(listener,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0&&listen(listener,1)==0,"private ephemeral bind/listen");int size=sizeof(address);check(getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)==0,"private endpoint");
        socket=::socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);check(socket!=INVALID_SOCKET,"owned peer");check(connect(socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"loopback connection");SOCKET child=accept(listener,nullptr,nullptr);closesocket(listener);check(child!=INVALID_SOCKET,"owned child endpoint");
        check(SetHandleInformation(reinterpret_cast<HANDLE>(socket),HANDLE_FLAG_INHERIT,0)&&SetHandleInformation(reinterpret_cast<HANDLE>(child),HANDLE_FLAG_INHERIT,HANDLE_FLAG_INHERIT),"endpoint inherit flags");
        auto allowed=handles;allowed.push_back(reinterpret_cast<HANDLE>(child));
        SIZE_T attr_size{};InitializeProcThreadAttributeList(nullptr,1,0,&attr_size);std::vector<unsigned char> attributes(attr_size);STARTUPINFOEXA si{};si.StartupInfo.cb=sizeof(si);si.lpAttributeList=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        check(InitializeProcThreadAttributeList(si.lpAttributeList,1,0,&attr_size)&&UpdateProcThreadAttribute(si.lpAttributeList,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,allowed.data(),allowed.size()*sizeof(HANDLE),nullptr,nullptr),"explicit payload/socket allowlist");
        job=CreateJobObjectA(nullptr,nullptr);JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        check(job&&SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits)),"contained owned child");
        std::string command='"'+exe+"\" "+(sending?"--sfx-file-send-v1 ":"--sfx-file-receive-v1 ")+std::to_string(child);PROCESS_INFORMATION pi{};
        const bool launched=CreateProcessA(exe.c_str(),command.data(),nullptr,nullptr,TRUE,EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,nullptr,directory.string().c_str(),&si.StartupInfo,&pi)!=0;DeleteProcThreadAttributeList(si.lpAttributeList);closesocket(child);check(launched,"launch exact file-only helper");process=pi.hProcess;
        if(!AssignProcessToJobObject(job,process)){TerminateProcess(process,99);WaitForSingleObject(process,2000);CloseHandle(pi.hThread);throw std::runtime_error("containment failed");}
        check(ResumeThread(pi.hThread)!=static_cast<DWORD>(-1),"resume owned contained child");CloseHandle(pi.hThread);u_long nonblocking=1;check(ioctlsocket(socket,FIONBIO,&nonblocking)==0,"nonblocking bounded peer");
    }
    ~Child(){if(process){if(WaitForSingleObject(process,0)==WAIT_TIMEOUT){TerminateJobObject(job,99);WaitForSingleObject(process,2000);}CloseHandle(process);}if(job)CloseHandle(job);if(socket!=INVALID_SOCKET)closesocket(socket);for(auto h:handles)CloseHandle(h);}
    bool alive(){return !aborted&&GetTickCount64()-start<300000;}
    bool flush(unsigned seconds=2){std::size_t p=0;const auto end=GetTickCount64()+static_cast<ULONGLONG>(seconds)*1000;while(p<used){if(!alive())return false;fd_set f;FD_ZERO(&f);FD_SET(socket,&f);timeval t{0,100000};int r=select(0,nullptr,&f,nullptr,&t);if(r==SOCKET_ERROR){aborted=true;return false;}if(r){const int n=send(socket,reinterpret_cast<const char*>(out.data()+p),static_cast<int>(used-p),0);if(n>0){p+=n;output+=n;continue;}if(!n||WSAGetLastError()!=WSAEWOULDBLOCK){aborted=true;return false;}}if(GetTickCount64()>=end)return false;}used=0;return true;}
    int put(unsigned char c,unsigned seconds){out[used++]=c;return used==out.size()&&!flush(seconds)?-1:0;}
    int get(unsigned seconds){if(!flush())return -1;const auto end=GetTickCount64()+static_cast<ULONGLONG>(seconds)*1000;for(;;){if(!alive())return -1;if(at<available){++input;return in[at++];}fd_set f;FD_ZERO(&f);FD_SET(socket,&f);timeval t{0,seconds?100000:0};const int r=select(0,&f,nullptr,nullptr,&t);if(r==SOCKET_ERROR){aborted=true;return -1;}if(r){const int n=recv(socket,reinterpret_cast<char*>(in.data()),static_cast<int>(in.size()),0);if(n>0){at=0;available=n;continue;}if(!n||WSAGetLastError()!=WSAEWOULDBLOCK){aborted=true;return -1;}}if(GetTickCount64()>=end)return -1;}}
    DWORD finish(){check(flush(),"peer output flushed");check(WaitForSingleObject(process,5000)==WAIT_OBJECT_0,"actual helper exited at final boundary");DWORD code{};check(GetExitCodeProcess(process,&code)!=0,"observed child exit");return code;}
    std::string stored(std::size_t i){LARGE_INTEGER zero{};check(SetFilePointerEx(handles.at(i),zero,nullptr,FILE_BEGIN)!=0,"rewind original locked host handle");std::string r;char b[8192];DWORD n;do{check(ReadFile(handles.at(i),b,sizeof(b),&n,nullptr)!=0,"read exact original payload handle");r.append(b,n);}while(n);return r;}
};
int put(void* d,unsigned char b,unsigned t){return static_cast<Child*>(d)->put(b,t);}
int get(void* d,unsigned t){return static_cast<Child*>(d)->get(t);}
BOOL connected(void* d){return static_cast<Child*>(d)->alive()?TRUE:FALSE;}
BOOL cancelled(void* d){return static_cast<Child*>(d)->aborted?TRUE:FALSE;}
BOOL waiting(void* d,unsigned t){auto& c=*static_cast<Child*>(d);if(!c.flush())return FALSE;if(c.at<c.available)return TRUE;fd_set f;FD_ZERO(&f);FD_SET(c.socket,&f);timeval timeout{static_cast<long>(t),0};return c.alive()&&select(0,&f,nullptr,nullptr,&timeout)>0?TRUE:FALSE;}
int log_peer(void* d,int level,const char* text){auto& c=*static_cast<Child*>(d);FILE* f=std::fopen((c.directory/"peer-events.log").string().c_str(),"ab");if(f){std::fprintf(f,"%d %s\n",level,text);std::fclose(f);}return 0;}
zmodem_t peer(Child& c,bool crc){zmodem_t z{};zmodem_init(&z,&c,log_peer,nullptr,put,get,connected,cancelled,waiting,nullptr);z.escape_telnet_iac=FALSE;z.can_fcs_32=crc;z.want_fcs_16=!crc;z.can_overlap_io=TRUE;z.max_errors=8;z.recv_timeout=3;z.send_timeout=2;z.max_file_size=1073741825;return z;}
struct Result {std::map<std::string,std::uint64_t> fields;std::vector<std::array<std::uint64_t,5>> files;};
Result receipt(const Child& c,char direction,std::size_t count){std::istringstream in(read_file(c.directory/"result.txt"));std::string line;std::getline(in,line);check(line=="SFX-FILE-TRANSFER 1","exact receipt version");std::getline(in,line);check(line==std::string("direction ")+direction,"exact receipt direction");Result r;for(unsigned i=0;i<7;++i){std::string k;std::uint64_t n;check(static_cast<bool>(in>>k>>n)&&r.fields.emplace(k,n).second,"unique receipt fields");}check(r.fields.size()==7&&r.fields.at("count")==count,"exact manifest count");for(std::size_t i=0;i<count;++i){std::string k;std::size_t index{};std::array<std::uint64_t,5> f{};check(static_cast<bool>(in>>k>>index>>f[0]>>f[1]>>f[2]>>f[3]>>f[4])&&k=="file"&&index==i,"exact indexed slot receipt");r.files.push_back(f);}in>>std::ws;check(in.eof(),"no receipt suffix");return r;}
void ledgers(const Child& c,const Result& r){check(r.fields.at("input")==c.output&&r.fields.at("output")==c.input,"exact peer/helper byte ledgers");}
bool finish_sender(Child& c,zmodem_t& z){
    // Generic upstream get_zfin alternates ZABORT after even one stale ready
    // frame. Model a completing client, not an unintended client cancellation.
    if(zmodem_send_zfin(&z)!=0)return false;
    int type=ZRINIT;for(unsigned i=0;i<8&&type==ZRINIT;++i)type=zmodem_recv_header(&z);
    return type==ZFIN&&put(&c,'O',2)==0&&put(&c,'O',2)==0;
}
bool offer(Child& c,zmodem_t& z,const File& file,bool crc,unsigned remaining){
    z.can_fcs_32=crc;
    if(file.bytes.empty()){
        // Independent zero-byte wire sequence: generic upstream sender refuses
        // initial ZRPOS0 for empty files. Do not share the helper's new branch.
        std::string metadata=file.name;metadata.push_back(0);metadata+="0";metadata.push_back(0);
        check(zmodem_send_pos_header(&z,ZFILE,0,FALSE)==0&&zmodem_send_data_subpkt(&z,ZCRCW,reinterpret_cast<unsigned char*>(metadata.data()),metadata.size())==0,"zero-byte offer encoded");
        int type=zmodem_recv_header(&z);if(type==ZRINIT)type=zmodem_recv_header(&z);
        if(type==ZSKIP)return false;
        check(type==ZRPOS&&z.rxd_header_pos==0,"zero-byte receiver ready at zero");
        check(zmodem_send_pos_header(&z,ZEOF,0,FALSE)==0&&zmodem_recv_header(&z)==ZRINIT,"zero-byte actual EOF accepted");return true;
    }
    const auto path=c.directory/"peer-input.bin";write_file(path,file.bytes);FILE* f=std::fopen(path.string().c_str(),"rb");check(f!=nullptr,"peer source opened");z.files_remaining=remaining;z.total_files=remaining;z.bytes_remaining=z.total_bytes=file.bytes.size();std::vector<char> name(file.name.begin(),file.name.end());name.push_back(0);std::uint64_t sent{};const bool ok=zmodem_send_file(&z,name.data(),f,FALSE,nullptr,&sent)!=0;check(std::fclose(f)==0&&ok,"actual send or explicit skip");return !z.file_skipped;
}
// Deterministic credit-wait regression, not a timing/GUI claim. Invoke the
// actual sender loop with a valid encoded ACK. Stop before reading more data;
// inspect the next requested wait rather than relying on scheduler timing.
struct CreditProbe {std::vector<unsigned char> ack;std::size_t at{};unsigned polls{};int after_ack{-1};bool stop{};};
int credit_encode(void* d,unsigned char byte,unsigned){static_cast<CreditProbe*>(d)->ack.push_back(byte);return 0;}
int credit_discard(void*,unsigned char,unsigned){return 0;}
int credit_read(void* d,unsigned){auto& p=*static_cast<CreditProbe*>(d);return p.at<p.ack.size()?p.ack[p.at++]:-1;}
BOOL credit_connected(void* d){return !static_cast<CreditProbe*>(d)->stop;}
BOOL credit_cancelled(void* d){return static_cast<CreditProbe*>(d)->stop;}
BOOL credit_wait(void* d,unsigned timeout){auto& p=*static_cast<CreditProbe*>(d);++p.polls;if(p.polls==1){check(timeout==0,"initial backchannel poll is nonblocking");return FALSE;}if(p.polls==2){check(timeout==1,"a full transmit window requires a bounded ACK wait");return TRUE;}p.after_ack=static_cast<int>(timeout);p.stop=true;return FALSE;}
void credit_wait_regression(const std::filesystem::path& root){
    const auto path=root/"credit-source.bin";write_file(path,payload(32768));
    for(unsigned offset:{4096U,0U}){
        CreditProbe probe;zmodem_t encoded{};zmodem_init(&encoded,&probe,nullptr,nullptr,credit_encode,credit_read,credit_connected,credit_cancelled,credit_wait,nullptr);check(zmodem_send_ack(&encoded,offset)==0,"valid independent ACK encoding");
        zmodem_t sender{};zmodem_init(&sender,&probe,nullptr,nullptr,credit_discard,credit_read,credit_connected,credit_cancelled,credit_wait,nullptr);sender.sfx_files=TRUE;sender.max_window_size=16384;sender.current_window_size=16384;sender.current_file_size=32768;
        FILE* input=std::fopen(path.string().c_str(),"rb");check(input!=nullptr,"credit fixture opened");std::uint64_t sent{};const int result=zmodem_send_from(&sender,input,16384,&sent);check(std::fclose(input)==0,"credit fixture closed");
        check(result==ZCAN&&sent==0,"controlled stop never claims successful transfer");
        check(sender.ack_file_pos==offset&&sender.current_window_size==16384-offset,"only validated ACK credit changes window accounting");
        check(probe.after_ack==(offset?0:1),"freed credit resumes immediately; duplicate ACK with full window retains blocking wait");
    }
    std::cout<<"PASS deterministic sender credit wait / duplicate ACK guard (in-process protocol model, not a child case)"<<std::endl;
}
void receive_batch(const std::string& exe,const std::filesystem::path& root,bool crc){
    std::vector<File> files{{"ZERO.BIN",{}},{"program.exe",payload(17)},{"archive.zip",payload(65537)},{"caf\x82.txt",payload(4097)}};
    Child c(exe,root,false,files);auto z=peer(c,crc);check(zmodem_get_zrinit(&z)==ZRINIT,"receive ready");check(z.recv_bufsize==8192,"standard finite receive credit advertised before file data");
    for(std::size_t i=0;i<files.size();++i)check(offer(c,z,files[i],crc,static_cast<unsigned>(files.size()-i)),"all known general files accepted regardless extension");
    check(finish_sender(c,z)&&c.finish()==0,"completed actual receive batch");const auto r=receipt(c,'R',files.size());ledgers(c,r);
    check(r.fields.at("attempted")==files.size()&&r.fields.at("refused")==0&&r.fields.at("batch")==1&&r.fields.at("failure")==0,"successful batch summary");
    for(std::size_t i=0;i<files.size();++i){check(r.files[i]==std::array<std::uint64_t,5>{1,1,0,files[i].bytes.size(),1},"full per-file CRC/EOF proof");check(c.stored(i)==files[i].bytes,"original inherited object exact bytes");check(!std::filesystem::exists(c.directory/files[i].name),"wire filename never becomes payload path");}
    std::cout<<"PASS receive batch4 CRC"<<(crc?32:16)<<std::endl;
}
void receive_refusals(const std::string& exe,const std::filesystem::path& root){std::vector<File> files{{"known.bin",payload(64),64},{"big.bin",payload(65),64},{"absent.bin",{},64}};Child c(exe,root,false,files);auto z=peer(c,true);check(zmodem_get_zrinit(&z)==ZRINIT,"refusal receiver ready");check(!offer(c,z,{"not-in-manifest.bin",payload(17)},true,4),"unknown offer skipped");check(!offer(c,z,files[1],true,3),"declared oversize skipped before data");check(offer(c,z,files[0],true,2),"exact inclusive cap accepted");check(!offer(c,z,files[0],true,1),"duplicate offer skipped");check(finish_sender(c,z)&&c.finish()==0,"refused batch finishes without session failure");const auto r=receipt(c,'R',3);ledgers(c,r);check(r.fields.at("attempted")==4&&r.fields.at("refused")==3&&r.fields.at("batch")==1&&!r.fields.at("failure"),"precise refusal summary");check(c.stored(0)==files[0].bytes&&c.stored(1).empty()&&c.stored(2).empty(),"no bytes written for refused or absent offers");check(r.files[1]==std::array<std::uint64_t,5>{1,0,2,0,0}&&r.files[2]==std::array<std::uint64_t,5>{0,0,0,0,0},"no false refused/absent delivery");std::cout<<"PASS unknown/oversize/duplicate/inclusive-cap"<<std::endl;}
void receive_empty(const std::string& exe,const std::filesystem::path& root,unsigned mode){Child c(exe,root,false,{{"empty.bin",{},64}});auto z=peer(c,true);check(zmodem_get_zrinit(&z)==ZRINIT,"empty ready");check(zmodem_send_zfin(&z)==0,"empty ZFIN sent");int type=ZRINIT;for(unsigned i=0;i<8&&type==ZRINIT;++i)type=zmodem_recv_header(&z);check(type==ZFIN,"empty final acknowledgement");check(put(&c,'O',2)==0&&put(&c,mode==1?'X':'O',2)==0,"final literal bytes");if(mode==2)check(put(&c,13,2)==0,"early Enter tail offered");const auto code=c.finish();const auto r=receipt(c,'R',1);check((code==0)==(mode!=1)&&r.fields.at("attempted")==0&&r.files[0]==std::array<std::uint64_t,5>{0,0,0,0,0}&&c.stored(0).empty(),"empty is no upload, bad end fails");check(r.fields.at("batch")==static_cast<unsigned>(mode!=1),"empty final proof exact");if(mode==0)ledgers(c,r);else if(mode==2)check(r.fields.at("input")+1==c.output,"early Enter not consumed as transfer data");else check(r.fields.at("input")==c.output&&r.fields.at("output")>=c.input,"bad-end cancellation output is not claimed received by peer");std::cout<<"PASS empty/end mode="<<mode<<std::endl;}
void send_batch(const std::string& exe,const std::filesystem::path& root,bool crc,unsigned skip){std::vector<File> files{{"zero.bin",{}},{"one.exe",payload(17)},{"three.zip",payload(65537)}};Child c(exe,root,true,files);auto z=peer(c,crc);for(std::size_t i=0;i<files.size();++i){check(zmodem_recv_init(&z)==ZFILE,"next actual send offer");check(z.current_file_name==files[i].name&&static_cast<std::uint64_t>(z.current_file_size)==files[i].bytes.size(),"manifest name and length offered");if(skip==2||(skip==1&&i==1)){check(zmodem_send_zskip(&z)==0,"explicit peer skip");continue;}const auto path=c.directory/("peer-"+std::to_string(i));FILE* out=std::fopen(path.string().c_str(),"wb");check(out!=nullptr,"peer output");const auto errors=zmodem_recv_file_data(&z,out,0);check(std::fclose(out)==0&&errors<=z.max_errors&&static_cast<std::uint64_t>(z.current_file_pos)==files[i].bytes.size(),"actual complete receive");check(read_file(path)==files[i].bytes,"download exact");}check(zmodem_recv_init(&z)==ZFIN&&get(&c,2)=='O'&&get(&c,2)=='O'&&c.finish()==0,"send batch final exchange");const auto r=receipt(c,'S',files.size());ledgers(c,r);check(r.fields.at("batch")==1&&!r.fields.at("failure"),"successful send batch");for(std::size_t i=0;i<files.size();++i){const bool skipped=skip==2||(skip==1&&i==1);check(r.files[i]==(skipped?std::array<std::uint64_t,5>{1,0,1,0,0}:std::array<std::uint64_t,5>{1,1,0,files[i].bytes.size(),1}),"skips not credited, complete per-file proof");check(c.stored(i)==files[i].bytes,"source objects unchanged");}std::cout<<"PASS send batch CRC"<<(crc?32:16)<<" skip="<<skip<<std::endl;}
void config_negative(const std::string& exe,const std::filesystem::path& root,unsigned mode){std::vector<File> files{{"valid.bin",{},64},{"second.bin",{},64}};if(mode==3)files[1].name="VALID.BIN";if(mode==4)files[0].name="../escape";if(mode==5)files[0].cap=1073741825;Child c(exe,root,false,files,mode);check(c.finish()!=0,"bad trusted manifest fails closed");check(!std::filesystem::exists(c.directory/"result.txt")&&c.stored(0).empty()&&c.stored(1).empty(),"invalid manifest no protocol/payload success");std::cout<<"PASS invalid manifest mode="<<mode<<std::endl;}
void raw_negative(const std::string& exe,const std::filesystem::path& root,unsigned mode){
    Child c(exe,root,false,{{"known.bin",{},64}});auto z=peer(c,false);check(zmodem_get_zrinit(&z)==ZRINIT,"raw receiver ready");z.can_fcs_32=FALSE;
    std::string m="known.bin";m.push_back(0);m+=(mode==2?"18446744073709551616":"64");m.push_back(0);
    check(zmodem_send_pos_header(&z,ZFILE,0,FALSE)==0&&zmodem_send_data_subpkt(&z,ZCRCW,reinterpret_cast<unsigned char*>(m.data()),m.size())==0,"raw metadata offered");
    if(mode!=2){
        int type=zmodem_recv_header(&z);if(type==ZRINIT)type=zmodem_recv_header(&z);
        check(type==ZRPOS&&z.rxd_header_pos==0,"ready at actual zero offset");
        if(mode==0||mode==3){
            auto data=payload(mode==0?65:64);check(zmodem_send_pos_header(&z,ZDATA,0,FALSE)==0&&c.flush(),"actual data header");
            check(zmodem_send_data_subpkt(&z,ZCRCE,reinterpret_cast<unsigned char*>(data.data()),data.size())==0,"actual bounded packet encoded");
            if(mode==3){
                check(c.used>2,"encoded CRC present");c.out[c.used-1]^=1;
                check(zmodem_recv_header(&z)==ZRPOS&&z.rxd_header_pos==0,"bad CRC requests retransmission at zero, no write");
                for(unsigned i=0;i<5;++i)check(put(&c,24,2)==0,"cancel after observed CRC retry");
            }
        }else if(mode==4){for(unsigned i=0;i<5;++i)check(put(&c,24,2)==0,"actual cancel byte");}
        else if(mode==5)check(shutdown(c.socket,SD_SEND)==0,"actual peer sending disconnect");
        else check(zmodem_send_pos_header(&z,ZEOF,64,FALSE)==0,"premature EOF offered");
    }
    check(c.finish()!=0,"invalid stream rejected");const auto r=receipt(c,'R',1);
    check(r.fields.at("failure")!=0&&r.fields.at("batch")==0&&r.files[0][1]==0&&c.stored(0).size()<=64,"no false complete/overshoot");
    if(mode==0||mode==3)check(c.stored(0).empty(),"invalid whole subpacket never written");
    std::cout<<"PASS raw invalid stream mode="<<mode<<std::endl;
}
void announced_terabyte(const std::string& exe,const std::filesystem::path& root){Child c(exe,root,false,{{"huge.bin",{},104857600}});auto z=peer(c,false);check(zmodem_get_zrinit(&z)==ZRINIT,"huge ready");std::string m="huge.bin";m.push_back(0);m+="1099511627776";m.push_back(0);check(zmodem_send_pos_header(&z,ZFILE,0,FALSE)==0&&zmodem_send_data_subpkt(&z,ZCRCW,reinterpret_cast<unsigned char*>(m.data()),m.size())==0,"one TiB declaration without payload allocation");int type=zmodem_recv_header(&z);if(type==ZRINIT)type=zmodem_recv_header(&z);check(type==ZSKIP&&finish_sender(c,z)&&c.finish()==0,"huge skipped and clean final handshake");auto r=receipt(c,'R',1);check(r.files[0]==std::array<std::uint64_t,5>{1,0,2,0,0}&&c.stored(0).empty(),"huge offer allocated no payload bytes");std::cout<<"PASS declared 1 TiB refusal"<<std::endl;}
void missing_finish(const std::string& exe,const std::filesystem::path& root){Child c(exe,root,false,{{"known.bin",payload(64),64}});auto z=peer(c,true);check(zmodem_get_zrinit(&z)==ZRINIT&&offer(c,z,{"known.bin",payload(64),64},true,1),"file reaches valid EOF");check(zmodem_send_zfin(&z)==0&&zmodem_recv_header(&z)==ZFIN&&put(&c,'O',2)==0,"only one final O");check(c.finish()!=0,"missing final O cannot complete batch");auto r=receipt(c,'R',1);check(r.fields.at("batch")==0&&r.fields.at("failure")!=0&&r.files[0][1]==1,"file EOF alone is NOT batch acceptance proof");std::cout<<"PASS missing final O after valid file"<<std::endl;}
void default_limit(const std::string& exe,const std::filesystem::path& root){constexpr std::size_t size=104857600;std::vector<File> files{{"default-limit.bin",payload(size),size}};Child c(exe,root,false,files);auto z=peer(c,true);check(zmodem_get_zrinit(&z)==ZRINIT&&offer(c,z,files[0],true,1)&&finish_sender(c,z)&&c.finish()==0,"actual 100 MiB receive at inclusive default cap");auto r=receipt(c,'R',1);ledgers(c,r);check(r.files[0]==std::array<std::uint64_t,5>{1,1,0,size,1}&&c.stored(0)==files[0].bytes,"all default-cap bytes exact on original object");std::cout<<"PASS actual 100 MiB default cap"<<std::endl;}
}
int main(int argc,char** argv){try{check(argc==3,"exact helper/new fixture required");const auto exe=std::filesystem::absolute(argv[1]).string();const auto root=std::filesystem::absolute(argv[2]);check(std::filesystem::space(root.parent_path()).available>1073741824,"at least 1 GiB available before bounded 200 MiB fixture");check(std::filesystem::create_directory(root),"new fixture root only");WSADATA data{};check(WSAStartup(MAKEWORD(2,2),&data)==0,"peer Winsock");credit_wait_regression(root);for(bool crc:{false,true}){receive_batch(exe,root,crc);send_batch(exe,root,crc,0);}receive_refusals(exe,root);for(unsigned mode=0;mode<3;++mode)receive_empty(exe,root,mode);send_batch(exe,root,true,1);send_batch(exe,root,true,2);for(unsigned mode=1;mode<=5;++mode)config_negative(exe,root,mode);for(unsigned mode=0;mode<6;++mode)raw_negative(exe,root,mode);announced_terabyte(exe,root);missing_finish(exe,root);default_limit(exe,root);WSACleanup();std::cout<<"PASS "<<checks<<" checks / "<<cases<<" actual file-helper process cases; NOT installed Runtime/Telnet/SSH qualification\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<" checks / "<<cases<<" cases: "<<e.what()<<std::endl;return 1;}}
