#include "sfx/platform/linux/file_payload_store.hpp"
#include <sodium.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <iostream>
#include <map>
#include <stdexcept>

namespace fs=std::filesystem;
namespace host=sfx::platform::linux_host;
namespace app=sfx::application;
namespace {
unsigned checks{};
void check(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
template<class F> void refused(F f,const char* why){bool caught=false;try{f();}catch(const std::runtime_error&){caught=true;}check(caught,why);}
void directory(const fs::path& path){check(fs::create_directory(path)&&chmod(path.c_str(),0700)==0,"private fixture directory");}
void append(host::FilePayloadLease& file,std::string_view text){file.append(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());}
std::string contents(const fs::path& path){const auto d=host::open_owned_directory(path.parent_path());const auto bytes=host::read_private_file(d.get(),path.filename().string(),65536);return {bytes.begin(),bytes.end()};}
void put(const fs::path& path,std::string_view text){const auto d=host::open_owned_directory(path.parent_path());host::publish_private_file(d.get(),path.filename().string(),{text.begin(),text.end()},false);}
// Deliberately test-only namespace provider. Real Linux copied-board snapshot
// preparation is a separate adapter; this verifies that payload operations
// never replace its portable IDs with native dev/inode values.
struct Bindings final:host::IFilePayloadBindings {
    std::map<std::pair<dev_t,ino_t>,app::FileObjectIdentity> objects;unsigned next{},moves{},removals{};bool fail_new{};
    static std::pair<dev_t,ino_t> key(int fd){struct stat s{};if(fstat(fd,&s))throw std::runtime_error("fixture stat");return {s.st_dev,s.st_ino};}
    app::FileObjectIdentity existing(const fs::path&,int fd)override{
        const auto it=objects.find(key(fd));if(it==objects.end())throw std::runtime_error("fixture unbound object");return it->second;
    }
    app::FileObjectIdentity created(const fs::path&,int fd)override{
        if(fail_new)throw std::runtime_error("fixture binding write failure");
        const app::FileObjectIdentity id{"portable-board",std::to_string(++next)};objects.emplace(key(fd),id);return id;
    }
    void moved(const fs::path&,const fs::path&,int fd,const app::FileObjectIdentity& id)override{
        check(existing({},fd).file_id==id.file_id,"rename keeps portable identity");++moves;
    }
    void removed(const fs::path&,const app::FileObjectIdentity&)override{++removals;}
    void bind(const fs::path& path){host::FileDescriptor fd(open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW));check(bool(fd),"fixture bind source");created(path,fd.get());}
};
void run(const fs::path& root){
    check(geteuid()!=0&&!fs::exists(root)&&root.is_absolute(),"normal user and isolated fixture");
    directory(root);for(const auto* name:{"files","work","config","Import"})directory(root/name);
    directory(root/"files/One");directory(root/"files/Two");
    auto bindings=std::make_shared<Bindings>();
    refused([&]{host::FilePayloadDirectory::private_store(root/"work",{});},"binding provider mandatory");
    auto spool=host::FilePayloadDirectory::private_store(root/"work",bindings);
    auto area=host::FilePayloadDirectory::area(root,root/"files/One",{root/"files/Two"},{root/"config"},bindings);
    auto other=host::FilePayloadDirectory::area(root,root/"files/Two",{root/"files/One"},{root/"config"},bindings);
    refused([&]{host::FilePayloadDirectory::area(root,root/"config",{}, {},bindings);},"configuration cannot become file area");
    refused([&]{host::FilePayloadDirectory::area(root,root/"files/One",{root/"files"},{},bindings);},"area ancestor overlap refused");
    refused([&]{host::FilePayloadDirectory::area(root,root/"files/One",{}, {root/"files/One"},bindings);},"protected overlap refused");
    refused([&]{host::FilePayloadDirectory::private_store(root/"work/../config",bindings);},"dot alias refused");
    refused([&]{spool.create("../escape",10);},"caller traversal refused");
    refused([&]{spool.create("zero",0);},"unbounded receive refused");
    app::FilePayloadIdentity identity;
    {
        auto file=area.create("hello.txt",64);check(file.byte_count()==0,"new file is empty");
        refused([&]{area.create("hello.txt",64);},"create never overwrites");
        refused([&]{area.read("hello.txt",true);},"cooperating writer lock excludes mutation");
        append(file,"Hello native BBS\r\n");file.flush();identity=file.identity();
        check(identity.bytes==18&&identity.volume=="portable-board","portable ID and exact byte count");
        check(file.native_object_identity().volume!=identity.volume,"native and durable IDs distinct");
        check(file.read(6,6)==std::vector<std::uint8_t>({'n','a','t','i','v','e'}),"bounded positional read");
        refused([&]{file.read(19);},"read beyond EOF refused");
        refused([&]{append(file,"x");},"hash seals writes");
        auto occupied=other.create("occupied.txt",1);append(occupied,"x");occupied.identity();
        refused([&]{file.rename_to(other,"occupied.txt");},"rename never replaces destination");
        check(contents(root/"files/Two/occupied.txt")=="x","collision bytes preserved");
        file.rename_to(other,"moved.txt");check(file.object_identity().file_id==identity.file_id&&area.absent("hello.txt"),"rename retains ID and removes old name");
    }
    check(bindings->moves==1,"only completed rename recorded");
    {auto file=other.read("moved.txt");check(file.identity().sha256==identity.sha256,"reopen exact digest");
        refused([&]{file.remove_exact();},"read capability cannot delete");
        refused([&]{file.identity([](auto,auto){return false;});},"hash cancellation honored");}
    check(other.names()==std::vector<std::string>({"moved.txt","occupied.txt"}),"sorted bounded regular names");
    refused([&]{other.names(1);},"listing bound fails without silent truncation");
    {auto cursor=other.enumerate();refused([&]{cursor.next(1,[]{return false;});},"listing cancellation honored");}
    {auto file=other.read("moved.txt",true);file.remove_exact();check(other.absent("moved.txt")&&bindings->removals==1,"exact removal confirmed and recorded");refused([&]{file.read(0);},"removed lease closed");}

    std::uint64_t ceiling=host::kFilePayloadFreeReserve+100;
    auto limited=host::FilePayloadDirectory::private_store(root/"work",bindings,[&]{return ceiling;});
    {auto first=limited.create("budget-one",80);refused([&]{limited.create("budget-two",21);},"concurrent lease reservations counted");
        check(limited.absent("budget-two"),"budget refusal creates no file");
        ceiling=host::kFilePayloadFreeReserve;refused([&]{append(first,"x");},"real write budget rechecked");
        check(first.byte_count()==0,"budget failure preserves empty data");ceiling=host::kFilePayloadFreeReserve+100;}
    {auto next=limited.create("budget-two",100);check(next.byte_count()==0,"retired lease releases reservation");refused([&]{append(next,std::string(101,'x'));},"receive byte cap enforced");}
    bindings->fail_new=true;refused([&]{spool.create("binding-failed",10);},"binding failure refuses adoption");bindings->fail_new=false;
    check(fs::exists(root/"work/binding-failed")&&fs::file_size(root/"work/binding-failed")==0,"failed created object preserved for recovery");

    app::FilePayloadIdentity prefix;
    {auto file=spool.create("partial",32);append(file,"good");prefix=file.identity();}
    {host::FileDescriptor fd(open((root/"work/partial").c_str(),O_WRONLY|O_APPEND|O_CLOEXEC));check(fd&&write(fd.get(),"tail",4)==4,"fixture interrupted uncommitted suffix");}
    auto bad=prefix;bad.sha256[0]^=1;
    refused([&]{spool.resume_private("partial",32,bad);},"changed durable prefix refused");
    check(contents(root/"work/partial")=="goodtail","failed resume does not truncate");
    refused([&]{spool.resume_private("partial",32,prefix,[](auto,auto){return false;});},"resume cancellation before truncation");
    check(contents(root/"work/partial")=="goodtail","cancelled suffix preserved");
    {auto file=spool.resume_private("partial",32,prefix);check(file.byte_count()==4,"only uncommitted suffix trimmed");append(file," done");check(file.identity().bytes==9,"verified resume appends");}
    refused([&]{area.resume_private("partial",32,prefix);},"area cannot claim private receipt recovery");

    app::FileObjectIdentity copy_id;
    {auto file=spool.create("copy",32);append(file,"good");copy_id=file.object_identity();file.flush();}
    {auto source=spool.read("partial");auto copy=spool.resume_owned_copy("copy",copy_id,source);append(copy," done");check(copy.identity().sha256==source.identity().sha256,"owned copy resumes matching prefix");}
    app::FilePayloadIdentity generated;generated.bytes=6;crypto_hash_sha256(generated.sha256.data(),reinterpret_cast<const unsigned char*>("secret"),6);
    {auto file=spool.create("metadata",6);append(file,"sec");const auto id=file.object_identity();generated.volume=id.volume;generated.file_id=id.file_id;file.flush();}
    refused([&]{spool.resume_owned_bytes("metadata",generated,"changed");},"regenerated bytes require original intent");
    check(contents(root/"work/metadata")=="sec","failed generated recovery preserves prefix");
    {auto file=spool.resume_owned_bytes("metadata",generated,"secret");append(file,"ret");check(file.identity().sha256==generated.sha256,"bounded generated recovery appends exact bytes");}

    put(root/"Import/source","import");bindings->bind(root/"Import/source");
    auto input=host::FilePayloadDirectory::import_source(root/"Import",{root/"config"},bindings);
    refused([&]{input.create("output",10);},"import capability has no output");
    refused([&]{input.read("source",true);},"import deletion needs explicit move");
    {auto source=input.read("source");check(source.byte_count()==6,"explicit input is readable");}
    auto moving=host::FilePayloadDirectory::import_source(root/"Import",{},bindings,true);
    {auto source=moving.read("source",true);refused([&]{source.rename_to(area,"imported");},"external move requires protected copy");source.remove_exact();}

    check(mkfifo((root/"work/fifo").c_str(),0600)==0,"fixture FIFO created");
    refused([&]{spool.read("fifo");},"FIFO refused without blocking");
    fs::create_symlink(root/"work/partial",root/"work/link");refused([&]{spool.read("link");},"symlink refused");
    fs::create_hard_link(root/"work/partial",root/"work/hard");refused([&]{spool.read("partial");},"hard link refused");fs::remove(root/"work/hard");
    check(chmod((root/"work/partial").c_str(),0644)==0,"fixture shared file");refused([&]{spool.read("partial");},"shared private payload refused");chmod((root/"work/partial").c_str(),0600);
    {auto held=spool.read("partial",true);fs::rename(root/"work/partial",root/"work/original");put(root/"work/partial","replacement");
        refused([&]{held.remove_exact();},"renamed replacement cannot be deleted through old lease");check(contents(root/"work/partial")=="replacement","replacement preserved");}
    {auto held=spool.read("original");host::FileDescriptor fd(open((root/"work/original").c_str(),O_WRONLY|O_CLOEXEC));check(fd&&pwrite(fd.get(),"X",1,0)==1,"uncooperative same-user change fixture");refused([&]{held.identity();},"same-length external mutation detected");}
    for(const bool rename:{false,true}){
        const std::string name=rename?"rename-race":"delete-race";
        {auto file=spool.create(name,8);append(file,"original");file.flush();}
        const auto old_removals=bindings->removals,old_moves=bindings->moves;
        auto racing=host::FilePayloadDirectory::private_store(root/"work",bindings,{},[&]{
            fs::rename(root/"work"/name,root/"work"/(name+"-saved"));put(root/"work"/name,"replacement");
        });
        {auto held=racing.read(name,true);
            if(rename)refused([&]{held.rename_to(other,"race-output");},"source substitution during rename refused");
            else refused([&]{held.remove_exact();},"source substitution at delete boundary refused");}
        check(contents(root/"work"/name)=="replacement"&&contents(root/"work"/(name+"-saved"))=="original","both substituted and original bytes preserved");
        check(bindings->removals==old_removals&&bindings->moves==old_moves,"substitution never reports completed logical mutation");
        check(other.absent("race-output"),"substituted source never published to destination");
    }
    {
        constexpr std::uint64_t cap=4ULL*1024*1024;
        auto stage=spool.create("child-transfer",cap);const auto original=stage.object_identity();
        auto output=stage.begin_receive_transfer();struct stat allocated{};
        check(fstat(output.get(),&allocated)==0&&allocated.st_size==0&&allocated.st_blocks>=static_cast<blkcnt_t>(cap/512),"child budget physically allocated without changing EOF");
        check((fcntl(output.get(),F_GETFL)&O_ACCMODE)==O_WRONLY,"child receives write-only description");
        char byte{};check(pread(output.get(),&byte,1,0)<0&&errno==EBADF,"receive descriptor cannot read");
        refused([&]{stage.begin_receive_transfer();},"second child capability refused");
        refused([&]{append(stage,"x");},"host append excluded during child ownership");
        refused([&]{stage.identity();},"active child cannot publish a sealed identity");
        refused([&]{stage.remove_exact();},"active child cannot be removed");
        const std::string data(8193,'\xA5');const auto child=fork();check(child>=0,"spawn isolated file-writer fixture");
        if(child==0){const auto n=write(output.get(),data.data(),data.size());_exit(n==static_cast<ssize_t>(data.size())&&fsync(output.get())==0?0:1);}
        output=host::FileDescriptor();int result{};check(waitpid(child,&result,0)==child&&WIFEXITED(result)&&WEXITSTATUS(result)==0,"actual child reaped before custody return");
        stage.finish_receive_transfer_after_reap();const auto received=stage.identity();
        check(received.bytes==data.size()&&received.file_id==original.file_id&&contents(stage.path())==data,"partial child receive preserves exact bytes and journal identity");
        struct stat released_bytes{};check(stat(stage.path().c_str(),&released_bytes)==0&&released_bytes.st_blocks>=0&&static_cast<std::uint64_t>(released_bytes.st_blocks)*512<cap/2,"unused physical allocation released");
        refused([&]{append(stage,"x");},"completed child capability remains sealed");
        auto input=stage.send_transfer_descriptor();check((fcntl(input.get(),F_GETFL)&O_ACCMODE)==O_RDONLY,"send receives read-only description");
        check(write(input.get(),"x",1)<0&&errno==EBADF,"send descriptor cannot modify payload");
        check(pread(input.get(),&byte,1,0)==1&&static_cast<unsigned char>(byte)==0xA5,"send capability reads actual content");
        refused([&]{stage.finish_receive_transfer_after_reap();},"duplicate child completion refused");
    }
    {
        auto empty=spool.create("child-cancelled",1048576);auto fd=empty.begin_receive_transfer();fd=host::FileDescriptor();
        empty.finish_receive_transfer_after_reap();check(empty.identity().bytes==0,"cancelled zero-byte child stage retained and sealed");
        struct stat released{};check(stat(empty.path().c_str(),&released)==0&&released.st_blocks==0,"cancelled allocation released");
    }
    {
        std::uint64_t available=host::kFilePayloadFreeReserve+1048576;
        auto bounded=host::FilePayloadDirectory::private_store(root/"work",bindings,[&]{return available;});
        auto empty=bounded.create("child-budget-failed",1048576);available=host::kFilePayloadFreeReserve-1;
        refused([&]{empty.begin_receive_transfer();},"post-allocation free reserve rechecked");
        check(empty.identity().bytes==0,"failed preallocation admission retains sealed empty stage");
        struct stat released{};check(stat(empty.path().c_str(),&released)==0&&released.st_blocks==0,"failed admission releases only newly allocated extent");
    }
    {
        auto changed=spool.create("child-replaced",1048576);auto fd=changed.begin_receive_transfer();
        check(write(fd.get(),"original",8)==8,"child replacement fixture writes original");fd=host::FileDescriptor();
        fs::rename(changed.path(),root/"work/child-replaced-original");put(changed.path(),"replacement");
        refused([&]{changed.finish_receive_transfer_after_reap();},"post-child name replacement cannot be adopted");
        check(contents(changed.path())=="replacement"&&contents(root/"work/child-replaced-original")=="original","post-child replacement and original retained");
    }
    fs::rename(root/"work",root/"OldWork");directory(root/"work");
    refused([&]{spool.create("replacement-dir",1);},"directory replacement cannot redirect existing capability");
    check(!fs::exists(root/"work/replacement-dir"),"replacement directory unchanged");
}
}
int main(int argc,char** argv){try{check(argc==2,"fixture path required");run(fs::path(argv[1]));std::cout<<"PASS "<<checks<<" Linux file payload custody checks\n";return 0;}catch(const std::exception& e){std::cerr<<"FAIL after "<<checks<<": "<<e.what()<<'\n';return 1;}}
