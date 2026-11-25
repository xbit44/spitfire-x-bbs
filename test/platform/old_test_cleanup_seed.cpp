// Creates four historical retained test files through normal offline owner
// APIs in the fresh RC16 installation only. Production roots are refused.
#include "network_setup_fixture.hpp"
#include "sfx/platform/win32/offline_file_administration.hpp"
#include "sfx/platform/win32/installation_security.hpp"
#include "sfx/archive/archive_engine.hpp"
#include <objbase.h>
#include <fstream>
#include <iostream>
namespace app=sfx::application;namespace native=sfx::platform::win32;namespace ins=native::installation;
app::Identifier128 fresh(){app::Identifier128 id;ins::require(CoCreateGuid(reinterpret_cast<GUID*>(id.bytes.data()))==S_OK,"seed_id");return id;}
app::FileManagementJobStatus run(native::OfflineFileAdministration& session,app::FileManagementTask task){task.job=fresh();auto status=session.start(task);const auto begun=GetTickCount64();while(status.state==app::FileManagementJobState::running&&GetTickCount64()-begun<300000){Sleep(25);status=session.status(task.job);}ins::require(status.state==app::FileManagementJobState::completed&&(task.kind==app::FileManagementTaskKind::preview_import||status.journal_confirmed),status.detail.c_str());return status;}
int wmain(int argc,wchar_t** argv)try{
    ins::require(argc==2&&!ins::elevated(),"new seed evidence directory only");const auto evidence=std::filesystem::absolute(argv[1]);ins::require(!std::filesystem::exists(evidence),"preserve prior seeds");std::filesystem::create_directories(evidence);
    const auto root=ins::canonical_root(network_setup_qualification_root());std::string diagnostic;auto session=native::OfflineFileAdministration::begin(root,ins::operator_sid(),diagnostic);ins::require(bool(session),diagnostic.c_str());
    {std::ofstream data(evidence/L"dummy20.bin",std::ios::binary);const std::string block(65536,'T');for(unsigned i=0;i<320;++i)data.write(block.data(),block.size());ins::require(bool(data),"seed_payload_write");}
    std::ofstream keys(evidence/L"keys.txt");
    for(unsigned i=0;i<4;++i){const auto filename="20meg"+std::to_string(i+1)+".zip";sfx::archive::CreateZipRequest zip;zip.source_root=evidence;zip.partial_path=evidence/(filename+".partial");zip.destination_path=evidence/filename;zip.input_paths={"dummy20.bin"};zip.compression_level=0;ins::require(sfx::archive::create_zip(zip).ok(),"seed_zip");
        app::FileManagementTask task;task.kind=app::FileManagementTaskKind::preview_import;task.choice.operation=fresh();task.choice.area={1};task.choice.source_path=zip.destination_path.u8string();task.choice.filename=filename;task.choice.use_diz=false;task.choice.allow_duplicate_content=true;
        const auto preview=run(*session,task);ins::require(preview.preview.has_value(),"seed_preview");task={};task.kind=app::FileManagementTaskKind::import_file;task.import=*preview.preview;const auto imported=run(*session,task);
        const auto original=session->detail(imported.result);ins::require(original&&original.value,"seed_registered");task={};task.operation.operation=fresh();task.operation.file=imported.result;task.operation.expected_revision=original.value->file.revision;task.operation.action=app::FileOperationAction::remove;run(*session,task);
        const auto retained=session->detail(imported.result);ins::require(retained&&retained.value&&!retained.value->purged&&retained.value->file.state==app::FileCatalogState::removed,"seed_historical_retained");keys<<retained.value->removed_key<<'\n';
    }
    ins::require(bool(keys),"seed_keys_write");std::cout<<"PASS four synthetic retained test ZIPs created through normal owner APIs\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}
