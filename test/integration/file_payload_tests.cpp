#include "sfx/platform/win32/file_payload_store.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace native=sfx::platform::win32;
namespace ins=native::installation;
namespace {
unsigned checks{};
void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
template<class F>void refused(F action,const char* message){bool threw{};try{action();}catch(const std::exception&){threw=true;}check(threw,message);}
}
int main(int argc,char** argv){try{
    check(argc==2,"new isolated test root required");const auto root=std::filesystem::absolute(argv[1]);
    check(!std::filesystem::exists(root),"never reuse/delete prior fixture");
    std::filesystem::create_directories(root.parent_path());const auto owner=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(owner);
    for(const auto& path:{root,root/L"Install",root/L"Install"/L"Files",root/L"Install"/L"Files"/L"General",root/L"Install"/L"Files"/L"Uploads",root/L"Install"/L"Data",root/L"External",root/L"Backup"})ins::create_directory(path,descriptor.get());
    const auto install=root/L"Install";
    auto general=native::FilePayloadDirectory::area(install,L"Files/General",{L"Files/Uploads",root/L"External"},{root/L"Backup"},owner);
    auto external=native::FilePayloadDirectory::area(install,root/L"External",{L"Files/General",L"Files/Uploads"},{root/L"Backup"},owner);
    check(general.names().empty()&&external.names().empty(),"empty safely held native area directories");
    for(const auto& bad:{install,root,install/L"Data",install/L"Files",install/L"Files"/L"General"/L".."/L"Uploads",root/L"Backup"})
        refused([&]{native::FilePayloadDirectory::area(install,bad,{L"Files/General"},{root/L"Backup"},owner);},"unsafe/overlapping/protected area refused");
    refused([&]{native::FilePayloadDirectory::area(install,L"Files/General",{L"Files/General"},{},owner);},"same directory cannot be shared by areas");
    for(const auto& bad:{"../test","NUL.txt","COM1 .zip","file:stream","x.","\xff"})refused([&]{general.create(bad,10);},"native filename rejects path/device/UTF8 aliases");
    sfx::application::FilePayloadIdentity original;
    {
        auto file=general.create("safe.txt",4);const unsigned char bytes[]{1,2,3,4};file.append(bytes,4);
        refused([&]{file.append(bytes,1);},"actual received bytes capped before write");
        original=file.identity();check(original.bytes==4&&!original.volume.empty()&&!original.file_id.empty(),"native stable identity and real byte size");
        check(file.read(0)==std::vector<std::uint8_t>({1,2,3,4}),"locked streaming read exact");
        refused([&]{file.append(bytes,1);},"sealed content cannot mutate after hash");
        refused([&]{general.create("safe.txt",10);},"CREATE_NEW cannot overwrite existing payload");
        auto writer=CreateFileW(file.path().c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
        check(writer==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"lease prevents concurrent writes");if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);
        check(!DeleteFileW(file.path().c_str())&&GetLastError()==ERROR_SHARING_VIOLATION,"lease prevents pathname deletion");
    }
    check(general.names()==std::vector<std::string>({"safe.txt"}),"plain filenames only, no paths");
    {
        auto reader=general.read("safe.txt");check(reader.identity().sha256==original.sha256,"reopened content digest stable");
        refused([&]{general.read("safe.txt",true);},"active reader blocks rename/removal lease");
        refused([&]{reader.remove_exact();},"read lease cannot delete");
        refused([&]{reader.rename_to(external,"safe.txt");},"read lease cannot rename");
    }
    {
        auto collision=external.create("taken.txt",10);collision.identity();
    }
    {
        auto file=general.read("safe.txt",true);
        refused([&]{file.rename_to(external,"taken.txt");},"rename never overwrites unrelated destination");
        check(file.identity().sha256==original.sha256,"failed rename retains exact source");
        file.rename_to(external,"moved.txt");const auto moved=file.identity();
        check(moved.sha256==original.sha256&&moved.file_id==original.file_id&&moved.volume==original.volume,"same-volume handle rename retains native identity");
    }
    check(general.names().empty()&&external.names().size()==2,"source/destination membership after exact move");
    {
        const auto existing=root/L"External"/L"moved.txt",alias=root/L"External"/L"alias.txt";
        check(CreateHardLinkW(alias.c_str(),existing.c_str(),nullptr)!=FALSE,"owned synthetic hardlink created for refusal test");
        refused([&]{external.read("moved.txt");},"multiple native links refused without changing either");
        check(std::filesystem::exists(existing)&&std::filesystem::exists(alias),"hardlink refusal preserves both paths");
    }
    {
        auto created=general.create("exact-delete.txt",10);const unsigned char byte[]{7};created.append(byte,1);created.identity();created.remove_exact();
        check(!std::filesystem::exists(root/L"Install"/L"Files"/L"General"/L"exact-delete.txt"),"explicit exact newly owned payload removed");
        check(std::filesystem::exists(root/L"External"/L"taken.txt"),"unrelated payload untouched by exact delete");
    }
    {
        auto ancestor=CreateFileW((root/L"Install"/L"Files").c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS,nullptr);
        check(ancestor==INVALID_HANDLE_VALUE&&GetLastError()==ERROR_SHARING_VIOLATION,"held ancestor cannot be renamed during payload operation");if(ancestor!=INVALID_HANDLE_VALUE)CloseHandle(ancestor);
    }
    {
        auto deep=root/L"Install"/L"Files";
        for(unsigned n=0;n<3;++n){deep/=std::wstring(60,static_cast<wchar_t>(L'a'+n));
            const std::filesystem::path native=L"\\\\?\\"+deep.native();SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES),descriptor.get(),FALSE};
            check(CreateDirectoryW(native.c_str(),&security)!=FALSE,"new exact private long descendant directory");}
        check(deep.native().size()>160,"test descendant exceeds root-only cap");
        auto directory=native::FilePayloadDirectory::area(install,deep,{L"Files/General",L"Files/Uploads"},{},owner);
        const std::string name(120,'z');
        auto payload=directory.create(name,4);const unsigned char data[]{4,3,2,1};payload.append(data,4);const auto original_long=payload.identity();
        check(payload.path().native().size()>260&&original_long.bytes==4,"actual Windows long payload path without global long-path policy changes");
        payload.rename_to(directory,"renamed.txt");check(payload.identity().file_id==original_long.file_id,"long-path native rename retains identity");
        check(directory.names()==std::vector<std::string>{"renamed.txt"},"long directory listing exact");payload.remove_exact();check(directory.names().empty(),"exact long-path newly owned deletion");
        refused([&]{native::FilePayloadDirectory::private_store(deep/L"..",owner);},"long paths still refuse aliases");
    }
    {
        check(general.available_bytes()>native::kFilePayloadFreeReserve,"actual available-space query on held NTFS directory");
        refused([&]{general.create("too-much-space.txt",static_cast<std::uint64_t>(INT64_MAX));},"space preflight refuses impossible allocation without creating a file");
        check(general.absent("too-much-space.txt"),"failed size preflight made no filesystem output");
        std::uint64_t ceiling=native::kFilePayloadFreeReserve+8;
        auto bounded=native::FilePayloadDirectory::private_store(general.path(),owner,[&]{return ceiling;});
        refused([&]{bounded.create("refused-space.txt",9);},"injected low-space boundary rejects declared maximum before create");
        auto payload=bounded.create("low-space.txt",8);const unsigned char bytes[]{1,2,3,4};payload.append(bytes,4);
        ceiling=native::kFilePayloadFreeReserve;refused([&]{payload.append(bytes,1);},"space is rechecked before each actual write");
        check(payload.identity().bytes==4&&bounded.absent("refused-space.txt"),"write refusal retains exact bounded partial bytes without cleanup");
    }
    {
        std::vector<std::string> expected=general.names();
        for(unsigned n=0;n<23;++n){const auto name="page-"+std::to_string(n)+".txt";auto payload=general.create(name,1);payload.identity();expected.push_back(name);}
        std::sort(expected.begin(),expected.end());auto enumeration=general.enumerate();std::vector<std::string> actual;unsigned pages{};
        for(;;){const auto page=enumeration.next(3);check(page.names.size()<=3,"directory page bounded");actual.insert(actual.end(),page.names.begin(),page.names.end());++pages;if(page.finished)break;}
        std::sort(actual.begin(),actual.end());check(actual==expected&&pages>=8,"real directory cursor returns all safe names once across small pages");
        check(enumeration.next(3).finished&&enumeration.next(3).names.empty(),"completed directory cursor remains terminal");
        auto cancelled=general.enumerate();refused([&]{cancelled.next(3,[]{return false;});},"native directory scan cancellation makes no payload changes");
        check(general.names()==expected,"cancelled directory listing preserves every file");
    }
    {
        sfx::application::FilePayloadIdentity unicode_identity;
        {auto payload=general.create(u8"École-\U00010400.txt",16);payload.append(reinterpret_cast<const std::uint8_t*>("unicode"),7);unicode_identity=payload.identity();}
        check(general.read(u8"École-\U00010400.txt").identity().file_id==unicode_identity.file_id,"UTF-8 accented/supplementary native name reopens exact object");
        refused([&]{general.create(u8"éCOLE-\U00010400.TXT",16);},"native case-insensitive collision cannot overwrite");
        refused([&]{general.create("\xed\xa0\x80.txt",16);},"invalid surrogate encoding cannot become native path");
    }
    {
        auto payload=general.create("transfer-handles.bin",4);const auto object=payload.object_identity();
        refused([&]{payload.duplicate_transfer_handle(false);},"unsealed new receive cannot become send input");
        {
            auto write=payload.duplicate_transfer_handle(true);DWORD flags{},done{};const unsigned char bytes[]{1,2,3,4};unsigned char read{};
            check(GetHandleInformation(write.get(),&flags)&&(flags&HANDLE_FLAG_INHERIT),"only explicit transfer duplicate inheritable");
            check(ins::file_id(write.get())==std::stoull(object.file_id),"transfer duplicate exact original identity");
            check(!ReadFile(write.get(),&read,1,&done,nullptr)&&GetLastError()==ERROR_ACCESS_DENIED,"receive duplicate cannot read payload");
            FILE_DISPOSITION_INFO remove{TRUE};check(!SetFileInformationByHandle(write.get(),FileDispositionInfo,&remove,sizeof(remove))&&GetLastError()==ERROR_ACCESS_DENIED,"receive duplicate cannot delete original");
            check(WriteFile(write.get(),bytes,4,&done,nullptr)&&done==4,"actual receive duplicate bounded write");
            refused([&]{payload.duplicate_transfer_handle(true);},"nonempty object cannot restart receive");
        }
        const auto final=payload.identity();check(final.bytes==4&&final.file_id==object.file_id,"original lease seals and hashes exact transferred object");
        refused([&]{payload.duplicate_transfer_handle(true);},"sealed receive cannot regain write duplicate");
        {
            auto read=payload.duplicate_transfer_handle(false);DWORD done{};unsigned char data[4]{};LARGE_INTEGER zero{};
            check(SetFilePointerEx(read.get(),zero,nullptr,FILE_BEGIN)&&ReadFile(read.get(),data,4,&done,nullptr)&&done==4&&data[0]==1&&data[3]==4,"send duplicate reads exact sealed payload");
            check(!WriteFile(read.get(),data,1,&done,nullptr)&&GetLastError()==ERROR_ACCESS_DENIED,"send duplicate cannot change source");
            FILE_DISPOSITION_INFO remove{TRUE};check(!SetFileInformationByHandle(read.get(),FileDispositionInfo,&remove,sizeof(remove))&&GetLastError()==ERROR_ACCESS_DENIED,"send duplicate cannot delete source");
        }
        auto over=general.create("external-overcap.bin",1);
        {auto write=over.duplicate_transfer_handle(true);DWORD n{};check(WriteFile(write.get(),"xx",2,&n,nullptr)&&n==2,"controlled external writer violates its contract for validation test");}
        refused([&]{over.identity();},"parent independently rejects helper actual-byte cap violation");
        check(std::filesystem::file_size(over.path())==2,"unexpected bytes retained privately, not falsely accepted or deleted");
    }
    {
        std::filesystem::path offline;const auto drives=GetLogicalDrives();
        for(wchar_t letter=L'Z';letter>=L'D';--letter){const auto drive=std::wstring(1,letter)+L":\\";if(!(drives&(1UL<<(letter-L'A')))&&GetDriveTypeW(drive.c_str())==DRIVE_NO_ROOT_DIR){offline=drive+L"OfflineArea";break;}}
        check(!offline.empty(),"an actually unused drive letter found without disk creation/mounts");
        auto healthy=native::FilePayloadDirectory::area(install,L"Files/General",{offline},{root/L"Backup"},owner);check(healthy.names()==general.names(),"unrelated offline-area exclusion does not disable healthy C: library");
        auto input=native::FilePayloadDirectory::import_source(root/L"External",{install,offline},owner);check(input.path()==external.path(),"unrelated offline protected exclusion does not block actual C: import input");
        refused([&]{native::FilePayloadDirectory::area(install,offline,{}, {},owner);},"the actually selected offline volume remains unavailable; no phantom directory/adoption");
    }
    std::cout<<"PASS native file payload "<<checks<<" checks; owned NTFS fixture, not installed/client/GUI qualification\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL native file payload after "<<checks<<": "<<e.what()<<" win32="<<GetLastError()<<'\n';return 1;}}
