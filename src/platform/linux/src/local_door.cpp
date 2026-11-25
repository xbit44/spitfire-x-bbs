#include "sfx/platform/linux/local_door.hpp"
#include "sfx/platform/linux/dos_setup.hpp"
#include "sfx/platform/linux/private_files.hpp"
#include "sfx/platform/linux/child_process.hpp"
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <termios.h>
#include <pty.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <limits>
#include <sodium.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <stdexcept>
#include <thread>
#include <charconv>
#include <cctype>
#include <sstream>

namespace sfx::platform::linux_host {
namespace {
namespace app=application;namespace fs=std::filesystem;
using Clock=std::chrono::steady_clock;using Time=Clock::time_point;
void need(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
struct Fd {int n{-1};Fd()=default;explicit Fd(int value):n(value){}~Fd(){if(n>=0)close(n);}Fd(Fd&& f)noexcept:n(f.n){f.n=-1;}Fd& operator=(Fd&& f)noexcept{if(n>=0)close(n);n=f.n;f.n=-1;return *this;}Fd(const Fd&)=delete;int get()const{return n;}explicit operator bool()const{return n>=0;}void reset(){if(n>=0)close(n);n=-1;}};
void nonblock(int fd){const auto flags=fcntl(fd,F_GETFL);need(flags>=0&&fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0,"door_nonblocking_failed");}
Fd directory(const fs::path& p){need(p.is_absolute(),"door_absolute_directory_required");Fd current(open("/",O_RDONLY|O_DIRECTORY|O_CLOEXEC));for(const auto& part:p.relative_path()){need(part!="."&&part!="..","door_directory_alias_refused");Fd child(openat(current.get(),part.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC));need(bool(child),"door_directory_unavailable");current=std::move(child);}return current;}
void private_directory(const fs::path& p){auto parent=directory(p.parent_path());if(mkdirat(parent.get(),p.filename().c_str(),0700)!=0)need(errno==EEXIST,"door_directory_create_failed");auto child=directory(p);struct stat s{};need(fstat(child.get(),&s)==0&&s.st_uid==geteuid()&&!(s.st_mode&0022),"door_directory_owner_invalid");}
std::string random_id(){std::array<unsigned char,16> b{};Fd f(open("/dev/urandom",O_RDONLY|O_CLOEXEC));need(bool(f)&&read(f.get(),b.data(),b.size())==static_cast<ssize_t>(b.size()),"door_nonce_unavailable");const char* h="0123456789abcdef";std::string out;for(auto c:b){out+=h[c>>4];out+=h[c&15];}return out;}
std::string file_text(const fs::path& p){Fd f(open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));if(!f){need(errno==ENOENT,"door_ini_unavailable");return {};}struct stat s{};need(!fstat(f.get(),&s)&&S_ISREG(s.st_mode)&&s.st_size>=0&&s.st_size<=16384,"door_ini_invalid");std::string out(static_cast<std::size_t>(s.st_size),'\0');std::size_t at{};while(at<out.size()){const auto n=read(f.get(),out.data()+at,out.size()-at);need(n>0,"door_ini_read_failed");at+=static_cast<std::size_t>(n);}return out;}
struct Files {
    struct Item{std::string name;Fd handle;dev_t device{};ino_t inode{};};
    Fd dir;fs::path path;std::vector<Item> items;bool temporary{};
    void capture(const fs::path& p,bool temp){dir=directory(p);path=p;temporary=temp;}
    void add(const std::string& name,const std::vector<std::uint8_t>& data){need(name.find_first_of("/\\")==name.npos,"door_filename_invalid");Fd f(openat(dir.get(),name.c_str(),O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600));need(bool(f),"door_drop_file_in_use_or_existing");struct stat s{};need(!fstat(f.get(),&s),"door_file_identity_failed");items.push_back({name,std::move(f),s.st_dev,s.st_ino});std::size_t at{};while(at<data.size()){const auto n=write(items.back().handle.get(),data.data()+at,data.size()-at);need(n>0,"door_drop_write_failed");at+=static_cast<std::size_t>(n);}need(fsync(items.back().handle.get())==0,"door_drop_flush_failed");}
    void add(const std::string& name,const std::string& data){add(name,std::vector<std::uint8_t>(data.begin(),data.end()));}
    bool remove()noexcept{bool ok=true;for(auto i=items.rbegin();i!=items.rend();++i){struct stat s{},held{};if(fstatat(dir.get(),i->name.c_str(),&s,AT_SYMLINK_NOFOLLOW)!=0){if(errno!=ENOENT||fstat(i->handle.get(),&held)||held.st_nlink!=0)ok=false;continue;}if(s.st_dev!=i->device||s.st_ino!=i->inode||!S_ISREG(s.st_mode)||unlinkat(dir.get(),i->name.c_str(),0))ok=false;}items.clear();if(temporary){struct stat held{},current{};if(fstat(dir.get(),&held)||lstat(path.c_str(),&current)||held.st_dev!=current.st_dev||held.st_ino!=current.st_ino)ok=false;else if(rmdir(path.c_str()))ok=false;}return ok;}
};
std::vector<std::string> arguments(std::string_view command){std::vector<std::string> out;std::string token;char quote{};bool active{};for(std::size_t n=0;n<command.size();++n){const char c=command[n];if(c=='\\'&&quote!='\''){need(++n<command.size(),"door_command_escape");token+=command[n];active=true;}else if((c=='"'||c=='\'')&&(!quote||quote==c)){quote=quote?0:c;active=true;}else if((c==' '||c=='\t')&&!quote){if(active){out.push_back(token);token.clear();active=false;}}else{token+=c;active=true;}}need(!quote,"door_command_quote");if(active)out.push_back(token);need(!out.empty()&&out.size()<=128&&!out[0].empty(),"door_command_empty_or_many_arguments");return out;}
Fd executable(fs::path& p,const fs::path& startup){if(p.is_relative())p=startup/p;p=fs::absolute(p);Fd f(open(p.c_str(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK));need(bool(f),"door_executable_unavailable");struct stat s{};std::array<char,4> magic{};need(!fstat(f.get(),&s)&&S_ISREG(s.st_mode)&&!(s.st_mode&0022)&&!(s.st_mode&(S_ISUID|S_ISGID))&&pread(f.get(),magic.data(),4,0)==4&&magic==std::array<char,4>{127,'E','L','F'},"door_native_elf_required");need(access(p.c_str(),X_OK)==0,"door_executable_permission_denied");const int high=fcntl(f.get(),F_DUPFD_CLOEXEC,10);need(high>=10,"door_executable_pin_failed");return Fd(high);}
std::string digest(int fd){need(sodium_init()>=0,"door_crypto_unavailable");crypto_hash_sha256_state state;crypto_hash_sha256_init(&state);std::array<unsigned char,16384> b{};off_t at{};for(;;){const auto n=pread(fd,b.data(),b.size(),at);need(n>=0,"door_runtime_hash_read_failed");if(!n)break;need(at<128*1024*1024,"door_runtime_oversized");crypto_hash_sha256_update(&state,b.data(),static_cast<unsigned long long>(n));at+=n;}std::array<unsigned char,32> hash{};crypto_hash_sha256_final(&state,hash.data());std::array<char,65> text{};sodium_bin2hex(text.data(),text.size(),hash.data(),hash.size());return text.data();}
struct Child {
    pid_t pid{};bool started{};int status{};
    bool exited(){siginfo_t info{};if(waitid(P_PID,static_cast<id_t>(pid),&info,WEXITED|WNOHANG|WNOWAIT)<0){need(errno==EINTR,"door_wait_failed");return false;}return info.si_pid==pid;}
    bool stop()noexcept{if(pid<=0)return true;kill(-pid,SIGTERM);const auto until=Clock::now()+std::chrono::milliseconds(250);while(Clock::now()<until){siginfo_t info{};if(!waitid(P_PID,static_cast<id_t>(pid),&info,WEXITED|WNOHANG|WNOWAIT)&&info.si_pid==pid)break;std::this_thread::sleep_for(std::chrono::milliseconds(2));}kill(-pid,SIGKILL);kill(pid,SIGKILL);pid_t result;do{result=waitpid(pid,&status,0);}while(result<0&&errno==EINTR);pid=0;return result>0;}
    ~Child(){stop();}
};
struct Pair{Fd parent,child;};
// Pipe EPIPE must affect this caller, never terminate the shared Runtime.
// Restore the thread's original mask without consuming a pre-existing signal.
struct PipeSignals {
    sigset_t original{},blocked{};bool pending{};
    PipeSignals(){sigemptyset(&blocked);sigaddset(&blocked,SIGPIPE);need(!pthread_sigmask(SIG_BLOCK,&blocked,&original),"door_signal_mask_failed");sigset_t before{};sigpending(&before);pending=sigismember(&before,SIGPIPE)==1;}
    ~PipeSignals(){if(!pending){timespec zero{};while(sigtimedwait(&blocked,nullptr,&zero)<0&&errno==EINTR){}}pthread_sigmask(SIG_SETMASK,&original,nullptr);}
};
Pair pipe_pair(bool input){int f[2]{};need(pipe2(f,O_CLOEXEC)==0,"door_pipe_failed");Pair p{Fd(f[input?1:0]),Fd(f[input?0:1])};nonblock(p.parent.get());return p;}
Pair socket_pair(){int f[2]{};need(socketpair(AF_UNIX,SOCK_STREAM|SOCK_CLOEXEC,0,f)==0,"door_socket_failed");Pair p{Fd(f[0]),Fd(f[1])};nonblock(p.parent.get());return p;}
Pair pty_pair(unsigned rows,unsigned columns){int parent{},child{};winsize size{};size.ws_row=static_cast<unsigned short>(rows);size.ws_col=static_cast<unsigned short>(columns);need(openpty(&parent,&child,nullptr,nullptr,&size)==0,"door_pty_failed");Pair p{Fd(parent),Fd(child)};termios mode{};need(tcgetattr(child,&mode)==0,"door_pty_attributes_failed");cfmakeraw(&mode);need(tcsetattr(child,TCSANOW,&mode)==0,"door_pty_raw_failed");fcntl(parent,F_SETFD,FD_CLOEXEC);fcntl(child,F_SETFD,FD_CLOEXEC);nonblock(parent);return p;}
void spawn(Child& child,Fd& program,Fd& cwd,std::vector<std::string>& args,std::vector<std::string>& environment,int input,int output,int error,int socket_fd){
    auto proof=pipe_pair(false);const auto high=[](int fd){const int n=fcntl(fd,F_DUPFD_CLOEXEC,10);need(n>=10,"door_descriptor_pin_failed");return Fd(n);};
    // Sources above the child ABI range cannot be clobbered by dup2(0..3).
    auto child_input=high(input),child_output=high(output),child_error=high(error),child_proof=high(proof.child.get());Fd child_socket;if(socket_fd>=0)child_socket=high(socket_fd);
    proof.child.reset();std::vector<char*> argv,env;for(auto& s:args)argv.push_back(s.data());argv.push_back(nullptr);for(auto& s:environment)env.push_back(s.data());env.push_back(nullptr);const auto parent=getpid();
    child.pid=fork();if(child.pid<0){child.pid=0;throw std::runtime_error("door_fork_failed");}if(child.pid==0){
        int failure{};if(setsid()<0||prctl(PR_SET_PDEATHSIG,SIGKILL)<0||getppid()!=parent||fchdir(cwd.get())<0)failure=errno?errno:EPERM;
        if(!failure&&(dup2(child_input.get(),0)<0||dup2(child_output.get(),1)<0||dup2(child_error.get(),2)<0||(socket_fd>=0&&dup2(child_socket.get(),3)<0)))failure=errno;
        // Enumerate the child's own descriptor table after fork. Raw getdents
        // needs no allocator/stdio locks and includes high-number descriptors.
        const int listing=open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
        if(listing<0)failure=errno;else{
            struct Entry{std::uint64_t inode;std::int64_t offset;unsigned short length;unsigned char type;char name[1];};
            alignas(Entry) char entries[32768];long count{};
            while((count=syscall(SYS_getdents64,listing,entries,sizeof(entries)))>0){
                for(long at=0;at<count;){const auto* e=reinterpret_cast<const Entry*>(entries+at);if(e->length<20||at+e->length>count){failure=EIO;break;}int fd{};const char* name=e->name;bool digits=*name>='0'&&*name<='9';while(*name>='0'&&*name<='9'){fd=fd*10+(*name++-'0');}if(digits&&!*name&&fd>=3&&fd!=listing&&fd!=program.get()&&fd!=child_proof.get()&&!(fd==3&&socket_fd>=0))close(fd);at+=e->length;}
                if(failure)break;
            }if(count<0)failure=errno;close(listing);
        }
        sigset_t unblocked{};sigemptyset(&unblocked);sigprocmask(SIG_SETMASK,&unblocked,nullptr);signal(SIGPIPE,SIG_DFL);
        if(!failure&&!drop_child_capabilities())failure=EPERM;
        if(!failure)fexecve(program.get(),argv.data(),env.data());
        if(!failure)failure=errno;
        const auto reported=write(child_proof.get(),&failure,sizeof(failure));(void)reported;_exit(126);
    }
    child_proof.reset();const auto until=Clock::now()+std::chrono::seconds(5);int error_code{};
    for(;;){const auto n=read(proof.parent.get(),&error_code,sizeof(error_code));if(n==0){child.started=true;return;}if(n>0)throw std::runtime_error("door_exec_failed_"+std::to_string(error_code));need(errno==EAGAIN||errno==EINTR,"door_exec_status_failed");need(Clock::now()<until,"door_exec_status_timeout");std::this_thread::sleep_for(std::chrono::milliseconds(2));}
}
struct Keys {bool discard{},escape{};Time since{};bool append(const std::vector<std::uint8_t>& b,std::string& out){for(auto c:b){if(discard){discard=false;if(c=='\n')continue;}if(escape){escape=false;if(c=='d'||c=='D')return false;out+='\x1d';if(c==0x1d)continue;}else if(c==0x1d){escape=true;since=Clock::now();continue;}out+=static_cast<char>(c);}return true;}void flush(std::string& out){if(escape&&Clock::now()-since>std::chrono::milliseconds(500)){out+='\x1d';escape=false;}}};
std::string config_path(const fs::path& path){const auto s=path.string();need(s.find_first_of(" \t\r\n\"\\")==s.npos,"door_dosemu_mapping_requires_simple_absolute_paths");return s;}
DoorOptions dos_runtime(const fs::path& root,DoorOptions options){
    need(options.dos_serial_input_interval_ms<=100,"door_dos_serial_input_interval_out_of_range");
    if(options.dosemu_root.empty()&&options.dosemu_binary.empty()){
        const auto saved=read_dos_runtime_profile(root);if(saved.revision){options.dosemu_root=saved.options.dosemu_root;options.dosemu_binary=saved.options.dosemu_binary;options.dosemu_sha256=saved.options.dosemu_sha256;}
    }
    if(options.dosemu_root.empty()&&options.dosemu_binary.empty()&&fs::is_directory(root/"bin"/"dosemu"))options.dosemu_root=root/"bin"/"dosemu";
    if(!options.dosemu_root.empty()){
        auto held=directory(options.dosemu_root);(void)held;
        if(options.dosemu_binary.empty())options.dosemu_binary=options.dosemu_root/"usr/libexec/dosemu2/dosemu2.bin";
        for(const auto* name:{"usr/lib/x86_64-linux-gnu","usr/lib/dosemu","usr/share/dosemu","usr/share/comcom32","usr/share/fdpp"}){
            auto part=directory(options.dosemu_root/name);(void)part;
        }
    }else if(options.dosemu_binary.empty())options.dosemu_binary="/usr/libexec/dosemu2/dosemu2.bin";
    need(fs::is_regular_file(options.dosemu_binary),"door_dosemu2_missing_prepare_Bin_Dosemu_or_select_runtime");
    return options;
}
app::DoorProgramResult launch(const fs::path& startup,const fs::path& node,const fs::path& drop,const app::DoorDefinition& door,app::DoorDropContext context,const app::DoorChannel& channel,const DoorOptions& options,const app::DoorDriverSettings& settings,Files& files,bool cleanup,std::vector<std::uint8_t> prefix,bool discard,bool& started){
    app::DoorProgramResult result;Child child;Files emulator_files;std::string child_errors;
    try{
        PipeSignals signals;Pair input=pipe_pair(true),output=pipe_pair(false),error=pipe_pair(false),duplex;
        if(auto end=channel.stop()){result.end=*end;return result;}
        const bool dos=door.program.type==app::DoorProgramType::dos,socket_io=!dos&&door.program.io==app::DoorIo::socket,pty=!dos&&!socket_io&&options.pseudo_terminal;
        if(socket_io)duplex=socket_pair();else if(pty)duplex=pty_pair(context.rows,context.columns);
        context.node_path=node.string();context.drop_path=drop.string();context.startup_path=startup.string();context.dos=dos;context.io=door.program.io;context.socket=3;
        context.path_style=dos?app::DoorDropPathStyle::windows:app::DoorDropPathStyle::posix;
        if(dos){context.node_path="E:\\";context.drop_path="F:\\";context.startup_path="D:\\";}
        const auto command=app::expand_door_command(cleanup?door.program.cleanup_command:door.program.command_line,{context.node_path,context.drop_path,context.startup_path,app::effective_door_code(door),context.node});need(command.has_value(),"door_command_substitution");
        std::vector<std::string> args;fs::path program_path;
        if(dos){
            // The pinned PTY serial backend cannot relay DTR changes through
            // Linux TIOCM ioctls. Do not silently promise CanDisconnect=true.
            need(!settings.can_disconnect,"door_dosemu2_dtr_unavailable_set_CanDisconnect_false");
            const auto private_path=node/("dosemu-"+random_id());private_directory(private_path);emulator_files.capture(private_path,true);
            const auto serial=door.program.io!=app::DoorIo::standard;
            std::string conf="$_hdimage = \""+config_path(private_path)+" "+config_path(startup)+" "+config_path(node)+" "+config_path(drop)+" +1\"\n$_sound = (off)\n$_mouse_internal = (off)\n$_hogthreshold = (1)\n$_cpu_vm = \"emulated\"\n$_cpu_vm_dpmi = \"emulated\"\n$_layout = \"us\"\n$_external_char_set = \"cp437\"\n$_internal_char_set = \"cp437\"\n";
            if(serial){duplex=pty_pair(context.rows,context.columns);char name[128]{};need(ttyname_r(duplex.child.get(),name,sizeof(name))==0,"door_dosemu_serial_pty_unavailable");conf+="$_com1 = \""+std::string(name)+" pseudo\"\n";}
            need(door.program.command_shell||command->find_first_of("&|<>^\r\n")==command->npos,"door_dos_shell_metacharacters_require_shell");
            std::string batch="@echo off\r\n";if(door.program.io==app::DoorIo::fossil_only||door.program.io==app::DoorIo::fossil_or_uart)batch+="fossil\r\nif errorlevel 2 goto done\r\n";
            batch+="D:\r\ncd \\\r\n"+*command+"\r\n:done\r\n";
            emulator_files.add("sfxrun.bat",batch);emulator_files.add("dosemu.conf",conf);
            program_path=options.dosemu_binary;args={program_path.string(),"-n","-f",(private_path/"dosemu.conf").string(),"--Flocal_dir",private_path.string(),serial?"-tde2":"-tde","-ks","-q","-o","/dev/null","-E","C:\\sfxrun.bat"};
            if(!options.dosemu_root.empty())for(const auto& option:std::vector<std::pair<std::string,std::string>>{
                {"--Flibdir","usr/share/dosemu"},{"--Fcmddir","usr/share/dosemu/commands"},{"--Fplugindir","usr/lib/dosemu"}}){
                args.push_back(option.first);args.push_back((options.dosemu_root/option.second).string());
            }
        }else if(door.program.command_shell){program_path=fs::canonical("/bin/sh");args={program_path.string(),"-c",*command};}
        else{args=arguments(*command);program_path=args[0];}
        auto program=executable(program_path,startup);if(dos&&!options.dosemu_sha256.empty())need(digest(program.get())==options.dosemu_sha256,"door_dosemu2_runtime_identity_mismatch");args[0]=program_path.string();auto cwd=directory(startup);
        std::vector<std::string> environment{"PATH=/usr/bin:/bin","LANG=C","TERM=ansi","HOME="+node.string()};
        if(dos&&!options.dosemu_root.empty()){
            const auto& base=options.dosemu_root;
            environment.push_back("LD_LIBRARY_PATH="+(base/"usr/lib/x86_64-linux-gnu").string()+":"+(base/"usr/lib/dosemu").string());
            environment.push_back("DOSEMU2_COMCOM_DIR="+(base/"usr/share/comcom32").string());
            environment.push_back("FDPP_KERNEL_DIR="+(base/"usr/share/fdpp").string());
        }
        if(!cleanup){const auto generated=app::make_door_drop_files(door.program.drop_file,context);need(generated.valid(),generated.diagnostic.c_str());for(const auto& file:generated.files)files.add(file.name,file.bytes);for(const auto& pair:generated.environment)environment.push_back(pair.first+"="+pair.second);}
        else if(door.program.drop_file==app::DoorDropFile::bbsdev_drp)environment.push_back("BBSDEV_DRP="+(dos?std::string("F:\\BBSDEV.DRP"):(drop/"BBSDEV.DRP").string()));
        const bool serial=dos&&door.program.io!=app::DoorIo::standard;const int in=pty?duplex.child.get():input.child.get(),out=pty?duplex.child.get():output.child.get();
        spawn(child,program,cwd,args,environment,in,out,error.child.get(),socket_io?duplex.child.get():-1);started=true;input.child.reset();output.child.reset();error.child.reset();if(!serial)duplex.child.reset();
        const int reader=(socket_io||pty||serial)?duplex.parent.get():output.parent.get(),writer=(socket_io||pty||serial)?duplex.parent.get():input.parent.get();
        std::string to_game,to_caller;Keys keys{discard};if(!keys.append(prefix,to_game)){result.end=app::DoorEnd::escaped;}else result.end=app::DoorEnd::returned;
        bool closed{},exited{};Time exit_at{},blocked=Clock::now(),next_serial_byte{};const auto cleanup_limit=Clock::now()+std::chrono::seconds(30);
        for(;;){
            if(result.end==app::DoorEnd::escaped)break;
            if(auto end=channel.stop()){result.end=*end;break;}if(cleanup&&Clock::now()>=cleanup_limit){result.end=app::DoorEnd::launch_failed;result.diagnostic="door_cleanup_timeout";break;}
            bool progress{};std::array<char,4096> b{};const auto diagnostic_bytes=read(error.parent.get(),b.data(),b.size());
            if(diagnostic_bytes>0&&child_errors.size()<4096)child_errors.append(b.data(),std::min<std::size_t>(static_cast<std::size_t>(diagnostic_bytes),4096-child_errors.size()));
            if(!closed&&to_caller.size()<65536){const auto n=read(reader,b.data(),std::min(b.size(),65536-to_caller.size()));if(n>0){to_caller.append(b.data(),static_cast<std::size_t>(n));progress=true;}else if(n==0||(n<0&&errno==EIO&&(pty||serial)))closed=true;else if(n<0&&errno!=EAGAIN&&errno!=EINTR)throw std::runtime_error("door_child_read_failed");}
            if(!cleanup&&!to_caller.empty()){const auto written=channel.write(to_caller);need(app::valid_interactive_stream_write_result(to_caller.size(),written),"door_channel_write_invalid");if(written.outcome!=app::InteractiveStreamOutcome::completed&&written.outcome!=app::InteractiveStreamOutcome::timed_out){result.end=app::DoorEnd::caller_disconnected;break;}to_caller.erase(0,written.bytes_written);progress|=written.bytes_written!=0;}else if(cleanup)to_caller.clear();
            if(!exited&&child.exited()){exited=true;exit_at=Clock::now();}if(progress&&exited)exit_at=Clock::now();if(exited&&to_caller.empty()&&(closed||Clock::now()-exit_at>std::chrono::seconds(1)))break;
            if(!exited&&!cleanup&&to_game.size()<65536){const auto read=channel.read(std::min<std::size_t>(4096,65536-to_game.size()));need(app::valid_interactive_stream_read_result(std::min<std::size_t>(4096,65536-to_game.size()),read),"door_channel_read_invalid");if(read.outcome!=app::InteractiveStreamOutcome::completed&&read.outcome!=app::InteractiveStreamOutcome::timed_out){result.end=app::DoorEnd::caller_disconnected;break;}if(!read.bytes.empty()){if(channel.keyboard)channel.keyboard();progress=true;if(!keys.append(read.bytes,to_game)){result.end=app::DoorEnd::escaped;break;}}}
            // Some legacy DOS readers lose closely spaced characters. Optional
            // serial pacing keeps caller bursts queued without sleeping through
            // stop/deadline checks or delaying output/native/standard I/O.
            const bool paced=serial&&options.dos_serial_input_interval_ms!=0;
            keys.flush(to_game);if(!exited&&!to_game.empty()&&(!paced||Clock::now()>=next_serial_byte)){
                const auto count=paced?std::size_t{1}:to_game.size();const auto n=socket_io?send(writer,to_game.data(),count,MSG_NOSIGNAL):write(writer,to_game.data(),count);
                if(n>0){to_game.erase(0,static_cast<std::size_t>(n));progress=true;if(paced)next_serial_byte=Clock::now()+std::chrono::milliseconds(options.dos_serial_input_interval_ms);}
                else if(n<0&&errno!=EAGAIN&&errno!=EINTR){result.end=closed&&settings.can_disconnect?app::DoorEnd::game_disconnect:app::DoorEnd::returned;break;}
            }
            if(closed&&!exited&&socket_io){result.end=settings.can_disconnect?app::DoorEnd::game_disconnect:app::DoorEnd::returned;break;}
            if(progress)blocked=Clock::now();
            if((!to_game.empty()||!to_caller.empty())&&Clock::now()-blocked>std::chrono::seconds(30)){result.end=app::DoorEnd::backpressure_timeout;break;}
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if(result.end==app::DoorEnd::caller_disconnected){input.parent.reset();duplex.parent.reset();const auto grace=Clock::now()+std::chrono::seconds(5);while(!child.exited()&&Clock::now()<grace){if(auto end=channel.stop())if(*end==app::DoorEnd::shutdown||*end==app::DoorEnd::operator_terminated)break;std::this_thread::sleep_for(std::chrono::milliseconds(5));}}
        const bool natural_exit=child.exited();result.cleanup_complete=child.stop();if(WIFEXITED(child.status))result.exit_code=static_cast<std::uint32_t>(WEXITSTATUS(child.status));else result.exit_code=128+(WIFSIGNALED(child.status)?static_cast<std::uint32_t>(WTERMSIG(child.status)):0);if(natural_exit&&result.end==app::DoorEnd::returned&&result.exit_code)result.end=app::DoorEnd::launch_failed;
    }catch(const std::exception& e){result.diagnostic=e.what();result.end=app::DoorEnd::launch_failed;result.cleanup_complete=child.stop();}
    if(result.end==app::DoorEnd::launch_failed&&!child_errors.empty()){
        if(result.diagnostic.empty())result.diagnostic="door_child_exit_"+std::to_string(result.exit_code);
        result.diagnostic+="; ";for(const auto c:child_errors)result.diagnostic+=static_cast<unsigned char>(c)>=32&&c!=127?c:' ';
    }
    result.cleanup_complete=emulator_files.remove()&&result.cleanup_complete;return result;
}
}
void ensure_door_nodes(const fs::path& root,unsigned count){need(count>=1&&count<=128,"door_node_count_invalid");private_directory(root/"node");for(unsigned n=1;n<=count;++n)private_directory(root/"node"/std::to_string(n));}
application::DoorProgramResult run_local_door(const fs::path& root,const app::DoorDefinition& door,app::DoorDropContext context,const app::DoorChannel& channel,const DoorOptions& options,std::vector<std::uint8_t> prefix,bool discard){
    app::DoorProgramResult result;Files files;
    try{
        need(app::valid_door_definition(door)&&door.kind==app::DoorExecutionKind::local&&context.node>=1&&context.node<=128&&prefix.size()<=4096&&channel.read&&channel.write&&channel.stop,"door_launch_context");if(auto end=channel.stop()){result.end=*end;return result;}
        if(prefix.size()<4096){const auto ready=channel.read(4096-prefix.size());if(ready.outcome!=app::InteractiveStreamOutcome::completed&&ready.outcome!=app::InteractiveStreamOutcome::timed_out){result.end=app::DoorEnd::caller_disconnected;return result;}need(app::valid_interactive_stream_read_result(4096-prefix.size(),ready),"door_channel_read_invalid");prefix.insert(prefix.end(),ready.bytes.begin(),ready.bytes.end());}
        const auto node=root/"node"/std::to_string(context.node);auto startup=door.program.startup_directory.empty()?root:fs::path(door.program.startup_directory);if(startup.is_relative())startup=root/startup;auto held=directory(startup);auto node_pin=directory(node);
        std::string error;const auto settings=app::parse_door_driver_settings(file_text(root/"bin"/"sfxexec.ini"),file_text(startup/"sfxexec.ini"),&error);need(settings.has_value(),error.c_str());
        auto drop=door.program.drop_location==app::DoorDropLocation::startup?startup:node;const bool temporary=door.program.drop_location==app::DoorDropLocation::temporary;if(temporary){drop=node/("door-"+random_id());private_directory(drop);}files.capture(drop,temporary);files.add("sfxdoor.lck","SFX private launch\n");
        auto selected=door.program.type==app::DoorProgramType::dos?dos_runtime(root,options):options;
        if(door.program.type==app::DoorProgramType::dos&&settings->dos_input_interval_ms)selected.dos_serial_input_interval_ms=*settings->dos_input_interval_ms;
        bool started{};result=launch(startup,node,drop,door,context,channel,selected,*settings,files,false,std::move(prefix),discard,started);
        if(started&&result.cleanup_complete&&!door.program.cleanup_command.empty()){
            app::DoorChannel silent;silent.read=[](std::size_t){return app::InteractiveStreamReadResult{app::InteractiveStreamOutcome::timed_out,{}};};silent.write=[](std::string_view s){return app::InteractiveStreamWriteResult{app::InteractiveStreamOutcome::completed,s.size()};};silent.stop=[&]()->std::optional<app::DoorEnd>{if(const auto end=channel.stop())if(*end==app::DoorEnd::shutdown||*end==app::DoorEnd::operator_terminated||*end==app::DoorEnd::calling_time||*end==app::DoorEnd::inactivity)return end;return {};};
            bool clean_started{};auto clean=launch(startup,node,drop,door,context,silent,selected,*settings,files,true,{},false,clean_started);result.cleanup_complete=clean.cleanup_complete;result.cleanup_command_failed=clean.end!=app::DoorEnd::returned;
        }
    }catch(const std::exception& e){result.diagnostic=e.what();result.end=result.diagnostic=="door_drop_file_in_use_or_existing"?app::DoorEnd::busy:app::DoorEnd::launch_failed;}
    result.cleanup_complete=files.remove()&&result.cleanup_complete;return result;
}
#include "dos_setup.inc"
} // namespace sfx::platform::linux_host
