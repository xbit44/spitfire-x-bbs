// QA34 actual Win32/private SQLite interoperability. No installed restore claim.
#include "sfx/adapters/storage/sqlite_storage.hpp"
#include "sfx/platform/win32/storage_path_policy.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include <iostream>
namespace ins=sfx::platform::win32::installation;
namespace win=sfx::platform::win32;
namespace app=sfx::application;
namespace db=sfx::adapters::storage;
namespace {
unsigned checks{};
void check(bool value,const char* text){++checks;if(!value)throw std::runtime_error(text);}
ins::Handle lease(const std::filesystem::path& file){return ins::open_object(file,false,GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE);}
void denies_delete(const std::filesystem::path& file){
    SetLastError(ERROR_SUCCESS);const auto raw=CreateFileW(file.c_str(),DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);const auto error=GetLastError();
    if(raw!=INVALID_HANDLE_VALUE)CloseHandle(raw);
    check(raw==INVALID_HANDLE_VALUE&&error==ERROR_SHARING_VIOLATION,"actual data-read lease refuses DELETE by sharing, not ACL");
}
}
int main(int argc,char** argv){try{
    check(argc==2&&!ins::elevated(),"ordinary token and new private fixture required");
    const auto root=std::filesystem::absolute(argv[1]).make_preferred();check(!std::filesystem::exists(root),"preserve old evidence");
    const auto who=ins::operator_sid();const auto descriptor=ins::mutable_descriptor(who);
    ins::create_directory(root,descriptor.get());ins::create_directory(root/L"Data",descriptor.get());
    win::WindowsLocalStoragePathPolicy paths(who);db::SqliteStorageSettings settings;settings.data_directory=root/L"Data";
    db::SqliteStorage storage(settings,paths);const auto file=root/L"Data"/L"SFX.DB";
    check(storage.start().succeeded()&&storage.shutdown().succeeded(),"normal shipping native path/schema initialization then settled writer");
    const auto original=storage.verified_ftn_serial_history(file);check(original&&original.value&&!original.value->predecessor&&!original.value->recovery_required,"normal read-only known history before lease");
    {
        auto control=ins::open_object(file,false,DELETE|READ_CONTROL);check(bool(control),"same token has DELETE rights without lease");
        SetLastError(ERROR_SUCCESS);const auto raw=CreateFileW(file.c_str(),GENERIC_READ|READ_CONTROL,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr);const auto error=GetLastError();
        if(raw!=INVALID_HANDLE_VALUE)CloseHandle(raw);check(raw==INVALID_HANDLE_VALUE&&error==ERROR_SHARING_VIOLATION,"capture immediate sharing error for preexisting DELETE conflict");
        bool refused{};try{auto blocked=lease(file);}catch(const std::exception&){refused=true;}
        check(refused,"preexisting DELETE handle refuses the exact proposed pin");
    }
    const auto initial_hash=ins::file_sha256(file);
    {
        auto pin=lease(file);const auto id=ins::file_id(pin.get());denies_delete(file);
        const auto read=storage.verified_ftn_serial_history(file);check(read&&read.value&&read.value->lineage==original.value->lineage&&read.value->high_water==original.value->high_water,"normal schema and read-only history discovery works under real read pin");
        check(ins::file_sha256(file)==initial_hash,"read-only discovery preserves DB bytes");
        check(storage.start().succeeded(),"normal actual SQLite writer opens beneath read/no-delete pin");
        check(storage.preserve_ftn_serial_history({true,{}},original.value).succeeded(),"actual preservation transaction permitted without arbitrary floor");
        const auto state=storage.ftn_serial_state({true,{}});check(state&&state.value&&!state.value->recovery_required&&state.value->lineage==original.value->lineage&&state.value->high_water==original.value->high_water,"same known history preserved through writer transaction");
        check(storage.shutdown().succeeded(),"SQLite checkpoint/shutdown allowed while pin stays held");
        denies_delete(file);check(ins::file_id(pin.get())==id,"exact pinned DB object retained after normal shutdown");
    }
    {
        auto pin=lease(file);check(storage.start().succeeded(),"actual store reopens under new exact pin");
        check(storage.preserve_ftn_serial_history({true,{}},{}).succeeded(),"normal unknown-prior publication retains coherent candidate through actual writer");
        const auto held=storage.ftn_serial_state({true,{}});check(held&&held.value&&!held.value->recovery_required,"no artificial unknown-history hold created");
        check(storage.shutdown().succeeded(),"held store stops normally with read pin retained");denies_delete(file);
    }
    const auto held=storage.verified_ftn_serial_history(file);check(held&&held.value&&!held.value->recovery_required,"healthy history survives stopped read-only verification");
    {auto control=ins::open_object(file,false,DELETE|READ_CONTROL);check(bool(control),"final pin release restores ordinary DELETE-open rights without deleting object");}
    std::cout<<"PASS "<<checks<<" actual private Win32 SQLite read/write/history/pin/conflict checks; fixture retained held; no installed restore/rollback/platform claim\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<checks<<": "<<error.what()<<'\n';return 1;}}
